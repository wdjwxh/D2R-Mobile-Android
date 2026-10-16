/* Included by vk_backend.c: one lock protects device, pools and submissions.
 * Portable baseline uses separate host-visible Vulkan storage. It does NOT
 * assume Metal's newBufferWithBytesNoCopy or unified CPU/GPU virtual addresses.
 * Future draw/dispatch replay must upload referenced CPU-written ranges before
 * GPU reads, and download GPU-written ranges at guest visibility boundaries. */
#include "nvn/nvn_dirty.h"
#include <unistd.h>
typedef struct pool_gpu_range { struct pool_gpu_range *next;size_t lo,hi;uint64_t writer; } pool_gpu_range;
enum { POOL_PREP_OTHER,POOL_PREP_VERTEX,POOL_PREP_INDEX,POOL_PREP_SSBO,POOL_PREP_TEXEL,POOL_PREP_COMPUTE,POOL_PREP_B2B,POOL_PREP_UBO,POOL_PREP_COUNT };
typedef struct {
    uint64_t requested,max_request,unknown,cold,lru_slot,free_slot,gap,evictions,invalidations;
    uint64_t range_compare,page_compare,range_snapshot,range_changed_pagewalk;
    uint64_t range_chunk_skips,range_chunk_uploads;
} pool_prepare_stats;
static unsigned pool_prepare_consumer;
struct dm_vk_pool {
    struct dm_vk_pool *next;
    uint8_t *guest, *mapped;
    size_t size;
    uint32_t flags; // Full NVN memory pool flags, independent of Vulkan memory type.
    uint64_t materialize_bytes,materialize_ranges; // Current frame-report window.
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkBool32 coherent;
    int shared_heap; // Borrowed allocation/map, independently owned VkBuffer.
    pool_gpu_range *gpu_dirty;
    uint64_t last_compute_use; // Compute/copy FIFO reads survive dirty ownership changes.
    pool_prepare_stats prepare[POOL_PREP_COUNT];
};
static dm_vk_pool *pools;
static unsigned pool_gpu_dirty_count; // Backend-lock-owned pool count.
static uint64_t pool_auto_skip_count;
// One atomic snapshot covers owned pools, active dispatch preparation and
// pending FIFO entries. Separate atomic zero tests could miss a transition
// from pending GPU work to completed-but-still-GPU-owned bytes.
static unsigned pool_cpu_hazards;
static void pool_prepare_begin(dm_vk_pool *p,size_t bytes,unsigned consumer) {
    pool_prepare_stats *s=&p->prepare[consumer];s->requested+=bytes;
    if(bytes>s->max_request)s->max_request=bytes;
}
static void pool_prepare_report(void) {
    static const char *names[]={"other","vertex","index","SSBO","texel","compute","B2B","UBO"};
    pool_prepare_stats *shown[8]={0};
    for(unsigned at=0;at<8;at++) {
        pool_prepare_stats *best=NULL;dm_vk_pool *owner=NULL;unsigned kind=0;
        for(dm_vk_pool *p=pools;p;p=p->next)for(unsigned i=0;i<POOL_PREP_COUNT;i++) {
            pool_prepare_stats *s=&p->prepare[i];if(!s->requested && !s->unknown)continue;
            int seen=0;for(unsigned j=0;j<at;j++)seen|=shown[j]==s;if(seen)continue;
            if(!best || s->unknown>best->unknown || (s->unknown==best->unknown && s->requested>best->requested)) {best=s;owner=p;kind=i;}
        }
        if(!best)break;shown[at]=best;
        LOG("Pool prepare: pool=%p poolBytes=%zu consumer=%s requested=%llu maxRequest=%llu unknownUpload=%llu cold=%llu lruSlot=%llu invalidatedFreeSlot=%llu gap=%llu evictions=%llu invalidations=%llu",
            (void *)owner->guest,owner->size,names[kind],(unsigned long long)best->requested,(unsigned long long)best->max_request,
            (unsigned long long)best->unknown,(unsigned long long)best->cold,(unsigned long long)best->lru_slot,
            (unsigned long long)best->free_slot,(unsigned long long)best->gap,(unsigned long long)best->evictions,(unsigned long long)best->invalidations);
    }
    // Upload volume and CPU snapshot volume have different hotspots. Rank the
    // latter independently so tiny unknown uploads cannot hide large captures.
    pool_prepare_stats *cpu_shown[8]={0};
    for(unsigned at=0;at<8;at++) {
        pool_prepare_stats *best=NULL;dm_vk_pool *owner=NULL;unsigned kind=0;
        for(dm_vk_pool *p=pools;p;p=p->next)for(unsigned i=0;i<POOL_PREP_COUNT;i++) {
            pool_prepare_stats *s=&p->prepare[i];
            if(!s->range_snapshot && !s->range_compare && !s->page_compare)continue;
            int seen=0;for(unsigned j=0;j<at;j++)seen|=cpu_shown[j]==s;if(seen)continue;
            if(!best || s->range_snapshot>best->range_snapshot ||
               (s->range_snapshot==best->range_snapshot &&
                s->range_compare+s->page_compare>best->range_compare+best->page_compare)) {
                best=s;owner=p;kind=i;
            }
        }
        if(!best)break;cpu_shown[at]=best;
        LOG("Pool CPU shadow: pool=%p consumer=%s rangeCompareRequested=%llu pageCompareRequested=%llu snapshotBytes=%llu changedPageWalkBytes=%llu unchangedChunks=%llu changedChunks=%llu",
            (void *)owner->guest,names[kind],(unsigned long long)best->range_compare,
            (unsigned long long)best->page_compare,(unsigned long long)best->range_snapshot,
            (unsigned long long)best->range_changed_pagewalk,(unsigned long long)best->range_chunk_skips,
            (unsigned long long)best->range_chunk_uploads);
    }
    for(dm_vk_pool *p=pools;p;p=p->next)memset(p->prepare,0,sizeof p->prepare);
}
static void pool_cpu_hazard_add(void) {
    __atomic_fetch_add(&pool_cpu_hazards,1,__ATOMIC_RELEASE);
}
static void pool_cpu_hazard_remove(void) {
    if(!__atomic_fetch_sub(&pool_cpu_hazards,1,__ATOMIC_RELEASE))
        die("Vulkan CPU buffer hazard accounting underflow");
}
#include "vk_pool_shadow.inc"
static void graphics_uniform_retire_range(uint64_t address,size_t size);
static void graphics_buffer_retire_range(uint64_t address,size_t size);
static void graphics_texel_cache_retire(VkBuffer buffer);
enum { GRAPHICS_DESCRIPTOR_BUFFER,GRAPHICS_DESCRIPTOR_IMAGE_VIEW,GRAPHICS_DESCRIPTOR_SAMPLER,GRAPHICS_DESCRIPTOR_TEXEL_VIEW };
static void graphics_descriptor_retire_object(unsigned kind,uint64_t handle);
static int pool_cpu_read_consumer_locked(dm_vk_pool *p,size_t offset,size_t size,const char *consumer);
// Attribute only actual GPU-to-CPU copies. Function identity is a compile-time
// string; normal reads add no clocks, stack walks, or per-call logging.
#define pool_cpu_read_locked(p,offset,size) pool_cpu_read_consumer_locked(p,offset,size,__func__)
static struct { const char *name;uint64_t bytes,ranges; } pool_materialize_consumers[24];
static void pool_materialize_consumer_add(const char *name,size_t bytes) {
    unsigned slot=23;
    for(unsigned i=0;i<23;i++) {
        if(!pool_materialize_consumers[i].name || !strcmp(pool_materialize_consumers[i].name,name)){slot=i;break;}
    }
    if(!pool_materialize_consumers[slot].name)pool_materialize_consumers[slot].name=slot==23?"overflow":name;
    pool_materialize_consumers[slot].bytes+=bytes;pool_materialize_consumers[slot].ranges++;
}
static void pool_materialize_consumers_report(void) {
    for(unsigned i=0;i<24;i++)if(pool_materialize_consumers[i].bytes) {
        LOG("Buffer materialize consumer: function=%s MiB=%.4f ranges=%llu",pool_materialize_consumers[i].name,
            pool_materialize_consumers[i].bytes/1048576.0,(unsigned long long)pool_materialize_consumers[i].ranges);
        pool_materialize_consumers[i].bytes=pool_materialize_consumers[i].ranges=0;
    }
    for(dm_vk_pool *p=pools;p;p=p->next)if(p->materialize_bytes) {
        LOG("Buffer materialize pool: base=%p size=%zu flags=%#x bytes=%llu ranges=%llu",
            (void *)p->guest,p->size,p->flags,(unsigned long long)p->materialize_bytes,
            (unsigned long long)p->materialize_ranges);
        p->materialize_bytes=p->materialize_ranges=0;
    }
    if(pool_auto_skip_count)LOG("Buffer materialize autoSkipPools=%llu",(unsigned long long)pool_auto_skip_count);
    pool_auto_skip_count=0;
}
static int pools_cpu_read_all_locked(void);
static int images_flush_cpu_overwrite(uint64_t address,size_t size);

// Recorded/submitted compute writes own these bytes until materialized. Each interval
// carries its producer fence serial; completion alone does not make guest RAM
// current. Pool read/write hazards are tracked separately from this list.
static int pool_gpu_forget_locked(dm_vk_pool *p,size_t offset,size_t size) {
    if(!size)return 0;
    int had=p->gpu_dirty!=NULL;
    size_t end=offset+size;pool_gpu_range **link=&p->gpu_dirty;
    while(*link) {
        pool_gpu_range *r=*link;
        if(r->lo>=end)break;
        if(r->hi<=offset){link=&r->next;continue;}
        if(r->lo<offset && r->hi>end) {
            pool_gpu_range *tail=malloc(sizeof *tail);if(!tail)return -1;
            *tail=(pool_gpu_range){r->next,end,r->hi,r->writer};r->hi=offset;r->next=tail;break;
        }
        if(r->lo<offset){r->hi=offset;link=&r->next;continue;}
        if(r->hi>end){r->lo=end;break;}
        *link=r->next;free(r);
    }
    if(had && !p->gpu_dirty){pool_gpu_dirty_count--;pool_cpu_hazard_remove();}
    return 0;
}
static int pool_gpu_written_locked(dm_vk_pool *p,size_t offset,size_t size,uint64_t writer) {
    if(!p || offset>p->size || size>p->size-offset || !p->mapped)return -1;
    if(!size)return 0;
    // Device writes to shared backing do not fault through the CPU page
    // tracker. Invalidate its generations now; consumers still wait for the
    // published writer before reading or taking a new content certificate.
    if(p->shared_heap)dm_guest_write_prep(p->guest+offset,size);
    unsigned count=0;for(pool_gpu_range *r=p->gpu_dirty;r;r=r->next)count++;
    if(count>=128 && pool_cpu_read_locked(p,0,p->size))return -1;
    pool_gpu_range *fresh=malloc(sizeof *fresh);if(!fresh)return -1;
    int had=p->gpu_dirty!=NULL;
    size_t lo=offset,hi=offset+size;pool_gpu_range **link=&p->gpu_dirty;
    while(*link && (*link)->hi<lo)link=&(*link)->next;
    while(*link && (*link)->lo<=hi) {
        pool_gpu_range *r=*link;if(r->lo<lo)lo=r->lo;if(r->hi>hi)hi=r->hi;
        if(r->writer>writer)writer=r->writer;
        *link=r->next;free(r);
    }
    *fresh=(pool_gpu_range){*link,lo,hi,writer};*link=fresh;
    if(!had){pool_gpu_dirty_count++;pool_cpu_hazard_add();}
    pool_shadow_invalidate(p,offset,size);
    graphics_uniform_retire_range((uintptr_t)p->guest+offset,size);
    graphics_buffer_retire_range((uintptr_t)p->guest+offset,size);
    return 0;
}

static void pool_free_locked(dm_vk_pool *p) {
    if(draw_pending_flush())die("Vulkan pool draw/post retirement failed");
    if(compute_wait_serial(p->last_compute_use))die("Vulkan pool compute retirement failed");
    if(p->buffer)graphics_texel_cache_retire(p->buffer);
    graphics_uniform_retire_range((uintptr_t)p->guest,p->size);
    graphics_buffer_retire_range((uintptr_t)p->guest,p->size);
    nvn_dirty_remove_range(p->guest);
    pool_shadow_invalidate(p,0,p->size);
    pool_range_forget(p);
    if(p->gpu_dirty){pool_gpu_dirty_count--;pool_cpu_hazard_remove();}
    while(p->gpu_dirty){pool_gpu_range *r=p->gpu_dirty;p->gpu_dirty=r->next;free(r);}
    if(p->mapped && !p->shared_heap) vkUnmapMemory(v.device,p->memory);
    if(p->buffer) {
        graphics_descriptor_retire_object(GRAPHICS_DESCRIPTOR_BUFFER,(uint64_t)(uintptr_t)p->buffer);
        vkDestroyBuffer(v.device,p->buffer,NULL);
    }
    if(p->memory && !p->shared_heap) device_memory_free(p->memory);
    free(p);
}
static void pools_close_locked(void) {
    while(pools) { dm_vk_pool *p=pools; pools=p->next; pool_free_locked(p); }
    pool_shadow_close();
    if(pool_gpu_dirty_count || __atomic_load_n(&pool_cpu_hazards,__ATOMIC_ACQUIRE))
        die("Vulkan CPU buffer hazards remained after close");
    __atomic_store_n(&pool_cpu_hazards,0,__ATOMIC_RELEASE);
    pool_auto_skip_count=0;
}
static dm_vk_pool *pool_find_locked(dm_vk_pool *p) {
    for(dm_vk_pool *it=pools;it;it=it->next) if(it==p) return it;
    return NULL;
}
static dm_vk_pool *pool_address_locked(uint64_t address, size_t size, size_t *offset) {
    for(dm_vk_pool *p=pools;p;p=p->next) {
        uint64_t base=(uintptr_t)p->guest;
        if(address>=base && address-base<=p->size && size<=p->size-(address-base)) {
            *offset=(size_t)(address-base); return p;
        }
    }
    return NULL;
}
// Explicit upload source lets validated index snapshots reach the GPU without
// re-reading mutable guest bytes between bounds scanning and submission.
static int pool_sync_source_pages_locked(dm_vk_pool *p, size_t offset, size_t size, int download,const uint8_t *source) {
    if(!p || offset>p->size || size>p->size-offset) return -1;
    if(!size || !p->mapped) return 0;
    if(p->shared_heap) {
        // Coherent backing already contains guest stores; submissions supply
        // host-write visibility. Real CPU consumers waited for writers before
        // entering this function. There is no second copy to materialize.
        if(download || !source || source==p->guest+offset)return 0;
        if(compute_wait_pool_range_locked(p,offset,size) || draw_pending_write((uintptr_t)p->guest+offset,size))return -1;
        dm_guest_write_prep(p->guest+offset,size);
        memmove(p->guest+offset,source,size);return 0;
    }
    // Noncoherent operations invalidate the whole allocation. Coherent
    // unchanged uploads can compare existing CPU shadows without waiting;
    // their actual mapped reads/writes wait below, including full fresh pages.
    if((download || !p->coherent) && compute_wait_serial(p->last_compute_use))return -1;
    int guest_source=source==NULL;
    if(!source)source=p->guest+offset;
    // Nonresident pools contain CPU data only; flushing or invalidating them
    // must not allocate a Vulkan buffer merely for texture/RT reserved bytes.
    // Whole allocation avoids nonCoherentAtomSize alignment hazards.
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=p->memory,.offset=0,.size=VK_WHOLE_SIZE};
    if(download) {
        // GPU writes may have changed any cached destination bytes. Discard
        // before invalidate so failures cannot leave an apparently valid page.
        pool_shadow_invalidate(p,offset,size);
        if(!p->coherent && vkInvalidateMappedMemoryRanges(v.device,1,&range)) return -1;
        dm_guest_write_prep(p->guest+offset,size);
        memcpy(p->guest+offset,p->mapped+offset,size);
    } else {
        // Preserve noncoherent atoms outside the exact uploaded subranges.
        // Invalidate before any uploads, so this cannot discard this call's
        // as-yet-unflushed CPU writes. No CPU shadow reads mapped GPU bytes.
        if(!p->coherent && vkInvalidateMappedMemoryRanges(v.device,1,&range)) {
            pool_shadow_invalidate(p,offset,size);return -1;
        }
        int waited=0,changed=0;
        for(size_t pos=offset,end=offset+size;pos<end;) {
            size_t page=pos/POOL_SHADOW_PAGE,base=page*POOL_SHADOW_PAGE;
            size_t length=p->size-base;if(length>POOL_SHADOW_PAGE)length=POOL_SHADOW_PAGE;
            size_t within=pos-base,n=length-within;if(n>end-pos)n=end-pos;
            int fresh=0;pool_shadow_page *e=pool_shadow_get(p,page,&fresh);
            int unknown_origin=e?fresh:1; // Slot source, not a proof of this key's past eviction.
            // Unknown shadow bytes never require GPU-to-CPU reads. Upload only
            // this request's CPU-authoritative interval using one bounded copy.
            // Allocation failure takes that same path without retaining a shadow.
            _Alignas(4) uint8_t capture[POOL_SHADOW_PAGE];int captured=0;
            const uint8_t *bytes=source+(pos-offset);
            // Nativebridge has aborted in HandleHostSignal without delivering
            // an ARM fault context after expanding protection to buffer pages.
            // Keep native ARM certificates; translated buffers use CPU shadows.
            extern unsigned char dm_no_fp_state;
            int tracked=!dm_no_fp_state && guest_source && e && nvn_dirty_tracked(p->guest+pos,n);
            if(tracked && e->stamp && within>=e->cert_lo && within+n<=e->cert_hi) {
                gpu_metrics.page_certificate_scans++;
                if(!nvn_dirty_since(p->guest+pos,n,e->stamp)) {
                    gpu_metrics.page_certificate_hits++;pos+=n;continue;
                }
            }
            // An explicit immutable source describes its own version, not
            // current guest RAM, so it can never preserve a guest certificate.
            if(e)e->stamp=0;
            uint64_t stamp=0;
            if(tracked) {
                nvn_dirty_protect(p->guest+pos,n);stamp=nvn_dirty_now();
                memcpy(capture,bytes,n);bytes=capture;captured=1;
                gpu_metrics.page_snapshot_bytes+=n;
            }
            int known=e && pool_shadow_known(e,within,n);
            int different=!known;
            if(known) {
                gpu_metrics.page_compare_requested_bytes+=n;
                p->prepare[pool_prepare_consumer].page_compare+=n;
                different=!dm_vk_bytes_equal(e->bytes+within,bytes,n);
            }
            if(different) {
                if(!captured) {memcpy(capture,bytes,n);bytes=capture;}
                // Noncoherent flush covers the allocation; retain the existing
                // whole-pool reader hazard, once per sync rather than per page.
                if(!waited) {
                    // Protect the entire sync request, not just this first
                    // changed page: subsequent pages share this wait proof.
                    if(compute_wait_pool_range_locked(p,offset,size)) {
                        pool_shadow_invalidate(p,offset,size);return -1;
                    }
                    if(draw_pending_write((uintptr_t)p->guest+(p->coherent?offset:0),p->coherent?size:p->size)) {
                        pool_shadow_invalidate(p,offset,size);return -1;
                    }
                    // Retirement callbacks may evict/recycle an entry. Never
                    // reuse a saved pointer or rebuild it by reading mapped RAM.
                    e=pool_shadow_get(p,page,&fresh);
                    // GPU/report writes evict affected entries; a surviving
                    // lookup still proves its bytes. Fresh entries start empty.
                    if(e)e->stamp=0;
                    waited=1;
                }
                memcpy(p->mapped+pos,bytes,n);
                // Patch only snapshots still valid after any retirement above.
                // The wrapper's own in-progress snapshot remains invalid until
                // every page and the final noncoherent flush have succeeded.
                pool_range_patch_uploaded(p,pos,n,bytes);
                if(!known) {
                    gpu_metrics.unknown_shadow_upload_bytes+=n;
                    pool_prepare_stats *stats=&p->prepare[pool_prepare_consumer];stats->unknown+=n;
                    if(unknown_origin==2)stats->lru_slot+=n;
                    else if(unknown_origin==3)stats->free_slot+=n;
                    else if(unknown_origin || !e)stats->cold+=n;
                    else stats->gap+=n;
                }
                if(e) {
                    memcpy(e->bytes+within,bytes,n);
                    pool_shadow_mark_known(e,within,n);
                }
                gpu_metrics.pool_upload_bytes+=n;
                changed=1;
            }
            if(tracked && e) {
                gpu_metrics.page_certificate_scans++;
                if(!nvn_dirty_since(p->guest+pos,n,stamp)) {
                    e->stamp=stamp;e->cert_lo=(uint16_t)within;e->cert_hi=(uint16_t)(within+n);
                }
            }
            pos+=n;
        }
        if(changed && !p->coherent && vkFlushMappedMemoryRanges(v.device,1,&range)) {
            pool_shadow_invalidate(p,offset,size);return -1;
        }
    }
    return 0;
}
static int pool_sync_source_bytes_locked(dm_vk_pool *p,size_t offset,size_t size,int download,const uint8_t *source) {
    if(!p || offset>p->size || size>p->size-offset)return -1;
    if(!size || !p->mapped)return 0;
    if(p->shared_heap)return pool_sync_source_pages_locked(p,offset,size,download,source);
    // Explicit index snapshots retain their own validated source and patch
    // overlapping valid range shadows only after changing mapped bytes.
    if(download || source)return pool_sync_source_pages_locked(p,offset,size,download,source);
    pool_range_shadow *e=pool_range_get(p,offset,size);
    if(!e)return pool_sync_source_pages_locked(p,offset,size,0,NULL);
    const uint8_t *guest=p->guest+offset;
    // Match the page cache's nativebridge compatibility boundary. Existing
    // texture tracking is independent and retains its verified write decoder.
    extern unsigned char dm_no_fp_state;
    int tracked=!dm_no_fp_state && nvn_dirty_tracked(guest,size);
    if(e->valid && tracked && e->stamp) {
        gpu_metrics.pool_certificate_scans++;
        if(!nvn_dirty_since(guest,size,e->stamp)) {
            gpu_metrics.pool_certificate_hits++;return 0;
        }
    }
    if(e->valid && !tracked) {
        // Requested comparison bytes, not actual reads: memcmp may exit early.
        gpu_metrics.range_compare_requested_bytes+=size;
        p->prepare[pool_prepare_consumer].range_compare+=size;
        size_t equal_prefix=dm_vk_equal_prefix(e->bytes,guest,size);
        if(equal_prefix==size)return 0;
        p->prepare[pool_prepare_consumer].range_changed_pagewalk+=size;
        // The range snapshot already describes the mapped version. Let exact
        // page uploads patch only its changed pieces using the same captured
        // bytes, rather than copying an entire large SSBO for a tiny change.
        // Retirement or failure may invalidate it; never restore valid here.
        if(p->coherent) {
            size_t begin=offset+equal_prefix;
            begin-=begin%POOL_SHADOW_PAGE;
            if(begin<offset)begin=offset;
            gpu_metrics.range_prefix_skipped_bytes+=begin-offset;
            for(size_t pos=begin,end=offset+size;pos<end;) {
                // A page upload may retire callbacks. Recheck the current
                // slot before reading its snapshot, never a saved bytes ptr.
                if(e->pool!=p || e->offset!=offset || e->size!=size || !e->valid || !e->bytes)
                    return pool_sync_source_pages_locked(p,pos,end-pos,0,NULL);
                size_t count=POOL_SHADOW_PAGE-pos%POOL_SHADOW_PAGE;
                if(count>end-pos)count=end-pos;
                gpu_metrics.page_compare_requested_bytes+=count;
                p->prepare[pool_prepare_consumer].page_compare+=count;
                // This valid range already proves the mapped bytes: unchanged
                // chunks need no page hash, LRU operation or known-byte scan.
                if(dm_vk_bytes_equal(e->bytes+(pos-offset),p->guest+pos,count))
                    p->prepare[pool_prepare_consumer].range_chunk_skips++;
                else {
                    p->prepare[pool_prepare_consumer].range_chunk_uploads++;
                    if(pool_sync_source_pages_locked(p,pos,count,0,NULL))return -1;
                }
                pos+=count;
            }
            return 0;
        }
        // Noncoherent memory retains one allocation-wide invalidate/flush.
        return pool_sync_source_pages_locked(p,offset,size,0,NULL);
    }
    e->valid=0;e->stamp=0;
    uint64_t stamp=0;
    if(tracked) { nvn_dirty_protect(guest,size);stamp=nvn_dirty_now(); }
    // Capture once: upload and the future comparison must describe exactly
    // the same bytes, even if the guest modifies its range during this call.
    memcpy(e->bytes,guest,size);
    gpu_metrics.range_snapshot_bytes+=size;
    p->prepare[pool_prepare_consumer].range_snapshot+=size;
    int result=pool_sync_source_pages_locked(p,offset,size,0,e->bytes);
    if(!result) {
        e->valid=1;
        // BUSY/NONE and writes during capture/upload prevent certification.
        // Keep the exact CPU snapshot even when it cannot be certified clean.
        if(tracked) {
            gpu_metrics.pool_certificate_scans++;
            if(!nvn_dirty_since(guest,size,stamp))e->stamp=stamp;
        }
    }
    return result;
}
static int pool_sync_source_locked(dm_vk_pool *p,size_t offset,size_t size,int download,const uint8_t *source) {
    int result=pool_sync_source_bytes_locked(p,offset,size,download,source);
    // Both an explicit CPU upload and a completed download hand these exact
    // bytes back to the CPU. Never later materialize an older GPU version.
    if(!result && p)result=pool_gpu_forget_locked(p,offset,size);
    return result;
}
static int pool_sync_locked(dm_vk_pool *p,size_t offset,size_t size,int download) {
    return pool_sync_source_locked(p,offset,size,download,NULL);
}
static int pool_upload_consumer_locked(dm_vk_pool *p,size_t offset,size_t size,const uint8_t *source,unsigned consumer) {
    unsigned previous=pool_prepare_consumer;pool_prepare_consumer=consumer;pool_prepare_begin(p,size,consumer);
    int result=pool_sync_source_locked(p,offset,size,0,source);pool_prepare_consumer=previous;return result;
}
// Event producers resolve real GPU bytes separately from deferred event posts.
static int pool_cpu_read_dirty_locked(dm_vk_pool *p,size_t offset,size_t size,const char *consumer) {
    if(!p || offset>p->size || size>p->size-offset)return -1;
    size_t end=offset+size;
    for(;;) {
        pool_gpu_range *r=p->gpu_dirty;
        while(r && r->hi<=offset)r=r->next;
        if(!r || r->lo>=end)return 0;
        size_t lo=r->lo>offset?r->lo:offset,hi=r->hi<end?r->hi:end;
        if(compute_wait_serial(r->writer))return -1;
        if(pool_sync_locked(p,lo,hi-lo,1))return -1;
        if(!p->shared_heap) {
            gpu_metrics.buffer_materialize_bytes+=hi-lo;gpu_metrics.buffer_materialize_ranges++;
            p->materialize_bytes+=hi-lo;p->materialize_ranges++;
            pool_materialize_consumer_add(consumer,hi-lo);
        }
    }
}
static int pools_cpu_read_address_locked(uint64_t address,size_t size) {
    if(size>UINT64_MAX-address)return -1;
    for(dm_vk_pool *p=pools;p;p=p->next) {
        uint64_t base=(uintptr_t)p->guest,lo=address>base?address:base;
        uint64_t end=address+size,hi=end<base+p->size?end:base+p->size;
        if(lo<hi && pool_cpu_read_locked(p,(size_t)(lo-base),(size_t)(hi-lo)))return -1;
    }
    return 0;
}
static int pools_cpu_read_all_locked(void) {
    for(dm_vk_pool *p=pools;p;p=p->next) {
        // NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS is 0x1. Finish/CPU fences still
        // complete the GPU, but cannot make this pool CPU-mappable. Preserve
        // its GPU authority until an explicit renderer conversion/read needs
        // a precise range. Do not discard its dirty intervals or hazard count.
        if(p->flags&0x1u){if(p->gpu_dirty)pool_auto_skip_count++;continue;}
        if(pool_cpu_read_locked(p,0,p->size))return -1;
    }
    return 0;
}
// The caller has retired a real CPU-visible fence's draw frontier. Reports
// before that frontier have already been published; later reports are not
// part of its visibility promise. Shared coherent bytes already occupy guest
// RAM and need no copy or invalidation. Keep all dirty writer metadata: an
// explicit range consumer still resolves it, but this older fence must not
// wait for a newer writer folded into a merged dirty interval.
static int pools_cpu_read_all_after_fence_locked(void) {
    for(dm_vk_pool *p=pools;p;p=p->next) {
        if(p->flags&0x1u){if(p->gpu_dirty)pool_auto_skip_count++;continue;}
        if(p->shared_heap && p->coherent) {
            perf_metrics.cpu_fence_shared_report_preflights_skipped++;
        } else if(pool_cpu_read_locked(p,0,p->size))return -1;
    }
    return 0;
}
static int pools_cpu_write_address_locked(uint64_t address,size_t size) {
    if(size>UINT64_MAX-address)return -1;
    // Preserve untouched packaged texels before an explicit CPU overwrite.
    if(images_flush_range(address,size))return -1;
    for(dm_vk_pool *p=pools;p;p=p->next) {
        uint64_t base=(uintptr_t)p->guest,lo=address>base?address:base;
        uint64_t end=address+size,hi=end<base+p->size?end:base+p->size;
        if(lo<hi && (compute_cpu_write_range_locked(p,(size_t)(lo-base),(size_t)(hi-lo)) ||
           draw_pending_write(lo,(size_t)(hi-lo)) ||
           pool_gpu_forget_locked(p,(size_t)(lo-base),(size_t)(hi-lo))))return -1;
    }
    return 0;
}
// Like upstream pool_res: registration is CPU-only; an actual GPU buffer
// binding/copy is the only operation that makes the pool resident.
static int pool_resident_locked(dm_vk_pool *p) {
    if(!p)return -1;
    if(p->mapped)return 0;
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=p->size,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|
            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(vkCreateBuffer(v.device,&bc,NULL,&p->buffer)) goto fail;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(v.device,p->buffer,&req);
    uintptr_t address=(uintptr_t)p->guest,base=(uintptr_t)guest_heap.mapped;
    if(guest_heap.mapped && address>=base && address-base<=guest_heap.size && p->size<=guest_heap.size-(address-base)) {
        size_t offset=address-base;
        if(!(offset%req.alignment) && (req.memoryTypeBits&(1u<<guest_heap.memory_type)) && req.size<=guest_heap.size-offset) {
            if(vkBindBufferMemory(v.device,p->buffer,guest_heap.memory,offset))goto fail;
            p->memory=guest_heap.memory;p->mapped=p->guest;p->coherent=1;p->shared_heap=1;
            LOG("Vulkan pool shared heap: guest=%p bytes=%zu offset=%zu",p->guest,p->size,offset);return 0;
        }
        LOG("Vulkan pool shared heap incompatible: guest=%p bytes=%zu alignment=%llu; using separate storage",p->guest,p->size,(unsigned long long)req.alignment);
    }
    VkPhysicalDeviceMemoryProperties mp=device_memory.properties;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) {
        if(!(req.memoryTypeBits&(1u<<i)) || !(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        type=i;
        if(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
    }
    if(type==UINT32_MAX) goto fail;
    p->coherent=(mp.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    if(device_memory_allocate(&ai,&p->memory,DEVICE_MEMORY_POOL) ||
       vkBindBufferMemory(v.device,p->buffer,p->memory,0) ||
       vkMapMemory(v.device,p->memory,0,VK_WHOLE_SIZE,0,(void **)&p->mapped)) goto fail;
    // Reserve backing only. Every GPU reader uploads its precise CPU-owned
    // interval; copy destinations publish only their actual GPU-written range.
    // A CPU invalidate must materialize dirty intersections, never uninitialized
    // or stale resident bytes outside those intersections.
    LOG("Vulkan pool resident: guest=%p reservedBytes=%zu initializedBytes=0 coherent=%u",p->guest,(size_t)req.size,p->coherent);
    return 0;
fail:
    if(p->mapped)vkUnmapMemory(v.device,p->memory);
    if(p->buffer)vkDestroyBuffer(v.device,p->buffer,NULL);
    if(p->memory)device_memory_free(p->memory);
    p->mapped=NULL;p->buffer=VK_NULL_HANDLE;p->memory=VK_NULL_HANDLE;
    return -1;
}
static int pool_gpu_read_impl(dm_vk_pool *p,size_t offset,size_t size) {
    if(!p || offset>p->size || size>p->size-offset)return -1;
    if(reports_gpu_range_prepare((uintptr_t)p->guest+offset,size,__func__))return -1;
    if(pool_resident_locked(p))return -1;
    if(p->shared_heap)return 0;
    size_t pos=offset,end=offset+size;
    while(pos<end) {
        pool_gpu_range *r=p->gpu_dirty;
        while(r && r->hi<=pos)r=r->next;
        if(!r || r->lo>=end)break;
        if(r->lo>pos) {
            size_t next=r->lo;
            if(pool_sync_source_bytes_locked(p,pos,next-pos,0,NULL))return -1;
            pos=next;
            // Upload waits can retire callbacks which alter the dirty list.
            continue;
        }
        pos=r->hi<end?r->hi:end;
    }
    return pos<end?pool_sync_source_bytes_locked(p,pos,end-pos,0,NULL):0;
}
static int pool_cpu_read_consumer_locked(dm_vk_pool *p,size_t offset,size_t size,const char *consumer) {
    if(!p || offset>p->size || size>p->size-offset)return -1;
    if(reports_range_drain_at((uintptr_t)p->guest+offset,size,consumer,"cpu-read"))return -1;
    return pool_cpu_read_dirty_locked(p,offset,size,consumer);
}
static int pool_gpu_read_consumer_locked(dm_vk_pool *p,size_t offset,size_t size,unsigned consumer) {
    if(!p)return -1;
    unsigned previous=pool_prepare_consumer;pool_prepare_consumer=consumer;pool_prepare_begin(p,size,consumer);
    int result=pool_gpu_read_impl(p,offset,size);pool_prepare_consumer=previous;return result;
}
dm_vk_pool *dm_vk_pool_create(void *guest, size_t size,uint32_t flags) {
    if(!guest || !size || size>UINTPTR_MAX-(uintptr_t)guest) return NULL;
    pthread_mutex_lock(&lock);
    dm_vk_pool *p=NULL;
    if(!v.device) goto done;
    for(dm_vk_pool *it=pools;it;it=it->next)
        if((uintptr_t)guest<(uintptr_t)it->guest+it->size &&
           (uintptr_t)it->guest<(uintptr_t)guest+size) goto done;
    p=calloc(1,sizeof *p);
    if(!p) goto done;
    p->guest=guest; p->size=size;p->flags=flags;
    p->next=pools; pools=p;
    // NVN pools normally satisfy page alignment. Do not protect unrelated
    // bytes around a small/unaligned allocation; those retain exact shadows.
    size_t page=(size_t)getpagesize();
    if(!((uintptr_t)guest%page) && !(size%page))nvn_dirty_add_range(guest,size);
    LOG("Vulkan pool registered (not resident): guest=%p bytes=%zu flags=%#x",guest,size,flags);
done:
    pthread_mutex_unlock(&lock); return p;
}
int dm_vk_pool_destroy(dm_vk_pool *p) {
    pthread_mutex_lock(&lock);
    int result=-1;
    dm_vk_pool **link=&pools;
    while(*link && *link!=p) link=&(*link)->next;
    if(*link && !images_retire_range_locked((uintptr_t)p->guest,p->size) &&
       vkQueueWaitIdle(v.queue)==VK_SUCCESS && !pool_cpu_read_locked(p,0,p->size)) {
        *link=p->next; pool_free_locked(p); result=0;
    }
    pthread_mutex_unlock(&lock); return result;
}
int dm_vk_pool_cpu_read(dm_vk_pool *p,size_t offset,size_t size) {
    pthread_mutex_lock(&lock);p=pool_find_locked(p);
    perf_metrics.cpu_pool_read_calls++;
    // CPU reads conflict with actual writers of these bytes, not with every
    // compute reader elsewhere in the pool. Dirty ownership keeps those exact
    // producer waits; noncoherent download retains its allocation-wide wait.
    int result=p && offset<=p->size && size<=p->size-offset &&
        !images_flush_range((uintptr_t)p->guest+offset,size)?pool_cpu_read_locked(p,offset,size):-1;
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_cpu_read_address(uint64_t address,size_t size) {
    if(!__atomic_load_n(&pool_cpu_hazards,__ATOMIC_ACQUIRE))return 0;
    pthread_mutex_lock(&lock);
    int result=images_flush_range(address,size)?-1:pools_cpu_read_address_locked(address,size);
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_cpu_write_address(uint64_t address,size_t size) {
    // The read-side summary does not cover pure graphics readers. Explicit
    // CPU writes must also check those readers before handing memory back.
    pthread_mutex_lock(&lock);
    perf_metrics.cpu_write_calls++;
    int result=pools_cpu_write_address_locked(address,size);
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_pool_sync(dm_vk_pool *p, size_t offset, size_t size, int download) {
    pthread_mutex_lock(&lock);
    int result=-1;
    p=pool_find_locked(p);
    // Flush/Invalidate is a range visibility operation, not a queue fence.
    // Alias/report materialization above the pool path retains its own waits;
    // changed uploads wait for compute use and overlapping draw readers, and
    // downloads materialize only dirty ranges after their writer fences.
    if(p && offset<=p->size && size<=p->size-offset &&
       !(download?images_flush_range((uintptr_t)p->guest+offset,size):
                   images_flush_cpu_overwrite((uintptr_t)p->guest+offset,size)))
        result=download?pool_cpu_read_locked(p,offset,size):pool_sync_locked(p,offset,size,0);
    pthread_mutex_unlock(&lock); return result;
}
static int compute_copy_record_locked(dm_vk_pool *src,size_t source_offset,dm_vk_pool *dst,size_t offset,size_t size);
int dm_vk_copy(uint64_t source, uint64_t destination, size_t size) {
    pthread_mutex_lock(&lock);
    // Keep the atomic fast path closed until this producer publishes dirty
    // destination ownership (or completes cleanup without submitting).
    pool_cpu_hazard_add();
    int result=-1; size_t so=0,doff=0;
    dm_vk_pool *src=pool_address_locked(source,size,&so), *dst=pool_address_locked(destination,size,&doff);
    if(!src || !dst || !size || (source|destination|size)&3) goto done;
    if(src==dst && so<doff+size && doff<so+size) goto done;
    if(images_flush_range(source,size) || images_flush_range(destination,size) || pool_gpu_read_consumer_locked(src,so,size,POOL_PREP_B2B) || pool_resident_locked(dst)) goto done;
    result=compute_copy_record_locked(src,so,dst,doff,size);
done:
    pool_cpu_hazard_remove();
    pthread_mutex_unlock(&lock); return result;
}
/* Command streams live in registered guest pools. Never dereference an
 * arbitrary command handle, including jumps/calls, before checking its range. */
static int guest_read_locked(uint64_t address,void *out,size_t size) {
    size_t off=0; dm_vk_pool *p=pool_address_locked(address,size,&off);
    if(p && (images_flush_range(address,size) || pool_cpu_read_locked(p,off,size)))p=NULL;
    if(p && size) memcpy(out,p->guest+off,size);
    return p?0:-1;
}
int dm_vk_guest_read(uint64_t address, void *out, size_t size) {
    pthread_mutex_lock(&lock);
    perf_metrics.guest_reads++;perf_metrics.guest_read_bytes+=size;
    int result=guest_read_locked(address,out,size);
    pthread_mutex_unlock(&lock);return result;
}
static int reports_peek_ready_locked(uint64_t address,size_t size);
static int images_peek_ready_locked(uint64_t address,size_t size);
static int compute_peek_ready_locked(dm_vk_pool *pool,size_t offset,size_t size);
static dm_vk_pool *guest_peek_pool_locked(uint64_t address,size_t size,size_t *offset) {
    if(!size || size>UINT64_MAX-address)return NULL;
    dm_vk_pool *p=pool_address_locked(address,size,offset);
    if(!p)return NULL;
    for(const pool_gpu_range *r=p->gpu_dirty;r;r=r->next)
        if(*offset<r->hi && r->lo<*offset+size)return NULL;
    if(!compute_peek_ready_locked(p,*offset,size) ||
       !reports_peek_ready_locked(address,size) || !images_peek_ready_locked(address,size))return NULL;
    return p;
}
int dm_vk_guest_range_peek(uint64_t address,void *out,size_t bytes) {
    if(!out)return 1;
    pthread_mutex_lock(&lock);
    // Include planner reads in the existing CPU lock/copy totals; excluding
    // them would make owned IR look cheaper merely by renaming its entry.
    perf_metrics.guest_reads++;
    size_t offset=0;dm_vk_pool *p=guest_peek_pool_locked(address,bytes,&offset);
    if(p){memcpy(out,p->guest+offset,bytes);perf_metrics.guest_read_bytes+=bytes;}
    pthread_mutex_unlock(&lock);return p?0:1;
}
int dm_vk_guest_record_peek(uint64_t address,void *out,size_t capacity) {
    if(!out || capacity<8)return 1;
    pthread_mutex_lock(&lock);
    perf_metrics.guest_reads++;
    int result=1;size_t offset=0;
    dm_vk_pool *p=guest_peek_pool_locked(address,8,&offset);
    if(!p)goto done;
    uint32_t header[2];memcpy(header,p->guest+offset,sizeof header);perf_metrics.guest_read_bytes+=sizeof header;
    size_t bytes=header[1];
    if(bytes<8 || (bytes&7) || bytes>capacity || bytes>UINT64_MAX-address)goto done;
    if(!guest_peek_pool_locked(address,bytes,&offset))goto done;
    memcpy(out,p->guest+offset,bytes);perf_metrics.guest_read_bytes+=bytes;
    // Reject a changed header rather than certify a length we did not check.
    // This is not a concurrency guarantee for unsynchronized guest mutation.
    result=memcmp(out,header,sizeof header)?1:0;
done:
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_guest_record_read(uint64_t address,void *out,size_t capacity) {
    if(!out || capacity<8)return -1;
    pthread_mutex_lock(&lock);
    perf_metrics.guest_reads++;perf_metrics.guest_read_bytes+=8;
    size_t offset=0;dm_vk_pool *p=pool_address_locked(address,8,&offset);
    int result=-1;
    if(!p || images_flush_range(address,8) || pool_cpu_read_locked(p,offset,8))goto done;
    memcpy(out,p->guest+offset,8);
    uint32_t bytes;memcpy(&bytes,(uint8_t *)out+4,sizeof bytes);
    if(bytes<8 || (bytes&7) || address>UINT64_MAX-bytes)goto done;
    if(bytes>capacity){result=1;goto done;}
    // The backend lock pins this registration. Reuse the checked header's
    // pool rather than resolving the same address twice more per command.
    if(bytes>p->size-offset)goto done;
    if(bytes>8) {
        size_t payload=bytes-8;
        perf_metrics.guest_read_bytes+=payload;
        if(images_flush_range(address+8,payload) || pool_cpu_read_locked(p,offset+8,payload))goto done;
        memcpy((uint8_t *)out+8,p->guest+offset+8,payload);
    }
    result=0;
done:
    pthread_mutex_unlock(&lock);return result;
}
static int compute_update_record_locked(dm_vk_pool *dst,size_t offset,const void *data,size_t size);
int dm_vk_guest_write(uint64_t address, const void *data, size_t size) {
    pthread_mutex_lock(&lock);
    size_t off=0; dm_vk_pool *p=pool_address_locked(address,size,&off); int result=-1;
    // OP_UPDATE_UB supplies an owned replay-time payload. Upstream encodes a
    // Metal blit here, not a CPU overwrite followed by a whole-pool GPU wait.
    // vkCmdUpdateBuffer copies its host payload while recording; no staging
    // allocation or payload lifetime beyond this call is needed.
    size_t source_offset=0;
    if(p && size && size<=65536 && !((off|size)&3) && data &&
       !pool_address_locked((uintptr_t)data,size,&source_offset)) {
        pool_cpu_hazard_add();
        if(!images_flush_range(address,size) && !pool_resident_locked(p))
            result=compute_update_record_locked(p,off,data,size);
        pool_cpu_hazard_remove();
        pthread_mutex_unlock(&lock);return result;
    }
    if(p && !images_flush_range(address,size) && !images_flush_range((uintptr_t)data,size) &&
       !pools_cpu_read_address_locked((uintptr_t)data,size) &&
       !compute_wait_serial(p->last_compute_use)) {
        dm_guest_write_prep(p->guest+off,size);
        memmove(p->guest+off,data,size);
        result=pool_sync_locked(p,off,size,0);
    }
    pthread_mutex_unlock(&lock); return result;
}
int dm_vk_guest_event(uint64_t address, uint32_t value, int add) {
    pthread_mutex_lock(&lock);
    uint64_t before=gpu_metrics.buffer_materialize_bytes;
    size_t off=0; dm_vk_pool *p=pool_address_locked(address,4,&off); int result=-1;
    // CPU event signaling is an independent producer: never wait for queued
    // translation, which may itself be waiting for this value. Pending GPU
    // event posts remain independent producers; only real GPU bytes/aliases
    // need materialization before this atomic CPU update.
    if(p && !(address&3) && !event_word_prepare_locked(p,off)) {
        dm_guest_write_prep(p->guest+off,4);
        uint32_t *dst=(uint32_t *)(p->guest+off);
        if(add) __atomic_fetch_add(dst,value,__ATOMIC_ACQ_REL);
        else __atomic_store_n(dst,value,__ATOMIC_RELEASE);
        pool_shadow_invalidate(p,off,4);pool_range_invalidate(p,off,4);
        graphics_uniform_retire_range(address,4);graphics_buffer_retire_range(address,4);
        result=0; // A later GPU consumer uploads the new CPU-owned value.
    }
    gpu_materialize_account(DM_VK_MATERIALIZE_EVENT,before);
    pthread_mutex_unlock(&lock); return result;
}
int dm_vk_guest_event_get(uint64_t address,uint32_t *value) {
    pthread_mutex_lock(&lock);
    uint64_t before=gpu_metrics.buffer_materialize_bytes;
    size_t off=0;dm_vk_pool *p=pool_address_locked(address,4,&off);int result=-1;
    // Poll event posts without waiting for them. A real GPU buffer/image
    // writer of this word still requires the existing exact materialization.
    if(p && value && !(address&3) && !draw_submissions_poll() && !event_word_prepare_locked(p,off)) {
        *value=__atomic_load_n((uint32_t *)(p->guest+off),__ATOMIC_ACQUIRE);result=0;
    }
    gpu_materialize_account(DM_VK_MATERIALIZE_EVENT,before);
    pthread_mutex_unlock(&lock);return result;
}
