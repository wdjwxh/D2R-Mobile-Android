/* Shared graphics descriptor preparation mirrors the synchronous compute
 * resource path. Resources remain alive until the submitted draw completes. */
#include "vk_graphics_image_cache.inc"
#include "vk_graphics_uniform_cache.inc"
#include "vk_graphics_buffer_cache.inc"
typedef struct graphics_resources {
    struct graphics_resources *idle_next;
    // Offset-only draws own their range keys, but lease immutable handles and
    // snapshots from one original owner. Never form chains of draw leases.
    struct graphics_resources *static_owner;
    graphics_descriptor_bundle *descriptor_bundle;
    graphics_descriptor_shape shape;graphics_descriptor_payload payload;
    uint64_t resident_storage; // Borrowed ordinary SSBO ranges; never scratch-owned.
    uint64_t resident_uniform; // Guarded ABI1, exact shared guest UBO slices.
    uint32_t uniform_vec4_counts[16]; // Final descriptors: fallback=4096.
    uint64_t resident_texel; // Mutable pool-backed read-only views, not snapshots.
    uint64_t dynamic_profile;unsigned dynamic_count;uint32_t dynamic_offsets[32];
    compute_scratch scratch[64];dm_vk_pool *guest[64];size_t offsets[64];
    gpu_image *images[64];VkImageView views[64];VkSampler samplers[64];VkBufferView buffer_views[64];
    unsigned view_slots[64],sampler_slots[64],texel_slots[64];
    unsigned uniform_refs[64],buffer_refs[64];
    VkDescriptorPool descriptors;VkDescriptorSet set;
    unsigned refs,count;int cacheable;
    const dm_graphics_shader *shader;VkDescriptorSetLayout layout;
    dm_shader_binding binding_keys[64];dm_compute_buffer input_keys[64];dm_compute_image image_keys[64];
} graphics_resources;
static int graphics_stage_image_equal(const dm_compute_image *a,const dm_compute_image *b);
static struct {uint64_t source_stages,candidates,reused,fallback,epoch_rejected;} graphics_image_binding_stats;
static void graphics_image_binding_reuse_report(void) {
    LOG("Graphics image binding reuse: sourceStages=%llu candidates=%llu reused=%llu fallback=%llu epochRejected=%llu",
        (unsigned long long)graphics_image_binding_stats.source_stages,(unsigned long long)graphics_image_binding_stats.candidates,
        (unsigned long long)graphics_image_binding_stats.reused,(unsigned long long)graphics_image_binding_stats.fallback,
        (unsigned long long)graphics_image_binding_stats.epoch_rejected);
    memset(&graphics_image_binding_stats,0,sizeof graphics_image_binding_stats);
}
static int graphics_resources_reuse_image(graphics_resources *r,unsigned i,const graphics_resources *source,
                                         const uint64_t *epoch,uint64_t expected) {
    if(!source || r->shader->bindings[i].kind!=DM_BIND_TEXTURE ||
       compute_binding_type(&r->shader->bindings[i])!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
       r->input_keys[i].writable)return 0;
    graphics_image_binding_stats.candidates++;
    if(*epoch!=expected) {graphics_image_binding_stats.epoch_rejected++;goto fallback;}
    const dm_compute_buffer *a=&source->input_keys[i],*b=&r->input_keys[i];
    const dm_shader_binding *old=&source->binding_keys[i],*now=&r->binding_keys[i];
    if(source->shader!=r->shader || source->count!=r->count || source->dynamic_profile!=r->dynamic_profile ||
       old->binding!=now->binding || old->kind!=now->kind || old->slot!=now->slot || old->flags!=now->flags ||
       old->type!=now->type || old->sampler_slot!=now->sampler_slot ||
       a->binding!=b->binding || a->address!=b->address || a->size!=b->size || a->writable || a->zero!=b->zero ||
       !a->image || !b->image || source->images[i]!=r->images[i] ||
       !graphics_stage_image_equal(a->image,b->image))goto fallback;
    if(!graphics_image_binding_retain(r->images[i]->image,source->views[i],source->view_slots[i],
                                     source->samplers[i],source->sampler_slots[i]))goto fallback;
    r->views[i]=source->views[i];r->view_slots[i]=source->view_slots[i];
    r->samplers[i]=source->samplers[i];r->sampler_slots[i]=source->sampler_slots[i];
    graphics_image_binding_stats.reused++;return 1;
fallback:
    graphics_image_binding_stats.fallback++;return 0;
}
// Only ownerless CPU objects are retained. Stage-cache and FIFO references keep
// their original lifetimes; no Vulkan handle or uniform slice survives release.
// FIFO retirement releases objects in batches. A 128-object cache was smaller
// than one 512-draw batch and repeatedly discarded still-useful CPU storage.
// Bound retained bytes, allocate only on demand, and retain no Vulkan owners.
enum { GRAPHICS_RESOURCE_IDLE_BYTES=32u*1024u*1024u,
       GRAPHICS_RESOURCE_IDLE_LIMIT=GRAPHICS_RESOURCE_IDLE_BYTES/sizeof(graphics_resources) };
static unsigned graphics_resources_idle_peak;
static uint64_t graphics_resources_idle_overflow;
static graphics_resources *graphics_resources_idle;
static unsigned graphics_resources_idle_count,graphics_resources_live;
static graphics_resources *graphics_resources_acquire(void) {
    graphics_resources *r=graphics_resources_idle;
    if(r) {
        graphics_resources_idle=r->idle_next;r->idle_next=NULL;
        graphics_resources_idle_count--;gpu_metrics.resource_objects_reused++;
    } else {
        r=calloc(1,sizeof *r);if(!r)return NULL;
        gpu_metrics.resource_objects_allocated++;
    }
    r->refs=1;graphics_resources_live++;return r;
}
static void graphics_resources_idle_close(void) {
    if(graphics_resources_live)die("Vulkan graphics resources remained referenced at close");
    while(graphics_resources_idle) {
        graphics_resources *r=graphics_resources_idle;graphics_resources_idle=r->idle_next;free(r);
    }
    graphics_resources_idle_count=0;
    graphics_resources_idle_peak=0;graphics_resources_idle_overflow=0;
}
static void graphics_resources_reset(graphics_resources *r) {
    // Fresh objects are zeroed once. Every prior used slot is erased on return,
    // so the untouched tail stays zero even when the next shader has more slots.
    // Clear all payload categories, including ones unused by this shader type.
    for(unsigned i=0;i<r->count;i++) {
        memset(&r->payload.buffers[i],0,sizeof r->payload.buffers[i]);
        memset(&r->payload.images[i],0,sizeof r->payload.images[i]);
        r->payload.texels[i]=VK_NULL_HANDLE;
        r->guest[i]=NULL;r->offsets[i]=0;r->images[i]=NULL;
        r->views[i]=VK_NULL_HANDLE;r->samplers[i]=VK_NULL_HANDLE;r->buffer_views[i]=VK_NULL_HANDLE;
        r->view_slots[i]=r->sampler_slots[i]=r->texel_slots[i]=0;
        r->uniform_refs[i]=r->buffer_refs[i]=0;
        memset(&r->binding_keys[i],0,sizeof r->binding_keys[i]);
        memset(&r->input_keys[i],0,sizeof r->input_keys[i]);
        memset(&r->image_keys[i],0,sizeof r->image_keys[i]);
        // scratch_free already reset the entire scratch owner, even on failure.
    }
    for(unsigned i=0;i<r->dynamic_count;i++)r->dynamic_offsets[i]=0;
    memset(&r->shape,0,sizeof r->shape);
    r->static_owner=NULL;r->descriptor_bundle=NULL;r->descriptors=VK_NULL_HANDLE;r->set=VK_NULL_HANDLE;
    r->resident_storage=r->resident_uniform=r->resident_texel=r->dynamic_profile=0;r->dynamic_count=0;
    memset(r->uniform_vec4_counts,0,sizeof r->uniform_vec4_counts);
    r->refs=r->count=0;r->cacheable=0;r->shader=NULL;r->layout=VK_NULL_HANDLE;
}
static void graphics_slow_binding(const char *phase,const dm_shader_binding *binding,const dm_compute_buffer *buffer,
                                  gpu_image *image,unsigned reference,double elapsed) {
    static unsigned logged;
    if(elapsed<=50 || logged>=8)return;
    logged++;
    LOG("Slow binding %u/8: phase=%s ms=%.1f kind=%u slot=%d binding=%u bytes=%zu address=%#llx writable=%d zero=%d serial=%llu packaged=%d imageBytes=%llu layout=%u uniformRef=%u",
        logged,phase,elapsed,binding->kind,binding->slot,binding->binding,buffer->size,(unsigned long long)buffer->address,buffer->writable,buffer->zero,
        image?(unsigned long long)image->serial:0,image?image->packaged:0,image?(unsigned long long)image->storage.size:0,image?(unsigned)image->layout:0,reference);
}
static void graphics_resources_free(graphics_resources *r) {
    if(!r)return;
    if(!r->refs){LOG("Stage resource reference underflow");abort();}
    if(--r->refs)return;
    if(r->static_owner) {
        graphics_resources_free(r->static_owner);
        // These are borrowed too; normal owners clear them with scratch_free.
        memset(r->scratch,0,sizeof r->scratch);
    } else {
    if(r->descriptor_bundle)graphics_descriptor_bundle_release(r->descriptor_bundle);
    if(r->descriptors)graphics_descriptor_pool_release(r->descriptors,r->set);
    // count is published before any per-binding owner can be allocated, so
    // this also covers partially constructed objects on every failed new path.
    for(unsigned i=0;i<r->count;i++) {
        graphics_view_release(r->views[i],r->view_slots[i]);
        graphics_sampler_release(r->samplers[i],r->sampler_slots[i]);
        graphics_texel_release(r->buffer_views[i],r->texel_slots[i]);
        graphics_uniform_release(r->uniform_refs[i]);
        graphics_buffer_release(r->buffer_refs[i]);
        scratch_free(&r->scratch[i]);
    }
    }
    if(!graphics_resources_live)die("Vulkan graphics resource live count underflow");
    graphics_resources_live--;
    if(graphics_resources_idle_count==GRAPHICS_RESOURCE_IDLE_LIMIT){graphics_resources_idle_overflow++;free(r);return;}
    graphics_resources_reset(r);
    r->idle_next=graphics_resources_idle;graphics_resources_idle=r;graphics_resources_idle_count++;
    if(graphics_resources_idle_count>graphics_resources_idle_peak)graphics_resources_idle_peak=graphics_resources_idle_count;
}
static int graphics_resources_new(graphics_resources *r,const dm_graphics_shader *shader,const dm_compute_buffer *buffers,unsigned n,VkDescriptorSetLayout layout,uint64_t dynamic_profile,int cacheable,
                                  const graphics_resources *image_source,const uint64_t *epoch,uint64_t expected) {
    if(n>64)return -1;
    const dm_shader_binding *bindings=shader->bindings;
    r->shader=shader;r->layout=layout;r->count=n;r->cacheable=cacheable;
    r->dynamic_profile=dynamic_profile;r->shape=graphics_descriptor_shape_get(shader,r->dynamic_profile);
    if(image_source)graphics_image_binding_stats.source_stages++;
    for(unsigned i=0;i<16;i++)r->uniform_vec4_counts[i]=4096;
    memcpy(r->binding_keys,bindings,n*sizeof *bindings);
    memcpy(r->input_keys,buffers,n*sizeof *buffers);
    for(unsigned i=0;i<n;i++)if(buffers[i].image) {
        r->image_keys[i]=*buffers[i].image;r->input_keys[i].image=&r->image_keys[i];
    }
    double timing=gpu_detail_time_ms();
    compute_scratch *scratch=r->scratch;dm_vk_pool **guest=r->guest;size_t *offsets=r->offsets;
    gpu_image **images=r->images;VkImageView *views=r->views;VkSampler *samplers=r->samplers;VkBufferView *buffer_views=r->buffer_views;
    const VkPhysicalDeviceProperties props=v.properties;
    for(unsigned i=0;i<n;i++) {
        if(buffers[i].binding!=bindings[i].binding)return -1;
        if(buffers[i].image)continue;
        if(!buffers[i].size)return -1;
        if(buffers[i].address) {
            double started=gpu_detail_time_ms();int flush_result=images_flush_range(buffers[i].address,buffers[i].size);
            double elapsed=gpu_detail_time_ms()-started;gpu_metrics.uniform_flush_ms+=elapsed;
            graphics_slow_binding("uniform-range-flush",&bindings[i],&buffers[i],NULL,0,elapsed);if(flush_result)return -1;
            started=gpu_detail_time_ms();guest[i]=pool_address_locked(buffers[i].address,buffers[i].size,&offsets[i]);
            elapsed=gpu_detail_time_ms()-started;gpu_metrics.uniform_lookup_ms+=elapsed;
            graphics_slow_binding("uniform-pool-lookup",&bindings[i],&buffers[i],NULL,0,elapsed);if(!guest[i])return -1;
            for(unsigned j=0;j<i;j++)if(guest[j]==guest[i] && (buffers[i].writable || buffers[j].writable) && offsets[i]<offsets[j]+buffers[j].size && offsets[j]<offsets[i]+buffers[i].size)return -1;
        } else if(!buffers[i].host)return -1;
    }
    // Buffer range validation/pool lookup is part of uniform preparation too.
    gpu_metrics.uniform_prepare_ms+=gpu_detail_time_ms()-timing;
    VkDescriptorBufferInfo *info=r->payload.buffers;VkDescriptorImageInfo *image_info=r->payload.images;
    for(unsigned i=0;i<n;i++) {
        VkDescriptorType type=compute_binding_type(&bindings[i]);
        if(buffers[i].image) {
            timing=gpu_detail_time_ms();
            const dm_compute_image *d=buffers[i].image;
            if(type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
                // The FUSE transaction prepared and validated this exact view.
                // Never upload guest bytes over the preceding subpass result.
                VkImageView input_view=draw_fuse_input_view(d,&images[i]);
                if(!input_view || !images[i] || buffers[i].writable || !(images[i]->usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ||
                   d->levels!=1 || d->layers!=1 || images[i]->layout!=VK_IMAGE_LAYOUT_GENERAL)goto done;
                // Borrow the exact framebuffer view, whose cache is retired only
                // after every referencing draw. Do not put it in owned views[].
                image_info[i]=(VkDescriptorImageInfo){VK_NULL_HANDLE,input_view,VK_IMAGE_LAYOUT_GENERAL};
                continue;
            }
            if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                nvn_texlayout layout;nvn_tex_layout(&d->desc,&layout);
                if(d->address && images_flush_range(d->address,layout.size))goto done;
                guest[i]=pool_address_locked(d->address,layout.size,&offsets[i]);if(d->address && !guest[i])goto done;
                VkDescriptorBufferInfo texel={0};
                VkDeviceSize alignment=props.limits.minTexelBufferOffsetAlignment;
                int bw=0,bh=0,bpb=nvn_fmt_bpb(d->format,&bw,&bh);
                VkFormat format=image_format(d->format);
                int direct=!buffers[i].writable && guest[i] && guest[i]->mapped && guest[i]->buffer &&
                    format && layout.size && (!alignment || offsets[i]%alignment==0) &&
                    bpb>0 && bw==1 && bh==1 && offsets[i]%(unsigned)bpb==0 &&
                    layout.size%(unsigned)bpb==0 && layout.size/(unsigned)bpb<=props.limits.maxTexelBufferElements;
                if(direct) {
                    // Device format capabilities are immutable. Reuse the
                    // existing device cache instead of querying per TBO bind.
                    VkFormatProperties fp;
                    if((unsigned)format<256) {
                        if(!v.format_properties_valid[format]) {
                            vkGetPhysicalDeviceFormatProperties(v.gpu,format,&v.format_properties[format]);
                            v.format_properties_valid[format]=1;
                        }
                        fp=v.format_properties[format];
                    } else vkGetPhysicalDeviceFormatProperties(v.gpu,format,&fp);
                    VkFormatFeatureFlags needed=type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER?
                        VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT:VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
                    direct=!!(fp.bufferFeatures&needed);
                }
                int cached=0;
                if(direct) {
                    if(pool_gpu_read_consumer_locked(guest[i],offsets[i],layout.size,POOL_PREP_TEXEL))goto done;
                    texel=(VkDescriptorBufferInfo){guest[i]->buffer,offsets[i],layout.size};
                    // The descriptor payload uses texels[i]; retain the actual
                    // buffer identity/range separately for mutable cache hits.
                    info[i]=texel;r->resident_texel|=UINT64_C(1)<<i;
                    gpu_metrics.resident_texel_prepares++;cached=1;
                } else {
                    if(guest[i] && pool_cpu_read_locked(guest[i],offsets[i],layout.size))goto done;
                    cached=!buffers[i].writable && guest[i] && graphics_buffer_get(d->address,guest[i]->guest+offsets[i],layout.size,&texel,&r->buffer_refs[i]);
                }
                if(!cached) {
                    if(scratch_new(&scratch[i],layout.size))goto done;
                    if(guest[i])memcpy(scratch[i].mapped,guest[i]->guest+offsets[i],layout.size);
                    if(scratch_sync(&scratch[i],0))goto done;
                    texel=(VkDescriptorBufferInfo){scratch[i].buffer,0,layout.size};
                }
                gpu_metrics.texture_prepare_ms+=gpu_detail_time_ms()-timing;gpu_metrics.texture_prepares++;
                timing=gpu_detail_time_ms();
                VkBufferViewCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=texel.buffer,.format=image_format(d->format),.offset=texel.offset,.range=texel.range};
                if(direct) {
                    buffer_views[i]=graphics_cached_texel(&bc,&r->texel_slots[i]);
                    if(!buffer_views[i])goto done;
                } else if(vkCreateBufferView(v.device,&bc,NULL,&buffer_views[i]))goto done;
                r->payload.texels[i]=buffer_views[i];
                gpu_metrics.view_sampler_ms+=gpu_detail_time_ms()-timing;gpu_metrics.view_sampler_prepares++;

            } else {
                images[i]=image_get(d);double split=gpu_detail_time_ms(),elapsed=split-timing;gpu_metrics.tex_lookup_ms+=elapsed;
                graphics_slow_binding("texture-lookup",&bindings[i],&buffers[i],images[i],0,elapsed);
                if(!images[i] || (buffers[i].writable && !(images[i]->usage&VK_IMAGE_USAGE_STORAGE_BIT)))goto done;
                int transfer_result=image_transfer(images[i],0);elapsed=gpu_detail_time_ms()-split;gpu_metrics.tex_transfer_ms+=elapsed;
                graphics_slow_binding("texture-transfer",&bindings[i],&buffers[i],images[i],0,elapsed);if(transfer_result)goto done;
                gpu_metrics.texture_prepare_ms+=gpu_detail_time_ms()-timing;gpu_metrics.texture_prepares++;
                timing=gpu_detail_time_ms();
                // Like upstream tex_fast_rid/samp_fast: retain an unchanged
                // resolved identity. The image_get/transfer above still runs.
                if(!graphics_resources_reuse_image(r,i,image_source,epoch,expected)) {
                    views[i]=graphics_cached_view(images[i],d,&bindings[i],&r->view_slots[i]);if(!views[i])goto done;
                    if(type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER){samplers[i]=graphics_cached_sampler(d,&r->sampler_slots[i]);if(!samplers[i])goto done;}
                    gpu_metrics.view_sampler_prepares++;
                }
                gpu_metrics.view_sampler_ms+=gpu_detail_time_ms()-timing;
                image_info[i]=(VkDescriptorImageInfo){samplers[i],views[i],VK_IMAGE_LAYOUT_GENERAL};
            }
            continue;
        }
        timing=gpu_detail_time_ms();
        size_t size=buffers[i].size;
        VkDescriptorType descriptor_type=graphics_binding_type(&bindings[i],r->dynamic_profile,i);
        int uniform=descriptor_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || descriptor_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        VkDeviceSize storage_alignment=props.limits.minStorageBufferOffsetAlignment;
        VkDeviceSize uniform_alignment=props.limits.minUniformBufferOffsetAlignment;
        int ordinary_uniform=uniform && bindings[i].kind==DM_BIND_UNIFORM;
        int direct_uniform=ordinary_uniform && shader->guarded_uniform_abi==1 &&
           bindings[i].slot>=0 && bindings[i].slot<16 && !buffers[i].writable && !buffers[i].zero &&
           size>=16 && size<=65536 && !(size&15) && guest[i] && guest[i]->shared_heap &&
           guest[i]->mapped && guest[i]->buffer && offsets[i]<=UINT32_MAX &&
           (!uniform_alignment || offsets[i]%uniform_alignment==0);
        if(ordinary_uniform) {
            gpu_metrics.uniform_direct_candidates++;
            if(!direct_uniform)gpu_metrics.uniform_direct_fallbacks++;
        }
        if(uniform && !direct_uniform && size<65536)size=65536;
        if(size>(uniform?props.limits.maxUniformBufferRange:props.limits.maxStorageBufferRange))goto done;
        int direct_storage=type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && bindings[i].kind==DM_BIND_STORAGE &&
           !buffers[i].writable && !buffers[i].zero && guest[i] && guest[i]->mapped && guest[i]->buffer &&
           offsets[i]<=UINT32_MAX && (!storage_alignment || offsets[i]%storage_alignment==0);
        if(guest[i] && !direct_storage && !direct_uniform && pool_cpu_read_locked(guest[i],offsets[i],buffers[i].size))goto done;
        const void *bytes=guest[i]?guest[i]->guest+offsets[i]:buffers[i].host;
        if(!guest[i] && !buffers[i].zero && reports_range_drain((uintptr_t)bytes,buffers[i].size))goto done;
        // Every immutable cache owns the exact CPU version it uploads. Borrow
        // that shadow through its resource ref instead of allocating a duplicate
        // per-binding snapshot. Scratch fallback deliberately has no stage cache.
        double split=gpu_detail_time_ms();
        int cached=0;
        if(direct_uniform) {
            if(pool_gpu_read_consumer_locked(guest[i],offsets[i],buffers[i].size,POOL_PREP_UBO))goto done;
            info[i]=(VkDescriptorBufferInfo){guest[i]->buffer,offsets[i],buffers[i].size};
            r->resident_uniform|=UINT64_C(1)<<i;
            r->uniform_vec4_counts[bindings[i].slot]=(uint32_t)(buffers[i].size/16);
            gpu_metrics.uniform_direct_prepares++;gpu_metrics.uniform_direct_bytes+=buffers[i].size;cached=1;
        }
        // Do not materialize a potentially multi-GiB pool for a small SSBO.
        // Already-resident, aligned read-only ordinary SSBOs can share its GPU
        // buffer. Pool sync orders writes against pending/in-flight readers.
        if(direct_storage) {
            if(pool_gpu_read_consumer_locked(guest[i],offsets[i],buffers[i].size,POOL_PREP_SSBO))goto done;
            info[i]=(VkDescriptorBufferInfo){guest[i]->buffer,offsets[i],buffers[i].size};
            r->resident_storage|=UINT64_C(1)<<i;gpu_metrics.resident_storage_prepares++;cached=1;
        }
        if(!cached && type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && !buffers[i].writable && guest[i])
            cached=graphics_buffer_get(buffers[i].address,bytes,buffers[i].size,&info[i],&r->buffer_refs[i]);
        // A changing guest UBO must share a bank VkBuffer across draws, not
        // merely use a dynamic descriptor around a separate immutable buffer.
        // Driver banks instead try their existing static content cache first;
        // a changed pinned driver version falls back to the shared arena.
        if(!cached && descriptor_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && bindings[i].kind==DM_BIND_UNIFORM && !buffers[i].writable && !buffers[i].zero)
            cached=graphics_uniform_dynamic_get(bytes,buffers[i].size,&info[i],&r->uniform_refs[i],r->cacheable);
        if(!cached)cached=graphics_uniform_get(&bindings[i],&buffers[i],bytes,type,&info[i],&r->uniform_refs[i],r->cacheable);
        double elapsed=gpu_detail_time_ms()-split;gpu_metrics.uniform_cache_ms+=elapsed;
        graphics_slow_binding("uniform-cache",&bindings[i],&buffers[i],NULL,r->uniform_refs[i],elapsed);
        if(cached<0)goto done;
        if(cached){
            if(r->cacheable && !buffers[i].zero && !buffers[i].writable) {
                unsigned ref=r->uniform_refs[i];
                if(r->buffer_refs[i])gpu_metrics.snapshot_borrowed_buffer++;
                else if(ref>GRAPHICS_GUEST_REFERENCE_BASE)gpu_metrics.snapshot_borrowed_guest++;
                else if(ref && ref<=GRAPHICS_UNIFORM_STATIC_COUNT)gpu_metrics.snapshot_borrowed_static++;
            }
            if(((r->resident_storage|r->resident_uniform)&(UINT64_C(1)<<i)) || r->buffer_refs[i]){} // Direct buffers are not uniform arena hits.
            else if(r->uniform_refs[i]>GRAPHICS_GUEST_REFERENCE_BASE)gpu_metrics.uniform_guest_hits++;
            else if(r->uniform_refs[i]>GRAPHICS_UNIFORM_STATIC_COUNT)gpu_metrics.uniform_arena_hits++;
            else gpu_metrics.uniform_static_hits++;
            gpu_metrics.uniform_prepare_ms+=gpu_detail_time_ms()-timing;gpu_metrics.uniform_prepares++;continue;
        }
        gpu_metrics.uniform_fallbacks++;split=gpu_detail_time_ms();int scratch_result=scratch_new(&scratch[i],size);
        elapsed=gpu_detail_time_ms()-split;gpu_metrics.uniform_scratch_ms+=elapsed;
        graphics_slow_binding("uniform-scratch",&bindings[i],&buffers[i],NULL,0,elapsed);if(scratch_result)goto done;
        gpu_metrics.uniform_zero_bytes+=size;split=gpu_detail_time_ms();
        if(!buffers[i].zero){memcpy(scratch[i].mapped,bytes,buffers[i].size);gpu_metrics.uniform_copy_bytes+=buffers[i].size;}
        elapsed=gpu_detail_time_ms()-split;gpu_metrics.uniform_copy_ms+=elapsed;
        graphics_slow_binding("uniform-copy",&bindings[i],&buffers[i],NULL,0,elapsed);split=gpu_detail_time_ms();
        int sync_result=scratch_sync(&scratch[i],0);elapsed=gpu_detail_time_ms()-split;gpu_metrics.uniform_sync_ms+=elapsed;
        graphics_slow_binding("uniform-sync",&bindings[i],&buffers[i],NULL,0,elapsed);if(sync_result)goto done;
        info[i]=(VkDescriptorBufferInfo){scratch[i].buffer,0,size};
        gpu_metrics.uniform_prepare_ms+=gpu_detail_time_ms()-timing;gpu_metrics.uniform_prepares++;
    }
    // Vulkan consumes dynamic offsets in binding-number order, independent
    // of reflection order. Validate the exact slice before zeroing set offset.
    uint64_t remaining=r->dynamic_profile;
    while(remaining) {
        unsigned at=64;
        for(unsigned i=0;i<n;i++)if((remaining&(UINT64_C(1)<<i)) &&
            (at==64 || bindings[i].binding<bindings[at].binding))at=i;
        if(at==64 || r->dynamic_count==32)goto done;
        VkDescriptorBufferInfo *bi=&info[at];VkDeviceSize capacity=0;
        unsigned ref=r->uniform_refs[at];
        if((r->resident_storage|r->resident_uniform)&(UINT64_C(1)<<at))capacity=guest[at]->size;
        else if(r->buffer_refs[at])capacity=graphics_buffers[r->buffer_refs[at]-1].data.size;
        else if(ref>GRAPHICS_GUEST_REFERENCE_BASE)capacity=graphics_guest_uniforms[ref-GRAPHICS_GUEST_REFERENCE_BASE-1].data.size;
        else if(ref && ref<=GRAPHICS_UNIFORM_STATIC_COUNT)capacity=graphics_uniform_statics[ref-1].data.size;
        else if(ref)capacity=graphics_uniform_bank_storage((ref-GRAPHICS_UNIFORM_STATIC_COUNT-1)/GRAPHICS_UNIFORM_BANK_SLOTS)->size;
        else capacity=scratch[at].size;
        int storage=r->shape.bindings[at].type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        VkDeviceSize alignment=storage?props.limits.minStorageBufferOffsetAlignment:props.limits.minUniformBufferOffsetAlignment;
        VkDeviceSize limit=storage?props.limits.maxStorageBufferRange:props.limits.maxUniformBufferRange;
        if(!bi->buffer || bi->offset>UINT32_MAX || (alignment && bi->offset%alignment) ||
           bi->range==VK_WHOLE_SIZE || bi->range>limit ||
           bi->offset>capacity || bi->range>capacity-bi->offset)goto done;
        r->dynamic_offsets[r->dynamic_count++]=(uint32_t)bi->offset;bi->offset=0;
        remaining&=~(UINT64_C(1)<<at);
        if(storage)gpu_metrics.dynamic_storage_bindings++;else gpu_metrics.dynamic_uniform_bindings++;
    }
    return 0;
done:return -1;
}
// Resolve contents for every draw; only descriptor identities persist across
// passes. New sets are fully updated BEFORE recording their first bind.
static int graphics_resources_write_set(graphics_resources *r) {
    uint64_t hash=graphics_descriptor_hash(&r->shape,&r->payload);
    graphics_descriptor_bundle *bundle=graphics_descriptor_bundle_find(&r->shape,&r->payload,hash);
    if(bundle) {
        r->descriptor_bundle=bundle;r->set=bundle->set;gpu_metrics.resolved_set_hits++;return 0;
    }
    gpu_metrics.resolved_set_misses++;
    bundle=graphics_descriptor_bundle_reserve();
    VkDescriptorUpdateTemplate update_template=VK_NULL_HANDLE;
    double timing=gpu_detail_time_ms();
    if(graphics_descriptor_pool_acquire(&r->shape,r->layout,&r->descriptors,&r->set,&update_template)) {
        if(bundle)graphics_descriptor_bundle_drop(bundle);
        return -1;
    }
    gpu_metrics.descriptor_sets++;
    if(update_template) {
        vkUpdateDescriptorSetWithTemplate(v.device,r->set,update_template,&r->payload);
        gpu_metrics.descriptor_template_updates++;
    } else {
        VkWriteDescriptorSet writes[64];
        for(unsigned i=0;i<r->count;i++) {
            VkDescriptorType type=(VkDescriptorType)r->shape.bindings[i].type;
            writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=r->set,
                .dstBinding=r->shape.bindings[i].binding,.descriptorCount=1,.descriptorType=type};
            if(type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                writes[i].pBufferInfo=&r->payload.buffers[i];
            else if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                writes[i].pTexelBufferView=&r->payload.texels[i];
            else writes[i].pImageInfo=&r->payload.images[i];
        }
        vkUpdateDescriptorSets(v.device,r->count,writes,0,NULL);
        gpu_metrics.descriptor_write_updates++;
    }
    gpu_metrics.descriptor_api_ms+=gpu_detail_time_ms()-timing;
    if(bundle) {
        unsigned bucket=(unsigned)hash&(GRAPHICS_DESCRIPTOR_BUNDLE_BUCKETS-1);
        *bundle=(graphics_descriptor_bundle){.shape=r->shape,.payload=r->payload,
            .pool=r->descriptors,.set=r->set,.hash=hash,.refs=1,.bucket=bucket};
        graphics_descriptor_bundle_publish(bundle);
        r->descriptor_bundle=bundle;r->descriptors=VK_NULL_HANDLE;
    }
    return 0;
}
static int graphics_resources_readback(graphics_resources *r,const dm_compute_buffer *buffers,unsigned n) {
    for(unsigned i=0;i<n;i++)if(buffers[i].writable) {
        if(r->images[i]){if(image_gpu_written(r->images[i]))return -1;continue;}
        if(!r->guest[i])continue;
        if(scratch_sync(&r->scratch[i],1))return -1;
        size_t size=buffers[i].image?r->scratch[i].size:buffers[i].size;
        dm_guest_write_prep(r->guest[i]->guest+r->offsets[i],size);
        memcpy(r->guest[i]->guest+r->offsets[i],r->scratch[i].mapped,size);
        if(pool_sync_locked(r->guest[i],r->offsets[i],size,0))return -1;
    }
    return 0;
}
