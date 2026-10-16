/* Immutable read-only SSBO/texel snapshots. All operations hold the Vulkan
 * lock; a resource reference pins its exact bytes until its submission retires. */
enum { GRAPHICS_BUFFER_SLOTS=64 };
#define GRAPHICS_BUFFER_BUDGET ((size_t)128*1024*1024)
#define GRAPHICS_BUFFER_MAX ((size_t)64*1024*1024)
typedef struct {
    compute_scratch data;uint8_t *shadow;size_t size,allocated;
    uint64_t address,stamp,checked_generation,used;
    unsigned refs;int valid,tracked;
} graphics_buffer_version;
static graphics_buffer_version graphics_buffers[GRAPHICS_BUFFER_SLOTS];
static size_t graphics_buffer_bytes;
static uint64_t graphics_buffer_clock;
static void graphics_buffer_drop(graphics_buffer_version *p) {
    if(p->refs){LOG("Read-only buffer cache eviction while referenced");abort();}
    graphics_buffer_bytes-=p->allocated;
    scratch_destroy(&p->data);free(p->shadow);memset(p,0,sizeof *p);
}
static void graphics_buffer_retire_range(uint64_t address,size_t size) {
    for(unsigned i=0;i<GRAPHICS_BUFFER_SLOTS;i++) {
        graphics_buffer_version *p=&graphics_buffers[i];
        int overlap=p->address>=address?p->address-address<size:address-p->address<p->size;
        if(p->valid && overlap){p->valid=0;p->checked_generation=0;}
    }
}
static int graphics_buffer_get(uint64_t address,const void *bytes,size_t size,
                               VkDescriptorBufferInfo *info,unsigned *reference) {
    *reference=0;
    if(!address || !bytes || !size || size>GRAPHICS_BUFFER_MAX)return 0;
    extern unsigned char dm_no_fp_state;
    int tracked=!dm_no_fp_state && nvn_dirty_tracked(bytes,size);
    uint64_t now=tracked?nvn_dirty_now():0;
    for(unsigned i=0;i<GRAPHICS_BUFFER_SLOTS;i++) {
        graphics_buffer_version *p=&graphics_buffers[i];
        if(!p->valid || p->address!=address || p->size!=size)continue;
        int clean=tracked && p->tracked && p->stamp &&
            (p->checked_generation==now || !nvn_dirty_since(bytes,size,p->stamp));
        if(!tracked) {
            double started=gpu_detail_time_ms();clean=!memcmp(p->shadow,bytes,size);
            gpu_metrics.buffer_compare_ms+=gpu_detail_time_ms()-started;
        }
        if(clean) {
            gpu_metrics.buffer_hits++;
            p->checked_generation=tracked?now:0;p->refs++;p->used=++graphics_buffer_clock;
            *reference=i+1;*info=(VkDescriptorBufferInfo){p->data.buffer,0,size};return 1;
        }
        // An old in-flight snapshot remains immutable after losing lookup identity.
        p->valid=0;p->checked_generation=0;
    }
    gpu_metrics.buffer_misses++;
    int candidate=-1,priority=-1;
    for(unsigned i=0;i<GRAPHICS_BUFFER_SLOTS;i++) {
        graphics_buffer_version *old=&graphics_buffers[i];
        if(old->refs)continue;
        // Preserve rotating guest buffers as distinct resident identities while
        // slots are available; otherwise A/B/C frames evict each other forever.
        int rank=old->size==size && old->address==address?4:
            !old->size?3:!old->valid && old->size==size?2:old->size==size?1:0;
        if(candidate<0 || rank>priority || (rank==priority && old->used<graphics_buffers[candidate].used)) {
            candidate=(int)i;priority=rank;
        }
    }
    if(candidate<0)return 0;
    graphics_buffer_version *p=&graphics_buffers[candidate];
    // The old CPU shadow describes this idle GPU allocation even if its guest
    // identity changes. Comparing bytes is safe across equal-sized identities.
    int incremental=p->size==size;
    if(p->size!=size) {
        graphics_buffer_drop(p);
        uint8_t *shadow=malloc(size);if(!shadow)return 0;
        compute_scratch data={0};
        if(scratch_new(&data,size)){scratch_destroy(&data);free(shadow);return 0;}
        VkMemoryRequirements requirement;vkGetBufferMemoryRequirements(v.device,data.buffer,&requirement);
        size_t allocated=(size_t)requirement.size;
        if(allocated>GRAPHICS_BUFFER_BUDGET){scratch_destroy(&data);free(shadow);return 0;}
        while(graphics_buffer_bytes>GRAPHICS_BUFFER_BUDGET-allocated) {
            int victim=-1;
            for(unsigned i=0;i<GRAPHICS_BUFFER_SLOTS;i++)if(graphics_buffers[i].size && !graphics_buffers[i].refs &&
                (victim<0 || graphics_buffers[i].used<graphics_buffers[victim].used))victim=(int)i;
            if(victim<0){scratch_destroy(&data);free(shadow);return 0;}
            graphics_buffer_drop(&graphics_buffers[victim]);
        }
        *p=(graphics_buffer_version){.data=data,.shadow=shadow,.size=size,.allocated=allocated};
        graphics_buffer_bytes+=allocated;
    }
    // This allocation is unreferenced: reuse it without scratch_new's full clear.
    // Invalidate lookup identity before touching either copy of the old version.
    p->valid=0;p->checked_generation=0;
    uint64_t stamp=0;
    // Translation uses ordinary CPU comparison and never adds protected pages.
    if(tracked){nvn_dirty_protect(bytes,size);stamp=nvn_dirty_now();}
    size_t uploaded=0;
    if(incremental) {
        const size_t block=64u*1024;
        for(size_t offset=0;offset<size;offset+=block) {
            size_t count=size-offset<block?size-offset:block;
            double started=gpu_detail_time_ms();
            int changed=memcmp(p->shadow+offset,(const uint8_t *)bytes+offset,count);
            gpu_metrics.buffer_compare_ms+=gpu_detail_time_ms()-started;
            if(!changed)continue;
            // Both copies receive the same captured bytes, even if guest writes race.
            memcpy(p->shadow+offset,(const uint8_t *)bytes+offset,count);
            memcpy((uint8_t *)p->data.mapped+offset,p->shadow+offset,count);uploaded+=count;
        }
    } else {
        // A fresh allocation has no shadow contents to compare.
        memcpy(p->shadow,bytes,size);memcpy(p->data.mapped,p->shadow,size);uploaded=size;
    }
    gpu_metrics.buffer_upload_bytes+=uploaded;
    if(uploaded && scratch_sync(&p->data,0)){graphics_buffer_drop(p);return 0;}
    p->address=address;p->stamp=stamp;p->used=++graphics_buffer_clock;p->refs=1;p->tracked=tracked;
    p->valid=!tracked || !nvn_dirty_since(bytes,size,stamp);
    p->checked_generation=p->valid && tracked?stamp:0;
    *reference=(unsigned)candidate+1;*info=(VkDescriptorBufferInfo){p->data.buffer,0,size};return 1;
}
static void graphics_buffer_release(unsigned reference) {
    if(!reference)return;
    graphics_buffer_version *p=&graphics_buffers[reference-1];
    if(!p->refs){LOG("Read-only buffer cache reference underflow");abort();}
    p->refs--;
}
static void graphics_buffer_cache_close(void) {
    for(unsigned i=0;i<GRAPHICS_BUFFER_SLOTS;i++)graphics_buffer_drop(&graphics_buffers[i]);
    graphics_buffer_clock=0;
}
