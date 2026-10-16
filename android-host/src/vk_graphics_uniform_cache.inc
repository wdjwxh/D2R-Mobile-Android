/* Read-only uniform storage. References are released by graphics_resources_free
 * only after its submission fence completes (or before an unsubmitted failure).
 * Keep the shader's 64 KiB range and zero tail without clearing it every draw. */
enum { GRAPHICS_UNIFORM_BYTES=65536,GRAPHICS_UNIFORM_BANK_SLOTS=64,GRAPHICS_UNIFORM_BANKS=64,
       GRAPHICS_CONSTANT_SLOTS=256,GRAPHICS_UNIFORM_STATIC_COUNT=GRAPHICS_CONSTANT_SLOTS+2,
       GRAPHICS_GUEST_SLOTS=128,GRAPHICS_GUEST_REFERENCE_BASE=GRAPHICS_UNIFORM_STATIC_COUNT+GRAPHICS_UNIFORM_BANK_SLOTS*GRAPHICS_UNIFORM_BANKS };
typedef struct {compute_scratch data;const void *key;size_t payload;unsigned refs;
    uint8_t *shadow;size_t shadow_capacity;} graphics_uniform_static;
typedef struct {
    compute_scratch data;size_t stride,payload[GRAPHICS_UNIFORM_BANK_SLOTS];uint64_t active,shadow_valid;
    uint8_t *shadow[GRAPHICS_UNIFORM_BANK_SLOTS];size_t shadow_capacity[GRAPHICS_UNIFORM_BANK_SLOTS];
} graphics_uniform_bank;
static graphics_uniform_static graphics_uniform_statics[GRAPHICS_UNIFORM_STATIC_COUNT];
static graphics_uniform_bank graphics_uniform_banks[GRAPHICS_UNIFORM_BANKS];
// Bank records own only slot metadata in unified mode. Exactly one owner
// retains the GPU allocation; chunked mode keeps the original per-bank owners.
static compute_scratch graphics_uniform_arena;
enum { UNIFORM_ARENA_UNTRIED,UNIFORM_ARENA_UNIFIED,UNIFORM_ARENA_CHUNKED };
static unsigned graphics_uniform_arena_mode;
static unsigned graphics_uniform_bank_count,graphics_uniform_free_count;
static int graphics_uniform_under_pressure(unsigned needed) {
    unsigned available=graphics_uniform_free_count+
        (GRAPHICS_UNIFORM_BANKS-graphics_uniform_bank_count)*GRAPHICS_UNIFORM_BANK_SLOTS;
    return available<needed;
}
static unsigned graphics_uniform_free_slots[GRAPHICS_UNIFORM_BANKS*GRAPHICS_UNIFORM_BANK_SLOTS];
static compute_scratch *graphics_uniform_bank_storage(unsigned bank) {
    return graphics_uniform_arena_mode==UNIFORM_ARENA_UNIFIED?
        &graphics_uniform_arena:&graphics_uniform_banks[bank].data;
}
#include "vk_uniform_content_cache.inc"
typedef struct {
    compute_scratch data;uint64_t address,stamp,checked_generation,used;
    size_t payload;unsigned refs,next,bucket;int valid,tracked;
    uint8_t *shadow;size_t shadow_capacity;
} graphics_guest_uniform;
static graphics_guest_uniform graphics_guest_uniforms[GRAPHICS_GUEST_SLOTS];
static unsigned graphics_guest_uniform_buckets[256];
static uint64_t graphics_guest_uniform_clock;
// HOST_COHERENT guarantees visibility, not cheap CPU reads. Validate against
// an ordinary-RAM copy of exactly the bytes uploaded, never the mapped GPU heap.
static int graphics_uniform_shadow_reserve(uint8_t **shadow,size_t *capacity,size_t payload) {
    if(payload<=*capacity)return 1;
    uint8_t *next=realloc(*shadow,payload);
    if(!next)return 0; // Preserve the old snapshot and allow dynamic fallback.
    *shadow=next;*capacity=payload;return 1;
}
static int graphics_uniform_compare(const void *a,const void *b,size_t bytes) {
    double start=gpu_detail_time_ms();int result=memcmp(a,b,bytes);
    gpu_metrics.uniform_compare_ms+=gpu_detail_time_ms()-start;
    gpu_metrics.uniform_compare_bytes+=bytes;
    return result;
}
static unsigned graphics_guest_uniform_bucket(uint64_t address,size_t payload) {
    uint64_t key=(address>>4)^(address>>23)^payload;
    return (unsigned)((key*UINT64_C(11400714819323198485))>>56);
}
static void graphics_guest_uniform_unlink(unsigned slot) {
    graphics_guest_uniform *p=&graphics_guest_uniforms[slot-1];
    if(p->valid) {
        unsigned *link=&graphics_guest_uniform_buckets[p->bucket];
        while(*link && *link!=slot)link=&graphics_guest_uniforms[*link-1].next;
        if(*link)*link=p->next;
    }
    p->valid=0;p->next=0;p->checked_generation=0;
}
static void graphics_uniform_retire_range(uint64_t address,size_t size) {
    for(unsigned i=0;i<GRAPHICS_GUEST_SLOTS;i++) {
        graphics_guest_uniform *p=&graphics_guest_uniforms[i];
        int overlap=p->address>=address?p->address-address<size:address-p->address<p->payload;
        if(p->valid && overlap)graphics_guest_uniform_unlink(i+1);
    }
    // Invalidate identity only: even if a preceding drain failed, live immutable
    // GPU versions remain allocated until their references complete. Never read
    // the old guest address again after it is removed from the lookup table.
}
static int graphics_guest_uniform_get(uint64_t address,const void *bytes,size_t payload,
                                      VkDescriptorBufferInfo *info,unsigned *reference) {
    if(!payload || payload>GRAPHICS_UNIFORM_BYTES)return 0;
    unsigned bucket=graphics_guest_uniform_bucket(address,payload);
    // Native bridge may optimize dynamic buffer writes into host operations
    // whose protection faults cannot be dispatched to an ARM signal handler.
    // Reuse exact snapshots there without introducing new UBO protected pages.
    extern unsigned char dm_no_fp_state;
    int tracked=!dm_no_fp_state && nvn_dirty_tracked(bytes,payload);uint64_t now=tracked?nvn_dirty_now():0;
    for(unsigned slot=graphics_guest_uniform_buckets[bucket],next;slot;slot=next) {
        graphics_guest_uniform *p=&graphics_guest_uniforms[slot-1];next=p->next;
        if(p->address!=address || p->payload!=payload)continue;
        int clean=tracked && p->tracked && p->stamp &&
            (p->checked_generation==now || !nvn_dirty_since(bytes,payload,p->stamp));
        if(!tracked)clean=!graphics_uniform_compare(p->shadow,bytes,payload);
        if(clean) {
            // Capture now BEFORE checking pages, so a concurrent write cannot
            // be certified clean by a later generation read.
            p->checked_generation=tracked?now:0;p->refs++;p->used=++graphics_guest_uniform_clock;
            *reference=GRAPHICS_GUEST_REFERENCE_BASE+slot;
            *info=(VkDescriptorBufferInfo){p->data.buffer,0,GRAPHICS_UNIFORM_BYTES};return 1;
        }
        graphics_guest_uniform_unlink(slot); // Keep old GPU contents while pinned.
    }
    int candidate=-1;
    for(unsigned i=0;i<GRAPHICS_GUEST_SLOTS;i++) {
        graphics_guest_uniform *p=&graphics_guest_uniforms[i];
        if(!p->refs && (candidate<0 || p->used<graphics_guest_uniforms[candidate].used))candidate=(int)i;
    }
    if(candidate<0)return 0; // All 8 MiB pinned: use the existing dynamic arena.
    graphics_guest_uniform *p=&graphics_guest_uniforms[candidate];graphics_guest_uniform_unlink(candidate+1);
    if(!graphics_uniform_shadow_reserve(&p->shadow,&p->shadow_capacity,payload))return 0;
    if(!p->data.buffer && scratch_new(&p->data,GRAPHICS_UNIFORM_BYTES)) {
        scratch_destroy(&p->data);p->payload=0;return -1;
    }
    uint64_t stamp=0;
    if(tracked){nvn_dirty_protect(bytes,payload);stamp=nvn_dirty_now();}
    // Capture once: reading guest bytes separately for shadow and GPU could
    // publish mismatched versions if the guest writes between the two copies.
    memcpy(p->shadow,bytes,payload);
    if(p->payload>payload)memset((uint8_t *)p->data.mapped+payload,0,p->payload-payload);
    memcpy(p->data.mapped,p->shadow,payload);p->payload=payload;
    if(scratch_sync(&p->data,0)){scratch_destroy(&p->data);p->payload=0;return -1;}
    p->address=address;p->tracked=tracked;p->stamp=stamp;p->checked_generation=0;
    p->refs=1;p->used=++graphics_guest_uniform_clock;p->bucket=bucket;
    // An upload concurrent with a CPU write is still an immutable snapshot for
    // this draw, but is not a reusable content version. Retain it only by refs.
    p->valid=!tracked || !nvn_dirty_since(bytes,payload,stamp);
    if(p->valid) {
        p->checked_generation=tracked?stamp:0;
        p->next=graphics_guest_uniform_buckets[bucket];graphics_guest_uniform_buckets[bucket]=candidate+1;
    }
    *reference=GRAPHICS_GUEST_REFERENCE_BASE+candidate+1;
    *info=(VkDescriptorBufferInfo){p->data.buffer,0,GRAPHICS_UNIFORM_BYTES};return 1;
}
static int graphics_uniform_static_get(unsigned index,const void *key,const void *bytes,size_t payload,size_t range,
                                       VkDescriptorBufferInfo *info,unsigned *reference) {
    graphics_uniform_static *p=&graphics_uniform_statics[index];
    // Content validation also handles temporary zero-source pointer reuse and
    // future changes to support values without changing an in-flight buffer.
    int same=p->data.buffer && p->data.size==range && p->payload==payload && p->key==key &&
        (!payload || !graphics_uniform_compare(p->shadow,bytes,payload));
    if(!same) {
        if(p->refs)return 0;
        if(!graphics_uniform_shadow_reserve(&p->shadow,&p->shadow_capacity,payload))return 0;
        if(p->data.buffer && p->data.size!=range){scratch_destroy(&p->data);p->payload=0;}
        if(!p->data.buffer && scratch_new(&p->data,range)){scratch_destroy(&p->data);return -1;}
        if(p->payload>payload)memset((uint8_t *)p->data.mapped+payload,0,p->payload-payload);
        if(payload){memcpy(p->shadow,bytes,payload);memcpy(p->data.mapped,p->shadow,payload);}
        // Set metadata before flush: a failed flush cannot leave stale tail
        // bookkeeping when the caller retries with a shorter payload.
        p->payload=payload;p->key=key;
        if(scratch_sync(&p->data,0)){scratch_destroy(&p->data);p->payload=0;p->key=NULL;return -1;}
    }
    p->refs++;*reference=index+1;*info=(VkDescriptorBufferInfo){p->data.buffer,0,range};return 1;
}
static int graphics_uniform_zero_get(VkDescriptorBufferInfo *info,unsigned *reference) {
    return graphics_uniform_static_get(0,NULL,NULL,0,GRAPHICS_UNIFORM_BYTES,info,reference);
}
static int graphics_uniform_bank_add(void) {
    if(graphics_uniform_bank_count==GRAPHICS_UNIFORM_BANKS)return 0;
    size_t alignment=(size_t)v.properties.limits.minUniformBufferOffsetAlignment;
    if(alignment<v.properties.limits.nonCoherentAtomSize)alignment=(size_t)v.properties.limits.nonCoherentAtomSize;
    if(!alignment)alignment=1;
    size_t stride=(GRAPHICS_UNIFORM_BYTES+alignment-1)/alignment*alignment;
    // A strict 256 MiB dynamic cap even on a device with unusually large alignment.
    if(stride>GRAPHICS_UNIFORM_BYTES)return 0;
    if(graphics_uniform_arena_mode==UNIFORM_ARENA_UNTRIED) {
        size_t bytes=stride*GRAPHICS_UNIFORM_BANK_SLOTS*GRAPHICS_UNIFORM_BANKS;
        if(!scratch_new_uniform_arena(&graphics_uniform_arena,bytes)) {
            graphics_uniform_arena_mode=UNIFORM_ARENA_UNIFIED;
            LOG("Uniform arena: mode=unified bytes=%zu slots=%u stride=%zu",bytes,
                GRAPHICS_UNIFORM_BANK_SLOTS*GRAPHICS_UNIFORM_BANKS,stride);
        } else {
            scratch_destroy(&graphics_uniform_arena);
            // One large-allocation attempt per device lifetime. The existing
            // lazy 4 MiB chunks preserve the fallback memory-pressure behavior.
            graphics_uniform_arena_mode=UNIFORM_ARENA_CHUNKED;
            LOG("Uniform arena: mode=chunked chunkBytes=%zu maxBytes=%zu unifiedAllocationFailed=1",
                stride*GRAPHICS_UNIFORM_BANK_SLOTS,bytes);
        }
    }
    graphics_uniform_bank *p=&graphics_uniform_banks[graphics_uniform_bank_count];
    if(graphics_uniform_arena_mode==UNIFORM_ARENA_CHUNKED &&
       scratch_new_uniform_arena(&p->data,stride*GRAPHICS_UNIFORM_BANK_SLOTS)){scratch_destroy(&p->data);return -1;}
    p->stride=stride;
    unsigned base=graphics_uniform_bank_count++*GRAPHICS_UNIFORM_BANK_SLOTS;
    for(unsigned i=0;i<GRAPHICS_UNIFORM_BANK_SLOTS;i++) {
        graphics_uniform_content[base+i].free_position=~0u;
        graphics_uniform_free_push(base+i);
    }
    return 1;
}
static int graphics_uniform_dynamic_get(const void *bytes,size_t payload,VkDescriptorBufferInfo *info,unsigned *reference,int need_shadow) {
    if(payload>GRAPHICS_UNIFORM_BYTES)return 0;
    // Immutable contents can be shared across resource bundles and submissions.
    // A cached idle version consumes no lease: it remains on the free stack.
    if(need_shadow && payload) {
        unsigned found=graphics_uniform_content_find(bytes,payload,graphics_uniform_content_hash(bytes,payload));
        if(found) {
            unsigned slot=found-1,local=slot%GRAPHICS_UNIFORM_BANK_SLOTS;
            graphics_uniform_bank *bank=&graphics_uniform_banks[slot/GRAPHICS_UNIFORM_BANK_SLOTS];
            if(!graphics_uniform_content[slot].refs) {
                graphics_uniform_free_take(slot);bank->active|=UINT64_C(1)<<local;
                gpu_metrics.uniform_content_idle_hits++;
            } else gpu_metrics.uniform_content_active_hits++;
            if(graphics_uniform_content[slot].refs==~0u)die("Vulkan uniform content reference overflow");
            graphics_uniform_content[slot].refs++;
            compute_scratch *storage=graphics_uniform_bank_storage(slot/GRAPHICS_UNIFORM_BANK_SLOTS);
            size_t offset=(graphics_uniform_arena_mode==UNIFORM_ARENA_UNIFIED?slot:local)*bank->stride;
            *reference=GRAPHICS_UNIFORM_STATIC_COUNT+slot+1;
            *info=(VkDescriptorBufferInfo){storage->buffer,offset,GRAPHICS_UNIFORM_BYTES};
            gpu_metrics.uniform_content_hits++;gpu_metrics.uniform_content_avoided_bytes+=payload;
            return 1;
        }
        gpu_metrics.uniform_content_misses++;
    }
    if(!graphics_uniform_free_count) {
        int result=graphics_uniform_bank_add();
        if(result<=0){if(!result)gpu_metrics.uniform_arena_spills++;return result;}
    }
    unsigned slot=graphics_uniform_free_slots[graphics_uniform_free_count-1];
    graphics_uniform_free_take(slot);
    graphics_uniform_bank *p=&graphics_uniform_banks[slot/GRAPHICS_UNIFORM_BANK_SLOTS];
    unsigned local=slot%GRAPHICS_UNIFORM_BANK_SLOTS;
    if(p->active&(UINT64_C(1)<<local))die("Vulkan uniform free slot remained pinned");
    if(graphics_uniform_content[slot].refs)die("Vulkan uniform free slot retained references");
    graphics_uniform_content_unlink(slot);
    p->shadow_valid&=~(UINT64_C(1)<<local);
    if(need_shadow) {
        size_t capacity=p->shadow_capacity[local];
        if(!graphics_uniform_shadow_reserve(&p->shadow[local],&p->shadow_capacity[local],payload)) {
            graphics_uniform_free_push(slot);
            gpu_metrics.uniform_arena_spills++;return 0;
        }
        if(payload>capacity) {
            gpu_metrics.snapshot_allocs++;
            if(capacity)gpu_metrics.snapshot_shadow_grows++;
        } else gpu_metrics.snapshot_reused_arena++;
        // Capture once while this slot is exclusively owned, then upload exactly
        // that version. Concurrent guest writes cannot split GPU/CPU identities.
        if(payload)memcpy(p->shadow[local],bytes,payload);
        gpu_metrics.snapshot_copy_bytes+=payload;bytes=p->shadow[local];
    }
    compute_scratch *storage=graphics_uniform_bank_storage(slot/GRAPHICS_UNIFORM_BANK_SLOTS);
    size_t offset=(graphics_uniform_arena_mode==UNIFORM_ARENA_UNIFIED?slot:local)*p->stride;
    uint8_t *dest=(uint8_t *)storage->mapped+offset;
    if(p->payload[local]>payload) {
        size_t tail=p->payload[local]-payload;
        memset(dest+payload,0,tail);gpu_metrics.uniform_dynamic_zero_bytes+=tail;
    }
    if(payload)memcpy(dest,bytes,payload);p->payload[local]=payload;
    gpu_metrics.uniform_dynamic_upload_bytes+=payload;
    if(!storage->coherent) {
        // Slot boundaries include nonCoherentAtomSize alignment, so flushing
        // this slot cannot overlap another submission's in-flight slot.
        VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=storage->memory,.offset=offset,.size=p->stride};
        if(vkFlushMappedMemoryRanges(v.device,1,&flush)){graphics_uniform_free_push(slot);return -1;}
    }
    p->active|=UINT64_C(1)<<local;
    graphics_uniform_content[slot].refs=1;
    if(need_shadow)p->shadow_valid|=UINT64_C(1)<<local;
    // Hash the captured version, never the guest data read before capture.
    if(need_shadow && payload)graphics_uniform_content_publish(slot,graphics_uniform_content_hash(p->shadow[local],payload));
    *reference=GRAPHICS_UNIFORM_STATIC_COUNT+slot+1;
    *info=(VkDescriptorBufferInfo){storage->buffer,offset,GRAPHICS_UNIFORM_BYTES};return 1;
}
static int graphics_uniform_get(const dm_shader_binding *binding,const dm_compute_buffer *buffer,const void *bytes,
                                VkDescriptorType type,VkDescriptorBufferInfo *info,unsigned *reference,int need_shadow) {
    *reference=0;
    if(buffer->writable)return 0;
    unsigned kind=binding->kind;
    // The explicit marker also covers empty shader constants; never guess zero
    // from a payload size or the lifetime of a temporary host pointer.
    if(buffer->zero) {
        if(buffer->address || buffer->size!=GRAPHICS_UNIFORM_BYTES)return -1;
        return graphics_uniform_zero_get(info,reference);
    }
    if(type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)return 0;
    int result=0;
    if(kind==DM_BIND_UNIFORM && buffer->address)
        result=graphics_guest_uniform_get(buffer->address,bytes,buffer->size,info,reference);
    else if(kind==DM_BIND_SUPPORT && buffer->size==1392)
        result=graphics_uniform_static_get(1,NULL,bytes,buffer->size,1392,info,reference);
    else if((kind==DM_BIND_CONSTANT || kind==DM_BIND_DRIVER) && buffer->size<=GRAPHICS_UNIFORM_BYTES) {
        // The replay stage owns stable driver-bank addresses. Key by that
        // identity, but compare/upload the captured bytes passed by caller;
        // pinned old contents still fall back to the dynamic arena.
        uintptr_t key=(uintptr_t)buffer->host;
        unsigned index=2+(unsigned)(((key>>4)^(key>>17))*UINT64_C(11400714819323198485)>>(64-8));
        result=graphics_uniform_static_get(index,buffer->host,bytes,buffer->size,GRAPHICS_UNIFORM_BYTES,info,reference);
    }
    if(result)return result;
    // A pinned static-cache collision or full arena falls back without eviction.
    return graphics_uniform_dynamic_get(bytes,buffer->size,info,reference,need_shadow);
}
static void graphics_uniform_release(unsigned reference) {
    if(!reference)return;
    if(reference>GRAPHICS_GUEST_REFERENCE_BASE) {
        graphics_guest_uniform *p=&graphics_guest_uniforms[reference-GRAPHICS_GUEST_REFERENCE_BASE-1];
        if(!p->refs){LOG("Vulkan guest uniform reference underflow");abort();}p->refs--;return;
    }
    if(reference<=GRAPHICS_UNIFORM_STATIC_COUNT) {
        graphics_uniform_static *p=&graphics_uniform_statics[reference-1];
        if(!p->refs){LOG("Vulkan uniform static reference underflow");abort();}p->refs--;return;
    }
    unsigned slot=reference-GRAPHICS_UNIFORM_STATIC_COUNT-1,local=slot%GRAPHICS_UNIFORM_BANK_SLOTS;
    graphics_uniform_bank *p=&graphics_uniform_banks[slot/GRAPHICS_UNIFORM_BANK_SLOTS];
    uint64_t bit=UINT64_C(1)<<local;
    if(!(p->active&bit) || !graphics_uniform_content[slot].refs){LOG("Vulkan uniform slot released twice");abort();}
    if(--graphics_uniform_content[slot].refs)return;
    p->active&=~bit;graphics_uniform_free_push(slot);
}
static void graphics_uniform_cache_close(void) {
    for(unsigned i=0;i<GRAPHICS_GUEST_SLOTS;i++) {
        graphics_guest_uniform *p=&graphics_guest_uniforms[i];
        if(p->refs){LOG("Vulkan guest uniform cache closed with active versions");abort();}scratch_destroy(&p->data);free(p->shadow);
    }
    memset(graphics_guest_uniforms,0,sizeof graphics_guest_uniforms);
    memset(graphics_guest_uniform_buckets,0,sizeof graphics_guest_uniform_buckets);graphics_guest_uniform_clock=0;
    for(unsigned i=0;i<GRAPHICS_UNIFORM_STATIC_COUNT;i++) {
        graphics_uniform_static *p=&graphics_uniform_statics[i];
        if(p->refs){LOG("Vulkan uniform cache closed with active references");abort();}scratch_destroy(&p->data);free(p->shadow);
    }
    for(unsigned i=0;i<graphics_uniform_bank_count;i++) {
        graphics_uniform_bank *p=&graphics_uniform_banks[i];
        if(p->active){LOG("Vulkan uniform arena closed with active slots");abort();}
        for(unsigned j=0;j<GRAPHICS_UNIFORM_BANK_SLOTS;j++) {
            if(graphics_uniform_content[i*GRAPHICS_UNIFORM_BANK_SLOTS+j].refs)die("Vulkan uniform arena closed with references");
            free(p->shadow[j]);
        }
        if(graphics_uniform_arena_mode==UNIFORM_ARENA_CHUNKED)scratch_destroy(&p->data);
    }
    scratch_destroy(&graphics_uniform_arena);graphics_uniform_arena_mode=UNIFORM_ARENA_UNTRIED;
    memset(graphics_uniform_statics,0,sizeof graphics_uniform_statics);memset(graphics_uniform_banks,0,sizeof graphics_uniform_banks);
    graphics_uniform_bank_count=graphics_uniform_free_count=0;
    memset(graphics_uniform_content,0,sizeof graphics_uniform_content);
    memset(graphics_uniform_content_buckets,0,sizeof graphics_uniform_content_buckets);
}
