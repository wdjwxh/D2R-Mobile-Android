/* Immutable descriptor contents survive render passes without retaining guest
 * snapshots or uniform arena slices. Each draw independently owns those data.
 * Actual Vulkan object destruction invalidates every descriptor using it. */
// Larger draw batches keep more uniform slices and descriptor identities live.
// Retain room for that working set; shared descriptor arenas have additional
// capacity for pinned draws, transient leases and partially occupied shapes.
enum { GRAPHICS_DESCRIPTOR_BUNDLES=8192,GRAPHICS_DESCRIPTOR_BUNDLE_BUCKETS=16384 };
typedef struct {
    graphics_descriptor_shape shape;
    graphics_descriptor_payload payload;
    VkDescriptorPool pool;VkDescriptorSet set;
    uint64_t hash;unsigned refs,bucket;
} graphics_descriptor_bundle;
static graphics_descriptor_bundle graphics_descriptor_bundles[GRAPHICS_DESCRIPTOR_BUNDLES];
static unsigned graphics_descriptor_bundle_buckets[GRAPHICS_DESCRIPTOR_BUNDLE_BUCKETS];
// Cache replacement touches only these small index nodes, never walks the
// multi-KiB descriptor payload array. Indices are one-based; zero is null.
enum { BUNDLE_FREE=1,BUNDLE_RESERVED,BUNDLE_PINNED,BUNDLE_IDLE };
typedef struct {unsigned prev,next,hash_prev,hash_next,state;} graphics_bundle_node;
static graphics_bundle_node graphics_bundle_nodes[GRAPHICS_DESCRIPTOR_BUNDLES];
static unsigned graphics_bundle_free,graphics_bundle_idle_head,graphics_bundle_idle_tail;
static int graphics_bundle_index_ready;
static void graphics_bundle_index_init(void) {
    if(graphics_bundle_index_ready)return;
    for(unsigned i=0;i<GRAPHICS_DESCRIPTOR_BUNDLES;i++)
        graphics_bundle_nodes[i]=(graphics_bundle_node){.next=i+1<GRAPHICS_DESCRIPTOR_BUNDLES?i+2:0,.state=BUNDLE_FREE};
    graphics_bundle_free=1;graphics_bundle_index_ready=1;
}
static void graphics_bundle_idle_remove(unsigned index) {
    graphics_bundle_node *node=&graphics_bundle_nodes[index-1];
    if(node->state!=BUNDLE_IDLE)die("Vulkan descriptor bundle missing idle lease");
    if(node->prev)graphics_bundle_nodes[node->prev-1].next=node->next;
    else graphics_bundle_idle_head=node->next;
    if(node->next)graphics_bundle_nodes[node->next-1].prev=node->prev;
    else graphics_bundle_idle_tail=node->prev;
    node->prev=node->next=0;node->state=BUNDLE_PINNED;
}
static void graphics_bundle_idle_append(unsigned index) {
    graphics_bundle_node *node=&graphics_bundle_nodes[index-1];
    if(node->state!=BUNDLE_PINNED)die("Vulkan descriptor bundle duplicated idle lease");
    node->prev=graphics_bundle_idle_tail;node->next=0;node->state=BUNDLE_IDLE;
    if(graphics_bundle_idle_tail)graphics_bundle_nodes[graphics_bundle_idle_tail-1].next=index;
    else graphics_bundle_idle_head=index;
    graphics_bundle_idle_tail=index;
}
static int graphics_descriptor_is_buffer(VkDescriptorType type) {
    return type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
        type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
static int graphics_descriptor_is_texel(VkDescriptorType type) {
    return type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}
static uint64_t graphics_descriptor_hash(const graphics_descriptor_shape *shape,const graphics_descriptor_payload *payload) {
    uint64_t h=UINT64_C(1469598103934665603);
#define MIX(value) h=(h^(uint64_t)(value))*UINT64_C(1099511628211)
    MIX(shape->stage);MIX(shape->count);
    for(unsigned i=0;i<shape->count;i++) {
        VkDescriptorType type=(VkDescriptorType)shape->bindings[i].type;
        MIX(shape->bindings[i].binding);MIX(type);
        if(graphics_descriptor_is_buffer(type)) {
            MIX((uintptr_t)payload->buffers[i].buffer);MIX(payload->buffers[i].offset);MIX(payload->buffers[i].range);
        } else if(graphics_descriptor_is_texel(type)) {MIX((uintptr_t)payload->texels[i]);}
        else {MIX((uintptr_t)payload->images[i].imageView);MIX((uintptr_t)payload->images[i].sampler);MIX(payload->images[i].imageLayout);}
    }
#undef MIX
    return h;
}
static int graphics_descriptor_payload_equal(const graphics_descriptor_shape *shape,
        const graphics_descriptor_payload *a,const graphics_descriptor_payload *b) {
    for(unsigned i=0;i<shape->count;i++) {
        VkDescriptorType type=(VkDescriptorType)shape->bindings[i].type;
        if(graphics_descriptor_is_buffer(type)) {
            if(a->buffers[i].buffer!=b->buffers[i].buffer || a->buffers[i].offset!=b->buffers[i].offset ||
               a->buffers[i].range!=b->buffers[i].range)return 0;
        } else if(graphics_descriptor_is_texel(type)) {
            if(a->texels[i]!=b->texels[i])return 0;
        } else if(a->images[i].imageView!=b->images[i].imageView || a->images[i].sampler!=b->images[i].sampler ||
                  a->images[i].imageLayout!=b->images[i].imageLayout)return 0;
    }
    return 1;
}
static void graphics_descriptor_bundle_drop(graphics_descriptor_bundle *b) {
    if(b->refs)die("Vulkan descriptor bundle retired with active references");
    if(!graphics_bundle_index_ready)return;
    unsigned index=(unsigned)(b-graphics_descriptor_bundles)+1;
    graphics_bundle_node *node=&graphics_bundle_nodes[index-1];
    if(node->state==BUNDLE_FREE)return;
    if(node->state==BUNDLE_IDLE)graphics_bundle_idle_remove(index);
    if(b->pool) {
        if(node->hash_prev)graphics_bundle_nodes[node->hash_prev-1].hash_next=node->hash_next;
        else graphics_descriptor_bundle_buckets[b->bucket]=node->hash_next;
        if(node->hash_next)graphics_bundle_nodes[node->hash_next-1].hash_prev=node->hash_prev;
        graphics_descriptor_pool_release(b->pool,b->set);
    }
    memset(b,0,sizeof *b);
    *node=(graphics_bundle_node){.next=graphics_bundle_free,.state=BUNDLE_FREE};
    graphics_bundle_free=index;
}
static void graphics_descriptor_bundle_release(graphics_descriptor_bundle *b) {
    if(!b || !b->refs)die("Vulkan descriptor bundle reference underflow");
    if(!--b->refs)graphics_bundle_idle_append((unsigned)(b-graphics_descriptor_bundles)+1);
}
static graphics_descriptor_bundle *graphics_descriptor_bundle_find(const graphics_descriptor_shape *shape,
        const graphics_descriptor_payload *payload,uint64_t hash) {
    unsigned bucket=(unsigned)hash&(GRAPHICS_DESCRIPTOR_BUNDLE_BUCKETS-1);
    for(unsigned at=graphics_descriptor_bundle_buckets[bucket];at;at=graphics_bundle_nodes[at-1].hash_next) {
        graphics_descriptor_bundle *b=&graphics_descriptor_bundles[at-1];
        if(b->set && b->hash==hash && !memcmp(&b->shape,shape,sizeof *shape) &&
           graphics_descriptor_payload_equal(shape,&b->payload,payload)) {
            if(!b->refs)graphics_bundle_idle_remove(at);
            b->refs++;return b;
        }
    }
    return NULL;
}
static graphics_descriptor_bundle *graphics_descriptor_bundle_reserve(void) {
    graphics_bundle_index_init();
    if(!graphics_bundle_free && graphics_bundle_idle_head) {
        graphics_descriptor_bundle_drop(&graphics_descriptor_bundles[graphics_bundle_idle_head-1]);
        gpu_metrics.descriptor_bundle_evictions++;
    }
    if(!graphics_bundle_free){gpu_metrics.descriptor_bundle_fallbacks++;return NULL;}
    unsigned index=graphics_bundle_free;
    graphics_bundle_node *node=&graphics_bundle_nodes[index-1];
    if(node->state!=BUNDLE_FREE)die("Invalid Vulkan descriptor free list");
    graphics_bundle_free=node->next;
    *node=(graphics_bundle_node){.state=BUNDLE_RESERVED};
    return &graphics_descriptor_bundles[index-1];
}
static void graphics_descriptor_bundle_publish(graphics_descriptor_bundle *b) {
    unsigned index=(unsigned)(b-graphics_descriptor_bundles)+1;
    graphics_bundle_node *node=&graphics_bundle_nodes[index-1];
    if(node->state!=BUNDLE_RESERVED || !b->refs || !b->pool || !b->set)
        die("Invalid Vulkan descriptor bundle publication");
    node->state=BUNDLE_PINNED;node->hash_next=graphics_descriptor_bundle_buckets[b->bucket];
    if(node->hash_next)graphics_bundle_nodes[node->hash_next-1].hash_prev=index;
    graphics_descriptor_bundle_buckets[b->bucket]=index;
}
// Idle cached sets must not monopolize all arena shapes. Reclaim one arena
// only when EVERY live lease belongs to an idle bundle, never an active draw
// or the transient fallback path. The allocator retries with that idle arena.
static int graphics_descriptor_bundles_trim_arena(void) {
    graphics_descriptor_arena *oldest=NULL;
    for(unsigned a=0;a<GRAPHICS_DESCRIPTOR_ARENAS;a++) {
        graphics_descriptor_arena *arena=&graphics_descriptor_pools[a];if(!arena->pool || !arena->live)continue;
        uint32_t covered=0;
        for(unsigned j=0;j<GRAPHICS_DESCRIPTOR_BUNDLES;j++) {
            graphics_descriptor_bundle *b=&graphics_descriptor_bundles[j];
            if(b->pool!=arena->pool || b->refs)continue;
            for(unsigned k=0;k<GRAPHICS_DESCRIPTOR_SETS;k++)if(arena->sets[k]==b->set){covered|=UINT32_C(1)<<k;break;}
        }
        if(covered==arena->live && (!oldest || arena->used<oldest->used))oldest=arena;
    }
    if(!oldest)return 0;
    for(unsigned j=0;j<GRAPHICS_DESCRIPTOR_BUNDLES;j++)if(graphics_descriptor_bundles[j].pool==oldest->pool) {
        graphics_descriptor_bundle_drop(&graphics_descriptor_bundles[j]);gpu_metrics.descriptor_bundle_evictions++;
    }
    return 1;
}
static void graphics_descriptor_retire_object(unsigned kind,uint64_t handle) {
    if(!handle)return;
    for(unsigned j=0;j<GRAPHICS_DESCRIPTOR_BUNDLES;j++) {
        graphics_descriptor_bundle *b=&graphics_descriptor_bundles[j];if(!b->set)continue;
        for(unsigned i=0;i<b->shape.count;i++) {
            VkDescriptorType type=(VkDescriptorType)b->shape.bindings[i].type;uint64_t object=0;
            if(kind==GRAPHICS_DESCRIPTOR_BUFFER && graphics_descriptor_is_buffer(type))object=(uintptr_t)b->payload.buffers[i].buffer;
            else if(kind==GRAPHICS_DESCRIPTOR_TEXEL_VIEW && graphics_descriptor_is_texel(type))object=(uintptr_t)b->payload.texels[i];
            else if(!graphics_descriptor_is_buffer(type) && !graphics_descriptor_is_texel(type)) {
                if(kind==GRAPHICS_DESCRIPTOR_IMAGE_VIEW)object=(uintptr_t)b->payload.images[i].imageView;
                if(kind==GRAPHICS_DESCRIPTOR_SAMPLER)object=(uintptr_t)b->payload.images[i].sampler;
            }
            if(object==handle){graphics_descriptor_bundle_drop(b);break;}
        }
    }
}
static void graphics_descriptor_bundles_close(void) {
    for(unsigned i=0;i<GRAPHICS_DESCRIPTOR_BUNDLES;i++)graphics_descriptor_bundle_drop(&graphics_descriptor_bundles[i]);
    memset(graphics_bundle_nodes,0,sizeof graphics_bundle_nodes);
    memset(graphics_descriptor_bundle_buckets,0,sizeof graphics_descriptor_bundle_buckets);
    graphics_bundle_free=graphics_bundle_idle_head=graphics_bundle_idle_tail=0;
    graphics_bundle_index_ready=0;
}
