/* Reports are ordered markers in the normal draw FIFO. Only actual range
 * consumers wait; report publication itself never submits or waits. */
typedef struct sample_interval { uint64_t total;unsigned refs,pending_reports;int current; } sample_interval;
typedef struct sample_owner {
    struct sample_owner *next;uint64_t *key;sample_interval *interval;
} sample_owner;
static sample_owner *sample_owners;
static void sample_release(sample_interval *s) {
    if(s && !--s->refs)free(s);
}
static sample_owner *sample_owner_get(uint64_t *key) {
    for(sample_owner *o=sample_owners;o;o=o->next)if(o->key==key)return o;
    sample_owner *o=calloc(1,sizeof *o);if(!o)return NULL;
    o->interval=calloc(1,sizeof *o->interval);
    if(!o->interval){free(o);return NULL;}
    o->key=key;o->interval->refs=1;o->interval->current=1;o->next=sample_owners;sample_owners=o;return o;
}
static sample_interval *sample_capture(uint64_t *key) {
    if(!key)return NULL;
    sample_owner *o=sample_owner_get(key);if(!o)return NULL;
    o->interval->refs++;return o->interval;
}
typedef struct timestamp_lease {
    struct timestamp_lease *next;VkQueryPool pool;int leased;
    VkBuffer buffer;VkDeviceMemory memory;VkDescriptorPool descriptors;VkDescriptorSet set;
} timestamp_lease;
static struct {VkPipeline pipeline;VkPipelineLayout layout;VkDescriptorSetLayout set;} timestamp_gpu;
typedef struct counter_report {
    struct counter_report *next;
    dm_vk_pool *pool;size_t offset;uint64_t serial;
    sample_interval *samples;timestamp_lease *timestamp;
    size_t size;uint32_t value;int event,add,gpu_published;
    const void *event_queue;uint64_t event_job; // Record execution provenance, not a guest fence.
} counter_report;
static timestamp_lease *timestamp_leases;
static counter_report *counter_reports;
// Conservative 64-page bloom under the backend lock. Collisions only cause
// the original exact scan; a missing bit proves there is no report overlap.
static uint64_t counter_report_pages;
static inline uint64_t report_page_mask(uint64_t address,size_t size);
static int reports_peek_ready_locked(uint64_t address,size_t size) {
    if(!(counter_report_pages&report_page_mask(address,size)))return 1;
    for(const counter_report *r=counter_reports;r;r=r->next) {
        uint64_t base=(uintptr_t)r->pool->guest+r->offset;
        if(r->size && (address>=base?address-base<r->size:base-address<size))return 0;
    }
    return 1;
}
static inline uint64_t report_page_mask(uint64_t address,size_t size) {
    if(!size || size>UINT64_MAX-address)return UINT64_MAX;
    uint64_t first=address>>12,last=(address+size-1)>>12;
    if(last-first>=63)return UINT64_MAX;
    unsigned n=(unsigned)(last-first+1),shift=(unsigned)first&63;
    uint64_t bits=(UINT64_C(1)<<n)-1;
    return (bits<<shift)|(shift?bits>>(64-shift):0);
}
static void report_track(counter_report *r) {
    r->next=counter_reports;counter_reports=r;
    counter_report_pages|=report_page_mask((uintptr_t)r->pool->guest+r->offset,r->size);
    pool_cpu_hazard_add();
}
static unsigned pending_event_count;
int dm_vk_completion_pending(void) { return __atomic_load_n(&pending_event_count,__ATOMIC_ACQUIRE)!=0; }
static timestamp_lease *timestamp_acquire(void) {
    for(timestamp_lease *q=timestamp_leases;q;q=q->next)if(!q->leased){q->leased=1;return q;}
    timestamp_lease *q=calloc(1,sizeof *q);if(!q)return NULL;
    VkQueryPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=1};
    if(vkCreateQueryPool(v.device,&ci,NULL,&q->pool)){free(q);return NULL;}
    q->leased=1;q->next=timestamp_leases;timestamp_leases=q;
    gpu_metrics.report_query_creates++;return q;
}
// Called in FIFO retirement order, after the enclosing hardware fence is done.
// Do not call guest_write/pool_sync here: either could recursively retire this
// very submission. Guest RAM becomes authoritative; the next GPU reader uses
// the normal upload path. No mapped memory is touched by this callback.
static int report_publish(counter_report *r) {
    uint64_t record[2]={0};
    if(!r->event && !r->gpu_published) {
        uint64_t raw=0;
        if(vkGetQueryPoolResults(v.device,r->timestamp->pool,0,1,sizeof raw,&raw,sizeof raw,VK_QUERY_RESULT_64_BIT))return -1;
        if(v.timestamp_bits<64)raw&=(UINT64_C(1)<<v.timestamp_bits)-1;
        uint64_t ticks=(uint64_t)((long double)raw*v.properties.limits.timestampPeriod*12.0L/625.0L);
        record[0]=r->samples?r->samples->total:ticks;record[1]=ticks;
    }
    // GPU reports already own their exact writer interval. This callback only
    // releases their query/scratch lease; never overwrite a later GPU writer.
    if(!r->gpu_published) {
    if(pool_gpu_forget_locked(r->pool,r->offset,r->size))return -1;
    uint8_t *dst=r->pool->guest+r->offset;
    dm_guest_write_prep(dst,r->size);
    if(r->event) {
        if(r->add)__atomic_fetch_add((uint32_t *)dst,r->value,__ATOMIC_ACQ_REL);
        else __atomic_store_n((uint32_t *)dst,r->value,__ATOMIC_RELEASE);
    } else memcpy(dst,record,r->size);
    pool_shadow_invalidate(r->pool,r->offset,r->size);
    pool_range_invalidate(r->pool,r->offset,r->size);
    graphics_uniform_retire_range((uintptr_t)dst,r->size);
    graphics_buffer_retire_range((uintptr_t)dst,r->size);
    }
    counter_report **link=&counter_reports;while(*link && *link!=r)link=&(*link)->next;
    if(!*link)die("Vulkan report ownership lost");
    *link=r->next;pool_cpu_hazard_remove();
    counter_report_pages=0;
    for(counter_report *p=counter_reports;p;p=p->next)
        counter_report_pages|=report_page_mask((uintptr_t)p->pool->guest+p->offset,p->size);
    if(r->samples) {
        if(!r->samples->pending_reports)die("Vulkan sample report reference underflow");
        r->samples->pending_reports--;
    }
    sample_release(r->samples);
    if(r->timestamp)r->timestamp->leased=0;
    if(r->event)__atomic_fetch_sub(&pending_event_count,1,__ATOMIC_RELEASE);
    free(r);
    gpu_metrics.report_published++;return 0;
}
static int report_record_locked(uint64_t address,uint64_t *samples,int top);
int dm_vk_report_counter(uint64_t address,uint64_t *samples,int top) {
    pthread_mutex_lock(&lock);
    int result=report_record_locked(address,samples,top);
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_samples_forget(uint64_t *key) {
    pthread_mutex_lock(&lock);
    // Finalization drains first; old intervals otherwise keep independent refs.
    sample_owner **link=&sample_owners;
    while(*link && (*link)->key!=key)link=&(*link)->next;
    if(*link){sample_owner *o=*link;*link=o->next;o->interval->current=0;sample_release(o->interval);free(o);}
    pthread_mutex_unlock(&lock);return 0;
}
static void timestamp_close(void) {
    if(counter_reports)die("Vulkan reports remained at device close");
    if(counter_report_pages)die("Vulkan report page summary remained at device close");
    if(dm_vk_completion_pending())die("Vulkan events remained at device close");
    while(timestamp_leases) {
        timestamp_lease *q=timestamp_leases;timestamp_leases=q->next;
        if(q->leased)die("Vulkan timestamp query closed with pending work");
        if(q->descriptors)vkDestroyDescriptorPool(v.device,q->descriptors,NULL);
        if(q->buffer)vkDestroyBuffer(v.device,q->buffer,NULL);
        if(q->memory)device_memory_free(q->memory);
        vkDestroyQueryPool(v.device,q->pool,NULL);free(q);
    }
    if(timestamp_gpu.pipeline)vkDestroyPipeline(v.device,timestamp_gpu.pipeline,NULL);
    if(timestamp_gpu.layout)vkDestroyPipelineLayout(v.device,timestamp_gpu.layout,NULL);
    if(timestamp_gpu.set)vkDestroyDescriptorSetLayout(v.device,timestamp_gpu.set,NULL);
    memset(&timestamp_gpu,0,sizeof timestamp_gpu);
    while(sample_owners) {
        sample_owner *o=sample_owners;sample_owners=o->next;
        if(o->interval->refs!=1)die("Vulkan sample interval remained in flight");
        sample_release(o->interval);free(o);
    }
}
