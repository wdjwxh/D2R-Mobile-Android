/* Two-subpass transaction. No draw is replayed after encoding, and a token
 * never survives a capacity submit, observer, upload or ordinary pass break. */
#include "vk_fuse_shader.h"
static int fuse_view_equal(const dm_compute_image *a,const dm_compute_image *b) {
    return a->serial==b->serial && a->format==b->format && a->base_level==b->base_level &&
        a->base_layer==b->base_layer && a->layers==b->layers;
}
static VkImageView draw_fuse_input_view(const dm_compute_image *view,gpu_image **image) {
    *image=NULL;if(!draw_fuse.token || !draw_fuse.requested)return VK_NULL_HANDLE;
    for(unsigned i=0;i<draw_fuse.count;i++)if(fuse_view_equal(view,&draw_fuse.union_views[i])) {
        *image=draw_fuse.images[i];return draw_fuse.views[i];
    }
    return VK_NULL_HANDLE;
}
static int fuse_targets_equal(const dm_draw *d,const dm_fuse_targets *t) {
    if(d->color_mask!=t->color_mask || !!d->has_depth!=!!t->has_depth)return 0;
    for(unsigned i=0;i<8;i++)if((t->color_mask&(1u<<i)) && !fuse_view_equal(&d->color[i],&t->color[i]))return 0;
    return !t->has_depth || fuse_view_equal(&d->depth,&t->depth);
}
static int fuse_union_add(const dm_compute_image *d,gpu_image **images,dm_compute_image *views,
        VkImageViewCreateInfo *vi,VkAttachmentDescription *ad,unsigned *count,unsigned *width,unsigned *height) {
    if(!d->serial || d->packaged || d->layers!=1 || d->base_level<0 || d->base_layer<0 || d->desc.target==2)return -1;
    for(unsigned i=0;i<*count;i++)if(views[i].serial==d->serial)
        return fuse_view_equal(&views[i],d)?(int)i:-1;
    if(*count==9)return -1;
    gpu_image *p=image_get(d);
    if(!p || !(p->usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) || (unsigned)d->base_level>=p->desc.levels || (unsigned)d->base_layer>=p->layers)return -1;
    unsigned w=image_mip(p->extent.width,d->base_level),h=image_mip(p->extent.height,d->base_level);
    // Equal extents keep both original render areas and Q pixel coordinates.
    if((*width && *width!=w) || (*height && *height!=h))return -1;
    *width=w;*height=h;
    VkFormat format=image_format(d->format);if(!format)return -1;
    VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(v.gpu,format,&fp);
    VkFormatFeatureFlags need=p->aspect==VK_IMAGE_ASPECT_COLOR_BIT?VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT:VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if(!(fp.optimalTilingFeatures&need))return -1;
    unsigned at=(*count)++;images[at]=p;views[at]=*d;views[at].levels=1;
    vi[at]=(VkImageViewCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=p->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=format,.subresourceRange={p->aspect,d->base_level,1,d->base_layer,1}};
    ad[at]=(VkAttachmentDescription){.format=format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    return (int)at;
}
int dm_vk_fuse_begin(const dm_fuse_plan *plan,uint64_t *token) {
    if(!token)return -1;*token=0;if(!plan)return 1;
    pthread_mutex_lock(&lock);int result=1,pass_cached=0,fb_cached=0;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer fb=VK_NULL_HANDLE;VkImageView handles[9]={0};
    gpu_image *images[9]={0};dm_compute_image views[9]={0};unsigned count=0,width=0,height=0;
    VkImageViewCreateInfo vi[9]={0};VkAttachmentDescription ad[9]={0};
    VkAttachmentReference colors[2][8],depth[2],inputs[8];uint32_t preserve[2][9];unsigned nc[2]={0},np[2]={0},ni=0,used[2]={0};
    const dm_graphics_shader *variant=NULL;
    if(draw_fuse.token && draw_active_end()){result=-1;goto done;}
    if(!v.device || !draw_batch || !plan->q_shader || !plan->q_shader->fuse || !plan->q_mask || plan->q_shader->fuse_mask)goto done;
    const dm_fuse_shader_info *facts=plan->q_shader->fuse;
    if(facts->ninputs>8 || (plan->q_mask>>facts->ninputs))goto done;
    for(unsigned i=0;i<facts->ninputs;i++)if(facts->inputs[i].input_index!=i || facts->inputs[i].binding_index>=plan->q_shader->nbindings)goto done;
    variant=dm_vk_graphics_fuse_variant(plan->q_shader,plan->q_mask);if(!variant)goto done;
    if(plan->q_clear_colors&~plan->targets[1].color_mask || ((plan->q_clear_depth || plan->q_clear_stencil) && !plan->targets[1].has_depth))goto done;
    if(attachment_clears_flush() || draw_active_end()){result=-1;goto done;}
    for(unsigned k=0;k<2;k++) {
        if(plan->targets[k].color_mask&~255u)goto done;
        for(unsigned j=0;j<8;j++) {
            colors[k][j]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
            if(!(plan->targets[k].color_mask&(1u<<j)))continue;
            int at=fuse_union_add(&plan->targets[k].color[j],images,views,vi,ad,&count,&width,&height);
            if(at<0 || images[at]->aspect!=VK_IMAGE_ASPECT_COLOR_BIT)goto done;
            if(used[k]&(1u<<at))goto done;
            colors[k][j].attachment=(unsigned)at;used[k]|=1u<<at;nc[k]=j+1;
        }
        depth[k]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
        if(plan->targets[k].has_depth) {
            int at=fuse_union_add(&plan->targets[k].depth,images,views,vi,ad,&count,&width,&height);
            if(at<0 || images[at]->aspect==VK_IMAGE_ASPECT_COLOR_BIT || (used[k]&(1u<<at)))goto done;
            depth[k].attachment=(unsigned)at;used[k]|=1u<<at;
        }
        if(!used[k] || nc[k]>v.properties.limits.maxColorAttachments)goto done;
    }
    unsigned q_outputs=used[1],input_count=0;
    for(unsigned i=0;i<8;i++)inputs[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
    for(unsigned i=0;i<facts->ninputs;i++)if(plan->q_mask&(1u<<i)) {
        unsigned index=facts->inputs[i].input_index;if(index>=8)goto done;
        const dm_compute_image *d=&plan->inputs[index];unsigned at=0;
        if((d->format>=0x69 && d->format<=0x78) || d->format==0x37 || d->format==0x99 || d->format==0x9b)goto done;
        while(at<count && !fuse_view_equal(d,&views[at]))at++;
        if(at==count || d->levels!=1 || images[at]->aspect!=VK_IMAGE_ASPECT_COLOR_BIT || !(used[0]&(1u<<at)) || (q_outputs&(1u<<at)))goto done;
        inputs[index].attachment=at;used[1]|=1u<<at;if(ni<=index)ni=index+1;input_count++;
    }
    if(ni>v.properties.limits.maxPerStageDescriptorInputAttachments || input_count>v.properties.limits.maxPerStageDescriptorInputAttachments ||
       input_count>v.properties.limits.maxDescriptorSetInputAttachments)goto done;
    // Private Q clears may move before P only if P has no attachment access.
    for(unsigned i=0;i<8;i++)if((plan->q_clear_colors&(1u<<i)) && (used[0]&(1u<<colors[1][i].attachment)))goto done;
    if((plan->q_clear_depth || plan->q_clear_stencil) && (used[0]&(1u<<depth[1].attachment)))goto done;
    if(!width || !height || width>v.properties.limits.maxFramebufferWidth || height>v.properties.limits.maxFramebufferHeight)goto done;
    for(unsigned k=0;k<2;k++)for(unsigned i=0;i<count;i++)if(!(used[k]&(1u<<i)))preserve[k][np[k]++]=i;
    VkSubpassDescription sub[2];memset(sub,0,sizeof sub);
    for(unsigned k=0;k<2;k++)sub[k]=(VkSubpassDescription){.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=nc[k],.pColorAttachments=colors[k],.pDepthStencilAttachment=plan->targets[k].has_depth?&depth[k]:NULL,
        .inputAttachmentCount=k?ni:0,.pInputAttachments=k?inputs:NULL,.preserveAttachmentCount=np[k],.pPreserveAttachments=preserve[k]};
    const VkPipelineStageFlags device_stages=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT|VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubpassDependency deps[5];
    for(unsigned k=0;k<2;k++) {
        deps[k]=(VkSubpassDependency){.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=k,.srcStageMask=device_stages,.dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
        deps[k+3]=(VkSubpassDependency){.srcSubpass=k,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    }
    deps[2]=(VkSubpassDependency){.srcSubpass=0,.dstSubpass=1,
        .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=count,.pAttachments=ad,
        .subpassCount=2,.pSubpasses=sub,.dependencyCount=5,.pDependencies=deps};
    if(draw_pass_get(&rp,&pass,&pass_cached) || !pass_cached)goto done;
    if(draw_attachments_get(pass,1,vi,count,width,height,1,handles,&fb,&fb_cached) || !fb_cached)goto done;
    for(unsigned i=0;i<count;i++)if(image_transfer(images[i],0)){result=-1;goto done;}
    VkClearRect rect={.rect={{0,0},{width,height}},.baseArrayLayer=0,.layerCount=1};
    for(unsigned i=0;i<8;i++)if(plan->q_clear_colors&(1u<<i)) {
        unsigned at=colors[1][i].attachment;VkClearValue value={0};memcpy(value.color.float32,plan->q_clear_rgba[i],16);
        if(attachment_clear_defer(images[at],&views[at],width,height,&rect,VK_IMAGE_ASPECT_COLOR_BIT,&value)){result=-1;goto done;}
    }
    if(plan->q_clear_depth || plan->q_clear_stencil) {
        unsigned at=depth[1].attachment;VkClearValue value={.depthStencil={plan->q_depth<0?0:plan->q_depth>1?1:plan->q_depth,plan->q_stencil&255u}};
        VkImageAspectFlags aspects=(plan->q_clear_depth?VK_IMAGE_ASPECT_DEPTH_BIT:0)|(plan->q_clear_stencil?VK_IMAGE_ASPECT_STENCIL_BIT:0);
        if((aspects&images[at]->aspect)!=aspects || attachment_clear_defer(images[at],&views[at],width,height,&rect,aspects,&value)){result=-1;goto done;}
    }
    if(attachment_clears_flush() || draw_active_end()){result=-1;goto done;}
    draw_fuse.plan=*plan;draw_fuse.pass=pass;draw_fuse.fb=fb;draw_fuse.count=count;draw_fuse.width=width;draw_fuse.height=height;
    memcpy(draw_fuse.images,images,sizeof images);memcpy(draw_fuse.union_views,views,sizeof views);memcpy(draw_fuse.views,handles,sizeof handles);
    draw_fuse.requested=draw_fuse.physical=draw_fuse.started=0;draw_fuse.cmd=VK_NULL_HANDLE;
    if(!++draw_fuse.sequence)die("FUSE token exhausted");*token=draw_fuse.token=draw_fuse.sequence;gpu_metrics.fuse_accepted++;result=0;
done:
    if(!fb_cached){if(fb)vkDestroyFramebuffer(v.device,fb,NULL);for(unsigned i=0;i<9;i++)if(handles[i])vkDestroyImageView(v.device,handles[i],NULL);}
    if(pass && !pass_cached)vkDestroyRenderPass(v.device,pass,NULL);
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_fuse_next(uint64_t token) {
    pthread_mutex_lock(&lock);int result=1;
    if(token && draw_fuse.token==token && !draw_fuse.requested && draw_fuse.started) {draw_fuse.requested=1;result=0;}
    else if(token && draw_fuse.token==token)draw_active_end();
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_fuse_end(uint64_t token) {
    pthread_mutex_lock(&lock);int result=1;
    if(token && draw_fuse.token==token) {
        // Explicit successful completion is not an abandonment statistic.
        result=draw_active_end()?-1:0;gpu_metrics.fuse_abandoned--;
    }
    pthread_mutex_unlock(&lock);return result;
}
static int fuse_draw_allowed(const dm_draw *d) {
    unsigned phase=draw_fuse.requested;
    if(!draw_fuse.token || !draw_batch || !fuse_targets_equal(d,&draw_fuse.plan.targets[phase]))return 0;
    if(phase && d->shader[1]!=draw_fuse.plan.q_shader)return 0;
    if(d->raster_samples>1 || d->alpha_to_coverage)return 0;
    const dm_fuse_shader_info *facts=draw_fuse.plan.q_shader->fuse;
    for(unsigned stage=0;stage<2;stage++)for(unsigned i=0;i<d->shader[stage]->nbindings;i++) {
        const dm_compute_buffer *b=&d->resources[stage][i];if(b->writable)return 0;
        if(!b->image)continue;
        int selected=0;
        if(phase && stage)for(unsigned j=0;j<facts->ninputs;j++)if((draw_fuse.plan.q_mask&(1u<<j)) && facts->inputs[j].binding_index==i) {
            if(!fuse_view_equal(b->image,&draw_fuse.plan.inputs[facts->inputs[j].input_index]))return 0;
            selected=1;
        }
        if(!selected)for(unsigned j=0;j<draw_fuse.count;j++)if(b->image->serial==draw_fuse.union_views[j].serial)return 0;
    }
    return 1;
}
