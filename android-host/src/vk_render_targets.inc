/* Vulkan 1.1 attachment clears. LOAD/STORE retain every untouched pixel and
 * aspect, including a scissored depth-only clear of a packed depth/stencil image.
 * Clears record immediately into the shared draw FIFO; CPU visibility drains it. */
static int draw_attachment_clear_record(const VkRenderPassCreateInfo *,
    const VkImageViewCreateInfo *,unsigned,unsigned,unsigned,const VkClearRect *,
    VkImageAspectFlags,const VkClearValue *);
static int attachment_clear_defer(gpu_image *,const dm_compute_image *,unsigned,unsigned,
    const VkClearRect *,VkImageAspectFlags,const VkClearValue *);
static int attachment_clear_gpu(gpu_image *p,const dm_compute_image *d,
                                unsigned w,unsigned h,const VkClearRect *rect,
                                VkImageAspectFlags aspects,const VkClearValue *value) {
    int result=-1;
    int depth_stencil=!!(p->aspect&(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT));
    if(image_transfer(p,0))goto done; // GPU-dirty images remain authoritative.
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=p->image,
        .viewType=d->layers>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D,.format=image_format(d->format),
        .subresourceRange={p->aspect,d->base_level,1,d->base_layer,d->layers}};
    VkAttachmentDescription attachment={.format=vi.format,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=depth_stencil?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp=depth_stencil?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ref={0,depth_stencil?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=depth_stencil?0:1,.pColorAttachments=depth_stencil?NULL:&ref,
        .pDepthStencilAttachment=depth_stencil?&ref:NULL};
    VkSubpassDependency dependencies[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT}};
    VkRenderPassCreateInfo pi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
        .subpassCount=1,.pSubpasses=&sub,.dependencyCount=2,.pDependencies=dependencies};
    result=draw_attachment_clear_record(&pi,&vi,w,h,d->layers,rect,aspects,value);
    if(!result) {
        image_set_dirty(p,1);p->upload_stamp=0;p->shadow_valid=0;
        // Packaged readback is deferred exactly like normal draw batching.
        // Existing image_transfer_raw and retirement drain this same FIFO.
        if(!draw_batch) { result=draw_pending_flush();if(!result && p->packaged)result=image_transfer(p,1); }
    }
done:
    return result;
}

int dm_vk_depth_attachment_clear(const dm_compute_image *d,const int rect[4],float depth,int depth_mask,int stencil,int stencil_mask) {
    pthread_mutex_lock(&lock);
    int result=-1;uint8_t *linear=NULL,*packed_bytes=NULL;
    gpu_image *p=v.device?image_get(d):NULL;
    if(!p || !(p->usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) || d->format!=p->desc.format || p->desc.target==2 || (p->desc.flags&0x10) ||
        d->base_level<0 || (unsigned)d->base_level>=p->desc.levels || d->base_layer<0 || d->layers<1 || (uint64_t)d->base_layer+d->layers>p->layers)goto done;
    unsigned width=image_mip(p->extent.width,d->base_level),height=image_mip(p->extent.height,d->base_level);
    int64_t x=rect[0],y=rect[1],right=x+rect[2],bottom=y+rect[3];if(x<0)x=0;if(y<0)y=0;if(right>width)right=width;if(bottom>height)bottom=height;
    if(right<=x || bottom<=y || rect[2]<=0 || rect[3]<=0){result=0;goto done;}
    VkImageAspectFlags aspects=0;
    if(depth_mask && (p->aspect&VK_IMAGE_ASPECT_DEPTH_BIT))aspects|=VK_IMAGE_ASPECT_DEPTH_BIT;
    unsigned stencil_bits=(unsigned)stencil_mask&255u;
    if(stencil_bits && (p->aspect&VK_IMAGE_ASPECT_STENCIL_BIT))aspects|=VK_IMAGE_ASPECT_STENCIL_BIT;
    if(!aspects){result=0;goto done;}
    if(!(aspects&VK_IMAGE_ASPECT_STENCIL_BIT) || stencil_bits==255u) {
        VkClearValue value={.depthStencil={depth<0?0:depth>1?1:depth,(uint32_t)stencil&255u}};
        VkClearRect cr={.rect={{(int32_t)x,(int32_t)y},{(uint32_t)(right-x),(uint32_t)(bottom-y)}},
            .baseArrayLayer=0,.layerCount=(uint32_t)d->layers};
        result=attachment_clear_defer(p,d,width,height,&cr,aspects,&value);goto done;
    }
    // Vulkan clears cannot express a partial stencil write mask. Materialize
    // only this fallback, merge the requested bits, and preserve the rest.
    if(draw_pending_flush())goto done;
    unsigned bpp=nvn_fmt_bpb(p->desc.format,NULL,NULL);size_t pitch=(size_t)width*bpp;
    packed_bytes=image_cpu_read(p);if(!packed_bytes)goto done;
    linear=malloc(pitch*height);if(!linear)goto done;
    float value=depth<0?0:depth>1?1:depth;
    uint32_t packed=0;if(p->desc.format==0x32)packed=(uint32_t)(value*65535.0+0.5);else if(p->desc.format==0x33 || p->desc.format==0x35)packed=(uint32_t)(value*16777215.0+0.5);else memcpy(&packed,&value,4);
    for(int layer=0;layer<d->layers;layer++) {
        if(nvn_deswizzle(&p->desc,d->base_level,d->base_layer+layer,packed_bytes,linear,pitch))goto done;
        for(int64_t row=y;row<bottom;row++)for(int64_t col=x;col<right;col++) {
            uint8_t *pixel=linear+row*pitch+col*bpp;
            if(depth_mask && (p->aspect&VK_IMAGE_ASPECT_DEPTH_BIT)) {
                unsigned bytes=p->desc.format==0x32?2:(p->desc.format==0x33 || p->desc.format==0x35)?3:4;memcpy(pixel,&packed,bytes);
            }
            if(stencil_mask && (p->aspect&VK_IMAGE_ASPECT_STENCIL_BIT)) {
                unsigned at=p->desc.format==0x31?0:p->desc.format==0x35?3:4;
                pixel[at]=(pixel[at]&~stencil_mask)|(stencil&stencil_mask);
            }
        }
        if(nvn_swizzle(&p->desc,d->base_level,d->base_layer+layer,linear,pitch,packed_bytes))goto done;
    }
    result=image_cpu_write(p,packed_bytes);
done:free(packed_bytes);free(linear);if(result)LOG("Vulkan depth/stencil clear failed: serial=%llu",(unsigned long long)d->serial);pthread_mutex_unlock(&lock);return result;
}
int dm_vk_color_attachment_clear(const dm_compute_image *d,const int rect[4],const float color[4]) {
    pthread_mutex_lock(&lock);
    int result=-1;
    gpu_image *p=v.device?image_get(d):NULL;
    if(!p || !(p->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || p->desc.target==2 ||
        d->base_level<0 || (unsigned)d->base_level>=p->desc.levels || d->base_layer<0 || d->layers<1 ||
        (uint64_t)d->base_layer+d->layers>p->layers)goto done;
    unsigned w=image_mip(p->extent.width,d->base_level),h=image_mip(p->extent.height,d->base_level);
    int64_t x=rect[0],y=rect[1],right=x+rect[2],bottom=y+rect[3];
    if(x<0)x=0;if(y<0)y=0;if(right>w)right=w;if(bottom>h)bottom=h;
    if(rect[2]<=0 || rect[3]<=0 || right<=x || bottom<=y){result=0;goto done;}
    VkClearValue value;memcpy(value.color.float32,color,16);
    VkClearRect cr={.rect={{(int32_t)x,(int32_t)y},{(uint32_t)(right-x),(uint32_t)(bottom-y)}},
        .baseArrayLayer=0,.layerCount=(uint32_t)d->layers};
    result=attachment_clear_defer(p,d,w,h,&cr,VK_IMAGE_ASPECT_COLOR_BIT,&value);
done:
    if(result)LOG("Vulkan color attachment clear failed: serial=%llu",(unsigned long long)d->serial);
    pthread_mutex_unlock(&lock);return result;
}
#include "vk_attachment_clears.inc"
