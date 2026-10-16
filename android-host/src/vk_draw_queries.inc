/* Shared batches own pages of distinct occlusion query indices. Their prefix
 * resets each used page once, before the entire body, on Vulkan 1.1.
 * Standalone draws retain single-query leases and their local resets.
 * All access holds the backend lock. Only completed/abandoned work releases
 * a lease, so a cached pool is never reset while an older pass can use it. */
enum { DRAW_QUERY_PAGE_SIZE=64,DRAW_QUERY_PAGE_IDLE_LIMIT=8 };
typedef struct draw_query_page {
    struct draw_query_page *next;VkQueryPool pool;unsigned used;
} draw_query_page;
// A page belongs to one entire recording/submitted batch, including its reset
// prefix. Even discarded query results cannot release it before that fence.
static draw_query_page *draw_batch_query_pages,*draw_query_pages_idle;
static unsigned draw_query_pages_idle_count,draw_query_pages_live;
static int draw_query_page_acquire(VkQueryPool *pool,unsigned *index) {
    draw_query_page *p=draw_batch_query_pages;
    if(!p || p->used==DRAW_QUERY_PAGE_SIZE) {
        if(draw_query_pages_idle) {
            p=draw_query_pages_idle;draw_query_pages_idle=p->next;
            draw_query_pages_idle_count--;perf_metrics.query_page_reuses++;
        } else {
            p=calloc(1,sizeof *p);if(!p)return -1;
            VkQueryPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=DRAW_QUERY_PAGE_SIZE};
            if(vkCreateQueryPool(v.device,&ci,NULL,&p->pool)){free(p);return -1;}
            perf_metrics.query_page_creates++;
        }
        p->used=0;p->next=draw_batch_query_pages;draw_batch_query_pages=p;
        draw_query_pages_live++;
    }
    *pool=p->pool;*index=p->used++;perf_metrics.query_page_indices++;
    return 0;
}
static void draw_query_pages_retire(draw_query_page *pages) {
    while(pages) {
        draw_query_page *p=pages;pages=p->next;
        if(!draw_query_pages_live)die("Vulkan query page ownership underflow");
        draw_query_pages_live--;
        if(draw_query_pages_idle_count==DRAW_QUERY_PAGE_IDLE_LIMIT) {
            vkDestroyQueryPool(v.device,p->pool,NULL);free(p);
        } else {
            p->used=0;p->next=draw_query_pages_idle;draw_query_pages_idle=p;
            draw_query_pages_idle_count++;
        }
    }
}
static void draw_query_pages_close(void) {
    if(draw_query_pages_live || draw_batch_query_pages)die("Vulkan query pages closed before batch retirement");
    while(draw_query_pages_idle) {
        draw_query_page *p=draw_query_pages_idle;draw_query_pages_idle=p->next;
        vkDestroyQueryPool(v.device,p->pool,NULL);free(p);
    }
    draw_query_pages_idle_count=0;
}
#define DRAW_QUERY_CACHE_COUNT 512
typedef struct { VkQueryPool pool;unsigned leased; } draw_query_entry;
static draw_query_entry draw_query_cache[DRAW_QUERY_CACHE_COUNT];
static unsigned draw_query_transient_count;
static int draw_query_acquire(VkQueryPool *pool) {
    *pool=VK_NULL_HANDLE;unsigned empty=DRAW_QUERY_CACHE_COUNT;
    for(unsigned i=0;i<DRAW_QUERY_CACHE_COUNT;i++) {
        draw_query_entry *e=&draw_query_cache[i];
        if(e->pool && !e->leased){e->leased=1;*pool=e->pool;gpu_metrics.query_cache_hits++;return 0;}
        if(!e->pool && empty==DRAW_QUERY_CACHE_COUNT)empty=i;
    }
    VkQueryPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=1};
    if(vkCreateQueryPool(v.device,&ci,NULL,pool))return -1;
    gpu_metrics.query_cache_creates++;
    if(empty<DRAW_QUERY_CACHE_COUNT)draw_query_cache[empty]=(draw_query_entry){*pool,1};
    else {draw_query_transient_count++;gpu_metrics.query_cache_fallbacks++;}
    return 0;
}
static void draw_query_release(VkQueryPool pool) {
    if(!pool)return;
    for(unsigned i=0;i<DRAW_QUERY_CACHE_COUNT;i++)if(draw_query_cache[i].pool==pool) {
        if(!draw_query_cache[i].leased)die("Vulkan query lease underflow");
        draw_query_cache[i].leased=0;return;
    }
    if(!draw_query_transient_count)die("Vulkan unknown transient query lease");
    draw_query_transient_count--;vkDestroyQueryPool(v.device,pool,NULL);
}
static void draw_query_cache_close(void) {
    if(draw_query_transient_count)die("Vulkan query cache closed with transient leases");
    for(unsigned i=0;i<DRAW_QUERY_CACHE_COUNT;i++) {
        draw_query_entry *e=&draw_query_cache[i];
        if(e->leased)die("Vulkan query cache closed with active leases");
        if(e->pool)vkDestroyQueryPool(v.device,e->pool,NULL);
    }
    memset(draw_query_cache,0,sizeof draw_query_cache);
}
