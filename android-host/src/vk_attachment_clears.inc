/* Full attachment clears are load operations, as in upstream R.clear_*.
 * Each image owns at most one exact subresource clear. The bounded intrusive
 * list is traversed only at explicit ordering boundaries, never per draw. */
typedef struct pending_attachment_clear {
    struct pending_attachment_clear *next,**link;
    gpu_image *image;dm_compute_image view;
    unsigned width,height;VkClearRect rect;
    VkImageAspectFlags aspects;VkClearValue value;
} pending_attachment_clear;
static pending_attachment_clear *attachment_clear_head;
static unsigned attachment_clear_count;
static int attachment_clears_peek_ready_locked(uint64_t address,size_t size) {
    for(const pending_attachment_clear *c=attachment_clear_head;c;c=c->next) {
        const gpu_image *p=c->image;
        if(!p->packaged || !p->address || !p->storage.size)continue;
        if(address>=p->address?address-p->address<p->storage.size:p->address-address<size)return 0;
    }
    return 1;
}
static int attachment_clears_flushing;
static int attachment_clear_view_same(const pending_attachment_clear *c,const dm_compute_image *d) {
    return c->view.serial==d->serial && c->view.format==d->format &&
        c->view.base_level==d->base_level && c->view.base_layer==d->base_layer && c->view.layers==d->layers;
}
static void attachment_clear_remove(pending_attachment_clear *c) {
    *c->link=c->next;if(c->next)c->next->link=c->link;
    c->image->pending_clear=NULL;attachment_clear_count--;
}
static void attachment_clear_consume(gpu_image *p) {
    pending_attachment_clear *c=p->pending_clear;if(!c)return;
    attachment_clear_remove(c);pool_cpu_hazard_remove();free(c);
    gpu_metrics.attachment_clear_loads++;
}
static int attachment_clear_flush_image(gpu_image *p) {
    pending_attachment_clear *c=p->pending_clear;if(!c)return 0;
    // Detach before recording: image_transfer and capacity submission may
    // reenter the flush hooks. Keep the producer hazard until FIFO ownership.
    attachment_clear_remove(c);
    int result=attachment_clear_gpu(p,&c->view,c->width,c->height,&c->rect,c->aspects,&c->value);
    if(result)die("Vulkan pending attachment clear recording failed; retaining resources");
    pool_cpu_hazard_remove();free(c);gpu_metrics.attachment_clear_fallbacks++;
    return 0;
}
static int attachment_clears_flush(void) {
    if(!attachment_clear_count || attachment_clears_flushing)return 0;
    attachment_clears_flushing=1;
    while(attachment_clear_head)if(attachment_clear_flush_image(attachment_clear_head->image)) {
        attachment_clears_flushing=0;return -1;
    }
    attachment_clears_flushing=0;return 0;
}
static int attachment_clear_defer(gpu_image *p,const dm_compute_image *d,unsigned width,unsigned height,
        const VkClearRect *rect,VkImageAspectFlags aspects,const VkClearValue *value) {
    int full=draw_batch && !rect->rect.offset.x && !rect->rect.offset.y &&
        rect->rect.extent.width==width && rect->rect.extent.height==height &&
        !rect->baseArrayLayer && rect->layerCount==(unsigned)d->layers;
    if(!full) {
        if(attachment_clear_flush_image(p))return -1;
        return attachment_clear_gpu(p,d,width,height,rect,aspects,value);
    }
    if(p->pending_clear && !attachment_clear_view_same(p->pending_clear,d))
        if(attachment_clear_flush_image(p))return -1;
    if(!p->pending_clear) {
        if(attachment_clear_count==9 && attachment_clears_flush())return -1;
        // Preserve uncleared levels/layers and the other depth/stencil plane.
        // Upload/initial transition keeps its existing asynchronous ownership.
        if(image_transfer(p,0) || draw_active_end())return -1;
        pending_attachment_clear *c=calloc(1,sizeof *c);if(!c)return -1;
        c->image=p;c->view=*d;c->width=width;c->height=height;c->rect=*rect;
        c->next=attachment_clear_head;c->link=&attachment_clear_head;
        if(c->next)c->next->link=&c->next;
        attachment_clear_head=c;p->pending_clear=c;attachment_clear_count++;pool_cpu_hazard_add();
    }
    pending_attachment_clear *c=p->pending_clear;
    if(aspects&VK_IMAGE_ASPECT_COLOR_BIT)c->value.color=value->color;
    if(aspects&VK_IMAGE_ASPECT_DEPTH_BIT)c->value.depthStencil.depth=value->depthStencil.depth;
    if(aspects&VK_IMAGE_ASPECT_STENCIL_BIT)c->value.depthStencil.stencil=value->depthStencil.stencil;
    c->aspects|=aspects;
    image_set_dirty(p,1);p->upload_stamp=0;p->shadow_valid=0;
    gpu_metrics.attachment_clear_deferred++;return 0;
}
// Only attachment preparation may retain the deferred clear. All image readers
// continue through image_transfer, which materializes it before their use.
static int image_attachment_prepare(gpu_image *p) {
    return p->pending_clear?0:image_transfer(p,0);
}
