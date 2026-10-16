/* Scan-out displays stored color bytes. Blitting an sRGB VkImage directly to
 * UNORM decodes them; image copies, unlike blits, preserve those encoded bytes.
 * Each presentation slot owns its scratch until its submit fence completes. */
typedef struct {VkImage image;VkDeviceMemory memory;VkFormat format;uint32_t width,height;} present_pixels_image;
typedef struct {present_pixels_image source,destination;} present_pixels_scratch;
static VkFormat present_pixels_raw_format(VkFormat format) {
    switch(format) {
    case VK_FORMAT_R8_SRGB:return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_SRGB:return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8B8_SRGB:return VK_FORMAT_R8G8B8_UNORM;
    case VK_FORMAT_B8G8R8_SRGB:return VK_FORMAT_B8G8R8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default:return format;
    }
}
static void present_pixels_free(present_pixels_image *p) {
    if(p->image)vkDestroyImage(v.device,p->image,NULL);
    if(p->memory)device_memory_free(p->memory);
    memset(p,0,sizeof *p);
}
static void present_pixels_close(present_pixels_scratch *scratch) {
    present_pixels_free(&scratch->source);present_pixels_free(&scratch->destination);
}
static VkResult present_pixels_ensure(present_pixels_image *p,VkFormat format,uint32_t width,uint32_t height) {
    if(p->image && p->format==format && p->width==width && p->height==height)return VK_SUCCESS;
    // The owning slot's previous submit fence completed before this call.
    present_pixels_free(p);
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,
        .extent={width,height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkResult r=vkCreateImage(v.device,&ci,NULL,&p->image);if(r)goto fail;
    VkMemoryRequirements req;vkGetImageMemoryRequirements(v.device,p->image,&req);
    VkPhysicalDeviceMemoryProperties props=device_memory.properties;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<props.memoryTypeCount;i++)if(req.memoryTypeBits&(1u<<i)) {
        type=i;if(props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)break;
    }
    if(type==UINT32_MAX){r=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    if((r=device_memory_allocate(&ai,&p->memory,DEVICE_MEMORY_PRESENT)) || (r=vkBindImageMemory(v.device,p->image,p->memory,0)))goto fail;
    p->format=format;p->width=width;p->height=height;
    LOG("Vulkan encoded-byte present scratch: format=%u %ux%u",format,width,height);return VK_SUCCESS;
fail:present_pixels_free(p);return r;
}
static int present_pixels_direct(VkFormat format,int width,int height) {
    return present_pixels_raw_format(format)==present_pixels_raw_format(v.surface_format) &&
        (uint32_t)width==v.extent.width && (uint32_t)height==v.extent.height;
}
static VkFormatProperties present_pixels_format_properties(VkFormat format) {
    if((unsigned)format<256) {
        if(!v.format_properties_valid[format]) {
            vkGetPhysicalDeviceFormatProperties(v.gpu,format,&v.format_properties[format]);
            v.format_properties_valid[format]=1;
        }
        return v.format_properties[format];
    }
    VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(v.gpu,format,&properties);return properties;
}
static VkResult present_pixels_prepare(present_pixels_scratch *scratch,VkFormat format,int width,int height) {
    if(present_pixels_direct(format,width,height))return VK_SUCCESS;
    VkFormat source=present_pixels_raw_format(format),destination=present_pixels_raw_format(v.surface_format);
    VkFormatProperties sp=present_pixels_format_properties(source),dp=present_pixels_format_properties(destination);
    if(!(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkResult r;
    if(source!=format && (r=present_pixels_ensure(&scratch->source,source,width,height)))return r;
    if(destination!=v.surface_format && (r=present_pixels_ensure(&scratch->destination,destination,v.extent.width,v.extent.height)))return r;
    return VK_SUCCESS;
}
static void present_pixels_barrier(VkCommandBuffer cmd,VkImage image,int written) {
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=written?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcAccessMask=written?VK_ACCESS_TRANSFER_WRITE_BIT:0,.dstAccessMask=written?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd,written?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
}
static void present_pixels_encode(present_pixels_scratch *scratch,VkCommandBuffer cmd,gpu_image *src,VkImage destination,int x,int y,int width,int height) {
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffset={x,y,0},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={width,height,1}};
    if(present_pixels_direct(src->format,width,height)) {
        vkCmdCopyImage(cmd,src->image,VK_IMAGE_LAYOUT_GENERAL,destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);return;
    }
    VkImage source=src->image,output=destination;VkImageLayout output_layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if(present_pixels_raw_format(src->format)!=src->format) {
        source=scratch->source.image;present_pixels_barrier(cmd,source,0);
        vkCmdCopyImage(cmd,src->image,VK_IMAGE_LAYOUT_GENERAL,source,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        present_pixels_barrier(cmd,source,1);x=y=0;
    }
    if(present_pixels_raw_format(v.surface_format)!=v.surface_format) {
        output=scratch->destination.image;output_layout=VK_IMAGE_LAYOUT_GENERAL;present_pixels_barrier(cmd,output,0);
    }
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{x,y,0},{x+width,y+height,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{v.extent.width,v.extent.height,1}}};
    vkCmdBlitImage(cmd,source,VK_IMAGE_LAYOUT_GENERAL,output,output_layout,1,&blit,VK_FILTER_NEAREST);
    if(output!=destination) {
        present_pixels_barrier(cmd,output,1);copy.srcOffset=(VkOffset3D){0,0,0};copy.extent=(VkExtent3D){v.extent.width,v.extent.height,1};
        vkCmdCopyImage(cmd,output,VK_IMAGE_LAYOUT_GENERAL,destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    }
}
