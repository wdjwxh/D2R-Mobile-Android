#include "vk_compute.h"
#include "compute_catalog.inc"
#include <time.h>
#include <unistd.h>
#include <zlib.h>
static int prep_cache_saved(const void *data,size_t size);

typedef struct { VkPipeline pipeline; VkPipelineLayout layout; VkDescriptorSetLayout set; } compute_pipeline;
static compute_pipeline compute_pipelines[sizeof compute_catalog/sizeof *compute_catalog];
static VkPipelineCache compute_cache;
static int compute_cache_imported;
static char compute_cache_path[1024];
static void images_close_locked(void);
static void graphics_image_cache_retire(VkImage image);
static void graphics_image_cache_close(void);
struct gpu_image;
static VkImageView graphics_cached_view(struct gpu_image *,const dm_compute_image *,const dm_shader_binding *,unsigned *);
static VkSampler graphics_cached_sampler(const dm_compute_image *,unsigned *);
static VkBufferView graphics_cached_texel(const VkBufferViewCreateInfo *,unsigned *);
static void graphics_view_release(VkImageView,unsigned);
static void graphics_sampler_release(VkSampler,unsigned);
static void graphics_texel_release(VkBufferView,unsigned);
static void graphics_uniform_cache_close(void);
static int graphics_uniform_zero_get(VkDescriptorBufferInfo *info,unsigned *reference);
static int graphics_uniform_dynamic_get(const void *,size_t,VkDescriptorBufferInfo *,unsigned *,int);
static void graphics_uniform_release(unsigned reference);
static int graphics_uniform_under_pressure(unsigned needed);
static void scratch_cache_close(void);
static unsigned images_cpu_owned_count(void);
static void images_compute_reset(void);
const dm_compute_shader *dm_vk_compute_lookup(const char *key) {
    for(unsigned i=0;i<sizeof compute_catalog/sizeof *compute_catalog;i++)if(!strcmp(compute_catalog[i].key,key))return &compute_catalog[i];
    return NULL;
}
static int compute_cache_save(void) {
    if(!compute_cache || !compute_cache_path[0])return -1;
    size_t size=0;if(vkGetPipelineCacheData(v.device,compute_cache,&size,NULL) || !size)return -1;
    void *data=malloc(size);if(!data)return -1;
    int result=-1;
    if(!vkGetPipelineCacheData(v.device,compute_cache,&size,data)) {
        char temp[1060];snprintf(temp,sizeof temp,"%s.tmp",compute_cache_path);
        FILE *f=fopen(temp,"wb");if(f){int ok=fwrite(data,1,size,f)==size;
            if(ok && (fflush(f) || fsync(fileno(f))))ok=0;
            if(fclose(f))ok=0;
            if(ok && !rename(temp,compute_cache_path))result=prep_cache_saved(data,size);
        }
    }
    free(data);return result;
}
static void compute_cache_checkpoint(void) {
    static time_t last;
    time_t now=time(NULL);
    if(now-last<10)return;
    compute_cache_save();last=now;
}
static int compute_cache_open(void) {
    if(compute_cache)return 0;
    VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(v.gpu,&p);
    const char *root=getenv("DM_ROOT");
    if(root)snprintf(compute_cache_path,sizeof compute_cache_path,"%s/vk-%x-%x-%x.cache",root,p.vendorID,p.deviceID,p.driverVersion);
    void *data=NULL;size_t size=0;
    FILE *f=compute_cache_path[0]?fopen(compute_cache_path,"rb"):NULL;
    if(f) {
        if(!fseek(f,0,SEEK_END)){long n=ftell(f);if(n>=32 && n<=64*1024*1024){size=(size_t)n;data=malloc(size);rewind(f);if(!data || fread(data,1,size,f)!=size){free(data);data=NULL;size=0;}}}
        fclose(f);
    }
    if(data) {
        uint32_t header[4];memcpy(header,data,sizeof header);
        if(header[0]<32 || header[0]>size || header[1]!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE || header[2]!=p.vendorID || header[3]!=p.deviceID || memcmp((uint8_t *)data+16,p.pipelineCacheUUID,16)) {free(data);data=NULL;size=0;}
    }
    VkPipelineCacheCreateInfo ci={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,.initialDataSize=size,.pInitialData=data};
    VkResult r=vkCreatePipelineCache(v.device,&ci,NULL,&compute_cache);
    if(r && data){ci.initialDataSize=0;ci.pInitialData=NULL;r=vkCreatePipelineCache(v.device,&ci,NULL,&compute_cache);}
    uint32_t imported_header=0;if(ci.initialDataSize>=32)memcpy(&imported_header,data,4);
    compute_cache_imported=!r && imported_header>=32 && ci.initialDataSize>imported_header;
    LOG("Vulkan compute cache: bytes=%zu result=%d",size,r);free(data);return r? -1:0;
}
static void compute_close_locked(void) {
    compute_cache_save();
    graphics_uniform_cache_close();
    images_close_locked();
    scratch_cache_close();
    for(unsigned i=0;i<sizeof compute_pipelines/sizeof *compute_pipelines;i++) {
        compute_pipeline *p=&compute_pipelines[i];
        if(p->pipeline)vkDestroyPipeline(v.device,p->pipeline,NULL);
        if(p->layout)vkDestroyPipelineLayout(v.device,p->layout,NULL);
        if(p->set)vkDestroyDescriptorSetLayout(v.device,p->set,NULL);
    }
    memset(compute_pipelines,0,sizeof compute_pipelines);
    if(compute_cache)vkDestroyPipelineCache(v.device,compute_cache,NULL);
    compute_cache=VK_NULL_HANDLE;compute_cache_path[0]=0;compute_cache_imported=0;
}
static VkDescriptorType compute_binding_type(const dm_shader_binding *b) {
    if(b->kind==DM_BIND_INPUT_ATTACHMENT)return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    if(b->kind==DM_BIND_TEXTURE)return (b->type&255)==2?VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if(b->kind==DM_BIND_IMAGE)return (b->type&255)==2?VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    return b->kind==DM_BIND_STORAGE || b->kind==DM_BIND_UBO_POINTER || b->kind==DM_BIND_COUNT ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
static compute_pipeline *compute_pipeline_get(const dm_compute_shader *s) {
    unsigned index=0;while(index<sizeof compute_catalog/sizeof *compute_catalog && s!=&compute_catalog[index])index++;
    if(index==sizeof compute_catalog/sizeof *compute_catalog || s->nbindings>64)return NULL;
    compute_pipeline *p=&compute_pipelines[index];if(p->pipeline)return p;
    VkDescriptorSetLayoutBinding bindings[64];
    for(unsigned i=0;i<s->nbindings;i++) {
        bindings[i]=(VkDescriptorSetLayoutBinding){s->bindings[i].binding,compute_binding_type(&s->bindings[i]),1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    }
    VkDescriptorSetLayoutCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=s->nbindings,.pBindings=bindings};
    if(vkCreateDescriptorSetLayout(v.device,&dc,NULL,&p->set))goto fail;
    VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&p->set};
    if(vkCreatePipelineLayout(v.device,&lc,NULL,&p->layout))goto fail;
    VkShaderModule module=VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mc={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=s->code_size,.pCode=s->code};
    if(vkCreateShaderModule(v.device,&mc,NULL,&module))goto fail;
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=p->layout};
    int cached=compute_cache_open();
    VkResult result=vkCreateComputePipelines(v.device,cached?VK_NULL_HANDLE:compute_cache,1,&pc,NULL,&p->pipeline);
    vkDestroyShaderModule(v.device,module,NULL);
    LOG("Vulkan compute pipeline %s local=%ux%ux%u result=%d",s->key,s->local[0],s->local[1],s->local[2],result);
    if(result)goto fail;
    compute_cache_checkpoint();return p;
fail:
    if(p->pipeline)vkDestroyPipeline(v.device,p->pipeline,NULL);
    if(p->layout)vkDestroyPipelineLayout(v.device,p->layout,NULL);
    if(p->set)vkDestroyDescriptorSetLayout(v.device,p->set,NULL);
    memset(p,0,sizeof *p);return NULL;
}
typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; int coherent; size_t size; } compute_scratch;
static compute_scratch scratch_cache[256];
static size_t scratch_cache_bytes;
static void scratch_destroy(compute_scratch *s) {
    if(s->mapped)vkUnmapMemory(v.device,s->memory);
    if(s->buffer) {
        graphics_descriptor_retire_object(GRAPHICS_DESCRIPTOR_BUFFER,(uint64_t)(uintptr_t)s->buffer);
        vkDestroyBuffer(v.device,s->buffer,NULL);
    }
    if(s->memory)device_memory_free(s->memory);
    memset(s,0,sizeof *s);
}
/* Every scratch owner waits for GPU completion before releasing it. Reuse
 * allocations without reusing contents: scratch_new still zeroes every byte. */
static void scratch_free(compute_scratch *s) {
    if(s->mapped && s->size<=64u*1024*1024-scratch_cache_bytes) {
        for(unsigned i=0;i<256;i++)if(!scratch_cache[i].buffer) {
            scratch_cache[i]=*s;scratch_cache_bytes+=s->size;memset(s,0,sizeof *s);return;
        }
    }
    scratch_destroy(s);
}
static void scratch_cache_close(void) {
    for(unsigned i=0;i<256;i++)scratch_destroy(&scratch_cache[i]);scratch_cache_bytes=0;
}
static int scratch_sync(compute_scratch *s,int download) {
    if(s->coherent)return 0;
    VkMappedMemoryRange r={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=s->memory,.size=VK_WHOLE_SIZE};
    return download?vkInvalidateMappedMemoryRanges(v.device,1,&r):vkFlushMappedMemoryRanges(v.device,1,&r);
}
static int scratch_new_preferred(compute_scratch *s,size_t size,int prefer_cached) {
    // Uniform arena allocations keep a distinct memory preference. A generic
    // cached scratch owner has no property certificate for that preference.
    for(unsigned i=0;!prefer_cached && i<256;i++)if(scratch_cache[i].buffer && scratch_cache[i].size==size) {
        *s=scratch_cache[i];memset(&scratch_cache[i],0,sizeof scratch_cache[i]);scratch_cache_bytes-=size;
        memset(s->mapped,0,size);return 0;
    }
    s->size=size;
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
        .usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|
            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(vkCreateBuffer(v.device,&bc,NULL,&s->buffer))return -1;
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(v.device,s->buffer,&req);
    VkPhysicalDeviceMemoryProperties mp=device_memory.properties;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        type=i;if(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;
    }
    if(type==UINT32_MAX)return -1;
    if(prefer_cached) {
        VkMemoryPropertyFlags desired=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i)) &&
            (mp.memoryTypes[i].propertyFlags&desired)==desired){type=i;break;}
    }
    s->coherent=(mp.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    if(device_memory_allocate(&ai,&s->memory,DEVICE_MEMORY_SCRATCH) || vkBindBufferMemory(v.device,s->buffer,s->memory,0) ||
       vkMapMemory(v.device,s->memory,0,VK_WHOLE_SIZE,0,&s->mapped))return -1;
    memset(s->mapped,0,size);
    if(prefer_cached)LOG("Uniform arena memory: bytes=%zu type=%u flags=%#x hostCached=%u",size,type,
        mp.memoryTypes[type].propertyFlags,!!(mp.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
    return 0;
}
static int scratch_new(compute_scratch *s,size_t size) {return scratch_new_preferred(s,size,0);}
static int scratch_new_uniform_arena(compute_scratch *s,size_t size) {
    if(!scratch_new_preferred(s,size,1))return 0;
    // Host-cached memory is optional. Allocation/map failure retries the
    // original path without changing Vulkan visibility or device requirements.
    scratch_destroy(s);return scratch_new(s,size);
}
// The FIFO retains recorded/submitted resources until real GPU completion.
// Its serials count logical operations; pending_shared binds an unsubmitted
// operation to an exact marker in the current draw batch under the same lock.
enum { COMPUTE_SUBMISSION_SLOTS=VK_COMPUTE_FIFO_CAPACITY };
_Static_assert(COMPUTE_SUBMISSION_SLOTS<=PERF_SLOTS,"compute FIFO must fit timing probe slots");
typedef struct {
    VkCommandBuffer cmd;VkDescriptorPool pool;
    VkDescriptorSet set;VkDescriptorSetLayout layout;
    int busy;
} compute_dispatch_lease;
static compute_dispatch_lease compute_dispatch_leases[COMPUTE_SUBMISSION_SLOTS];
enum { COMPUTE_POOL_READ=1,COMPUTE_POOL_WRITE=2,COMPUTE_POOL_RANGES=64 };
typedef struct {dm_vk_pool *pool;size_t offset,size;unsigned access;} compute_pool_range;
typedef struct {
    // A mapped entry never owns/resets the draw fence. Its monotonic serial
    // remains valid after that draw slot has been retired and reused.
    VkFence fence;uint64_t serial,completion_draw_serial,completion_value;
    int pending_shared; // Recorded in the current draw CB, not submitted yet.
    size_t texture_staging_bytes;
    VkCommandBuffer cmd;VkDescriptorPool descriptors;
    compute_dispatch_lease *lease;
    compute_scratch scratch[64];unsigned uniform_refs[64];
    VkImageView views[64];VkSampler samplers[64];VkBufferView buffer_views[64];
    unsigned view_slots[64],sampler_slots[64],texel_slots[64];
    compute_pool_range pool_ranges[COMPUTE_POOL_RANGES];
    unsigned pool_range_count,pool_ranges_complete;
} compute_submission;
static compute_submission compute_submissions[COMPUTE_SUBMISSION_SLOTS];
static unsigned compute_submission_head,compute_submission_count;
static uint64_t compute_submitted_serial,compute_completed_serial;
static size_t compute_texture_staging_bytes;
static int compute_peek_ready_locked(dm_vk_pool *pool,size_t offset,size_t size) {
    uint64_t latest=pool->last_compute_use;
    if(latest<=compute_completed_serial)return 1;
    if(latest>compute_submitted_serial || compute_submission_count>COMPUTE_SUBMISSION_SLOTS ||
       compute_submitted_serial-compute_completed_serial!=compute_submission_count)return 0;
    int covered=0;
    for(unsigned i=0;i<compute_submission_count;i++) {
        const compute_submission *s=&compute_submissions[(compute_submission_head+i)%COMPUTE_SUBMISSION_SLOTS];
        if(s->serial!=compute_completed_serial+i+1)return 0;
        if(s->serial>latest)continue;
        if(!s->pool_ranges_complete || s->pool_range_count>COMPUTE_POOL_RANGES)return 0;
        for(unsigned j=0;j<s->pool_range_count;j++) {
            const compute_pool_range *r=&s->pool_ranges[j];
            if(r->pool!=pool)continue;
            if(!r->size || r->offset>pool->size || r->size>pool->size-r->offset ||
               !r->access || (r->access&~(COMPUTE_POOL_READ|COMPUTE_POOL_WRITE)))return 0;
            if(s->serial==latest)covered=1;
            // Pending and submitted writers both reject. Read-only users of
            // these exact bytes do not prevent an independent CPU snapshot.
            if((r->access&COMPUTE_POOL_WRITE) && offset<r->offset+r->size && r->offset<offset+size)return 0;
        }
    }
    return covered;
}
static void compute_draw_batch_submitted(uint64_t serial,uint64_t draw_serial) {
    for(unsigned i=0;i<compute_submission_count;i++) {
        compute_submission *slot=&compute_submissions[(compute_submission_head+i)%COMPUTE_SUBMISSION_SLOTS];
        if(slot->serial!=serial)continue;
        if(!slot->pending_shared || slot->completion_draw_serial!=draw_serial || slot->cmd)
            die("Vulkan shared compute submission certificate mismatch");
        slot->pending_shared=0;return;
    }
    die("Vulkan shared compute submission owner missing");
}
// Incomplete coverage never authorizes an overwrite; retain the whole-pool
// certificate as a conservative fallback.
static void compute_pool_range_add(compute_submission *slot,dm_vk_pool *pool,size_t offset,size_t size,unsigned access) {
    if(!pool || !size || offset>pool->size || size>pool->size-offset ||
       !access || (access&~(COMPUTE_POOL_READ|COMPUTE_POOL_WRITE)) ||
       slot->pool_range_count==COMPUTE_POOL_RANGES) {
        slot->pool_ranges_complete=0;return;
    }
    slot->pool_ranges[slot->pool_range_count++]=(compute_pool_range){pool,offset,size,access};
}
static void compute_submission_retire(compute_submission *slot) {
    if(slot->pending_shared)die("Vulkan retiring unsubmitted shared compute");
    if(!slot->completion_draw_serial)gpu_probe_retire(PERF_COMPUTE,(unsigned)(slot-compute_submissions));
    if(slot->cmd && !slot->lease)vkFreeCommandBuffers(v.device,v.commands,1,&slot->cmd);
    if(slot->descriptors)vkDestroyDescriptorPool(v.device,slot->descriptors,NULL);
    for(unsigned i=0;i<64;i++) {
        graphics_view_release(slot->views[i],slot->view_slots[i]);
        graphics_sampler_release(slot->samplers[i],slot->sampler_slots[i]);
        graphics_texel_release(slot->buffer_views[i],slot->texel_slots[i]);
        graphics_uniform_release(slot->uniform_refs[i]);scratch_free(&slot->scratch[i]);
    }
    if(slot->lease)slot->lease->busy=0;
    if(slot->texture_staging_bytes>compute_texture_staging_bytes)die("Vulkan texture staging accounting underflow");
    compute_texture_staging_bytes-=slot->texture_staging_bytes;
    compute_completed_serial=slot->serial;
    VkFence fence=slot->fence;memset(slot,0,sizeof *slot);slot->fence=fence;
    compute_submission_head=(compute_submission_head+1)%COMPUTE_SUBMISSION_SLOTS;
    compute_submission_count--;
    pool_cpu_hazard_remove();
}
static int compute_submissions_poll(void) {
    // One counter refresh covers both FIFOs, including every mapped draw
    // serial below. Only mapped entries need draw retirement here: otherwise
    // fence fallback would perform unrelated driver probes on every call.
    if(completion_poll())return -1;
    int mapped=0;
    for(unsigned i=0;i<compute_submission_count;i++)
        if(compute_submissions[(compute_submission_head+i)%COMPUTE_SUBMISSION_SLOTS].completion_draw_serial){mapped=1;break;}
    if(mapped && draw_submissions_poll_cached())return -1;
    while(compute_submission_count) {
        compute_submission *slot=&compute_submissions[compute_submission_head];
        if(slot->pending_shared) {
            if(!draw_compute_pending_valid(slot->serial,slot->completion_draw_serial))return -1;
            perf_metrics.shared_compute_pending_polls++;return 0;
        }
        if(slot->completion_draw_serial) {
            // Retirement dependencies are one-way: draw callbacks may publish
            // reports and release resources, but must not retire compute FIFO.
            int ready=draw_serial_ready(slot->completion_draw_serial);
            if(ready<0)return -1;
            if(!ready)return 0;
        } else {
            int ready=completion_ready(slot->completion_value,slot->fence);
            if(!ready)return 0;
            if(ready<0)return -1;
        }
        compute_submission_retire(slot);
    }
    return 0;
}
static int compute_wait_serial_at(uint64_t serial,const char *site) {
    if(serial>compute_submitted_serial)return -1;
    if(compute_completed_serial<serial) {
        int pending=0;
        for(unsigned i=0;i<compute_submission_count;i++) {
            const compute_submission *target=&compute_submissions[(compute_submission_head+i)%COMPUTE_SUBMISSION_SLOTS];
            if(target->serial!=serial)continue;
            pending=target->pending_shared;
            if(pending && !draw_compute_pending_valid(serial,target->completion_draw_serial))return -1;
            break;
        }
        // Copy the state before submitting: clear/report retirement can recurse.
        // Re-resolve all certificates below after the pending batch is published.
        if(pending) {
            perf_metrics.shared_compute_wait_submits++;
            if(draw_pending_submit())return -1;
        }
    }
    if(completion.semaphore && compute_completed_serial<serial) {
        uint64_t value=0,draw_serial=0;
        for(unsigned i=0;i<compute_submission_count;i++) {
            const compute_submission *target=&compute_submissions[(compute_submission_head+i)%COMPUTE_SUBMISSION_SLOTS];
            if(target->serial==serial){value=target->completion_value;draw_serial=target->completion_draw_serial;break;}
        }
        // Copy the certificate before draw retirement can invoke callbacks.
        // Compute serials follow actual submissions on the same queue, so the
        // target certificate also covers every earlier compute FIFO entry.
        if(draw_serial) {
            if(draw_wait_serial_at(draw_serial,site))return -1;
        } else if(!value || completion_wait_at(value,VK_NULL_HANDLE,site))return -1;
    }
    while(compute_completed_serial<serial) {
        if(!compute_submission_count)return -1;
        compute_submission *slot=&compute_submissions[compute_submission_head];
        if(slot->completion_draw_serial) {
            if(draw_wait_serial_at(slot->completion_draw_serial,site))return -1;
        } else if(completion.semaphore) {
            if(completion_ready(slot->completion_value,VK_NULL_HANDLE)!=1)return -1;
        } else if(completion_wait_at(slot->completion_value,slot->fence,site))return -1;
        compute_submission_retire(slot);
    }
    return 0;
}
// Independent leases are acquired only after binding preparation. Texture and
// copy submissions may consume FIFO slots, but cannot borrow these objects.
static uint64_t compute_pool_range_target_locked(dm_vk_pool *pool,size_t offset,size_t size,int *fallback) {
    *fallback=0;
    uint64_t whole=pool->last_compute_use;
    if(whole<=compute_completed_serial)return 0;
    if(!pool->coherent || offset>pool->size || size>pool->size-offset ||
       whole>compute_submitted_serial || compute_submission_count>COMPUTE_SUBMISSION_SLOTS ||
       compute_submitted_serial-compute_completed_serial!=compute_submission_count)goto fallback;
    uint64_t wait=0;int latest_covered=0;
    for(unsigned i=0;i<compute_submission_count;i++) {
        const compute_submission *slot=&compute_submissions[(compute_submission_head+i)%COMPUTE_SUBMISSION_SLOTS];
        if(!slot->pool_ranges_complete || slot->pool_range_count>COMPUTE_POOL_RANGES ||
           slot->serial!=compute_completed_serial+i+1 ||
           (!slot->completion_draw_serial && (completion.semaphore?
               (!slot->completion_value || slot->completion_value>completion.submitted):!slot->fence)) ||
           (slot->pending_shared?!draw_compute_pending_valid(slot->serial,slot->completion_draw_serial):
               (slot->completion_draw_serial && draw_serial_ready(slot->completion_draw_serial)<0)))goto fallback;
        for(unsigned j=0;j<slot->pool_range_count;j++) {
            const compute_pool_range *range=&slot->pool_ranges[j];
            if(range->pool!=pool)continue;
            if(!range->size || range->offset>pool->size || range->size>pool->size-range->offset ||
               !range->access || (range->access&~(COMPUTE_POOL_READ|COMPUTE_POOL_WRITE)) || slot->serial>whole)goto fallback;
            if(slot->serial==whole)latest_covered=1;
            // Protect the entire request: callers may wait only once before
            // uploading several changed pages, not merely this first page.
            if(size && offset<range->offset+range->size && range->offset<offset+size)wait=slot->serial;
        }
    }
    if(!latest_covered)goto fallback;
    return wait;
fallback:
    *fallback=1;return whole;
}
static int compute_wait_pool_range_from_locked(dm_vk_pool *pool,size_t offset,size_t size,int cpu_owner) {
    gpu_metrics.compute_range_wait_calls++;
    if(!pool)return -1;
    uint64_t whole=pool->last_compute_use;
    if(whole<=compute_completed_serial){gpu_metrics.compute_range_already_retired++;if(cpu_owner)perf_metrics.cpu_write_compute[0]++;return 0;}
    int fallback=0;
    uint64_t wait=compute_pool_range_target_locked(pool,offset,size,&fallback);
    if(fallback){gpu_metrics.compute_range_fallback++;if(cpu_owner)perf_metrics.cpu_write_compute[4]++;return compute_wait_serial(wait);}
    if(cpu_owner)perf_metrics.cpu_write_compute[!wait?1:wait<whole?2:3]++;
    if(!wait)gpu_metrics.compute_range_no_overlap++;
    else if(wait<whole)gpu_metrics.compute_range_older++;
    else gpu_metrics.compute_range_latest++;
    // Potential serial distance, never a count of driver calls saved.
    gpu_metrics.compute_range_skipped_distance+=whole-(wait?wait:compute_completed_serial);
    // No slot/range pointer survives retirement callbacks.
    return compute_wait_serial(wait);
}
static int compute_wait_pool_range_locked(dm_vk_pool *pool,size_t offset,size_t size) {
    return compute_wait_pool_range_from_locked(pool,offset,size,0);
}
static int compute_cpu_write_range_locked(dm_vk_pool *pool,size_t offset,size_t size) {
    return compute_wait_pool_range_from_locked(pool,offset,size,1);
}
static int compute_fifo_room(void) {
    while(compute_submission_count==COMPUTE_SUBMISSION_SLOTS) {
        if(compute_submissions_poll())return -1;
        if(compute_submission_count==COMPUTE_SUBMISSION_SLOTS &&
           compute_wait_serial(compute_submissions[compute_submission_head].serial))return -1;
    }
    return 0;
}
static compute_dispatch_lease *compute_dispatch_acquire(VkDescriptorSetLayout layout,int standalone) {
    int polled=0;
    for(;;) {
        compute_dispatch_lease *p=NULL;
        for(unsigned i=0;i<COMPUTE_SUBMISSION_SLOTS;i++) {
            compute_dispatch_lease *candidate=&compute_dispatch_leases[i];if(candidate->busy)continue;
            if(!p)p=candidate;
            if(candidate->layout==layout && candidate->set){p=candidate;break;}
        }
        if(p) {
            if(!p->pool) {
                VkDescriptorPoolSize sizes[6]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,64},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,64},
                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,64},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,64},
                    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,64},{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,64}};
                VkDescriptorPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=6,.pPoolSizes=sizes};
                if(vkCreateDescriptorPool(v.device,&ci,NULL,&p->pool))return NULL;
                gpu_metrics.compute_descriptor_pools++;
            }
            if(p->layout!=layout || !p->set) {
                if(vkResetDescriptorPool(v.device,p->pool,0))return NULL;
                p->set=VK_NULL_HANDLE;p->layout=VK_NULL_HANDLE;
                VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,
                    .descriptorSetCount=1,.pSetLayouts=&layout};
                if(vkAllocateDescriptorSets(v.device,&ai,&p->set))return NULL;
                p->layout=layout;gpu_metrics.compute_descriptor_allocs++;
            } else gpu_metrics.compute_descriptor_reuses++;
            if(standalone && !p->cmd) {
                VkCommandBufferAllocateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=v.commands,
                    .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
                if(vkAllocateCommandBuffers(v.device,&ci,&p->cmd))return NULL;
                gpu_metrics.compute_command_allocs++;
            } else if(standalone && vkResetCommandBuffer(p->cmd,0))return NULL;
            p->busy=1;return p;
        }
        // Exhaustion is the only reason to query completion here. Poll once,
        // then wait actual FIFO certificates (including intervening transfers)
        // until a lease frees; never reset a submitted descriptor/command pool.
        if(!polled) {
            if(compute_submissions_poll())return NULL;
            polled=1;continue;
        }
        if(!compute_submission_count || compute_wait_serial(compute_submissions[compute_submission_head].serial))return NULL;
    }
}
// Texture uploads use the same eight-slot FIFO as dispatch/copy. Reserve room
// before allocating staging: ordinary in-flight uploads stay within 64 MiB;
// an oversized single texture drains earlier uploads and occupies it alone.
static int compute_texture_room(size_t bytes) {
    const size_t budget=64u*1024*1024;
    int polled=0;
    while(compute_submission_count==COMPUTE_SUBMISSION_SLOTS ||
          (bytes && compute_texture_staging_bytes &&
           (bytes>budget || compute_texture_staging_bytes>budget-bytes))) {
        if(!polled) {
            if(compute_submissions_poll())return -1;
            polled=1;continue;
        }
        gpu_metrics.texture_staging_waits++;
        if(!compute_submission_count || compute_wait_serial(compute_submissions[compute_submission_head].serial))return -1;
    }
    return 0;
}
// Called immediately after recording: no callbacks, waits or recoverable
// failures until the draw marker and staging owner have both been published.
static uint64_t compute_texture_record_locked(VkCommandBuffer command,uint64_t draw_serial,compute_scratch *staging) {
    size_t bytes=staging?staging->size:0;
    if(compute_submission_count==COMPUTE_SUBMISSION_SLOTS || compute_submitted_serial==UINT64_MAX)
        die("Vulkan shared texture FIFO ownership changed after preparation");
    compute_submission *slot=&compute_submissions[(compute_submission_head+compute_submission_count)%COMPUTE_SUBMISSION_SLOTS];
    uint64_t serial=++compute_submitted_serial;slot->serial=serial;
    draw_compute_record_locked(command,serial,draw_serial);
    slot->pending_shared=1;slot->completion_draw_serial=draw_serial;slot->cmd=VK_NULL_HANDLE;
    // Texture transfers reference image/staging allocations, not resident pools.
    slot->pool_ranges_complete=1;slot->pool_range_count=0;
    if(staging){slot->scratch[0]=*staging;memset(staging,0,sizeof *staging);}
    slot->texture_staging_bytes=bytes;compute_texture_staging_bytes+=bytes;
    if(compute_texture_staging_bytes>gpu_metrics.texture_staging_peak_bytes)
        gpu_metrics.texture_staging_peak_bytes=compute_texture_staging_bytes;
    compute_submission_count++;pool_cpu_hazard_add();
    gpu_metrics.texture_shared_records++;gpu_metrics.texture_shared_bytes+=bytes;return serial;
}
// B2B transfers share the compute FIFO's completion serials and pool hazards.
// The draw batch owns the command buffer; this FIFO entry retains pool usage
// until that batch completes, exactly as a deferred compute dispatch does.
static int compute_copy_record_locked(dm_vk_pool *src,size_t source_offset,dm_vk_pool *dst,size_t offset,size_t size) {
    VkCommandBuffer cmd=VK_NULL_HANDLE;uint64_t draw_serial=0;
    if(draw_compute_begin_locked(&cmd,&draw_serial,0))return -1;
    if(compute_submission_count==COMPUTE_SUBMISSION_SLOTS || compute_submitted_serial==UINT64_MAX)
        die("Vulkan shared copy FIFO changed after preparation");
    // No callbacks, waits or recoverable failures until both owners, the
    // marker, range certificates and pool hazards have been published.
    pool_shadow_invalidate(dst,offset,size);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
    VkBufferCopy copy={source_offset,offset,size};vkCmdCopyBuffer(cmd,src->buffer,dst->buffer,1,&copy);
    mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);
    compute_submission *slot=&compute_submissions[(compute_submission_head+compute_submission_count)%COMPUTE_SUBMISSION_SLOTS];
    uint64_t serial=++compute_submitted_serial;
    slot->serial=serial;slot->cmd=VK_NULL_HANDLE;
    slot->pending_shared=1;slot->completion_draw_serial=draw_serial;
    draw_compute_record_locked(cmd,serial,draw_serial);
    slot->pool_ranges_complete=1;slot->pool_range_count=0;
    compute_pool_range_add(slot,src,source_offset,size,COMPUTE_POOL_READ);
    compute_pool_range_add(slot,dst,offset,size,COMPUTE_POOL_WRITE);
    compute_submission_count++;pool_cpu_hazard_add();
    perf_metrics.shared_compute_copies++;
    // Readers matter too: a CPU overwrite or pool destruction must not race
    // the transfer source even if this copy never owns any source dirty bytes.
    src->last_compute_use=serial;dst->last_compute_use=serial;
    // Interval overflow may wait and retire this very FIFO slot. All resource
    // ownership has been published; do not access the slot after this call.
    if(pool_gpu_written_locked(dst,offset,size,serial))
        die("Vulkan submitted copy output ownership allocation failed");
    return 0;
}
// CPU-owned immutable uniform payload -> GPU-owned destination, ordered in the
// same body/FIFO as copies and dispatches. Retirement retains the pool range.
static int compute_update_record_locked(dm_vk_pool *dst,size_t offset,const void *data,size_t size) {
    VkCommandBuffer cmd=VK_NULL_HANDLE;uint64_t draw_serial=0;
    if(draw_compute_begin_locked(&cmd,&draw_serial,0))return -1;
    if(compute_submission_count==COMPUTE_SUBMISSION_SLOTS || compute_submitted_serial==UINT64_MAX)
        die("Vulkan shared uniform update FIFO changed after preparation");
    pool_shadow_invalidate(dst,offset,size);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
    vkCmdUpdateBuffer(cmd,dst->buffer,offset,size,data);
    mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);
    compute_submission *slot=&compute_submissions[(compute_submission_head+compute_submission_count)%COMPUTE_SUBMISSION_SLOTS];
    uint64_t serial=++compute_submitted_serial;
    slot->serial=serial;slot->cmd=VK_NULL_HANDLE;
    slot->pending_shared=1;slot->completion_draw_serial=draw_serial;
    slot->pool_ranges_complete=1;slot->pool_range_count=0;
    compute_pool_range_add(slot,dst,offset,size,COMPUTE_POOL_WRITE);
    draw_compute_record_locked(cmd,serial,draw_serial);
    compute_submission_count++;pool_cpu_hazard_add();dst->last_compute_use=serial;
    perf_metrics.uniform_gpu_updates++;perf_metrics.uniform_gpu_update_bytes+=size;
    // Publish both owners before dirty-list overflow can retire this slot.
    if(pool_gpu_written_locked(dst,offset,size,serial))
        die("Vulkan uniform update output ownership allocation failed");
    return 0;
}
#include "vk_timestamp_gpu.inc"
static int compute_pending_flush(void) { return compute_wait_serial(compute_submitted_serial); }
static void compute_submissions_close(void) {
    if(compute_pending_flush())die("Vulkan compute retirement failed during close");
    if(compute_texture_staging_bytes)die("Vulkan texture staging remained after FIFO drain");
    for(unsigned i=0;i<COMPUTE_SUBMISSION_SLOTS;i++) {
        compute_dispatch_lease *p=&compute_dispatch_leases[i];
        if(p->busy)die("Vulkan compute dispatch lease remained after FIFO drain");
        if(p->cmd)vkFreeCommandBuffers(v.device,v.commands,1,&p->cmd);
        if(p->pool)vkDestroyDescriptorPool(v.device,p->pool,NULL);
    }
    memset(compute_dispatch_leases,0,sizeof compute_dispatch_leases);
    if(__atomic_load_n(&pool_cpu_hazards,__ATOMIC_ACQUIRE)!=pool_gpu_dirty_count+images_cpu_owned_count())
        die("Vulkan active compute hazards remained after FIFO drain");
    for(unsigned i=0;i<COMPUTE_SUBMISSION_SLOTS;i++)if(compute_submissions[i].fence)
        vkDestroyFence(v.device,compute_submissions[i].fence,NULL);
    memset(compute_submissions,0,sizeof compute_submissions);
    compute_submission_head=compute_submission_count=0;
    compute_submitted_serial=compute_completed_serial=0;
    images_compute_reset();
    for(dm_vk_pool *p=pools;p;p=p->next) {
        p->last_compute_use=0;
        for(pool_gpu_range *r=p->gpu_dirty;r;r=r->next)r->writer=0;
    }
}
#include "vk_images.inc"
#include "vk_image_copy.inc"
#include "vk_render_targets.inc"
int dm_vk_compute(const dm_compute_shader *s,const dm_compute_buffer *buffers,unsigned n,uint32_t x,uint32_t y,uint32_t z) {
    pthread_mutex_lock(&lock);
    // Publish before preparation and before any submit, including synchronous
    // fallback. Hold this contribution until FIFO/dirty ownership is published
    // or all failure cleanup completes, so the summary cannot transiently hit 0.
    pool_cpu_hazard_add();
    // Statistics must not add two clock syscalls to every dispatch. Keep exact
    // call counts and sparse, unscaled timings; game deadlines are unaffected.
    int timing_sample=gpu_metrics.compute_calls%CPU_OPERATION_TIMING_STRIDE==0;
    double compute_started=timing_sample?gpu_time_ms():0;gpu_metrics.compute_calls++;
    int result=-1,dispatch_inflight=0,defer=1;compute_scratch scratch[64]={0};dm_vk_pool *guest[64]={0};size_t offsets[64]={0};unsigned uniform_refs[64]={0};
    size_t binding_sizes[64]={0};uint8_t resident[64]={0};
    compute_dispatch_lease *lease=NULL;VkCommandBuffer cmd=VK_NULL_HANDLE;
    gpu_image *images[64]={0};VkImageView views[64]={0};VkSampler samplers[64]={0};VkBufferView buffer_views[64]={0};
    unsigned view_slots[64]={0},sampler_slots[64]={0},texel_slots[64]={0};
    if(!v.device || !s || n!=s->nbindings || n>64)goto done;
    const VkPhysicalDeviceProperties props=v.properties;
    if(x>props.limits.maxComputeWorkGroupCount[0] || y>props.limits.maxComputeWorkGroupCount[1] || z>props.limits.maxComputeWorkGroupCount[2])goto done;
    if(!x || !y || !z){result=0;goto done;}
    // Reclaim only under actual arena pressure, before any binding borrows
    // bytes/resources that a retirement callback could release.
    if(graphics_uniform_under_pressure(64)) {
        if(compute_submissions_poll())goto done;
        if(graphics_uniform_under_pressure(64)) {
            // The compute poll just refreshed this queue's completion counter;
            // no submission occurred between it and this draw retirement.
            perf_metrics.timeline_pressure_reused++;
            if(draw_submissions_poll_cached())goto done;
        }
    }
    // Aligned SSBOs can borrow their resident pool; other bindings retain
    // snapshots. Keep rejecting writable aliases, including buffer textures:
    // mixing a direct binding and an independent snapshot would change alias
    // semantics. No rounding/rebasing of shader-visible offsets is allowed.
    for(unsigned i=0;i<n;i++) {
        if(buffers[i].binding!=s->bindings[i].binding)goto done;
        uint64_t address=buffers[i].address;size_t size=buffers[i].size;
        if(buffers[i].image) {
            VkDescriptorType type=compute_binding_type(&s->bindings[i]);
            if(type!=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && type!=VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)continue;
            nvn_texlayout layout;nvn_tex_layout(&buffers[i].image->desc,&layout);
            address=buffers[i].image->address;size=layout.size;
        }
        if(!size)goto done;binding_sizes[i]=size;
        if(address) {
            if(reports_gpu_range_prepare(address,size,__func__) || images_flush_alias_range(address,size))goto done;
            guest[i]=pool_address_locked(address,size,&offsets[i]);if(!guest[i])goto done;
            for(unsigned j=0;j<i;j++)if(guest[j]==guest[i] && (buffers[i].writable || buffers[j].writable) &&
                offsets[i]<offsets[j]+binding_sizes[j] && offsets[j]<offsets[i]+size) {LOG("Vulkan compute overlapping writable bindings %u/%u",i,j);goto done;}
        } else if(!buffers[i].image && !buffers[i].host)goto done;
    }
    compute_pipeline *pipeline=compute_pipeline_get(s);if(!pipeline)goto done;
    // Keep independent prior draws pending for a possible shared submission.
    // Resource preparation still submits/waits at real upload/CPU dependencies;
    // do not borrow a FIFO slot until every such boundary has completed.
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkDescriptorBufferInfo info[64];VkDescriptorImageInfo image_info[64];VkWriteDescriptorSet writes[64];
    for(unsigned i=0;i<n;i++) {
        VkDescriptorType type=compute_binding_type(&s->bindings[i]);
        writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=buffers[i].binding,.descriptorCount=1,.descriptorType=type};
        if(buffers[i].image) {
            const dm_compute_image *d=buffers[i].image;
            if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                nvn_texlayout layout;nvn_tex_layout(&d->desc,&layout);
                if(d->address && (reports_gpu_range_prepare(d->address,layout.size,__func__) || images_flush_alias_range(d->address,layout.size)))goto done;
                guest[i]=pool_address_locked(d->address,layout.size,&offsets[i]);if(d->address && !guest[i])goto done;
                VkDeviceSize alignment=props.limits.minTexelBufferOffsetAlignment;
                int bw=0,bh=0,bpb=nvn_fmt_bpb(d->format,&bw,&bh);
                int direct=guest[i] && guest[i]->mapped && guest[i]->buffer &&
                    (!alignment || offsets[i]%alignment==0) && bpb>0 && bw==1 && bh==1 &&
                    offsets[i]%(unsigned)bpb==0 && layout.size%(unsigned)bpb==0 &&
                    layout.size/(unsigned)bpb<=props.limits.maxTexelBufferElements;
                if(direct) {
                    if(pool_gpu_read_consumer_locked(guest[i],offsets[i],layout.size,POOL_PREP_COMPUTE))goto done;
                    VkBufferViewCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                        .buffer=guest[i]->buffer,.format=image_format(d->format),.offset=offsets[i],.range=layout.size};
                    buffer_views[i]=graphics_cached_texel(&bc,&texel_slots[i]);if(!buffer_views[i])goto done;
                    gpu_metrics.compute_texel_bindings++;
                    resident[i]=1;writes[i].pTexelBufferView=&buffer_views[i];continue;
                }
                // Unaligned/nonresident texel views retain a private snapshot.
                // Read-only snapshots can be fence-owned; writable snapshots
                // still require the original synchronous CPU copyback path.
                if(scratch_new(&scratch[i],layout.size))goto done;
                if(guest[i]) {
                    if(pool_cpu_read_locked(guest[i],offsets[i],layout.size))goto done;
                    memcpy(scratch[i].mapped,guest[i]->guest+offsets[i],layout.size);
                }
                if(scratch_sync(&scratch[i],0))goto done;
                VkBufferViewCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=scratch[i].buffer,.format=image_format(d->format),.range=layout.size};
                if(vkCreateBufferView(v.device,&bc,NULL,&buffer_views[i]))goto done;
                writes[i].pTexelBufferView=&buffer_views[i];
            } else {
                images[i]=image_get(d);if(!images[i] || (buffers[i].writable && !(images[i]->usage&VK_IMAGE_USAGE_STORAGE_BIT)) || image_transfer(images[i],0))goto done;
                views[i]=graphics_cached_view(images[i],d,&s->bindings[i],&view_slots[i]);if(!views[i])goto done;
                gpu_metrics.compute_view_bindings++;
                if(type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER){
                    samplers[i]=graphics_cached_sampler(d,&sampler_slots[i]);if(!samplers[i])goto done;
                    gpu_metrics.compute_sampler_bindings++;
                }
                image_info[i]=(VkDescriptorImageInfo){samplers[i],views[i],VK_IMAGE_LAYOUT_GENERAL};writes[i].pImageInfo=&image_info[i];
            }
            continue;
        }
        size_t size=buffers[i].size;
        if(type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && size<65536)size=65536;
        if(size>(type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?props.limits.maxUniformBufferRange:props.limits.maxStorageBufferRange))goto done;
        if(buffers[i].zero && !buffers[i].writable) {
            if(buffers[i].address || buffers[i].size!=65536 || graphics_uniform_zero_get(&info[i],&uniform_refs[i])<0)goto done;
            writes[i].pBufferInfo=&info[i];continue;
        }
        VkDeviceSize alignment=props.limits.minStorageBufferOffsetAlignment;
        if(!alignment)alignment=1;
        if(type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && guest[i] && !buffers[i].zero &&
           !(offsets[i]%alignment)) {
            // pool lookup validated the exact range; size was checked against
            // maxStorageBufferRange above. Do not pad a pool-backed binding.
            if(pool_gpu_read_consumer_locked(guest[i],offsets[i],size,POOL_PREP_COMPUTE))goto done;
            resident[i]=1;
            info[i]=(VkDescriptorBufferInfo){guest[i]->buffer,offsets[i],size};
            writes[i].pBufferInfo=&info[i];continue;
        }
        if(guest[i] && pool_cpu_read_locked(guest[i],offsets[i],buffers[i].size))goto done;
        if(!guest[i] && !buffers[i].zero && reports_range_drain((uintptr_t)buffers[i].host,buffers[i].size))goto done;
        if(type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && !buffers[i].writable && !buffers[i].zero) {
            int arena=graphics_uniform_dynamic_get(guest[i]?guest[i]->guest+offsets[i]:buffers[i].host,
                buffers[i].size,&info[i],&uniform_refs[i],0);
            if(arena<0)goto done;
            if(arena) {gpu_metrics.compute_uniform_arena++;writes[i].pBufferInfo=&info[i];continue;}
        }
        if(scratch_new(&scratch[i],size))goto done;
        if(!buffers[i].zero)memcpy(scratch[i].mapped,guest[i]?guest[i]->guest+offsets[i]:buffers[i].host,buffers[i].size);
        if(scratch_sync(&scratch[i],0))goto done;
        info[i]=(VkDescriptorBufferInfo){scratch[i].buffer,0,size};
        writes[i].pBufferInfo=&info[i];
    }
    for(unsigned i=0;i<n;i++)if(buffers[i].writable && !images[i] &&
        (!resident[i] || (s->bindings[i].kind!=DM_BIND_STORAGE && !buffer_views[i])))defer=0;
    // Separate packaged image objects and buffer views can represent the same
    // guest bytes. Reject writable overlaps until these aliases share one
    // Vulkan allocation/version; a CPU wait cannot fix conflicting outputs.
    // Repeated bindings of
    // the very same image are safe and retain asynchronous execution.
    for(unsigned i=0;i<n;i++)if(images[i] && images[i]->packaged && images[i]->address) {
        uint64_t lo=images[i]->address,hi=lo+images[i]->storage.size;
        for(unsigned j=0;j<n;j++)if(i!=j && images[j]!=images[i] &&
            (buffers[i].writable || buffers[j].writable)) {
            uint64_t other=0;size_t bytes=0;
            if(images[j] && images[j]->packaged) {other=images[j]->address;bytes=images[j]->storage.size;}
            else if(!images[j] && guest[j]) {other=(uintptr_t)guest[j]->guest+offsets[j];bytes=binding_sizes[j];}
            if(other && lo<other+bytes && other<hi) {
                LOG("Vulkan compute overlapping packaged writable bindings %u/%u",i,j);goto done;
            }
        }
    }
    lease=compute_dispatch_acquire(pipeline->set,!defer);if(!lease)goto done;
    set=lease->set;
    for(unsigned i=0;i<n;i++)writes[i].dstSet=set;
    vkUpdateDescriptorSets(v.device,n,writes,0,NULL);
    uint64_t completion_draw_serial=0;
    if(defer) {
        if(draw_compute_begin_locked(&cmd,&completion_draw_serial,0))goto done;
        if(compute_submission_count==COMPUTE_SUBMISSION_SLOTS || compute_submitted_serial==UINT64_MAX)
            die("Vulkan shared compute FIFO changed after preparation");
    } else {
        cmd=lease->cmd;
        VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if(vkBeginCommandBuffer(cmd,&bi))goto done;
    }
    // From shared Begin through FIFO publication below, perform no operation
    // which can fail recoverably, submit, wait or invoke retirement callbacks.
    for(unsigned i=0;i<n;i++)if(resident[i] && buffers[i].writable)
        pool_shadow_invalidate(guest[i],offsets[i],binding_sizes[i]);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,NULL,0,NULL);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline->pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline->layout,0,1,&set,0,NULL);
    vkCmdDispatch(cmd,x,y,z);
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT|VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);
    if(!defer && vkEndCommandBuffer(cmd))goto done;
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    if(defer) {
        // Resources belong to the compute FIFO; the command buffer belongs
        // exclusively to the draw batch. A real wait will submit it first.
        compute_submission *slot=&compute_submissions[(compute_submission_head+compute_submission_count)%COMPUTE_SUBMISSION_SLOTS];
        uint64_t serial=++compute_submitted_serial;slot->serial=serial;
        draw_compute_record_locked(cmd,serial,completion_draw_serial);
        slot->pool_ranges_complete=1;slot->pool_range_count=0;
        for(unsigned i=0;i<n;i++)if(resident[i])
            compute_pool_range_add(slot,guest[i],offsets[i],binding_sizes[i],
                COMPUTE_POOL_READ|(buffers[i].writable?COMPUTE_POOL_WRITE:0));
        slot->completion_draw_serial=completion_draw_serial;
        slot->pending_shared=1;slot->cmd=VK_NULL_HANDLE;slot->lease=lease;
        memcpy(slot->scratch,scratch,sizeof scratch);memcpy(slot->uniform_refs,uniform_refs,sizeof uniform_refs);
        memcpy(slot->views,views,sizeof views);memcpy(slot->samplers,samplers,sizeof samplers);
        memcpy(slot->view_slots,view_slots,sizeof view_slots);memcpy(slot->sampler_slots,sampler_slots,sizeof sampler_slots);
        memcpy(slot->texel_slots,texel_slots,sizeof texel_slots);
        memcpy(slot->buffer_views,buffer_views,sizeof buffer_views);
        cmd=VK_NULL_HANDLE;lease=NULL;
        memset(scratch,0,sizeof scratch);memset(uniform_refs,0,sizeof uniform_refs);
        memset(views,0,sizeof views);memset(samplers,0,sizeof samplers);memset(buffer_views,0,sizeof buffer_views);
        memset(view_slots,0,sizeof view_slots);memset(sampler_slots,0,sizeof sampler_slots);
        memset(texel_slots,0,sizeof texel_slots);
        compute_submission_count++;pool_cpu_hazard_add();gpu_metrics.compute_deferred++;
        perf_metrics.shared_compute_dispatches++;
        for(unsigned i=0;i<n;i++)if(resident[i])guest[i]->last_compute_use=serial;
        for(unsigned i=0;i<n;i++)if(images[i]) {
            images[i]->last_compute_use=serial;
            if(buffers[i].writable) {
                // Publish authority without invoking the synchronous packaged
                // image readback in image_gpu_written. GPU consumers stay in
                // queue order; CPU alias reads materialize at their boundary.
                image_set_dirty(images[i],1);images[i]->upload_stamp=0;images[i]->shadow_valid=0;
            }
        }
        // Overflow handling may wait and retire this very submission. Never
        // touch slot again after publishing it and all resident read hazards.
        for(unsigned i=0;i<n;i++)if(resident[i] && buffers[i].writable)
            if(pool_gpu_written_locked(guest[i],offsets[i],binding_sizes[i],serial))
                die("Vulkan submitted compute output ownership allocation failed");
        result=0;goto done;
    }
    // Scratch CPU copyback retains the original standalone completion path;
    // no pending draw may be overtaken by this synchronous dispatch.
    if(draw_pending_submit())goto done;
    if(vkQueueSubmit(v.queue,1,&si,VK_NULL_HANDLE))die("Vulkan synchronous compute submit failed; retaining resources");
    dispatch_inflight=1;
    // This wait is required: all writable buffer readback below consumes this
    // dispatch, and its descriptors/scratch cannot be released while pending.
    if(vkQueueWaitIdle(v.queue))goto done;
    dispatch_inflight=0;
    for(unsigned i=0;i<n;i++)if(buffers[i].writable) {
        if(images[i]){if(image_gpu_written(images[i]))goto done;continue;}
        if(!guest[i])continue;
        if(resident[i]) {
            // The dispatch has completed, but its bytes remain GPU-owned.
            // Direct SSBO/vertex consumers keep this version; actual CPU
            // consumers and NVN CPU visibility boundaries materialize it.
            if(pool_gpu_written_locked(guest[i],offsets[i],binding_sizes[i],0))goto done;
            continue;
        }
        if(scratch_sync(&scratch[i],1))goto done;
        size_t size=buffers[i].image?scratch[i].size:buffers[i].size;
        dm_guest_write_prep(guest[i]->guest+offsets[i],size);
        memcpy(guest[i]->guest+offsets[i],scratch[i].mapped,size);
        gpu_metrics.compute_readback_bytes+=size;
        if(pool_sync_locked(guest[i],offsets[i],size,0))goto done;
    }
    result=0;
done:
    // Only a successful submit followed by a failed completion wait needs a
    // cleanup wait. Normal completion (including later readback failure) and
    // allocation/recording failures must not idle the queue again.
    if(dispatch_inflight) {
        VkResult waited=vkQueueWaitIdle(v.queue);
        if(waited)die("Vulkan compute completion failed result=%d; retaining pending resources and terminating",waited);
        dispatch_inflight=0;
    }
    for(unsigned i=0;i<64;i++) {
        graphics_view_release(views[i],view_slots[i]);
        graphics_sampler_release(samplers[i],sampler_slots[i]);
        graphics_texel_release(buffer_views[i],texel_slots[i]);
        graphics_uniform_release(uniform_refs[i]);
        scratch_free(&scratch[i]);
    }
    if(lease)lease->busy=0;
    if(result)LOG("Vulkan compute failed: key=%s groups=%u,%u,%u",s?s->key:"null",x,y,z);
    if(timing_sample){gpu_metrics.compute_ms+=gpu_time_ms()-compute_started;gpu_metrics.compute_timing_samples++;}
    pool_cpu_hazard_remove();
    pthread_mutex_unlock(&lock);return result;
}
