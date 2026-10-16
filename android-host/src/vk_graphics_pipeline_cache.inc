/* Backend-lock-owned index over immutable, fully normalized dm_draw keys.
 * The projection only selects candidates; full memcmp remains authoritative.
 * next owns the close traversal, independently of the hash bucket links. */
typedef struct graphics_cached_pipeline {
    struct graphics_cached_pipeline *next;dm_draw key;VkPipeline pipeline;VkPipelineLayout layout;VkDescriptorSetLayout sets[2];
    uint32_t vertex_locations;uint64_t dynamic_profiles[2];
    struct graphics_cached_pipeline *hash_next;uint64_t hash;
} graphics_cached_pipeline;
enum { GRAPHICS_PIPELINE_BUCKETS=1024 };
static graphics_cached_pipeline *graphics_pipelines,*graphics_last_pipeline;
static unsigned graphics_pipeline_count;
static graphics_cached_pipeline *graphics_pipeline_buckets[GRAPHICS_PIPELINE_BUCKETS];
static struct {
    uint64_t lookup,last_hits,hash_probes,misses;
} graphics_pipeline_cache_metrics;

static uint64_t graphics_pipeline_hash_word(uint64_t hash,uint64_t value) {
    return (hash^value)*UINT64_C(1099511628211);
}
static uint64_t graphics_pipeline_hash(const dm_draw *key) {
    // Read a small typed projection, not the entire multi-KiB key. Omitted
    // state (including blend/vertex details) can only add hash collisions.
    uint64_t hash=UINT64_C(14695981039346656037);
    hash=graphics_pipeline_hash_word(hash,(uintptr_t)key->shader[0]);
    hash=graphics_pipeline_hash_word(hash,(uintptr_t)key->shader[1]);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->primitive);
    hash=graphics_pipeline_hash_word(hash,key->color_mask);
    hash=graphics_pipeline_hash_word(hash,key->fuse_pass);
    hash=graphics_pipeline_hash_word(hash,key->fuse_subpass);
    for(unsigned i=0;i<8;i++)hash=graphics_pipeline_hash_word(hash,(uint32_t)key->color[i].format);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->has_depth);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->depth.format);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->cull);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->front);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->depth_test);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->depth_write);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->depth_compare);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->stencil_test);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->raster_samples);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->alpha_to_coverage);
    hash=graphics_pipeline_hash_word(hash,(uint32_t)key->depth_bias);
    hash=graphics_pipeline_hash_word(hash,key->nattributes);
    // Fold high pointer bits into the low bucket bits after the projection.
    hash^=hash>>33;hash*=UINT64_C(0xff51afd7ed558ccd);
    hash^=hash>>33;hash*=UINT64_C(0xc4ceb9fe1a85ec53);
    return hash^(hash>>33);
}
static graphics_cached_pipeline *graphics_pipeline_find(const dm_draw *key) {
    graphics_pipeline_cache_metrics.lookup++;
    graphics_cached_pipeline *entry=graphics_last_pipeline;
    if(entry && !memcmp(&entry->key,key,sizeof *key)) {
        graphics_pipeline_cache_metrics.last_hits++;return entry;
    }
    uint64_t hash=graphics_pipeline_hash(key);
    for(entry=graphics_pipeline_buckets[hash&(GRAPHICS_PIPELINE_BUCKETS-1)];entry;entry=entry->hash_next) {
        graphics_pipeline_cache_metrics.hash_probes++;
        if(entry->hash==hash && !memcmp(&entry->key,key,sizeof *key)) {
            graphics_last_pipeline=entry;return entry;
        }
    }
    graphics_pipeline_cache_metrics.misses++;return NULL;
}
// Insert once, only after every key/resource field has been initialized.
static void graphics_pipeline_insert(graphics_cached_pipeline *entry) {
    entry->hash=graphics_pipeline_hash(&entry->key);
    unsigned bucket=(unsigned)(entry->hash&(GRAPHICS_PIPELINE_BUCKETS-1));
    entry->hash_next=graphics_pipeline_buckets[bucket];graphics_pipeline_buckets[bucket]=entry;
    entry->next=graphics_pipelines;graphics_pipelines=entry;
    graphics_last_pipeline=entry;graphics_pipeline_count++;
}
static void graphics_pipeline_cache_reset_metrics(void) {
    memset(&graphics_pipeline_cache_metrics,0,sizeof graphics_pipeline_cache_metrics);
}
static void graphics_pipeline_cache_report(void) {
    LOG("Vulkan graphics pipeline cache: lookup=%llu lastHits=%llu hashProbes=%llu misses=%llu count=%u",
        (unsigned long long)graphics_pipeline_cache_metrics.lookup,
        (unsigned long long)graphics_pipeline_cache_metrics.last_hits,
        (unsigned long long)graphics_pipeline_cache_metrics.hash_probes,
        (unsigned long long)graphics_pipeline_cache_metrics.misses,graphics_pipeline_count);
}
// Call only after the next ownership chain has been destroyed by close.
// This index neither releases Vulkan objects nor changes their lifetime.
static void graphics_pipeline_cache_clear_index(void) {
    memset(graphics_pipeline_buckets,0,sizeof graphics_pipeline_buckets);
    graphics_pipelines=graphics_last_pipeline=NULL;graphics_pipeline_count=0;
    graphics_pipeline_cache_reset_metrics();
}
