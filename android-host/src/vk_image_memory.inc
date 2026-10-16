/* Optimal images only: no buffers, linear images, imported memory or guest
 * address aliasing share these blocks. The backend lock owns all metadata.
 * Existing image retirement proves completion BEFORE releasing any segment. */
#define IMAGE_MEMORY_BLOCK_BYTES (UINT64_C(32)*1024*1024)
typedef struct image_memory_segment {
    struct image_memory_segment *prev,*next;
    VkDeviceSize offset,size;
    int used;
} image_memory_segment;
typedef struct image_memory_block {
    struct image_memory_block *next;
    VkDeviceMemory memory;
    unsigned type,live;
    image_memory_segment *segments;
} image_memory_block;
typedef struct {
    VkDeviceMemory memory;
    VkDeviceSize offset,size;
    image_memory_block *block;
    image_memory_segment *segment;
} image_memory_allocation;
static image_memory_block *image_memory_blocks;
static struct {
    uint64_t blocks,block_bytes,dedicated,dedicated_bytes,images,live_bytes;
    uint64_t peak_blocks,peak_block_bytes,peak_images,peak_live_bytes;
    uint64_t suballocations,dedicated_allocations,block_failures,metadata_failures;
} image_memory_stats;
static void image_memory_publish(image_memory_allocation *a) {
    image_memory_stats.images++;image_memory_stats.live_bytes+=a->size;
    if(image_memory_stats.images>image_memory_stats.peak_images)image_memory_stats.peak_images=image_memory_stats.images;
    if(image_memory_stats.live_bytes>image_memory_stats.peak_live_bytes)image_memory_stats.peak_live_bytes=image_memory_stats.live_bytes;
    if(a->block)image_memory_stats.suballocations++;
    else {
        image_memory_stats.dedicated++;image_memory_stats.dedicated_bytes+=a->size;
        image_memory_stats.dedicated_allocations++;
    }
}
static void image_memory_block_destroy(image_memory_block *b) {
    if(b->live)die("Vulkan image memory block still in use");
    image_memory_block **link=&image_memory_blocks;
    while(*link && *link!=b)link=&(*link)->next;
    if(!*link || !image_memory_stats.blocks || image_memory_stats.block_bytes<IMAGE_MEMORY_BLOCK_BYTES)
        die("Vulkan image memory block accounting underflow");
    *link=b->next;
    while(b->segments) {
        image_memory_segment *s=b->segments;b->segments=s->next;
        if(s->used)die("Vulkan image memory live segment at block destruction");
        free(s);
    }
    device_memory_free(b->memory);
    image_memory_stats.blocks--;image_memory_stats.block_bytes-=IMAGE_MEMORY_BLOCK_BYTES;
    free(b);
}
// A segment owns its metadata until release, so release/coalesce never allocates.
// Return 1 for a reservation, 0 for no fit, -1 for metadata allocation failure.
static int image_memory_block_take(image_memory_block *b,const VkMemoryRequirements *req,image_memory_allocation *out) {
    for(image_memory_segment *s=b->segments;s;s=s->next) {
        if(s->used)continue;
        VkDeviceSize pad=(req->alignment-s->offset%req->alignment)%req->alignment;
        if(pad>s->size || req->size>s->size-pad)continue;
        VkDeviceSize tail=s->size-pad-req->size,offset=s->offset+pad;
        image_memory_segment *before=pad?calloc(1,sizeof *before):NULL;
        image_memory_segment *after=tail?calloc(1,sizeof *after):NULL;
        if((pad && !before) || (tail && !after)) {
            free(before);free(after);image_memory_stats.metadata_failures++;return -1;
        }
        // All metadata exists before the free extent is changed.
        if(before) {
            *before=(image_memory_segment){.prev=s->prev,.next=s,.offset=s->offset,.size=pad};
            if(s->prev)s->prev->next=before;else b->segments=before;
            s->prev=before;
        }
        if(after) {
            *after=(image_memory_segment){.prev=s,.next=s->next,.offset=offset+req->size,.size=tail};
            if(s->next)s->next->prev=after;
            s->next=after;
        }
        s->offset=offset;s->size=req->size;s->used=1;b->live++;
        *out=(image_memory_allocation){.memory=b->memory,.offset=offset,.size=req->size,.block=b,.segment=s};
        image_memory_publish(out);return 1;
    }
    return 0;
}
static image_memory_block *image_memory_block_new(unsigned type) {
    image_memory_block *b=calloc(1,sizeof *b);
    image_memory_segment *s=calloc(1,sizeof *s);
    if(!b || !s) {
        free(b);free(s);image_memory_stats.metadata_failures++;return NULL;
    }
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=IMAGE_MEMORY_BLOCK_BYTES,.memoryTypeIndex=type};
    if(device_memory_allocate(&ai,&b->memory,DEVICE_MEMORY_IMAGE)) {
        free(s);free(b);image_memory_stats.block_failures++;return NULL;
    }
    s->size=IMAGE_MEMORY_BLOCK_BYTES;b->segments=s;b->type=type;
    b->next=image_memory_blocks;image_memory_blocks=b;
    image_memory_stats.blocks++;image_memory_stats.block_bytes+=IMAGE_MEMORY_BLOCK_BYTES;
    if(image_memory_stats.blocks>image_memory_stats.peak_blocks)image_memory_stats.peak_blocks=image_memory_stats.blocks;
    if(image_memory_stats.block_bytes>image_memory_stats.peak_block_bytes)image_memory_stats.peak_block_bytes=image_memory_stats.block_bytes;
    return b;
}
static int image_memory_allocate(VkImage image,image_memory_allocation *out) {
    // All callers create non-sparse, non-disjoint, optimal-tiling images.
    VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dedicated};
    VkImageMemoryRequirementsInfo2 query={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=image};
    vkGetImageMemoryRequirements2(v.device,&query,&requirements);
    const VkMemoryRequirements *req=&requirements.memoryRequirements;
    if(!req->size || !req->alignment)return -1;
    const VkPhysicalDeviceMemoryProperties *mp=&device_memory.properties;
    unsigned type=UINT32_MAX;
    // Preserve the original first device-local, otherwise last-compatible choice.
    for(unsigned i=0;i<mp->memoryTypeCount;i++)if(req->memoryTypeBits&(1u<<i)) {
        type=i;if(mp->memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)break;
    }
    if(type==UINT32_MAX)return -1;
    if(!dedicated.requiresDedicatedAllocation && !dedicated.prefersDedicatedAllocation && req->size<=IMAGE_MEMORY_BLOCK_BYTES/2) {
        int metadata_failed=0;
        for(image_memory_block *b=image_memory_blocks;b;b=b->next)if(b->type==type) {
            int taken=image_memory_block_take(b,req,out);
            if(taken>0)return 0;
            if(taken<0){metadata_failed=1;break;}
        }
        if(!metadata_failed) {
            image_memory_block *b=image_memory_block_new(type);
            if(b) {
                int taken=image_memory_block_take(b,req,out);
                if(taken>0)return 0;
                image_memory_block_destroy(b);
            }
        }
        // Allocation/metadata failure may use a smaller exact allocation.
        // Nothing has bound this image yet; never retry after a bind failure.
    }
    VkMemoryDedicatedAllocateInfo target={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=image};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&target,
        .allocationSize=req->size,.memoryTypeIndex=type};
    VkDeviceMemory memory=VK_NULL_HANDLE;
    if(device_memory_allocate(&ai,&memory,DEVICE_MEMORY_IMAGE))return -1;
    *out=(image_memory_allocation){.memory=memory,.size=req->size};
    image_memory_publish(out);return 0;
}
// Caller has already destroyed the VkImage after its existing completion proof.
static void image_memory_release(image_memory_allocation *a) {
    if(!a->memory)return;
    if(!image_memory_stats.images || image_memory_stats.live_bytes<a->size)
        die("Vulkan image memory allocation accounting underflow");
    image_memory_stats.images--;image_memory_stats.live_bytes-=a->size;
    if(!a->block) {
        if(!image_memory_stats.dedicated || image_memory_stats.dedicated_bytes<a->size)
            die("Vulkan dedicated image memory accounting underflow");
        image_memory_stats.dedicated--;image_memory_stats.dedicated_bytes-=a->size;
        device_memory_free(a->memory);
    } else {
        image_memory_block *b=a->block;image_memory_segment *s=a->segment;
        if(!b->live || !s || !s->used || b->memory!=a->memory || s->offset!=a->offset || s->size!=a->size)
            die("Vulkan image memory allocation owner mismatch");
        s->used=0;b->live--;
        if(s->prev && !s->prev->used) {
            image_memory_segment *before=s->prev;
            if(before->offset+before->size!=s->offset)die("Vulkan image memory segment gap");
            before->size+=s->size;before->next=s->next;
            if(s->next)s->next->prev=before;
            free(s);s=before;
        }
        if(s->next && !s->next->used) {
            image_memory_segment *after=s->next;
            if(s->offset+s->size!=after->offset)die("Vulkan image memory segment gap");
            s->size+=after->size;s->next=after->next;
            if(after->next)after->next->prev=s;
            free(after);
        }
        if(!b->live)image_memory_block_destroy(b);
    }
    memset(a,0,sizeof *a);
}
static void image_memory_report(void) {
    LOG("Vulkan image memory: blockBytes=%llu blocks=%llu dedicatedBytes=%llu dedicatedImages=%llu liveRequirementBytes=%llu activeImages=%llu peakBlockBytes=%llu peakBlocks=%llu peakLiveRequirementBytes=%llu peakImages=%llu suballocations=%llu dedicatedAllocations=%llu blockFailures=%llu metadataFailures=%llu cumulative=1",
        (unsigned long long)image_memory_stats.block_bytes,(unsigned long long)image_memory_stats.blocks,
        (unsigned long long)image_memory_stats.dedicated_bytes,(unsigned long long)image_memory_stats.dedicated,
        (unsigned long long)image_memory_stats.live_bytes,(unsigned long long)image_memory_stats.images,
        (unsigned long long)image_memory_stats.peak_block_bytes,(unsigned long long)image_memory_stats.peak_blocks,
        (unsigned long long)image_memory_stats.peak_live_bytes,(unsigned long long)image_memory_stats.peak_images,
        (unsigned long long)image_memory_stats.suballocations,(unsigned long long)image_memory_stats.dedicated_allocations,
        (unsigned long long)image_memory_stats.block_failures,(unsigned long long)image_memory_stats.metadata_failures);
}
static void image_memory_closed(void) {
    if(image_memory_blocks || image_memory_stats.blocks || image_memory_stats.block_bytes ||
       image_memory_stats.images || image_memory_stats.live_bytes || image_memory_stats.dedicated || image_memory_stats.dedicated_bytes)
        die("Vulkan image memory remained after image close");
    // Peaks/cumulative counts survive NVN sessions like the shared guest heap.
}
