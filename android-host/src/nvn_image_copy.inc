static dm_compute_image replay_image(const tex_t *t,const view_t *view) {
    if(!t || t->magic!=MAGIC('T','E','X',0))die("Invalid copy texture");
    dm_compute_image d={.desc=t->d,.serial=t->serial,.address=(uintptr_t)t->mem,
        .packaged=t->has_layout && t->mem,
        .levels=t->d.levels,.layers=t->L.layers,.format=t->d.format};
    if(view) {
        if(view->set_levels){d.base_level=view->base_level;d.levels=view->nlevels;}
        if(view->set_layers){d.base_layer=view->min_layer;d.layers=view->nlayers;}
        if(view->set_format)d.format=view->format;
    }
    return d;
}
static dm_image_region replay_region(const NVNcopyRegion *r) {
    return (dm_image_region){r->xoffset,r->yoffset,r->zoffset,r->width,r->height,r->depth};
}
static void read_texels(const NVNtexture *t,const NVNtextureView *view,const NVNcopyRegion *region,void *destination,ptrdiff_t row,ptrdiff_t image) {
    nvn_translate_drain();
    if(!region)die("Invalid CPU texture read region");
    dm_compute_image d=replay_image(CTX(t),(const view_t *)view);
    dm_image_region r=replay_region(region);
    if(dm_vk_image_read_texels(&d,&r,destination,row,image))die("Vulkan CPU texture read failed");
}
NVN(nvnTextureReadTexels,void,const NVNtexture *t,const NVNtextureView *v,const NVNcopyRegion *r,void *destination) {
    read_texels(t,v,r,destination,0,0);
}
NVN(nvnTextureReadTexelsStrided,void,const NVNtexture *t,const NVNtextureView *v,const NVNcopyRegion *r,void *destination,ptrdiff_t row,ptrdiff_t image) {
    read_texels(t,v,r,destination,row,image);
}
// Upstream's invalidation is intentionally empty: ReadTexels itself performs
// the explicit GPU read. Invalidation does not copy an RT into its pool.
NVN(nvnTextureInvalidateTexels,void,const NVNtexture *t,const NVNtextureView *v,const NVNcopyRegion *r) {
    (void)t;(void)v;(void)r;
}
static void replay_copy_buffer_image(replay_state *s,const p_copy_b2t *p,int download) {
    if(p->flags & ~(NVN_COPY_FLAGS_ENGINE_2D_BIT|NVN_COPY_FLAGS_LINEAR_FILTER_BIT))die("Unsupported buffer/image copy flags %#x",p->flags);
    dm_compute_image d=replay_image(p->tex,p->has_view?&p->view:NULL);
    dm_image_region r=replay_region(&p->r);
    if(dm_vk_image_buffer(&d,&r,p->src,s->copy_stride,download))die("Vulkan buffer/image copy failed");
}
static void replay_copy_image(const p_copy_t2t *p) {
    if(p->flags & ~(NVN_COPY_FLAGS_ENGINE_2D_BIT|NVN_COPY_FLAGS_LINEAR_FILTER_BIT))die("Unsupported image copy flags %#x",p->flags);
    dm_compute_image src=replay_image(p->src,p->has_sv?&p->sv:NULL),dst=replay_image(p->dst,p->has_dv?&p->dv:NULL);
    dm_image_region sr=replay_region(&p->sr),dr=replay_region(&p->dr);
    if(dm_vk_image_copy(&src,&sr,&dst,&dr))die("Vulkan image copy failed");
}
