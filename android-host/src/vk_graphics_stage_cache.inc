/* Bounded immutable descriptor bundles for each stage of the active pass.
 * Each cache lease and queued draw own separate references to its bundle. */
static int graphics_stage_image_equal(const dm_compute_image *a,const dm_compute_image *b) {
#define SAME(field) if(a->field!=b->field)return 0
    SAME(serial);SAME(address);SAME(base_level);SAME(levels);SAME(base_layer);SAME(layers);SAME(format);SAME(packaged);
    SAME(desc.target);SAME(desc.format);SAME(desc.flags);SAME(desc.samples);SAME(desc.width);SAME(desc.height);
    SAME(desc.depth);SAME(desc.levels);SAME(desc.stride);SAME(desc.pkg_block_h);
    SAME(minf);SAME(magf);SAME(ws);SAME(wt);SAME(wr);SAME(compare);SAME(compare_func);
    SAME(lod_min);SAME(lod_max);SAME(lod_bias);SAME(aniso);
    for(unsigned i=0;i<4;i++)SAME(border[i]);
#undef SAME
    return 1;
}
static const uint8_t *graphics_stage_shadow(graphics_resources *r,unsigned i,size_t size) {
    if(r->buffer_refs[i]) {
        graphics_buffer_version *p=&graphics_buffers[r->buffer_refs[i]-1];
        return p->size==size?p->shadow:NULL;
    }
    unsigned ref=r->uniform_refs[i];
    if(!ref)return NULL; // Scratch fallback is deliberately not cached.
    if(ref>GRAPHICS_GUEST_REFERENCE_BASE) {
        graphics_guest_uniform *p=&graphics_guest_uniforms[ref-GRAPHICS_GUEST_REFERENCE_BASE-1];
        return p->payload==size?p->shadow:NULL;
    }
    if(ref<=GRAPHICS_UNIFORM_STATIC_COUNT) {
        graphics_uniform_static *p=&graphics_uniform_statics[ref-1];
        return p->payload==size?p->shadow:NULL;
    }
    unsigned slot=ref-GRAPHICS_UNIFORM_STATIC_COUNT-1,local=slot%GRAPHICS_UNIFORM_BANK_SLOTS;
    graphics_uniform_bank *p=&graphics_uniform_banks[slot/GRAPHICS_UNIFORM_BANK_SLOTS];
    return (p->active&p->shadow_valid&(UINT64_C(1)<<local)) && p->payload[local]==size?p->shadow[local]:NULL;
}
static int graphics_stage_cacheable(graphics_resources *r) {
    if(!r->cacheable)return 0;
    for(unsigned i=0;i<r->count;i++) {
        const dm_compute_buffer *b=&r->input_keys[i];
        if(b->writable)return 0;
        if((r->resident_storage|r->resident_uniform|r->resident_texel)&(UINT64_C(1)<<i))continue;
        if(b->image) {
            if(!r->images[i] && !r->buffer_refs[i])return 0;
        } else if(!b->zero && !graphics_stage_shadow(r,i,b->size))return 0;
    }
    return 1;
}
static int graphics_stage_guest_clean(graphics_resources *r,unsigned i,uint64_t address,const void *bytes,size_t size) {
    extern unsigned char dm_no_fp_state;
    // Arena refs reach the fallback below and compare their own immutable
    // shadow. Only caches that own a matching dirty stamp may skip comparison.
    if(dm_no_fp_state || !address || !nvn_dirty_tracked(bytes,size))return 0;
    uint64_t stamp,*checked;
    if(r->buffer_refs[i]) {
        graphics_buffer_version *p=&graphics_buffers[r->buffer_refs[i]-1];
        if(!p->valid || !p->tracked || !p->stamp || p->address!=address || p->size!=size)return 0;
        stamp=p->stamp;checked=&p->checked_generation;
    } else if(r->uniform_refs[i]>GRAPHICS_GUEST_REFERENCE_BASE) {
        graphics_guest_uniform *p=&graphics_guest_uniforms[r->uniform_refs[i]-GRAPHICS_GUEST_REFERENCE_BASE-1];
        if(!p->valid || !p->tracked || !p->stamp || p->address!=address || p->payload!=size)return 0;
        stamp=p->stamp;checked=&p->checked_generation;
    } else return 0;
    // Pool lookup has already resolved the current registration. Pool removal
    // invalidates both caches under this same backend lock before unregistering
    // its dirty pages, so a recycled address cannot inherit a valid stamp.
    // Capture before the scan: a later generation must never certify this scan.
    uint64_t now=nvn_dirty_now();
    if(*checked==now)return 1;
    if(nvn_dirty_since(bytes,size,stamp))return 0;
    *checked=now;return 1;
}
static int graphics_stage_matches(graphics_resources *r,const dm_graphics_shader *shader,
                                  VkDescriptorSetLayout layout,const dm_compute_buffer *buffers,uint64_t dynamic_profile) {
    // Persistent pipeline variants create separate but identically-defined
    // layouts for the same shader and pair-selected dynamic profile. Vulkan permits their descriptor sets to be
    // interchanged: flags/pNext/immutable samplers are absent, counts are one,
    // and the fixed shader stage plus bindings below fully define the layout.
    // Include new layout extensions here if that creation contract changes.
    (void)layout;
    if(!graphics_stage_cacheable(r) || r->shader!=shader || r->count!=shader->nbindings ||
       r->dynamic_profile!=dynamic_profile)return 0;
    for(unsigned i=0;i<r->count;i++) {
        const dm_shader_binding *a=&r->binding_keys[i],*b=&shader->bindings[i];
        if(a->binding!=b->binding || a->kind!=b->kind || a->slot!=b->slot || a->flags!=b->flags ||
           a->type!=b->type || a->sampler_slot!=b->sampler_slot)return 0;
        const dm_compute_buffer *old=&r->input_keys[i],*now=&buffers[i];
        if(old->binding!=now->binding || old->address!=now->address || old->size!=now->size ||
           old->writable!=now->writable || old->zero!=now->zero || !!old->image!=!!now->image)return 0;
        if(now->image && !graphics_stage_image_equal(old->image,now->image))return 0;
    }
    // Preserve image alias flushes/uploads even when descriptors are unchanged.
    // These operations can end the active pass; caller pins r and checks epoch.
    for(unsigned i=0;i<r->count;i++)if(buffers[i].image && r->images[i]) {
        int input=shader->bindings[i].kind==DM_BIND_INPUT_ATTACHMENT;
        gpu_image *p=input?image_serial_get(buffers[i].image->serial):image_get(buffers[i].image);
        if(p!=r->images[i] || (!input && image_transfer(p,0)))return 0;
        if(input) {
            gpu_image *resolved=NULL;
            if(draw_fuse_input_view(buffers[i].image,&resolved)!=r->payload.images[i].imageView || resolved!=p)return 0;
        }
    }
    const void *bytes[64]={0};size_t sizes[64]={0};uint64_t addresses[64]={0};
    // Complete all GPU->guest synchronization before comparing any snapshots.
    for(unsigned i=0;i<r->count;i++) {
        const dm_compute_buffer *b=&buffers[i];
        if(b->image && r->images[i])continue;
        uint64_t address=b->address;size_t size=b->size;
        if(b->image) {
            nvn_texlayout storage;nvn_tex_layout(&b->image->desc,&storage);
            address=b->image->address;size=storage.size;
        }
        sizes[i]=size;addresses[i]=address;
        if(address) {
            size_t offset;
            if(images_flush_range(address,size))return 0;
            dm_vk_pool *pool=pool_address_locked(address,size,&offset);
            if(!pool)return 0;
            if((r->resident_storage|r->resident_uniform|r->resident_texel)&(UINT64_C(1)<<i)) {
                // This descriptor names mutable resident storage, not an
                // immutable snapshot. Re-resolve its current registration and
                // upload through the same reader-hazard path as vertex data.
                if(pool!=r->guest[i] || !pool->mapped || pool->buffer!=r->payload.buffers[i].buffer || offset!=r->offsets[i])return 0;
                if(((r->resident_texel|r->resident_uniform)&(UINT64_C(1)<<i)) && size!=r->payload.buffers[i].range)return 0;
                unsigned consumer=(r->resident_texel&(UINT64_C(1)<<i))?POOL_PREP_TEXEL:
                    (r->resident_uniform&(UINT64_C(1)<<i))?POOL_PREP_UBO:POOL_PREP_SSBO;
                if(pool_gpu_read_consumer_locked(pool,offset,size,consumer))return 0;
            } else if(pool_cpu_read_locked(pool,offset,size))return 0;
            bytes[i]=pool->guest+offset;
        } else {
            bytes[i]=b->host;
            if(!b->zero && reports_range_drain((uintptr_t)b->host,size))return 0;
        }
    }
    for(unsigned i=0;i<r->count;i++) {
        const dm_compute_buffer *b=&buffers[i];
        if((b->image && r->images[i]) || b->zero || ((r->resident_storage|r->resident_uniform|r->resident_texel)&(UINT64_C(1)<<i)))continue;
        const uint8_t *shadow=graphics_stage_shadow(r,i,sizes[i]);
        if(!shadow || !bytes[i])return 0;
        if(graphics_stage_guest_clean(r,i,addresses[i],bytes[i],sizes[i]))continue;
        // Shadow comparison also works after pool re-registration. Never use
        // the cached guest pointer, or compare against uncached GPU memory.
        if(memcmp(shadow,bytes[i],sizes[i]))return 0;
    }
    return 1;
}
// Dynamic descriptors identify a pool VkBuffer and fixed range; moving a read
// within that same pool changes only the bind offset. Retain a separate draw
// record so pending reader hazards continue to describe the ORIGINAL slice.
static graphics_resources *graphics_stage_reoffset(graphics_resources *source,const dm_graphics_shader *shader,
                                  VkDescriptorSetLayout layout,const dm_compute_buffer *buffers,uint64_t dynamic_profile) {
    if(source->shader!=shader || source->count!=shader->nbindings ||
       source->dynamic_profile!=dynamic_profile || !graphics_stage_cacheable(source))return NULL;
    uint64_t movable=dynamic_profile&(source->resident_uniform|source->resident_storage),changed=0;
    size_t offsets[64];
    for(unsigned i=0;i<source->count;i++) {
        const dm_compute_buffer *old=&source->input_keys[i],*now=&buffers[i];
        if(old->binding!=now->binding || old->size!=now->size || old->writable!=now->writable ||
           old->zero!=now->zero || !!old->image!=!!now->image)return NULL;
        if(now->image && !graphics_stage_image_equal(old->image,now->image))return NULL;
        if(old->address==now->address)continue;
        uint64_t bit=UINT64_C(1)<<i;
        if(!(movable&bit) || now->image || now->writable || now->zero || !now->address)return NULL;
        dm_vk_pool *pool=pool_address_locked(now->address,now->size,&offsets[i]);
        VkDeviceSize alignment=(source->resident_uniform&bit)?v.properties.limits.minUniformBufferOffsetAlignment:
            v.properties.limits.minStorageBufferOffsetAlignment;
        const VkDescriptorBufferInfo *info=&source->payload.buffers[i];
        if(!pool || pool!=source->guest[i] || !pool->mapped || pool->buffer!=info->buffer ||
           info->offset!=0 || info->range==VK_WHOLE_SIZE || offsets[i]>UINT32_MAX ||
           (alignment && offsets[i]%alignment) || offsets[i]>pool->size || info->range>pool->size-offsets[i])return NULL;
        changed|=bit;
    }
    if(!changed)return NULL;
    graphics_resources *r=graphics_resources_acquire();if(!r)return NULL;
    memcpy(r,source,sizeof *r);r->refs=1;r->idle_next=NULL;
    r->static_owner=source->static_owner?source->static_owner:source;
    r->static_owner->refs++;
    for(unsigned i=0;i<r->count;i++) {
        // Host driver banks may move between draws. The full validation below
        // compares their current bytes against the owner's immutable shadow.
        r->input_keys[i].host=buffers[i].host;
        if(r->input_keys[i].image)r->input_keys[i].image=&r->image_keys[i];
        if(changed&(UINT64_C(1)<<i)) {
            r->input_keys[i].address=buffers[i].address;r->offsets[i]=offsets[i];
        }
    }
    // Vulkan orders dynamic offsets by binding number, not reflection index.
    uint64_t remaining=dynamic_profile;unsigned dynamic=0;
    while(remaining) {
        unsigned at=64;
        for(unsigned i=0;i<r->count;i++)if((remaining&(UINT64_C(1)<<i)) &&
           (at==64 || shader->bindings[i].binding<shader->bindings[at].binding))at=i;
        if(at==64 || dynamic>=r->dynamic_count)goto fail;
        if(changed&(UINT64_C(1)<<at))r->dynamic_offsets[dynamic]=(uint32_t)offsets[at];
        dynamic++;remaining&=~(UINT64_C(1)<<at);
    }
    // Preserve all ordinary image alias/upload and exact buffer reader work,
    // plus immutable snapshot validation. A flush may retire the source cache;
    // our root lease already pins every borrowed handle during that operation.
    if(dynamic!=r->dynamic_count || !graphics_stage_matches(r,shader,layout,buffers,dynamic_profile))goto fail;
    return r;
fail:
    graphics_resources_free(r);return NULL;
}
