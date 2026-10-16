static void graphics_destroy_texel_view(VkBufferView handle) {
    if(!handle)return;
    graphics_descriptor_retire_object(GRAPHICS_DESCRIPTOR_TEXEL_VIEW,(uint64_t)(uintptr_t)handle);
    vkDestroyBufferView(v.device,handle,NULL);
}
static void graphics_destroy_image_view(VkImageView handle) {
    if(!handle)return;
    graphics_descriptor_retire_object(GRAPHICS_DESCRIPTOR_IMAGE_VIEW,(uint64_t)(uintptr_t)handle);
    vkDestroyImageView(v.device,handle,NULL);
}
static void graphics_destroy_sampler(VkSampler handle) {
    if(!handle)return;
    graphics_descriptor_retire_object(GRAPHICS_DESCRIPTOR_SAMPLER,(uint64_t)(uintptr_t)handle);
    vkDestroySampler(v.device,handle,NULL);
}
/* The Vulkan lock protects these shared caches. Graphics and compute leases
 * stay pinned until their respective submission really completes. */
#define GRAPHICS_VIEW_CACHE_COUNT 512
#define GRAPHICS_SAMPLER_CACHE_COUNT 128
typedef struct { VkImageViewCreateInfo key;VkImageView handle;unsigned refs,next,bucket;uint64_t used; } graphics_view_entry;
typedef struct { VkSamplerCreateInfo key;VkSampler handle;unsigned refs,next,bucket;uint64_t used; } graphics_sampler_entry;
static graphics_view_entry graphics_view_cache[GRAPHICS_VIEW_CACHE_COUNT];
static graphics_sampler_entry graphics_sampler_cache[GRAPHICS_SAMPLER_CACHE_COUNT];
static unsigned graphics_view_buckets[1024],graphics_sampler_buckets[256];
static unsigned graphics_last_view,graphics_last_sampler; // Slot + 1, zero = none.
static uint64_t graphics_image_cache_clock;
static void graphics_view_release(VkImageView handle,unsigned slot) {
    if(!slot){graphics_destroy_image_view(handle);return;}
    if(slot>GRAPHICS_VIEW_CACHE_COUNT || graphics_view_cache[slot-1].handle!=handle ||
       !graphics_view_cache[slot-1].refs)die("Vulkan shared image view lease lost");
    graphics_view_cache[slot-1].refs--;
}
static void graphics_sampler_release(VkSampler handle,unsigned slot) {
    if(!slot){graphics_destroy_sampler(handle);return;}
    if(slot>GRAPHICS_SAMPLER_CACHE_COUNT || graphics_sampler_cache[slot-1].handle!=handle ||
       !graphics_sampler_cache[slot-1].refs)die("Vulkan shared sampler lease lost");
    graphics_sampler_cache[slot-1].refs--;
}
// Transfer no ownership: acquire two independent leases for another draw.
// Validate BOTH slots before incrementing either, so fallback is transactional.
// Uncached handles (slot zero) remain exclusive to their original owner.
static int graphics_image_binding_retain(VkImage image,VkImageView view,unsigned view_slot,
                                         VkSampler sampler,unsigned sampler_slot) {
    if(!view || !sampler || !view_slot || view_slot>GRAPHICS_VIEW_CACHE_COUNT ||
       !sampler_slot || sampler_slot>GRAPHICS_SAMPLER_CACHE_COUNT)return 0;
    graphics_view_entry *ve=&graphics_view_cache[view_slot-1];
    graphics_sampler_entry *se=&graphics_sampler_cache[sampler_slot-1];
    if(ve->handle!=view || ve->key.image!=image || !ve->refs || ve->refs==~0u ||
       se->handle!=sampler || !se->refs || se->refs==~0u)return 0;
    ve->refs++;se->refs++;
    ve->used=++graphics_image_cache_clock;se->used=++graphics_image_cache_clock;
    return 1;
}
static int graphics_sampler_limits_valid;
static VkPhysicalDeviceFeatures graphics_sampler_features;
static VkPhysicalDeviceProperties graphics_sampler_properties;
// Pool-backed texel views describe resource identity, never its mutable data.
// Temporary/snapshot buffers keep uncached views with their original owners.
#define GRAPHICS_TEXEL_CACHE_COUNT 512
typedef struct {
    VkBuffer buffer;VkDeviceSize offset,range;VkFormat format;VkBufferView handle;
    unsigned refs,next,bucket;uint64_t used;
} graphics_texel_entry;
static graphics_texel_entry graphics_texel_cache[GRAPHICS_TEXEL_CACHE_COUNT];
static unsigned graphics_texel_buckets[1024],graphics_last_texel;
static uint64_t graphics_texel_clock;
static void graphics_texel_release(VkBufferView handle,unsigned slot) {
    if(!slot){graphics_destroy_texel_view(handle);return;}
    if(slot>GRAPHICS_TEXEL_CACHE_COUNT || graphics_texel_cache[slot-1].handle!=handle ||
       !graphics_texel_cache[slot-1].refs)die("Vulkan shared texel view lease lost");
    graphics_texel_cache[slot-1].refs--;
}
static void graphics_texel_unlink(unsigned slot) {
    graphics_texel_entry *e=&graphics_texel_cache[slot-1];
    unsigned *link=&graphics_texel_buckets[e->bucket];
    while(*link && *link!=slot)link=&graphics_texel_cache[*link-1].next;
    if(*link)*link=e->next;
    if(graphics_last_texel==slot)graphics_last_texel=0;
}
static int graphics_texel_equal(const graphics_texel_entry *e,const VkBufferViewCreateInfo *ci) {
    return e->handle && e->buffer==ci->buffer && e->offset==ci->offset && e->range==ci->range && e->format==ci->format;
}
static VkBufferView graphics_cached_texel(const VkBufferViewCreateInfo *ci,unsigned *slot) {
    *slot=0;
    uint64_t h=(uint64_t)(uintptr_t)ci->buffer;
    h^=ci->offset*UINT64_C(0x9e3779b97f4a7c15);h^=ci->range*UINT64_C(0xbf58476d1ce4e5b9);
    h^=(uint64_t)ci->format;h^=h>>32;unsigned bucket=(unsigned)h&1023;
    unsigned hit=graphics_last_texel;
    if(!hit || !graphics_texel_equal(&graphics_texel_cache[hit-1],ci)) {
        for(hit=graphics_texel_buckets[bucket];hit;hit=graphics_texel_cache[hit-1].next)
            if(graphics_texel_equal(&graphics_texel_cache[hit-1],ci))break;
    }
    if(hit) {
        gpu_metrics.texel_view_cache_hits++;
        graphics_texel_entry *e=&graphics_texel_cache[hit-1];e->refs++;e->used=++graphics_texel_clock;
        *slot=graphics_last_texel=hit;return e->handle;
    }
    unsigned candidate=GRAPHICS_TEXEL_CACHE_COUNT;uint64_t oldest=UINT64_MAX;
    for(unsigned i=0;i<GRAPHICS_TEXEL_CACHE_COUNT;i++) {
        graphics_texel_entry *e=&graphics_texel_cache[i];
        if(!e->handle){candidate=i;break;}
        if(!e->refs && e->used<oldest){candidate=i;oldest=e->used;}
    }
    VkBufferView handle=VK_NULL_HANDLE;
    if(vkCreateBufferView(v.device,ci,NULL,&handle))return VK_NULL_HANDLE;
    gpu_metrics.texel_view_cache_creates++;
    if(candidate<GRAPHICS_TEXEL_CACHE_COUNT) {
        graphics_texel_entry *e=&graphics_texel_cache[candidate];
        if(e->handle){graphics_texel_unlink(candidate+1);graphics_destroy_texel_view(e->handle);}
        *e=(graphics_texel_entry){.buffer=ci->buffer,.offset=ci->offset,.range=ci->range,.format=ci->format,
            .handle=handle,.refs=1,.used=++graphics_texel_clock,.bucket=bucket,.next=graphics_texel_buckets[bucket]};
        graphics_texel_buckets[bucket]=candidate+1;*slot=graphics_last_texel=candidate+1;
    }
    // Full, pinned cache: the caller owns this temporary view (slot remains 0).
    return handle;
}
static void graphics_texel_cache_retire(VkBuffer buffer) {
    for(unsigned i=0;i<GRAPHICS_TEXEL_CACHE_COUNT;i++) {
        graphics_texel_entry *e=&graphics_texel_cache[i];
        if(!e->handle || (buffer && e->buffer!=buffer))continue;
        if(e->refs){LOG("Vulkan texel buffer retired with active draw references");abort();}
        graphics_texel_unlink(i+1);graphics_destroy_texel_view(e->handle);memset(e,0,sizeof *e);
    }
}
// Layout shape is copied by value: no catalog or pipeline-layout pointers
// survive their owner. All graphics bindings have count=1, flags=0, no pNext
// and no immutable sampler; stage and binding/type fully define the layout.
// Keep enough layout shapes to avoid repeatedly evicting character/world
// shaders. Even at 64 bindings/set, 128*32*64 descriptors reserve less than
// the previous generic pools' 32*32*(6*64) worst-case descriptor capacity.
// 16384 sets leave headroom above the 8192 cached bundles for pinned draws,
// transient leases and partially occupied arenas of different shapes.
enum { GRAPHICS_DESCRIPTOR_ARENAS=512, GRAPHICS_DESCRIPTOR_SETS=32 };
typedef struct {unsigned count,stage;struct {uint32_t binding,type;} bindings[64];} graphics_descriptor_shape;
// Profiles belong to a shader PAIR: each stage first receives half the device
// budget, then lends unused capacity to its partner. UBO and SSBO budgets are
// independent; their combined per-stage count is bounded by the offset array.
static int graphics_dynamic_next(const dm_graphics_shader *shader,unsigned storage,uint64_t mask) {
    int best=-1,best_priority=4;
    for(unsigned i=0;i<shader->nbindings && i<64;i++) {
        const dm_shader_binding *b=&shader->bindings[i];int priority;
        if(mask&(UINT64_C(1)<<i))continue;
        if(storage) {
            if(b->kind!=DM_BIND_STORAGE || (b->flags&1) || compute_binding_type(b)!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)continue;
            priority=0;
        } else {
            if(compute_binding_type(b)!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)continue;
            if(b->kind==DM_BIND_DRIVER && b->slot==0)priority=0;
            else if(b->kind==DM_BIND_UNIFORM)priority=1;
            else if(b->kind==DM_BIND_DRIVER && b->slot==2)priority=2;
            else continue;
        }
        if(best<0 || priority<best_priority || (priority==best_priority && b->binding<shader->bindings[best].binding)) {
            best=(int)i;best_priority=priority;
        }
    }
    return best;
}
static void graphics_dynamic_profiles(const dm_graphics_shader *const shaders[2],uint64_t profiles[2]) {
    profiles[0]=profiles[1]=0;unsigned total[2]={0};
    for(unsigned storage=0;storage<2;storage++) {
        unsigned budget=storage?v.properties.limits.maxDescriptorSetStorageBuffersDynamic:
            v.properties.limits.maxDescriptorSetUniformBuffersDynamic;
        unsigned used=0;
        for(unsigned stage=0;stage<2;stage++)for(unsigned n=0;n<budget/2 && total[stage]<32;n++) {
            int at=graphics_dynamic_next(shaders[stage],storage,profiles[stage]);if(at<0)break;
            profiles[stage]|=UINT64_C(1)<<at;total[stage]++;used++;
        }
        while(used<budget) {
            int progress=0;
            for(unsigned stage=0;stage<2 && used<budget;stage++) {
                if(total[stage]==32)continue;
                int at=graphics_dynamic_next(shaders[stage],storage,profiles[stage]);if(at<0)continue;
                profiles[stage]|=UINT64_C(1)<<at;total[stage]++;used++;progress=1;
            }
            if(!progress)break;
        }
    }
}
static VkDescriptorType graphics_binding_type(const dm_shader_binding *binding,uint64_t profile,unsigned i) {
    if(profile&(UINT64_C(1)<<i))return binding->kind==DM_BIND_STORAGE?
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    return compute_binding_type(binding);
}
static graphics_descriptor_shape graphics_descriptor_shape_get(const dm_graphics_shader *shader,uint64_t profile) {
    graphics_descriptor_shape shape={.count=shader->nbindings,.stage=shader->stage};
    for(unsigned i=0;i<shape.count && i<64;i++) {
        shape.bindings[i].binding=shader->bindings[i].binding;
        shape.bindings[i].type=graphics_binding_type(&shader->bindings[i],profile,i);
    }
    return shape;
}
typedef struct {
    VkDescriptorBufferInfo buffers[64];VkDescriptorImageInfo images[64];VkBufferView texels[64];
} graphics_descriptor_payload;
typedef struct {
    VkDescriptorPool pool;VkDescriptorSet sets[GRAPHICS_DESCRIPTOR_SETS];
    VkDescriptorUpdateTemplate update_template;
    graphics_descriptor_shape shape;uint32_t live;uint64_t used;
} graphics_descriptor_arena;
static graphics_descriptor_arena graphics_descriptor_pools[GRAPHICS_DESCRIPTOR_ARENAS];
static unsigned graphics_descriptor_fallback_live;
static uint64_t graphics_descriptor_clock;
static VkDescriptorUpdateTemplate graphics_descriptor_template(const graphics_descriptor_shape *shape,VkDescriptorSetLayout layout) {
    if(!shape->count)return VK_NULL_HANDLE;
    VkDescriptorUpdateTemplateEntry entries[64];
    for(unsigned i=0;i<shape->count;i++) {
        VkDescriptorType type=(VkDescriptorType)shape->bindings[i].type;size_t offset,stride;
        if(type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            stride=sizeof(VkDescriptorBufferInfo);offset=offsetof(graphics_descriptor_payload,buffers)+i*stride;
        } else if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            stride=sizeof(VkBufferView);offset=offsetof(graphics_descriptor_payload,texels)+i*stride;
        } else {
            stride=sizeof(VkDescriptorImageInfo);offset=offsetof(graphics_descriptor_payload,images)+i*stride;
        }
        entries[i]=(VkDescriptorUpdateTemplateEntry){.dstBinding=shape->bindings[i].binding,.descriptorCount=1,
            .descriptorType=type,.offset=offset,.stride=stride};
    }
    VkDescriptorUpdateTemplateCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount=shape->count,.pDescriptorUpdateEntries=entries,
        .templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,.descriptorSetLayout=layout};
    VkDescriptorUpdateTemplate result=VK_NULL_HANDLE;
    if(vkCreateDescriptorUpdateTemplate(v.device,&ci,NULL,&result))return VK_NULL_HANDLE;
    return result;
}
static int graphics_descriptor_create(const graphics_descriptor_shape *shape,VkDescriptorSetLayout layout,
                                      unsigned count,VkDescriptorPool *pool,VkDescriptorSet *sets) {
    VkDescriptorPoolSize sizes[12];unsigned ns=0;
    for(unsigned i=0;i<shape->count;i++) {
        unsigned j=0;while(j<ns && sizes[j].type!=(VkDescriptorType)shape->bindings[i].type)j++;
        if(j==ns){if(ns==12)return -1;sizes[ns++]=(VkDescriptorPoolSize){(VkDescriptorType)shape->bindings[i].type,0};}
        sizes[j].descriptorCount+=count;
    }
    VkDescriptorPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=count,.poolSizeCount=ns,.pPoolSizes=ns?sizes:NULL};
    VkDescriptorPool created=VK_NULL_HANDLE;
    if(vkCreateDescriptorPool(v.device,&ci,NULL,&created))return -1;
    VkDescriptorSetLayout layouts[GRAPHICS_DESCRIPTOR_SETS];
    for(unsigned i=0;i<count;i++)layouts[i]=layout;
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=created,.descriptorSetCount=count,.pSetLayouts=layouts};
    if(vkAllocateDescriptorSets(v.device,&ai,sets)){vkDestroyDescriptorPool(v.device,created,NULL);return -1;}
    gpu_metrics.descriptor_set_alloc+=count;
    *pool=created;return 0;
}
static int graphics_descriptor_bundles_trim_arena(void);
static int graphics_descriptor_pool_acquire(const graphics_descriptor_shape *wanted,VkDescriptorSetLayout layout,
                                            VkDescriptorPool *pool,VkDescriptorSet *set,VkDescriptorUpdateTemplate *update_template) {
    *pool=VK_NULL_HANDLE;*set=VK_NULL_HANDLE;*update_template=VK_NULL_HANDLE;
    if(wanted->count>64 || wanted->stage>1)return -1;
    graphics_descriptor_shape shape=*wanted;
    int candidate=-1;
    for(unsigned i=0;i<GRAPHICS_DESCRIPTOR_ARENAS;i++) {
        graphics_descriptor_arena *a=&graphics_descriptor_pools[i];
        if(a->pool && a->live!=UINT32_MAX && !memcmp(&a->shape,&shape,sizeof shape)) {
            unsigned slot=(unsigned)__builtin_ctz(~a->live);
            a->live|=UINT32_C(1)<<slot;a->used=++graphics_descriptor_clock;
            gpu_metrics.descriptor_set_reuse++;
            *pool=a->pool;*set=a->sets[slot];*update_template=a->update_template;return 0;
        }
        if(!a->live && (candidate<0 || a->used<graphics_descriptor_pools[candidate].used))candidate=(int)i;
    }
    if(candidate>=0) {
        graphics_descriptor_arena *a=&graphics_descriptor_pools[candidate];
        // Only an entirely idle arena can change shape. Individual idle slots
        // otherwise retain their set handles; no pool reset invalidates peers.
        if(a->update_template)vkDestroyDescriptorUpdateTemplate(v.device,a->update_template,NULL);
        if(a->pool)vkDestroyDescriptorPool(v.device,a->pool,NULL);
        memset(a,0,sizeof *a);
        if(!graphics_descriptor_create(&shape,layout,GRAPHICS_DESCRIPTOR_SETS,&a->pool,a->sets)) {
            a->shape=shape;a->live=1;a->used=++graphics_descriptor_clock;
            // Template creation is optional; a driver/allocation failure keeps
            // the already allocated arena usable through ordinary writes.
            a->update_template=graphics_descriptor_template(&shape,layout);
            *pool=a->pool;*set=a->sets[0];*update_template=a->update_template;return 0;
        }
        memset(a,0,sizeof *a);
    }
    if(candidate<0 && graphics_descriptor_bundles_trim_arena())
        return graphics_descriptor_pool_acquire(wanted,layout,pool,set,update_template);
    // All arenas pinned, or batch allocation failed: transient one-set pool.
    VkDescriptorPool fallback=VK_NULL_HANDLE;VkDescriptorSet fallback_set=VK_NULL_HANDLE;
    if(graphics_descriptor_create(&shape,layout,1,&fallback,&fallback_set))return -1;
    *pool=fallback;*set=fallback_set;graphics_descriptor_fallback_live++;return 0;
}
static void graphics_descriptor_pool_release(VkDescriptorPool pool,VkDescriptorSet set) {
    for(unsigned i=0;i<GRAPHICS_DESCRIPTOR_ARENAS;i++) {
        graphics_descriptor_arena *a=&graphics_descriptor_pools[i];
        if(a->pool!=pool)continue;
        unsigned slot=0;while(slot<GRAPHICS_DESCRIPTOR_SETS && a->sets[slot]!=set)slot++;
        if(slot==GRAPHICS_DESCRIPTOR_SETS || !(a->live&(UINT32_C(1)<<slot))) {
            LOG("Descriptor set lease underflow or unknown slot");abort();
        }
        a->live&=~(UINT32_C(1)<<slot);return;
    }
    if(!graphics_descriptor_fallback_live){LOG("Descriptor fallback reference underflow");abort();}
    graphics_descriptor_fallback_live--;vkDestroyDescriptorPool(v.device,pool,NULL);
}
#include "vk_graphics_descriptor_bundles.inc"
static int graphics_view_key_equal(const VkImageViewCreateInfo *a,const VkImageViewCreateInfo *b) {
    return a->sType==b->sType && a->pNext==b->pNext && a->flags==b->flags && a->image==b->image &&
        a->viewType==b->viewType && a->format==b->format &&
        a->components.r==b->components.r && a->components.g==b->components.g &&
        a->components.b==b->components.b && a->components.a==b->components.a &&
        a->subresourceRange.aspectMask==b->subresourceRange.aspectMask &&
        a->subresourceRange.baseMipLevel==b->subresourceRange.baseMipLevel &&
        a->subresourceRange.levelCount==b->subresourceRange.levelCount &&
        a->subresourceRange.baseArrayLayer==b->subresourceRange.baseArrayLayer &&
        a->subresourceRange.layerCount==b->subresourceRange.layerCount;
}
static int graphics_sampler_key_equal(const VkSamplerCreateInfo *a,const VkSamplerCreateInfo *b) {
    return a->sType==b->sType && a->pNext==b->pNext && a->flags==b->flags &&
        a->magFilter==b->magFilter && a->minFilter==b->minFilter && a->mipmapMode==b->mipmapMode &&
        a->addressModeU==b->addressModeU && a->addressModeV==b->addressModeV && a->addressModeW==b->addressModeW &&
        a->mipLodBias==b->mipLodBias && a->anisotropyEnable==b->anisotropyEnable && a->maxAnisotropy==b->maxAnisotropy &&
        a->compareEnable==b->compareEnable && a->compareOp==b->compareOp && a->minLod==b->minLod && a->maxLod==b->maxLod &&
        a->borderColor==b->borderColor && a->unnormalizedCoordinates==b->unnormalizedCoordinates;
}
static uint64_t graphics_key_mix(uint64_t h,uint64_t value) {
    return (h ^ (value + UINT64_C(0x9e3779b97f4a7c15) + (h<<6) + (h>>2)))*UINT64_C(0x100000001b3);
}
static unsigned graphics_view_bucket(const VkImageViewCreateInfo *key) {
    uint64_t h=graphics_key_mix(0,(uint64_t)(uintptr_t)key->image);
    h=graphics_key_mix(h,key->viewType);h=graphics_key_mix(h,key->format);
    h=graphics_key_mix(h,key->components.r);h=graphics_key_mix(h,key->components.g);
    h=graphics_key_mix(h,key->components.b);h=graphics_key_mix(h,key->components.a);
    h=graphics_key_mix(h,key->subresourceRange.aspectMask);
    h=graphics_key_mix(h,key->subresourceRange.baseMipLevel);h=graphics_key_mix(h,key->subresourceRange.levelCount);
    h=graphics_key_mix(h,key->subresourceRange.baseArrayLayer);h=graphics_key_mix(h,key->subresourceRange.layerCount);
    h=graphics_key_mix(h,key->flags);return (unsigned)(h^(h>>32))&1023u;
}
static uint32_t graphics_float_key(float value) {
    // Equality treats +0 and -0 alike, so their hash must also agree.
    uint32_t bits=0;if(value!=0)memcpy(&bits,&value,sizeof bits);return bits;
}
static unsigned graphics_sampler_bucket(const VkSamplerCreateInfo *key) {
    uint64_t h=graphics_key_mix(0,key->flags);
    h=graphics_key_mix(h,key->magFilter);h=graphics_key_mix(h,key->minFilter);h=graphics_key_mix(h,key->mipmapMode);
    h=graphics_key_mix(h,key->addressModeU);h=graphics_key_mix(h,key->addressModeV);h=graphics_key_mix(h,key->addressModeW);
    h=graphics_key_mix(h,graphics_float_key(key->mipLodBias));h=graphics_key_mix(h,key->anisotropyEnable);
    h=graphics_key_mix(h,graphics_float_key(key->maxAnisotropy));h=graphics_key_mix(h,key->compareEnable);h=graphics_key_mix(h,key->compareOp);
    h=graphics_key_mix(h,graphics_float_key(key->minLod));h=graphics_key_mix(h,graphics_float_key(key->maxLod));
    h=graphics_key_mix(h,key->borderColor);h=graphics_key_mix(h,key->unnormalizedCoordinates);
    return (unsigned)(h^(h>>32))&255u;
}
static void graphics_view_unlink(unsigned slot) {
    graphics_view_entry *e=&graphics_view_cache[slot-1];unsigned *link=&graphics_view_buckets[e->bucket];
    while(*link && *link!=slot)link=&graphics_view_cache[*link-1].next;
    if(*link)*link=e->next;
    if(graphics_last_view==slot)graphics_last_view=0;
}
static void graphics_sampler_unlink(unsigned slot) {
    graphics_sampler_entry *e=&graphics_sampler_cache[slot-1];unsigned *link=&graphics_sampler_buckets[e->bucket];
    while(*link && *link!=slot)link=&graphics_sampler_cache[*link-1].next;
    if(*link)*link=e->next;
    if(graphics_last_sampler==slot)graphics_last_sampler=0;
}
// Share the exact descriptor view range with attachment-alias decisions.
// A failed range is never evidence that two resources are disjoint.
static int graphics_image_view_range(gpu_image *p,const dm_compute_image *d,const dm_shader_binding *b,
                                     VkImageViewType *view_type,VkImageSubresourceRange *range) {
    uint32_t kind=b->type&255;int array=(b->type&256)!=0;
    VkImageViewType type=kind==1?(array?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D):kind==4?VK_IMAGE_VIEW_TYPE_3D:
        kind==5?(array?VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:VK_IMAGE_VIEW_TYPE_CUBE):(array?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D);
    unsigned layers=type==VK_IMAGE_VIEW_TYPE_3D?1:array?d->layers:kind==5?6:1;
    unsigned levels=b->kind==DM_BIND_IMAGE?1:d->levels;
    if(d->base_level<0 || d->base_layer<0 || !levels || !layers ||
       (unsigned)d->base_level>=p->desc.levels || levels>p->desc.levels-(unsigned)d->base_level ||
       (unsigned)d->base_layer>=p->layers || layers>p->layers-(unsigned)d->base_layer)return -1;
    *view_type=type;
    *range=(VkImageSubresourceRange){p->aspect&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:p->aspect,d->base_level,levels,d->base_layer,layers};
    return 0;
}
static VkImageView graphics_cached_view(gpu_image *p,const dm_compute_image *d,const dm_shader_binding *b,unsigned *slot) {
    *slot=0;
    VkImageViewType type;VkImageSubresourceRange range;
    if(graphics_image_view_range(p,d,b,&type,&range))return VK_NULL_HANDLE;
    VkImageViewCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=p->image,.viewType=type,.format=image_format(d->format),
        .subresourceRange=range};
    if((d->format>=0x69 && d->format<=0x78) || d->format==0x37 || d->format==0x99 || d->format==0x9b)ci.components.a=VK_COMPONENT_SWIZZLE_ONE;
    graphics_view_entry *hit=graphics_last_view?&graphics_view_cache[graphics_last_view-1]:NULL;
    if(hit && graphics_view_key_equal(&hit->key,&ci)) {
        gpu_metrics.image_view_cache_hits++;
        hit->refs++;hit->used=++graphics_image_cache_clock;*slot=graphics_last_view;return hit->handle;
    }
    unsigned bucket=graphics_view_bucket(&ci);
    for(unsigned at=graphics_view_buckets[bucket];at;at=graphics_view_cache[at-1].next) {
        hit=&graphics_view_cache[at-1];
        if(graphics_view_key_equal(&hit->key,&ci)) {
            gpu_metrics.image_view_cache_hits++;
            hit->refs++;hit->used=++graphics_image_cache_clock;*slot=graphics_last_view=at;return hit->handle;
        }
    }
    // Only misses search for an unreferenced LRU victim; hits never scan capacity.
    int candidate=-1;
    for(unsigned i=0;i<GRAPHICS_VIEW_CACHE_COUNT;i++) {
        graphics_view_entry *e=&graphics_view_cache[i];
        if(!e->refs && (candidate<0 || e->used<graphics_view_cache[candidate].used))candidate=(int)i;
    }
    VkImageView handle=VK_NULL_HANDLE;
    if(vkCreateImageView(v.device,&ci,NULL,&handle))return VK_NULL_HANDLE;
    gpu_metrics.image_view_cache_creates++;
    if(candidate>=0) {
        graphics_view_entry *e=&graphics_view_cache[candidate];
        if(e->handle){graphics_view_unlink(candidate+1);graphics_destroy_image_view(e->handle);}
        *e=(graphics_view_entry){.key=ci,.handle=handle,.refs=1,.used=++graphics_image_cache_clock,
            .bucket=bucket,.next=graphics_view_buckets[bucket]};
        graphics_view_buckets[bucket]=candidate+1;*slot=graphics_last_view=candidate+1;
    }
    return handle; // All entries pinned: this draw owns the uncached handle.
}
static VkSampler graphics_cached_sampler(const dm_compute_image *d,unsigned *slot) {
    *slot=0;
    if(!v.mirror_clamp && (d->ws==2 || d->ws==3 || d->wt==2 || d->wt==3 || d->wr==2 || d->wr==3))return VK_NULL_HANDLE;
    if(!graphics_sampler_limits_valid) {
        vkGetPhysicalDeviceFeatures(v.gpu,&graphics_sampler_features);vkGetPhysicalDeviceProperties(v.gpu,&graphics_sampler_properties);
        graphics_sampler_limits_valid=1;
    }
    VkBorderColor border=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if(d->border[0]==1 && d->border[1]==1 && d->border[2]==1 && d->border[3]==1)border=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if(d->border[0]==0 && d->border[1]==0 && d->border[2]==0 && d->border[3]==1)border=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=d->magf?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
        .minFilter=(d->minf&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.mipmapMode=d->minf>=4?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=sampler_wrap(d->ws),.addressModeV=sampler_wrap(d->wt),.addressModeW=sampler_wrap(d->wr),
        .mipLodBias=d->lod_bias,.anisotropyEnable=graphics_sampler_features.samplerAnisotropy && d->aniso>1,
        .maxAnisotropy=d->aniso>graphics_sampler_properties.limits.maxSamplerAnisotropy?graphics_sampler_properties.limits.maxSamplerAnisotropy:d->aniso,
        .compareEnable=d->compare!=0,.compareOp=d->compare_func?d->compare_func-1:VK_COMPARE_OP_ALWAYS,
        .minLod=d->lod_min<0?0:d->lod_min,.maxLod=d->minf<2?0:d->lod_max,.borderColor=border};
    graphics_sampler_entry *hit=graphics_last_sampler?&graphics_sampler_cache[graphics_last_sampler-1]:NULL;
    if(hit && graphics_sampler_key_equal(&hit->key,&ci)) {
        gpu_metrics.sampler_cache_hits++;
        hit->refs++;hit->used=++graphics_image_cache_clock;*slot=graphics_last_sampler;return hit->handle;
    }
    unsigned bucket=graphics_sampler_bucket(&ci);
    for(unsigned at=graphics_sampler_buckets[bucket];at;at=graphics_sampler_cache[at-1].next) {
        hit=&graphics_sampler_cache[at-1];
        if(graphics_sampler_key_equal(&hit->key,&ci)) {
            gpu_metrics.sampler_cache_hits++;
            hit->refs++;hit->used=++graphics_image_cache_clock;*slot=graphics_last_sampler=at;return hit->handle;
        }
    }
    int candidate=-1;
    for(unsigned i=0;i<GRAPHICS_SAMPLER_CACHE_COUNT;i++) {
        graphics_sampler_entry *e=&graphics_sampler_cache[i];
        if(!e->refs && (candidate<0 || e->used<graphics_sampler_cache[candidate].used))candidate=(int)i;
    }
    VkSampler handle=VK_NULL_HANDLE;if(vkCreateSampler(v.device,&ci,NULL,&handle))return VK_NULL_HANDLE;
    gpu_metrics.sampler_cache_creates++;
    if(candidate>=0) {
        graphics_sampler_entry *e=&graphics_sampler_cache[candidate];
        if(e->handle){graphics_sampler_unlink(candidate+1);graphics_destroy_sampler(e->handle);}
        *e=(graphics_sampler_entry){.key=ci,.handle=handle,.refs=1,.used=++graphics_image_cache_clock,
            .bucket=bucket,.next=graphics_sampler_buckets[bucket]};
        graphics_sampler_buckets[bucket]=candidate+1;*slot=graphics_last_sampler=candidate+1;
    }
    return handle;
}
static void graphics_image_cache_retire(VkImage image) {
    for(unsigned i=0;i<GRAPHICS_VIEW_CACHE_COUNT;i++) {
        graphics_view_entry *e=&graphics_view_cache[i];if(!e->handle || e->key.image!=image)continue;
        // Retirement callers drain pending draws before destroying their image.
        if(e->refs){LOG("Vulkan image retired with active descriptor references");abort();}
        graphics_view_unlink(i+1);graphics_destroy_image_view(e->handle);memset(e,0,sizeof *e);
    }
}
static void graphics_image_cache_close(void) {
    graphics_descriptor_bundles_close();
    graphics_texel_cache_retire(VK_NULL_HANDLE);graphics_texel_clock=0;
    if(graphics_descriptor_fallback_live){LOG("Descriptor fallback pools closed with active draws");abort();}
    for(unsigned i=0;i<GRAPHICS_DESCRIPTOR_ARENAS;i++) {
        graphics_descriptor_arena *a=&graphics_descriptor_pools[i];
        if(a->live){LOG("Descriptor arena closed with active draws");abort();}
        if(a->update_template)vkDestroyDescriptorUpdateTemplate(v.device,a->update_template,NULL);
        if(a->pool)vkDestroyDescriptorPool(v.device,a->pool,NULL);
    }
    memset(graphics_descriptor_pools,0,sizeof graphics_descriptor_pools);graphics_descriptor_clock=0;
    for(unsigned i=0;i<GRAPHICS_VIEW_CACHE_COUNT;i++) {
        graphics_view_entry *e=&graphics_view_cache[i];
        if(e->refs){LOG("Vulkan view cache closed with active draws");abort();}
        if(e->handle)graphics_destroy_image_view(e->handle);
    }
    for(unsigned i=0;i<GRAPHICS_SAMPLER_CACHE_COUNT;i++) {
        graphics_sampler_entry *e=&graphics_sampler_cache[i];
        if(e->refs){LOG("Vulkan sampler cache closed with active draws");abort();}
        if(e->handle)graphics_destroy_sampler(e->handle);
    }
    memset(graphics_view_cache,0,sizeof graphics_view_cache);memset(graphics_sampler_cache,0,sizeof graphics_sampler_cache);
    memset(graphics_view_buckets,0,sizeof graphics_view_buckets);memset(graphics_sampler_buckets,0,sizeof graphics_sampler_buckets);
    graphics_last_view=graphics_last_sampler=0;
    graphics_image_cache_clock=0;graphics_sampler_limits_valid=0;
}
