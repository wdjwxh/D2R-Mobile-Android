/* Vulkan copies combined D/S formats as separate depth and stencil aspects.
 * Tegra guest storage remains interleaved D24S8 or D32F+S8 (8 bytes/pixel). */
static int depth_transfer(gpu_image *p,int download,uint8_t *override) {
    int result=-1;size_t offset=0;dm_vk_pool *pool=pool_address_locked(p->address,p->storage.size,&offset);
    uint8_t *host=override?override:pool?pool->guest+offset:NULL;
    if(!host || p->desc.target==2 || (p->desc.flags&0x10))return -1;
    unsigned count=p->layers*p->desc.levels,bpp=p->desc.format==0x35?4:8;
    VkBufferImageCopy *regions=calloc(count*2,sizeof *regions);if(!regions)return -1;
    size_t size=0,max_pixels=0;unsigned k=0;
    for(unsigned layer=0;layer<p->layers;layer++)for(unsigned level=0;level<p->desc.levels;level++,k++) {
        VkExtent3D extent={image_mip(p->extent.width,level),image_mip(p->extent.height,level),1};size_t pixels=(size_t)extent.width*extent.height;
        size=(size+15)&~(size_t)15;regions[k*2]=(VkBufferImageCopy){.bufferOffset=size,.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,level,layer,1},.imageExtent=extent};size+=pixels*4;
        size=(size+15)&~(size_t)15;regions[k*2+1]=(VkBufferImageCopy){.bufferOffset=size,.imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,level,layer,1},.imageExtent=extent};size+=pixels;
        if(pixels>max_pixels)max_pixels=pixels;
    }
    uint8_t *linear=malloc(max_pixels*bpp);compute_scratch staging={0};VkCommandBuffer cmd=VK_NULL_HANDLE;
    if(!linear || scratch_new(&staging,size))goto done;
    if(!download)for(k=0;k<count;k++) {
        VkBufferImageCopy *r=&regions[k*2];size_t pixels=(size_t)r->imageExtent.width*r->imageExtent.height;
        if(nvn_deswizzle(&p->desc,r->imageSubresource.mipLevel,r->imageSubresource.baseArrayLayer,host,linear,(size_t)r->imageExtent.width*bpp))goto done;
        uint32_t *depth=(uint32_t *)((uint8_t *)staging.mapped+r->bufferOffset);uint8_t *stencil=(uint8_t *)staging.mapped+regions[k*2+1].bufferOffset;
        for(size_t i=0;i<pixels;i++) {
            uint32_t value;memcpy(&value,linear+i*bpp,4);depth[i]=bpp==4?value&0xffffff:value;stencil[i]=linear[i*bpp+(bpp==4?3:4)];
        }
    }
    if(!download && scratch_sync(&staging,0))goto done;
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=v.commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(vkAllocateCommandBuffers(v.device,&ca,&cmd))goto done;
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};if(vkBeginCommandBuffer(cmd,&bi))goto done;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=p->layout,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcAccessMask=p->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=download?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=p->image,.subresourceRange={p->aspect,0,p->desc.levels,0,p->layers}};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(download)vkCmdCopyImageToBuffer(cmd,p->image,VK_IMAGE_LAYOUT_GENERAL,staging.buffer,count*2,regions);
    else vkCmdCopyBufferToImage(cmd,staging.buffer,p->image,VK_IMAGE_LAYOUT_GENERAL,count*2,regions);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=download?VK_ACCESS_HOST_READ_BIT:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,download?VK_PIPELINE_STAGE_HOST_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);
    if(vkEndCommandBuffer(cmd))goto done;VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    if(vkQueueSubmit(v.queue,1,&si,VK_NULL_HANDLE) || vkQueueWaitIdle(v.queue))goto done;p->layout=VK_IMAGE_LAYOUT_GENERAL;
    if(download) {
        if(scratch_sync(&staging,1))goto done;
        for(k=0;k<count;k++) {
            VkBufferImageCopy *r=&regions[k*2];size_t pixels=(size_t)r->imageExtent.width*r->imageExtent.height;
            if(nvn_deswizzle(&p->desc,r->imageSubresource.mipLevel,r->imageSubresource.baseArrayLayer,host,linear,(size_t)r->imageExtent.width*bpp))goto done;
            uint32_t *depth=(uint32_t *)((uint8_t *)staging.mapped+r->bufferOffset);uint8_t *stencil=(uint8_t *)staging.mapped+regions[k*2+1].bufferOffset;
            for(size_t i=0;i<pixels;i++){uint32_t value=bpp==4?(depth[i]&0xffffff)|((uint32_t)stencil[i]<<24):depth[i];memcpy(linear+i*bpp,&value,4);if(bpp==8)linear[i*bpp+4]=stencil[i];}
            if(nvn_swizzle(&p->desc,r->imageSubresource.mipLevel,r->imageSubresource.baseArrayLayer,linear,(size_t)r->imageExtent.width*bpp,host))goto done;
            if(p->capture_shadow && nvn_swizzle(&p->desc,r->imageSubresource.mipLevel,r->imageSubresource.baseArrayLayer,linear,(size_t)r->imageExtent.width*bpp,p->upload_shadow))goto done;
        }
        if(!override && pool_sync_locked(pool,offset,p->storage.size,0))goto done;
    }
    result=0;
done:
    if(cmd){vkQueueWaitIdle(v.queue);vkFreeCommandBuffers(v.device,v.commands,1,&cmd);}scratch_free(&staging);free(linear);free(regions);return result;
}
