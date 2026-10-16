/* Keep compatible image copies on the GPU, as upstream does. CPU copies are
 * reserved for aliased images and cross-dimensional cases; materialize their
 * guest ranges before preserving untouched Tegra-layout texels. */
typedef struct { size_t row,rows,plane,size; unsigned depth; } region_layout;
static int region_layout_get(const dm_compute_image *d,const dm_image_region *r,int64_t stride,region_layout *out) {
    int bw,bh,bpb=nvn_fmt_bpb(d->desc.format,&bw,&bh);
    if(!bpb || d->base_level<0 || (unsigned)d->base_level>=d->desc.levels || d->base_layer<0 || stride<0 ||
        r->x<0 || r->y<0 || r->z<0 || r->width<=0 || r->height<=0 || r->depth<0)return -1;
    unsigned w=image_mip(d->desc.width,d->base_level),h=d->desc.target==0 || d->desc.target==3?1:image_mip(d->desc.height,d->base_level);
    unsigned nz=r->depth?r->depth:1;
    nvn_texlayout storage;nvn_tex_layout(&d->desc,&storage);
    unsigned depth=d->desc.target==2?image_mip(d->desc.depth,d->base_level):storage.layers;
    uint64_t start=d->desc.target==2?r->z:(uint64_t)d->base_layer+r->z;
    if((uint64_t)r->x+r->width>w || (uint64_t)r->y+r->height>h || start+nz>depth || r->x%bw || r->y%bh ||
       (r->width%bw && (unsigned)(r->x+r->width)!=w) || (r->height%bh && (unsigned)(r->y+r->height)!=h))return -1;
    size_t row=(size_t)((r->width+bw-1)/bw)*bpb,rows=(r->height+bh-1)/bh;
    if(stride && (uint64_t)stride<row)return -1;
    size_t pitch=stride?(size_t)stride:row;
    if(pitch>SIZE_MAX/rows || pitch*rows>SIZE_MAX/nz)return -1;
    *out=(region_layout){row,rows,pitch*rows,pitch*rows*nz,nz};return 0;
}
static int region_move(const dm_compute_image *d,const dm_image_region *r,uint8_t *packed,uint8_t *buffer,size_t pitch,int download) {
    region_layout region;if(region_layout_get(d,r,pitch,&region))return -1;
    nvn_texlayout storage;nvn_tex_layout(&d->desc,&storage);if(!packed)return -1;
    int bw,bh,bpb=nvn_fmt_bpb(d->desc.format,&bw,&bh),is3d=d->desc.target==2;
    unsigned w=image_mip(d->desc.width,d->base_level),h=d->desc.target==0 || d->desc.target==3?1:image_mip(d->desc.height,d->base_level);
    unsigned depth=is3d?image_mip(d->desc.depth,d->base_level):1;
    size_t row=(size_t)((w+bw-1)/bw)*bpb,rows=(h+bh-1)/bh,plane=row*rows;
    if(plane>SIZE_MAX/depth)return -1;
    uint8_t *linear=malloc(plane*depth);if(!linear)return -1;
    int result=-1;
    for(unsigned z=0;z<region.depth;z++) {
        unsigned layer=is3d?0:d->base_layer+r->z+z;
        if(!is3d || !z) {
            if(d->desc.flags&0x10) {
                if(d->base_level || storage.layers!=1 || depth!=1)goto done;
                size_t srcrow=d->desc.stride?d->desc.stride:(row+31)&~(size_t)31;
                if(srcrow<row || srcrow>storage.size/rows)goto done;
                for(size_t y=0;y<rows;y++)memcpy(linear+y*row,packed+y*srcrow,row);
            } else if(nvn_deswizzle(&d->desc,d->base_level,layer,packed,linear,row))goto done;
        }
        uint8_t *texels=linear+(is3d?(r->z+z)*plane:0)+(r->y/bh)*row+(r->x/bw)*bpb;
        for(size_t y=0;y<region.rows;y++) {
            uint8_t *buf=buffer+z*region.plane+y*pitch;
            if(download)memcpy(buf,texels+y*row,region.row);else memcpy(texels+y*row,buf,region.row);
        }
        if(!download && (!is3d || z+1==region.depth)) {
            if(d->desc.flags&0x10) {
                size_t dstrow=d->desc.stride?d->desc.stride:(row+31)&~(size_t)31;
                for(size_t y=0;y<rows;y++)memcpy(packed+y*dstrow,linear+y*row,row);
            } else if(nvn_swizzle(&d->desc,d->base_level,layer,linear,row,packed))goto done;
        }
    }
    result=0;
done:free(linear);return result;
}
// NVN CPU texture reads follow upstream's split: packaged bytes come directly
// from the pool; ordinary images use an explicit GPU snapshot. Stride gaps in
// the caller's destination must remain untouched.
int dm_vk_image_read_texels(const dm_compute_image *d,const dm_image_region *r,void *destination,int64_t row_stride,int64_t image_stride) {
    pthread_mutex_lock(&lock);
    int result=-1;region_layout tight;uint8_t *packed=NULL,*linear=NULL;
    if(!v.device || !destination || image_stride<0 || region_layout_get(d,r,0,&tight) || row_stride<0)goto done;
    size_t row=row_stride?(size_t)row_stride:tight.row;
    if(row<tight.row || row>SIZE_MAX/tight.rows)goto done;
    size_t plane=image_stride?(size_t)image_stride:row*tight.rows;
    if(plane<row*tight.rows || plane>SIZE_MAX/tight.depth)goto done;
    size_t span=(tight.depth-1)*plane+(tight.rows-1)*row+tight.row;
    if(span>UINTPTR_MAX-(uintptr_t)destination || draw_pending_flush())goto done;
    const uint8_t *source=NULL;
    if(d->packaged) {
        nvn_texlayout storage;nvn_tex_layout(&d->desc,&storage);size_t off=0;
        dm_vk_pool *pool=pool_address_locked(d->address,storage.size,&off);
        if(!pool || images_flush_range(d->address,storage.size) || pool_cpu_read_locked(pool,off,storage.size))goto done;
        source=pool->guest+off;
    } else {
        gpu_image *image=image_get(d);
        if(!image || !(packed=image_cpu_read(image)))goto done;
        source=packed;
    }
    linear=malloc(tight.size);if(!linear)goto done;
    if(region_move(d,r,(uint8_t *)source,linear,tight.row,1))goto done;
    // Snapshot before writing also handles a caller whose output aliases a
    // packaged source pool. No ordinary RT bytes are written into that pool.
    dm_guest_write_prep(destination,span);
    for(unsigned z=0;z<tight.depth;z++)for(size_t y=0;y<tight.rows;y++) {
        if(pools_cpu_write_address_locked((uintptr_t)destination+z*plane+y*row,tight.row))goto done;
        memcpy((uint8_t *)destination+z*plane+y*row,linear+z*tight.plane+y*tight.row,tight.row);
    }
    result=0;
done:
    free(linear);free(packed);pthread_mutex_unlock(&lock);return result;
}
int dm_vk_image_buffer(const dm_compute_image *d,const dm_image_region *r,uint64_t address,int64_t stride,int download) {
    pthread_mutex_lock(&lock);
    if(draw_pending_flush()){pthread_mutex_unlock(&lock);return -1;}
    int result=-1;region_layout layout;size_t offset;uint8_t *snapshot=NULL,*packed=NULL;
    if(!v.device || region_layout_get(d,r,stride,&layout))goto done;
    dm_vk_pool *pool=pool_address_locked(address,layout.size,&offset);if(!pool)return pthread_mutex_unlock(&lock),-1;
    if(images_flush_range(address,layout.size) || pool_cpu_read_locked(pool,offset,layout.size))goto done;
    gpu_image *image=image_get(d);if(!image || !(packed=image_cpu_read(image)))goto done;
    snapshot=malloc(layout.size);if(!snapshot)goto done;
    // Retain inter-row padding in the destination buffer.
    memcpy(snapshot,pool->guest+offset,layout.size);
    result=region_move(d,r,packed,snapshot,stride?(size_t)stride:layout.row,download);
    if(!result) {
        if(download){dm_guest_write_prep(pool->guest+offset,layout.size);memcpy(pool->guest+offset,snapshot,layout.size);result=pool_sync_locked(pool,offset,layout.size,0);}
        else result=image_cpu_write(image,packed);
    }
done:
    free(snapshot);free(packed);
    if(result)LOG("Vulkan image/buffer copy failed: serial=%llu download=%d region=%d,%d,%d %dx%dx%d stride=%lld",(unsigned long long)d->serial,download,r->x,r->y,r->z,r->width,r->height,r->depth,(long long)stride);
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_image_copy(const dm_compute_image *src,const dm_image_region *sr,const dm_compute_image *dst,const dm_image_region *dr) {
    pthread_mutex_lock(&lock);
    int result=-1;region_layout sl,dl;
    if(!v.device || region_layout_get(src,sr,0,&sl) || region_layout_get(dst,dr,0,&dl) ||
       src->desc.format!=dst->desc.format || sr->width!=dr->width || sr->height!=dr->height || sl.depth!=dl.depth)goto done;
    nvn_texlayout src_storage,dst_storage;
    nvn_tex_layout(&src->desc,&src_storage);nvn_tex_layout(&dst->desc,&dst_storage);
    int alias=src->serial==dst->serial || (src->address && dst->address &&
        src->address<dst->address+dst_storage.size && dst->address<src->address+src_storage.size);
    // Vulkan 1.1's 2D/3D compatibility rules need separate handling. Preserve
    // the existing linear snapshot path for those and all memory aliases.
    int src_dimension=src->desc.target==2?3:(src->desc.target==0 || src->desc.target==3)?1:2;
    int dst_dimension=dst->desc.target==2?3:(dst->desc.target==0 || dst->desc.target==3)?1:2;
    if(!alias && src_dimension==dst_dimension) {
        gpu_image *a=image_get(src),*b=image_get(dst);
        if(!a || !b || a->image==b->image || a->format!=b->format || a->aspect!=b->aspect ||
           image_transfer(a,0) || image_transfer(b,0))goto done;
        VkCommandBuffer cmd=VK_NULL_HANDLE;uint64_t draw_serial=0;
        if(draw_compute_begin_locked(&cmd,&draw_serial,0))goto done;
        if(compute_submission_count==COMPUTE_SUBMISSION_SLOTS || compute_submitted_serial==UINT64_MAX)
            die("Vulkan shared image copy FIFO changed after preparation");
        // Record and publish both owners without callbacks or recoverable
        // failures. This shared command buffer belongs solely to the draw FIFO.
        VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
        VkImageCopy copies[2];unsigned count=0;
        for(unsigned aspect=VK_IMAGE_ASPECT_COLOR_BIT;aspect<=VK_IMAGE_ASPECT_STENCIL_BIT;aspect<<=1)if(a->aspect&aspect) {
            int is3d=src_dimension==3;
            copies[count++]=(VkImageCopy){
                .srcSubresource={aspect,src->base_level,is3d?0:src->base_layer+sr->z,is3d?1:sl.depth},
                .srcOffset={sr->x,sr->y,is3d?sr->z:0},
                .dstSubresource={aspect,dst->base_level,is3d?0:dst->base_layer+dr->z,is3d?1:dl.depth},
                .dstOffset={dr->x,dr->y,is3d?dr->z:0},
                .extent={sr->width,sr->height,is3d?sl.depth:1}};
        }
        vkCmdCopyImage(cmd,a->image,VK_IMAGE_LAYOUT_GENERAL,b->image,VK_IMAGE_LAYOUT_GENERAL,count,copies);
        mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);
        compute_submission *slot=&compute_submissions[(compute_submission_head+compute_submission_count)%COMPUTE_SUBMISSION_SLOTS];
        uint64_t serial=++compute_submitted_serial;
        slot->serial=serial;slot->cmd=VK_NULL_HANDLE;
        slot->pending_shared=1;slot->completion_draw_serial=draw_serial;
        slot->pool_ranges_complete=1;slot->pool_range_count=0;
        draw_compute_record_locked(cmd,serial,draw_serial);
        compute_submission_count++;pool_cpu_hazard_add();
        perf_metrics.shared_image_copies++;
        // Both source reads and destination writes must finish before either
        // image can be retired or materialized by a real CPU consumer.
        a->last_compute_use=serial;b->last_compute_use=serial;
        image_set_dirty(b,1);b->upload_stamp=0;b->shadow_valid=0;
        result=0;
        goto done;
    }
    if(draw_pending_flush())goto done;
    gpu_image *source=image_get(src),*destination=image_get(dst);
    if(!source || !destination)goto done;
    uint8_t *source_bytes=image_cpu_read(source),*destination_bytes=NULL;
    uint8_t *snapshot=malloc(sl.size);
    if(!source_bytes || !snapshot){free(source_bytes);free(snapshot);goto done;}
    result=region_move(src,sr,source_bytes,snapshot,sl.row,1);
    if(!result && !(destination_bytes=image_cpu_read(destination)))result=-1;
    if(!result)result=region_move(dst,dr,destination_bytes,snapshot,dl.row,0);
    if(!result)result=image_cpu_write(destination,destination_bytes);
    free(source_bytes);free(destination_bytes);free(snapshot);
done:
    if(result)LOG("Vulkan image copy failed: serial=%llu -> %llu",(unsigned long long)src->serial,(unsigned long long)dst->serial);
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_image_clear(const dm_compute_image *d,const dm_image_region *r,const void *color) {
    pthread_mutex_lock(&lock);
    int result=-1;VkCommandBuffer cmd=VK_NULL_HANDLE;region_layout layout;
    if(!v.device || region_layout_get(d,r,0,&layout) || d->format!=d->desc.format)return pthread_mutex_unlock(&lock),-1;
    gpu_image *p=image_get(d);if(!p || !(p->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))goto done;
    if(r->x || r->y || (unsigned)r->width!=image_mip(p->extent.width,d->base_level) ||
       (unsigned)r->height!=image_mip(p->extent.height,d->base_level) ||
       (d->desc.target==2 && (r->z || layout.depth!=image_mip(p->extent.depth,d->base_level))))goto done;
    if(image_transfer(p,0))goto done;
    uint64_t draw_serial=0;
    if(draw_compute_begin_locked(&cmd,&draw_serial,0))goto done;
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,d->base_level,1,d->desc.target==2?0:d->base_layer+r->z,d->desc.target==2?1:layout.depth};
    VkClearColorValue value;memcpy(&value,color,sizeof value);
    vkCmdClearColorImage(cmd,p->image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
    mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);
    uint64_t serial=compute_texture_record_locked(cmd,draw_serial,NULL);cmd=VK_NULL_HANDLE;
    gpu_metrics.texture_shared_clears++;
    p->last_compute_use=serial;
    image_set_dirty(p,1);p->upload_stamp=0;p->shadow_valid=0;
    result=0;
done:
    if(cmd)vkFreeCommandBuffers(v.device,v.commands,1,&cmd);
    if(result)LOG("Vulkan texture clear failed: serial=%llu format=%d",(unsigned long long)d->serial,d->format);
    pthread_mutex_unlock(&lock);return result;
}
