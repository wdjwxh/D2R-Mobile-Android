#include "tex/nvn_deswizzle.h"
#include "vk_formats.inc"
#include "vk_image_memory.inc"
typedef struct gpu_image {
    struct pending_attachment_clear *pending_clear;
    struct gpu_image *next; uint64_t serial,address; nvn_texdesc desc; nvn_texlayout storage;
    struct gpu_image *hash_next,*dirty_next,**dirty_link;
    VkImage image; image_memory_allocation allocation; VkFormat format; VkExtent3D extent;
    uint32_t layers; VkImageUsageFlags usage; VkImageLayout layout;VkImageAspectFlags aspect;
    uint8_t *upload_shadow;
    uint64_t upload_stamp,checked_generation,last_compute_use,last_present_use;
    int capture_shadow,shadow_valid;
    int draw_dirty;
    int gpu_only_snapshot;
    int packaged;
    uint64_t frame_uploads;
    const char *upload_source;
} gpu_image;
static int attachment_clear_flush_image(gpu_image *p);
static gpu_image *gpu_images;
static void images_upload_report(void) {
    image_memory_report();
    gpu_image *top[3]={0};
    for(gpu_image *p=gpu_images;p;p=p->next)if(p->frame_uploads) {
        for(unsigned i=0;i<3;i++)if(!top[i] || p->frame_uploads>top[i]->frame_uploads) {
            for(unsigned j=2;j>i;j--)top[j]=top[j-1];top[i]=p;break;
        }
    }
    for(unsigned i=0;i<3;i++)if(top[i]) {
        gpu_image *p=top[i];
        LOG("Texture upload source: serial=%llu source=%s calls=%llu bytesPerUpload=%zu packaged=%d format=%d extent=%ux%ux%u levels=%u address=%#llx",
            (unsigned long long)p->serial,p->upload_source,(unsigned long long)p->frame_uploads,p->storage.size,
            p->packaged,p->desc.format,p->extent.width,p->extent.height,p->extent.depth,p->desc.levels,(unsigned long long)p->address);
    }
    for(gpu_image *p=gpu_images;p;p=p->next)p->frame_uploads=0;
}
#define IMAGE_SERIAL_BUCKETS 8192u
static gpu_image *image_serial_table[IMAGE_SERIAL_BUCKETS];
static gpu_image *packaged_dirty_images;
static unsigned packaged_dirty_count;
static int attachment_clears_peek_ready_locked(uint64_t address,size_t size);
static int images_peek_ready_locked(uint64_t address,size_t size) {
    // Inspect only active producers, never the complete asset registry on a
    // per-record path. Ordinary GPU targets do not mirror their reserved RAM.
    for(const gpu_image *p=packaged_dirty_images;p;p=p->dirty_next) {
        if(!p->packaged || !p->address || !p->storage.size)continue;
        if(address>=p->address?address-p->address<p->storage.size:p->address-address<size)return 0;
    }
    return attachment_clears_peek_ready_locked(address,size);
}
static unsigned images_cpu_owned_count(void) { return packaged_dirty_count; }
static void images_compute_reset(void) {
    for(gpu_image *p=gpu_images;p;p=p->next)p->last_compute_use=0;
}
static size_t image_shadow_bytes;
static unsigned image_serial_bucket(uint64_t serial) {
    serial^=serial>>33;serial*=UINT64_C(0xff51afd7ed558ccd);serial^=serial>>33;
    return (unsigned)serial&(IMAGE_SERIAL_BUCKETS-1);
}
static gpu_image *image_serial_get(uint64_t serial) {
    for(gpu_image *p=image_serial_table[image_serial_bucket(serial)];p;p=p->hash_next)
        if(p->serial==serial)return p;
    return NULL;
}
static void image_serial_remove(gpu_image *p) {
    gpu_image **link=&image_serial_table[image_serial_bucket(p->serial)];
    while(*link && *link!=p)link=&(*link)->hash_next;
    if(*link)*link=p->hash_next;
    p->hash_next=NULL;
}
// The device lock owns both the serial index and this dirty worklist. All
// producer/clear/retire paths use this helper, including attachment draws.
static void image_set_dirty(gpu_image *p,int dirty) {
    p->checked_generation=0;
    if(p->packaged && dirty && !p->dirty_link) {
        p->dirty_next=packaged_dirty_images;p->dirty_link=&packaged_dirty_images;
        if(p->dirty_next)p->dirty_next->dirty_link=&p->dirty_next;
        packaged_dirty_images=p;packaged_dirty_count++;
        pool_cpu_hazard_add();
    } else if(p->dirty_link && !dirty) {
        *p->dirty_link=p->dirty_next;
        if(p->dirty_next)p->dirty_next->dirty_link=p->dirty_link;
        p->dirty_link=NULL;p->dirty_next=NULL;packaged_dirty_count--;
        pool_cpu_hazard_remove();
    }
    p->draw_dirty=!!dirty;
}
static void images_close_locked(void) {
    if(compute_pending_flush())die("Vulkan image compute retirement failed during close");
    draw_attachments_close();
    while(gpu_images){gpu_image *p=gpu_images;gpu_images=p->next;image_set_dirty(p,0);graphics_image_cache_retire(p->image);vkDestroyImage(v.device,p->image,NULL);image_memory_release(&p->allocation);free(p->upload_shadow);free(p);}
    image_memory_closed();
    graphics_image_cache_close();
    memset(image_serial_table,0,sizeof image_serial_table);packaged_dirty_images=NULL;packaged_dirty_count=0;
    image_shadow_bytes=0;
}
static uint32_t image_mip(uint32_t x,unsigned l){x>>=l;return x?x:1;}
static int image_transfer(gpu_image *p,int download);
static int images_flush_all(void) {
    while(packaged_dirty_images)if(image_transfer(packaged_dirty_images,1))return -1;
    return 0;
}
static int images_flush_alias_range(uint64_t address,size_t size) {
    if(!packaged_dirty_count || !address || !size)return 0;
    for(gpu_image *p=packaged_dirty_images,*next;p;p=next) {
        next=p->dirty_next;
        if(p->address && address<p->address+p->storage.size && p->address<address+size && image_transfer(p,1))return -1;
    }
    return 0;
}
static int images_flush_range_at(uint64_t address,size_t size,const char *site) {
    if(reports_range_drain_at(address,size,site,"images_flush/mixed"))return -1;
    return images_flush_alias_range(address,size);
}
// FlushMappedRange follows a CPU write. Preserve the newly written intersection
// while materializing an older GPU image version, including untouched texels
// outside the CPU range. The caller uploads the restored bytes afterwards.
static int images_flush_cpu_overwrite(uint64_t address,size_t size) {
    if(reports_cpu_overwrite(address,size))return -1;
    if(!packaged_dirty_count || !address || !size)return 0;
    if(size>UINT64_MAX-address)return -1;
    for(gpu_image *p=packaged_dirty_images,*next;p;p=next) {
        next=p->dirty_next;
        uint64_t lo=address>p->address?address:p->address;
        uint64_t end=address+size,hi=end<p->address+p->storage.size?end:p->address+p->storage.size;
        if(!p->address || lo>=hi)continue;
        size_t bytes=(size_t)(hi-lo);uint8_t *saved=malloc(bytes);if(!saved)return -1;
        memcpy(saved,(const void *)(uintptr_t)lo,bytes);
        int result=image_transfer(p,1);
        // Restore even on transfer failure: the explicit CPU write must not
        // silently disappear after a partially completed GPU readback.
        dm_guest_write_prep((void *)(uintptr_t)lo,bytes);
        memcpy((void *)(uintptr_t)lo,saved,bytes);free(saved);
        if(result)return -1;
    }
    return 0;
}
static gpu_image *image_get(const dm_compute_image *d) {
    if(d->packaged && d->address) {
        nvn_texlayout storage;nvn_tex_layout(&d->desc,&storage);
        if(reports_range_drain(d->address,storage.size))return NULL;
    }
    gpu_image *have=image_serial_get(d->serial);
    // Only packaged images mirror pool bytes. Ordinary GPU images and their
    // feedback snapshots do not alias merely because NVN reserved the same RAM.
    if(d->packaged && d->address && packaged_dirty_count) {
        nvn_texlayout wanted;if(have)wanted=have->storage;else nvn_tex_layout(&d->desc,&wanted);
        for(gpu_image *p=packaged_dirty_images,*next;p;p=next) {
            next=p->dirty_next;
            if(p!=have && p->address && d->address<p->address+p->storage.size && p->address<d->address+wanted.size && image_transfer(p,1))return NULL;
        }
    }
    if(have)return have;
    if(d->desc.samples>1 || d->desc.target==10 || !d->desc.levels || d->desc.levels>16)return NULL;
    VkFormat format=image_format(d->desc.format);if(!format)return NULL;
    gpu_image *p=calloc(1,sizeof *p);if(!p)return NULL;
    p->serial=d->serial;p->address=d->address;p->desc=d->desc;p->format=format;nvn_tex_layout(&p->desc,&p->storage);
    p->packaged=d->packaged;
    p->aspect=d->desc.format==0x31?VK_IMAGE_ASPECT_STENCIL_BIT:
        d->desc.format>=0x32 && d->desc.format<=0x36?VK_IMAGE_ASPECT_DEPTH_BIT|(d->desc.format>=0x35?VK_IMAGE_ASPECT_STENCIL_BIT:0):VK_IMAGE_ASPECT_COLOR_BIT;
    p->layers=p->storage.layers;
    p->extent=(VkExtent3D){d->desc.width,d->desc.target==0 || d->desc.target==3?1:d->desc.height,d->desc.target==2?d->desc.depth:1};
    VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(v.gpu,format,&fp);
    p->usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)p->usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
    if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)p->usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)p->usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)p->usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if(p->desc.target!=2 && p->desc.target!=0 && p->desc.target!=3 &&
       (p->usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))p->usage|=VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType=p->desc.target==2?VK_IMAGE_TYPE_3D:(p->desc.target==0 || p->desc.target==3)?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D,
        .format=format,.extent=p->extent,.mipLevels=p->desc.levels,.arrayLayers=p->layers,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=p->usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    if(ic.imageType==VK_IMAGE_TYPE_2D && p->layers>=6 && p->extent.width==p->extent.height)ic.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if(vkCreateImage(v.device,&ic,NULL,&p->image))goto fail;
    if(image_memory_allocate(p->image,&p->allocation) ||
       vkBindImageMemory(v.device,p->image,p->allocation.memory,p->allocation.offset))goto fail;
    p->next=gpu_images;gpu_images=p;
    unsigned bucket=image_serial_bucket(p->serial);p->hash_next=image_serial_table[bucket];image_serial_table[bucket]=p;
    LOG("Vulkan image created: serial=%llu format=%u %ux%ux%u layers=%u levels=%u",(unsigned long long)p->serial,format,p->extent.width,p->extent.height,p->extent.depth,p->layers,p->desc.levels);
    return p;
fail:
    if(p->image)vkDestroyImage(v.device,p->image,NULL);
    image_memory_release(&p->allocation);free(p);return NULL;
}
static gpu_image *image_feedback_get(gpu_image *source) {
    if(source && attachment_clear_flush_image(source))return NULL;
    const uint64_t snapshot_bit=UINT64_C(0x4000000000000000);
    if(!source || source->gpu_only_snapshot || (source->serial&snapshot_bit))return NULL;
    uint64_t serial=source->serial|snapshot_bit;
    gpu_image *have=image_serial_get(serial);
    if(have)return have->gpu_only_snapshot && !have->address ? have : NULL;
    // No CPU backing: neither creation nor later descriptor lookup may flush
    // the source to guest memory or upload stale guest bytes into the copy.
    dm_compute_image desc={0};desc.serial=serial;desc.desc=source->desc;
    gpu_image *snapshot=image_get(&desc);
    if(snapshot)snapshot->gpu_only_snapshot=1;
    return snapshot;
}
static int image_feedback_encode(VkCommandBuffer cmd,gpu_image *source,gpu_image *snapshot,const VkImageSubresourceRange *requested) {
    if(!source || !snapshot || !snapshot->gpu_only_snapshot || snapshot->address ||
       source==snapshot || source->layout!=VK_IMAGE_LAYOUT_GENERAL ||
       (snapshot->layout!=VK_IMAGE_LAYOUT_UNDEFINED && snapshot->layout!=VK_IMAGE_LAYOUT_GENERAL) ||
       source->format!=snapshot->format || source->layers!=snapshot->layers ||
       source->desc.levels!=snapshot->desc.levels || source->aspect!=snapshot->aspect ||
       source->extent.width!=snapshot->extent.width || source->extent.height!=snapshot->extent.height ||
       source->extent.depth!=snapshot->extent.depth)return -1;
    VkImageSubresourceRange range={source->aspect,0,source->desc.levels,0,source->layers};
    VkImageSubresourceRange copied=range;
    if(source->desc.target!=2 && requested && requested->levelCount && requested->layerCount &&
       requested->baseMipLevel<source->desc.levels && requested->levelCount<=source->desc.levels-requested->baseMipLevel &&
       requested->baseArrayLayer<source->layers && requested->layerCount<=source->layers-requested->baseArrayLayer) {
        copied.baseMipLevel=requested->baseMipLevel;copied.levelCount=requested->levelCount;
        copied.baseArrayLayer=requested->baseArrayLayer;copied.layerCount=requested->layerCount;
    }
    // Layout is tracked for the whole snapshot. Keep whole-image barriers
    // even when copying fewer subresources, including its first transition.
    VkImageMemoryBarrier before[2]={
        {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
         .image=source->image,.subresourceRange=range},
        {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask=snapshot->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=snapshot->layout,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
         .image=snapshot->image,.subresourceRange=range}
    };
    // The reusable copy may still be read by an earlier pending draw. Encoding
    // this dependency in the new draw preserves order without a CPU wait.
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,2,before);
    VkImageCopy regions[48];unsigned count=0;
    uint64_t full_texels=0,copy_texels=0;
    const VkImageAspectFlags aspects[]={VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_ASPECT_STENCIL_BIT};
    for(unsigned a=0;a<3;a++)if(source->aspect&aspects[a])
        for(unsigned level=0;level<source->desc.levels;level++) {
            uint64_t mip_texels=(uint64_t)image_mip(source->extent.width,level)*image_mip(source->extent.height,level)*image_mip(source->extent.depth,level);
            full_texels+=mip_texels*source->layers;
            if(level<copied.baseMipLevel || level-copied.baseMipLevel>=copied.levelCount)continue;
            // Depth/stencil copy regions must select exactly one aspect.
            // Copy the union of redirected shader views, never just the
            // attachment view: post effects can sample other mip levels.
            VkImageSubresourceLayers layers={aspects[a],level,copied.baseArrayLayer,copied.layerCount};
            copy_texels+=mip_texels*copied.layerCount;
            regions[count++]=(VkImageCopy){.srcSubresource=layers,.dstSubresource=layers,
                .extent={image_mip(source->extent.width,level),image_mip(source->extent.height,level),image_mip(source->extent.depth,level)}};
        }
    vkCmdCopyImage(cmd,source->image,VK_IMAGE_LAYOUT_GENERAL,snapshot->image,VK_IMAGE_LAYOUT_GENERAL,count,regions);
    gpu_metrics.feedback_full_texels+=full_texels;gpu_metrics.feedback_copy_texels+=copy_texels;
    if(copy_texels<full_texels)gpu_metrics.feedback_partial_copies++;
    VkImageMemoryBarrier after[2]={before[0],before[1]};
    after[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    after[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    after[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    after[1].oldLayout=VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,0,NULL,0,NULL,2,after);
    // The caller commits snapshot->layout only when this command has actually
    // entered the pending queue or completed submission. Failed recording must
    // not publish an UNDEFINED -> GENERAL transition which never ran.
    return 0;
}
static int image_retire_serial_locked(uint64_t serial) {
    if(draw_pending_flush())return -1;
    const uint64_t feedback=serial|UINT64_C(0x4000000000000000);
    // Only CPU-backed assets need materialization before address reuse. An
    // ordinary GPU image is retired without ever overwriting its reserved RAM.
    gpu_image *canonical=image_serial_get(serial);
    if(canonical && canonical->packaged && canonical->draw_dirty && image_transfer(canonical,1))return -1;
    gpu_image **link=&gpu_images;
    while(*link) {
        gpu_image *p=*link;
        if(p->serial!=serial && !(p->serial==feedback && p->gpu_only_snapshot)){link=&p->next;continue;}
        // The compute FIFO owns views/descriptors referencing this image.
        // Retire those handles before freeing the image or its memory.
        if(compute_wait_serial(p->last_compute_use) || present_wait_serial_locked(p->last_present_use))return -1;
        *link=p->next;
        image_set_dirty(p,0);image_serial_remove(p);
        draw_attachments_retire_image(p->image);graphics_image_cache_retire(p->image);
        vkDestroyImage(v.device,p->image,NULL);image_memory_release(&p->allocation);
        if(p->upload_shadow)image_shadow_bytes-=p->storage.size;
        free(p->upload_shadow);free(p);
    }
    return 0;
}
static int images_retire_range_locked(uint64_t address,size_t size) {
    if(draw_pending_flush())return -1;
    for(;;) {
        gpu_image *p=gpu_images;
        while(p && (!p->address || address>=p->address+p->storage.size || p->address>=address+size))p=p->next;
        if(!p)return 0;
        if(image_retire_serial_locked(p->serial))return -1;
    }
}
int dm_vk_texture_retire(uint64_t serial) {
    pthread_mutex_lock(&lock);
    int result=image_retire_serial_locked(serial);
    pthread_mutex_unlock(&lock);return result;
}
#include "vk_depth_transfer.inc"
static int image_transfer_raw(gpu_image *p,int download,uint8_t *override) {
    if(p->gpu_only_snapshot)return download?-1:0;
    // GPU uploads preserve same-queue ordering without waiting on the CPU.
    // Explicit downloads and CPU/depth conversion retain their real completion.
    int synchronous=download || override || p->desc.format==0x35 || p->desc.format==0x36;
    if(synchronous) {
        if(draw_pending_flush() || compute_wait_serial(p->last_compute_use))return -1;
    }
    if(!p->address && !override && download)return 0;
    if(!override && pools_cpu_read_address_locked(p->address,p->storage.size))return -1;
    if(p->desc.format==0x35 || p->desc.format==0x36)return depth_transfer(p,download,override);
    int result=-1;size_t offset=0;dm_vk_pool *pool=pool_address_locked(p->address,p->storage.size,&offset);
    uint8_t *host=override?override:pool?pool->guest+offset:NULL;
    if(!host && p->address)return -1;
    unsigned count=p->layers*p->desc.levels;
    VkBufferImageCopy *regions=calloc(count,sizeof *regions);if(!regions)return -1;
    size_t bytes=0;int bw,bh,bpb=nvn_fmt_bpb(p->desc.format,&bw,&bh);unsigned k=0;
    for(unsigned layer=0;layer<p->layers;layer++)for(unsigned level=0;level<p->desc.levels;level++,k++) {
        VkExtent3D e={image_mip(p->extent.width,level),image_mip(p->extent.height,level),image_mip(p->extent.depth,level)};
        size_t align=16*(size_t)bpb;bytes=((bytes+align-1)/align)*align;
        regions[k]=(VkBufferImageCopy){.bufferOffset=bytes,.imageSubresource={p->aspect,level,layer,1},.imageExtent=e};
        bytes+=(size_t)((e.width+bw-1)/bw)*bpb*((e.height+bh-1)/bh)*e.depth;
    }
    compute_scratch staging={0};VkCommandBuffer cmd=VK_NULL_HANDLE;uint64_t draw_serial=0;
    if(!synchronous && compute_texture_room(bytes))goto done;
    if(scratch_new(&staging,bytes))goto done;
    if(!download && !override && p->packaged && pool && !nvn_dirty_tracked(host,p->storage.size)) {
        const size_t cap=256u*1024*1024;
        if(!p->upload_shadow && p->storage.size<=cap-image_shadow_bytes) {
            p->upload_shadow=malloc(p->storage.size);
            if(p->upload_shadow)image_shadow_bytes+=p->storage.size;
        }
        // Untracked guest writes cannot invalidate a post-upload snapshot.
        // Capture once and deswizzle exactly this version, so cache validation
        // describes the submitted staging contents rather than a later guest read.
        if(p->upload_shadow) {
            memcpy(p->upload_shadow,host,p->storage.size);host=p->upload_shadow;
            p->capture_shadow=1;
        }
    }
    if(!download && host)for(k=0;k<count;k++) {
        VkBufferImageCopy *r=&regions[k];size_t pitch=(size_t)((r->imageExtent.width+bw-1)/bw)*bpb;
        uint8_t *dst=(uint8_t *)staging.mapped+r->bufferOffset;
        if(p->desc.flags&0x10) {
            if(p->desc.levels!=1 || p->layers!=1)goto done;
            size_t stride=p->desc.stride?p->desc.stride:(pitch+31)&~(size_t)31;
            for(unsigned row=0;row<(r->imageExtent.height+bh-1)/(unsigned)bh;row++)memcpy(dst+row*pitch,host+row*stride,pitch);
        } else if(nvn_deswizzle(&p->desc,r->imageSubresource.mipLevel,r->imageSubresource.baseArrayLayer,host,dst,pitch))goto done;
    }
    if(!download && scratch_sync(&staging,0))goto done;
    if(synchronous) {
        VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=v.commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        if(vkAllocateCommandBuffers(v.device,&ca,&cmd))goto done;
        VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if(vkBeginCommandBuffer(cmd,&bi))goto done;
    } else if(draw_compute_begin_locked(&cmd,&draw_serial,staging.size))goto done;
    // No fallible preparation or callback below this point for a shared CB.
    VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=p->layout,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=p->image,
        .subresourceRange={p->aspect,0,p->desc.levels,0,p->layers},
        .srcAccessMask=p->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=download?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_TRANSFER_WRITE_BIT};
    VkMemoryBarrier staging_ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,download?0:1,download?NULL:&staging_ready,0,NULL,1,&ib);
    if(download)vkCmdCopyImageToBuffer(cmd,p->image,VK_IMAGE_LAYOUT_GENERAL,staging.buffer,count,regions);
    else vkCmdCopyBufferToImage(cmd,staging.buffer,p->image,VK_IMAGE_LAYOUT_GENERAL,count,regions);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=download?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=download?VK_ACCESS_HOST_READ_BIT:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,download?VK_PIPELINE_STAGE_HOST_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);
    if(!synchronous) {
        uint64_t serial=compute_texture_record_locked(cmd,draw_serial,&staging);cmd=VK_NULL_HANDLE;
        p->last_compute_use=serial;p->layout=VK_IMAGE_LAYOUT_GENERAL;
        gpu_metrics.texture_async_uploads++;result=0;goto done;
    }
    if(vkEndCommandBuffer(cmd))goto done;
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    VkResult submitted=vkQueueSubmit(v.queue,1,&si,VK_NULL_HANDLE);
    if(submitted)die("Vulkan synchronous image transfer submit failed; retaining resources");
    if(vkQueueWaitIdle(v.queue))die("Vulkan synchronous image transfer completion failed; retaining resources");
    p->layout=VK_IMAGE_LAYOUT_GENERAL;
    if(download) {
        if(scratch_sync(&staging,1))goto done;
        for(k=0;k<count;k++) {
            VkBufferImageCopy *r=&regions[k];size_t pitch=(size_t)((r->imageExtent.width+bw-1)/bw)*bpb;
            uint8_t *src=(uint8_t *)staging.mapped+r->bufferOffset;
            if(p->desc.flags&0x10) {
                size_t stride=p->desc.stride?p->desc.stride:(pitch+31)&~(size_t)31;
                for(unsigned row=0;row<(r->imageExtent.height+bh-1)/(unsigned)bh;row++)memcpy(host+row*stride,src+row*pitch,pitch);
                if(p->capture_shadow)for(unsigned row=0;row<(r->imageExtent.height+bh-1)/(unsigned)bh;row++)memcpy(p->upload_shadow+row*stride,src+row*pitch,pitch);
            } else {
                if(nvn_swizzle(&p->desc,r->imageSubresource.mipLevel,r->imageSubresource.baseArrayLayer,src,pitch,host))goto done;
                if(p->capture_shadow && nvn_swizzle(&p->desc,r->imageSubresource.mipLevel,r->imageSubresource.baseArrayLayer,src,pitch,p->upload_shadow))goto done;
            }
        }
        if(!override && pool_sync_locked(pool,offset,p->storage.size,0))goto done;
    }
    result=0;
done:
    // Async recording moved staging into the FIFO and the body into its batch. Any remaining command
    // is unsubmitted, or belongs to the completed synchronous fallback above.
    if(cmd)vkFreeCommandBuffers(v.device,v.commands,1,&cmd);
    scratch_free(&staging);free(regions);return result;
}
// Non-packaged textures are GPU objects, not mirrors of their reserved NVN
// address range. Initial creation only transitions layout; guest bytes have no
// authority over these textures. Missing-binding dummy images remain zeroed.
static int image_gpu_prepare(gpu_image *p) {
    if(p->layout!=VK_IMAGE_LAYOUT_UNDEFINED)return 0;
    if(!p->address)return image_transfer_raw(p,0,NULL);
    VkCommandBuffer cmd=VK_NULL_HANDLE;int result=-1;
    uint64_t draw_serial=0;
    if(draw_compute_begin_locked(&cmd,&draw_serial,0))goto done;
    // Final preparation can recursively initialize this image while flushing
    // deferred clears. Preserve that version instead of discarding it again.
    VkImageLayout old_layout=p->layout;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=old_layout,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=p->image,
        .subresourceRange={p->aspect,0,p->desc.levels,0,p->layers},
        .srcAccessMask=old_layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,old_layout==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
    uint64_t serial=compute_texture_record_locked(cmd,draw_serial,NULL);cmd=VK_NULL_HANDLE;
    p->last_compute_use=serial;p->layout=VK_IMAGE_LAYOUT_GENERAL;
    gpu_metrics.texture_async_initializations++;result=0;
done:
    if(cmd)vkFreeCommandBuffers(v.device,v.commands,1,&cmd);return result;
}
static int image_transfer(gpu_image *p,int download) {
    if(attachment_clear_flush_image(p))return -1;
    if(p->packaged && p->address && reports_range_drain(p->address,p->storage.size))return -1;
    if(p->gpu_only_snapshot)return download?-1:0;
    // Explicit CPU consumers use image_cpu_read/write with independent storage.
    // Reject accidental ordinary-RT readback instead of pretending it occurred.
    if(!p->packaged)return download?-1:image_gpu_prepare(p);
    if(!download && p->draw_dirty)return 0;
    size_t offset=0;dm_vk_pool *pool=p->address?pool_address_locked(p->address,p->storage.size,&offset):NULL;
    // The identical report range was drained above. Only conditions and a
    // pool lookup intervened; preserve writer materialization without scanning
    // those reports a second time, and retain this consumer's attribution.
    if(pool && pool_cpu_read_dirty_locked(pool,offset,p->storage.size,__func__))return -1;
    void *guest=pool?pool->guest+offset:NULL;
    int tracked=guest && nvn_dirty_tracked(guest,p->storage.size);
    if(tracked)gpu_metrics.tex_tracked++;else gpu_metrics.tex_untracked++;
    if(!download && tracked && p->upload_stamp) {
        // Upstream NvPkg.seen: skip the per-page scan if no tracked guest write
        // occurred. Capture BEFORE the scan, so writes during/after it change
        // the next check's generation instead of being blessed clean.
        uint64_t now=nvn_dirty_now();
        if(p->checked_generation==now){gpu_metrics.tex_generation_hits++;return 0;}
        double checked_at=gpu_detail_time_ms();int dirty=nvn_dirty_since(guest,p->storage.size,p->upload_stamp);
        gpu_metrics.tex_page_ms+=gpu_detail_time_ms()-checked_at;gpu_metrics.tex_page_scans++;
        if(!dirty) {
            p->checked_generation=now;return 0;
        }
    }
    p->checked_generation=0;
    // Re-establish a clean generation from the exact GPU readback, not from a
    // post-copy snapshot of guest memory (which could include concurrent writes).
    if(!download && tracked && p->shadow_valid) {
        nvn_dirty_protect(guest,p->storage.size);uint64_t seen=nvn_dirty_now();
        double compared_at=gpu_detail_time_ms();int changed=memcmp(guest,p->upload_shadow,p->storage.size);
        gpu_metrics.tex_compare_ms+=gpu_detail_time_ms()-compared_at;gpu_metrics.tex_compare_bytes+=p->storage.size;
        if(!changed){p->upload_stamp=seen;return 0;}
    }
    // Exact byte comparison is the fallback for untracked packaged pool pages.
    if(!download && !tracked && pool && p->upload_shadow && p->shadow_valid) {
        double compared_at=gpu_detail_time_ms();int changed=memcmp(p->upload_shadow,pool->guest+offset,p->storage.size);
        gpu_metrics.tex_compare_ms+=gpu_detail_time_ms()-compared_at;gpu_metrics.tex_compare_bytes+=p->storage.size;
        if(!changed)return 0;
    }
    uint64_t stamp=0;
    if(tracked && !download) {
        // Same ordering as upstream pkg_upload_: protect before taking the
        // content generation so writes during upload force a later refresh.
        nvn_dirty_protect(guest,p->storage.size);stamp=nvn_dirty_now();
    }
    if(tracked && download)dm_guest_write_prep(guest,p->storage.size);
    p->capture_shadow=0;
    if(download && tracked) {
        const size_t cap=256u*1024*1024;
        if(!p->upload_shadow && p->storage.size<=cap-image_shadow_bytes) {
            p->upload_shadow=malloc(p->storage.size);
            if(p->upload_shadow)image_shadow_bytes+=p->storage.size;
        }
        if(p->upload_shadow) {
            // Keep layout padding; the raw transfer overwrites all actual
            // texels in this independent snapshot using GPU staging bytes.
            memcpy(p->upload_shadow,guest,p->storage.size);p->capture_shadow=1;
        }
    }
    p->shadow_valid=0;
    double transfer_started=gpu_time_ms();
    int result=image_transfer_raw(p,download,NULL);
    gpu_metrics.image_ms+=gpu_time_ms()-transfer_started;
    if(download){gpu_metrics.downloads++;gpu_metrics.down_bytes+=p->storage.size;}
    else{gpu_metrics.uploads++;gpu_metrics.up_bytes+=p->storage.size;p->frame_uploads++;p->upload_source="pool-refresh";}
    if(!result && download)image_set_dirty(p,0);
    if(!result && p->capture_shadow)p->shadow_valid=1;
    p->capture_shadow=0;
    if(!result && tracked) {
        // A host writer may race the readback. Only a CPU->GPU upload can
        // establish an exact clean generation; do not bless concurrent writes.
        if(download)stamp=0;
        p->upload_stamp=stamp;
    }
    if(!result && pool && !tracked && (download || p->desc.format==0x35 || p->desc.format==0x36)) {
        const size_t cap=256u*1024*1024;
        if(!p->upload_shadow && p->storage.size<=cap-image_shadow_bytes) {
            p->upload_shadow=malloc(p->storage.size);
            if(p->upload_shadow)image_shadow_bytes+=p->storage.size;
        }
        if(p->upload_shadow){memcpy(p->upload_shadow,pool->guest+offset,p->storage.size);p->shadow_valid=1;}
    }
    return result;
}
static int image_gpu_written(gpu_image *p) {
    image_set_dirty(p,1);p->upload_stamp=0;p->shadow_valid=0;
    return p->packaged && !draw_batch?image_transfer(p,1):0;
}
// CPU conversion fallback / explicit texture read: never write an ordinary
// render target back into a pool merely because its GPU work completed.
static uint8_t *image_cpu_read(gpu_image *p) {
    uint8_t *bytes=calloc(1,p->storage.size);if(!bytes)return NULL;
    if(p->packaged) {
        size_t off=0;dm_vk_pool *pool=pool_address_locked(p->address,p->storage.size,&off);
        if(!pool || (p->draw_dirty && image_transfer(p,1)) || pool_cpu_read_locked(pool,off,p->storage.size)){free(bytes);return NULL;}
        memcpy(bytes,pool->guest+off,p->storage.size);
    } else {
        if(image_transfer(p,0)){free(bytes);return NULL;}
        double started=gpu_time_ms();int result=image_transfer_raw(p,1,bytes);
        gpu_metrics.image_ms+=gpu_time_ms()-started;gpu_metrics.downloads++;gpu_metrics.down_bytes+=p->storage.size;
        if(result){free(bytes);return NULL;}
    }
    return bytes;
}
static int image_cpu_write(gpu_image *p,uint8_t *bytes) {
    if(p->packaged) {
        size_t off=0;dm_vk_pool *pool=pool_address_locked(p->address,p->storage.size,&off);if(!pool)return -1;
        dm_guest_write_prep(pool->guest+off,p->storage.size);memcpy(pool->guest+off,bytes,p->storage.size);
        image_set_dirty(p,0);p->upload_stamp=0;p->shadow_valid=0;
        if(pool_sync_locked(pool,off,p->storage.size,0))return -1;
        return image_transfer(p,0);
    }
    double started=gpu_time_ms();int result=image_transfer_raw(p,0,bytes);
    gpu_metrics.image_ms+=gpu_time_ms()-started;gpu_metrics.uploads++;gpu_metrics.up_bytes+=p->storage.size;
    p->frame_uploads++;p->upload_source="cpu-write";
    if(result)return -1;
    return image_gpu_written(p);
}
// Draws can only change their scissor rectangle. Download that rectangle and
// update canonical Tegra storage directly, preserving every other mip/layer.
static int image_download_rect(gpu_image *p,const dm_compute_image *a,VkRect2D rect) {
    if(draw_pending_flush())return -1;
    p->upload_stamp=0;p->checked_generation=0;p->shadow_valid=0;
    if(!rect.extent.width || !rect.extent.height)return 0;
    if(p->desc.levels==1 && p->layers==1 && (uint64_t)rect.extent.width*rect.extent.height>(uint64_t)p->extent.width*p->extent.height/2)return image_transfer(p,1);
    int bw,bh,bpp=nvn_fmt_bpb(p->desc.format,&bw,&bh);
    if(p->aspect!=VK_IMAGE_ASPECT_COLOR_BIT || bw!=1 || bh!=1 || p->desc.target==2 || a->layers!=1)return image_transfer(p,1);
    size_t off=0;dm_vk_pool *pool=pool_address_locked(p->address,p->storage.size,&off);if(!pool)return -1;
    if(pool_cpu_read_locked(pool,off,p->storage.size))return -1;
    size_t pitch=(size_t)rect.extent.width*bpp,bytes=pitch*rect.extent.height;
    compute_scratch staging={0};VkCommandBuffer cmd=VK_NULL_HANDLE;int result=-1;
    if(scratch_new(&staging,bytes))goto done;
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=v.commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(vkAllocateCommandBuffers(v.device,&ca,&cmd))goto done;
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};if(vkBeginCommandBuffer(cmd,&bi))goto done;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=p->image,.subresourceRange={p->aspect,a->base_level,1,a->base_layer,1}};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy region={.imageSubresource={p->aspect,a->base_level,a->base_layer,1},.imageOffset={rect.offset.x,rect.offset.y,0},.imageExtent={rect.extent.width,rect.extent.height,1}};
    vkCmdCopyImageToBuffer(cmd,p->image,VK_IMAGE_LAYOUT_GENERAL,staging.buffer,1,&region);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);
    if(vkEndCommandBuffer(cmd))goto done;
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    if(vkQueueSubmit(v.queue,1,&si,VK_NULL_HANDLE) || vkQueueWaitIdle(v.queue) || scratch_sync(&staging,1))goto done;
    uint32_t row_bytes=0,rows=0,slices=0,block_pixel=0;uint64_t base=0;unsigned th=0,td=0;
    int linear=(p->desc.flags&0x10)!=0;
    if(!linear && nvn_deswizzle_geom(&p->desc,a->base_level,a->base_layer,&row_bytes,&rows,&slices,&base,&th,&td,&block_pixel))goto done;
    size_t stride=p->desc.stride?p->desc.stride:(((size_t)p->extent.width*bpp+31)&~(size_t)31);
    // Direct mapped writes bypass pool_sync; invalidate before the first row
    // so partial-copy or flush failures cannot retain stale page snapshots.
    if(compute_wait_serial(pool->last_compute_use))goto done;
    pool_shadow_invalidate(pool,off,p->storage.size);
    dm_guest_write_prep(pool->guest+off,p->storage.size);
    for(unsigned y=0;y<rect.extent.height;y++)for(size_t x=0;x<pitch;) {
        uint32_t xb=(uint32_t)rect.offset.x*bpp+x,yb=rect.offset.y+y;
        size_t n=16-(xb&15);if(n>pitch-x)n=pitch-x;
        uint64_t dest=linear?(uint64_t)yb*stride+xb:base+nvn_bl_offset(xb,yb,0,row_bytes,rows,th,td);
        if(dest>p->storage.size || n>p->storage.size-dest)goto done;
        const uint8_t *src=(uint8_t *)staging.mapped+y*pitch+x;
        memcpy(pool->guest+off+dest,src,n);
        if(pool->mapped)memcpy(pool->mapped+off+dest,src,n);
        if(p->upload_shadow)memcpy(p->upload_shadow+dest,src,n);
        x+=n;
    }
    if(pool->mapped && !pool->coherent) {
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=pool->memory,.offset=0,.size=VK_WHOLE_SIZE};
        if(vkFlushMappedMemoryRanges(v.device,1,&range))goto done;
    }
    result=0;
done:
    if(cmd){vkQueueWaitIdle(v.queue);vkFreeCommandBuffers(v.device,v.commands,1,&cmd);}scratch_free(&staging);return result;
}
static VkSamplerAddressMode sampler_wrap(unsigned mode) {
    if(mode==1)return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if(mode==6)return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    if(mode==2 || mode==3)return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    if(mode==4 || mode==5)return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}
