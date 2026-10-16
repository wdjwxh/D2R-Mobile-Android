/* Diagnostic probes share existing submissions/fences. No profiling wait or
 * extra queue submission; TOP/BOTTOM measures queue span, not shader-only time. */
enum { PERF_DRAW,PERF_COMPUTE,PERF_MIXED,PERF_KINDS,
    PERF_SLOTS=VK_COMPUTE_FIFO_CAPACITY>VK_DRAW_FIFO_CAPACITY?VK_COMPUTE_FIFO_CAPACITY:VK_DRAW_FIFO_CAPACITY };
typedef struct { VkQueryPool pool;VkCommandBuffer cb[2];int armed; } gpu_probe;
static gpu_probe gpu_probes[PERF_KINDS][PERF_SLOTS];
static uint64_t gpu_probe_sequence[PERF_KINDS];
static int gpu_probe_disabled;
typedef struct {
    const char *site,*via;
    uint64_t hits,events,timestamps,samples,pending,submits,timeline_waits,fence_waits;
    double timeline_ms,fence_ms;
} report_wait_site;
typedef struct {
    const char *site;uint64_t attempts,ready,waits;double query_ms,wait_ms;
} completion_wait_site;
// Backend lock protects both the table and this dynamically scoped label.
// A nested drain temporarily replaces the label, then restores its caller.
static int report_wait_active=-1;
static struct {
    struct {uint64_t samples,dropped;double sum_ms,max_ms;} gpu[PERF_KINDS];
    uint64_t waits,timeouts,polls,ready,not_ready,queue_idle,device_idle;
    double wait_ms,queue_idle_ms,device_idle_ms;
    uint64_t draw_submits,draw_capacity;
    uint64_t draw_cb_alloc,draw_cb_reuse;
    uint64_t guest_reads,guest_read_bytes;
    uint64_t uniform_gpu_updates,uniform_gpu_update_bytes;
    uint64_t wsi_acquires,wsi_submits,wsi_presents;
    double wsi_acquire_ms,wsi_submit_ms,wsi_present_ms;
    double wsi_acquire_max_ms,wsi_submit_max_ms,wsi_present_max_ms;
    uint64_t query_segments,query_prefix_batches,sample_pass_kept,event_pass_kept;
    uint64_t query_page_creates,query_page_reuses,query_page_indices,query_page_resets,query_page_reset_slots;
    uint64_t query_timestamp_resets,query_standalone_resets;
    uint64_t full_barrier_encoded,full_barrier_merged;
    uint64_t pass_cache_hits,pass_cache_misses,pass_capacity_fallback,pass_allocation_fallback;
    uint64_t attachment_cache_hits,attachment_cache_misses,attachment_capacity_fallback,attachment_uncached_pass,attachment_allocation_fallback;
    uint64_t cache_only_batched;
    uint64_t shared_compute_dispatches,shared_compute_copies,shared_image_copies,shared_compute_batches,shared_compute_wait_submits,shared_compute_pending_polls;
    uint64_t upload_no_overlap_polls_avoided,upload_overlap_polls;
    uint64_t event_wait_ready,event_wait_kept_pending,event_wait_progress;
    uint64_t event_witness_hits,event_witness_pending,event_witness_miss[8];
    uint64_t cpu_pool_read_calls,cpu_write_calls,cpu_write_compute[5];
    uint64_t cpu_fence_shared_report_preflights_skipped;
    uint64_t timeline_submits,timeline_polls,timeline_waits,timeline_wait_cached;
    uint64_t timeline_fence_certificates;
    uint64_t timeline_empty_draw_polls,timeline_pressure_reused;
    double timeline_wait_ms;
    uint64_t nvn_barriers[256],nvn_active_barriers[256],nvn_unknown_barriers;
    uint32_t nvn_unknown_bits;
    struct {const char *function;unsigned line;uint64_t count;double ms;} idle_sites[8];
    uint64_t idle_site_overflow;
    double idle_site_overflow_ms;
    struct {const char *function;unsigned line;uint64_t submits,draws,records,capacity,mixed;} submit_sites[32];
    report_wait_site report_sites[32];
    completion_wait_site completion_sites[32];
} perf_metrics;
static completion_wait_site *completion_wait_site_get(const char *site) {
    for(unsigned i=0;i<31;i++) {
        completion_wait_site *s=&perf_metrics.completion_sites[i];
        if(!s->site){s->site=site;return s;}
        if(s->site==site || !strcmp(s->site,site))return s;
    }
    perf_metrics.completion_sites[31].site="overflow";return &perf_metrics.completion_sites[31];
}
static int report_wait_site_get(const char *site,const char *via) {
    for(unsigned i=0;i<31;i++) {
        report_wait_site *s=&perf_metrics.report_sites[i];
        if(!s->site){s->site=site;s->via=via;return (int)i;}
        if(!strcmp(s->site,site) && !strcmp(s->via,via))return (int)i;
    }
    perf_metrics.report_sites[31].site="overflow";perf_metrics.report_sites[31].via="mixed";return 31;
}
static void report_wait_elapsed(int timeline,double elapsed) {
    if(report_wait_active<0)return;
    report_wait_site *s=&perf_metrics.report_sites[report_wait_active];
    if(timeline){s->timeline_waits++;s->timeline_ms+=elapsed;}
    else {s->fence_waits++;s->fence_ms+=elapsed;}
}
// Explicit caller values survive nested preparation/submission. The last slot
// is overflow; collect only actual successful submits, with no extra clocks.
static void gpu_submit_site_record(const char *function,unsigned line,unsigned draws,unsigned records,int mixed,int capacity) {
    if(report_wait_active>=0)perf_metrics.report_sites[report_wait_active].submits++;
    unsigned site=31;
    for(unsigned i=0;i<31;i++) {
        if(!perf_metrics.submit_sites[i].function) {
            perf_metrics.submit_sites[i].function=function;perf_metrics.submit_sites[i].line=line;site=i;break;
        }
        if(perf_metrics.submit_sites[i].function==function && perf_metrics.submit_sites[i].line==line){site=i;break;}
    }
    perf_metrics.submit_sites[site].submits++;
    perf_metrics.submit_sites[site].draws+=draws;perf_metrics.submit_sites[site].records+=records;
    perf_metrics.submit_sites[site].capacity+=capacity;perf_metrics.submit_sites[site].mixed+=mixed;
}
static VkResult perf_wait(VkDevice device,uint32_t count,const VkFence *fences,VkBool32 all,uint64_t timeout) {
    double start=gpu_time_ms();VkResult r=vkWaitForFences(device,count,fences,all,timeout);
    double elapsed=gpu_time_ms()-start;
    perf_metrics.waits++;perf_metrics.wait_ms+=elapsed;report_wait_elapsed(0,elapsed);
    if(r==VK_TIMEOUT)perf_metrics.timeouts++;return r;
}
static VkResult perf_poll(VkDevice device,VkFence fence) {
    VkResult r=vkGetFenceStatus(device,fence);perf_metrics.polls++;
    perf_metrics.ready+=r==VK_SUCCESS;perf_metrics.not_ready+=r==VK_NOT_READY;return r;
}
static VkResult perf_queue_idle(VkQueue queue,const char *function,unsigned line) {
    double start=gpu_time_ms();VkResult r=vkQueueWaitIdle(queue);
    double elapsed=gpu_time_ms()-start;
    perf_metrics.queue_idle++;perf_metrics.queue_idle_ms+=elapsed;
    for(unsigned i=0;i<8;i++) {
        if(!perf_metrics.idle_sites[i].function) {
            perf_metrics.idle_sites[i].function=function;perf_metrics.idle_sites[i].line=line;
        }
        if(perf_metrics.idle_sites[i].line==line && !strcmp(perf_metrics.idle_sites[i].function,function)) {
            perf_metrics.idle_sites[i].count++;perf_metrics.idle_sites[i].ms+=elapsed;return r;
        }
    }
    perf_metrics.idle_site_overflow++;perf_metrics.idle_site_overflow_ms+=elapsed;return r;
}
static VkResult perf_device_idle(VkDevice device) {
    double start=gpu_time_ms();VkResult r=vkDeviceWaitIdle(device);
    perf_metrics.device_idle++;perf_metrics.device_idle_ms+=gpu_time_ms()-start;return r;
}
static void gpu_probe_destroy(gpu_probe *p) {
    for(unsigned i=0;i<2;i++)if(p->cb[i])vkFreeCommandBuffers(v.device,v.commands,1,&p->cb[i]);
    if(p->pool)vkDestroyQueryPool(v.device,p->pool,NULL);
    memset(p,0,sizeof *p);
}
static int gpu_probe_prepare(unsigned kind,unsigned slot,VkSubmitInfo *submit,VkCommandBuffer *wrapped) {
    if((++gpu_probe_sequence[kind]&31) || gpu_probe_disabled || !v.timestamp_bits ||
       v.timestamp_bits>64 || !(v.properties.limits.timestampPeriod>0))return 0;
    gpu_probe *p=&gpu_probes[kind][slot];
    if(p->armed)die("Vulkan timing probe reused before retirement");
    if(!p->pool) {
        VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        if(vkCreateQueryPool(v.device,&qi,NULL,&p->pool))goto fail;
        for(unsigned i=0;i<2;i++) {
            VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool=v.commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
            if(vkAllocateCommandBuffers(v.device,&ai,&p->cb[i]))goto fail;
            VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            if(vkBeginCommandBuffer(p->cb[i],&bi))goto fail;
            if(!i)vkCmdResetQueryPool(p->cb[i],p->pool,0,2);
            vkCmdWriteTimestamp(p->cb[i],i?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,p->pool,i);
            if(vkEndCommandBuffer(p->cb[i]))goto fail;
        }
    }
    wrapped[0]=p->cb[0];
    memcpy(wrapped+1,submit->pCommandBuffers,submit->commandBufferCount*sizeof *wrapped);
    wrapped[submit->commandBufferCount+1]=p->cb[1];
    submit->pCommandBuffers=wrapped;submit->commandBufferCount+=2;p->armed=1;return 1;
fail:
    gpu_probe_destroy(p);gpu_probe_disabled=1;perf_metrics.gpu[kind].dropped++;
    LOG("GPU timing probes disabled after allocation/recording failure");return 0;
}
static void gpu_probe_retire(unsigned kind,unsigned slot) {
    gpu_probe *p=&gpu_probes[kind][slot];if(!p->armed)return;
    uint64_t ticks[2];
    VkResult r=vkGetQueryPoolResults(v.device,p->pool,0,2,sizeof ticks,ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
    p->armed=0;
    if(r!=VK_SUCCESS){perf_metrics.gpu[kind].dropped++;return;}
    uint64_t delta=ticks[1]-ticks[0];
    if(v.timestamp_bits<64)delta&=(UINT64_C(1)<<v.timestamp_bits)-1;
    double ms=(double)delta*v.properties.limits.timestampPeriod/1000000.0;
    perf_metrics.gpu[kind].samples++;perf_metrics.gpu[kind].sum_ms+=ms;
    if(ms>perf_metrics.gpu[kind].max_ms)perf_metrics.gpu[kind].max_ms=ms;
}
static void gpu_performance_report(void) {
    LOG("CPU fence shared visibility: coherentPoolsRetained=%llu scope=completedFenceOnly dirtyWriters=retained explicitRangeSync=preserved",
        (unsigned long long)perf_metrics.cpu_fence_shared_report_preflights_skipped);
    LOG("CPU memory ownership: poolReadCalls=%llu writeCalls=%llu computeRetired=%llu computeNoOverlap=%llu computeOlder=%llu computeLatest=%llu computeFallback=%llu",
        (unsigned long long)perf_metrics.cpu_pool_read_calls,(unsigned long long)perf_metrics.cpu_write_calls,
        (unsigned long long)perf_metrics.cpu_write_compute[0],(unsigned long long)perf_metrics.cpu_write_compute[1],
        (unsigned long long)perf_metrics.cpu_write_compute[2],(unsigned long long)perf_metrics.cpu_write_compute[3],
        (unsigned long long)perf_metrics.cpu_write_compute[4]);
    LOG("GPU completion fence certificates: advanced=%llu",(unsigned long long)perf_metrics.timeline_fence_certificates);
    LOG("GPU event wait preparation: ready=%llu readyKeptPending=%llu progressSubmitChecks=%llu",
        (unsigned long long)perf_metrics.event_wait_ready,(unsigned long long)perf_metrics.event_wait_kept_pending,
        (unsigned long long)perf_metrics.event_wait_progress);
    uint64_t site_submits=0;
    for(unsigned i=0;i<32;i++)if(perf_metrics.submit_sites[i].submits) {
        site_submits+=perf_metrics.submit_sites[i].submits;
        LOG("GPU submission site: function=%s line=%u submits=%llu draws=%llu records=%llu capacity=%llu mixed=%llu",
            i==31?"overflow":perf_metrics.submit_sites[i].function,perf_metrics.submit_sites[i].line,
            (unsigned long long)perf_metrics.submit_sites[i].submits,(unsigned long long)perf_metrics.submit_sites[i].draws,
            (unsigned long long)perf_metrics.submit_sites[i].records,(unsigned long long)perf_metrics.submit_sites[i].capacity,
            (unsigned long long)perf_metrics.submit_sites[i].mixed);
    }
    if(site_submits!=perf_metrics.draw_submits)die("Vulkan submission site count mismatch");
    for(unsigned bits=0;bits<256;bits++)if(perf_metrics.nvn_barriers[bits])
        LOG("NVN barrier flags: lowBits=%#x calls=%llu activePass=%llu",bits,
            (unsigned long long)perf_metrics.nvn_barriers[bits],
            (unsigned long long)perf_metrics.nvn_active_barriers[bits]);
    if(perf_metrics.nvn_unknown_barriers)LOG("NVN barrier unknown flags: calls=%llu bits=%#x",
        (unsigned long long)perf_metrics.nvn_unknown_barriers,perf_metrics.nvn_unknown_bits);
    LOG("GPU timeline completion: submits=%llu counterPolls=%llu waits=%llu cachedWaits=%llu waitMs=%.3f",
        (unsigned long long)perf_metrics.timeline_submits,(unsigned long long)perf_metrics.timeline_polls,
        (unsigned long long)perf_metrics.timeline_waits,(unsigned long long)perf_metrics.timeline_wait_cached,perf_metrics.timeline_wait_ms);
    for(unsigned i=0;i<32;i++) {
        const completion_wait_site *s=&perf_metrics.completion_sites[i];if(!s->attempts)continue;
        LOG("GPU completion wait source: site=%s probes=%llu ready=%llu notReady=%llu queryMs=%.3f waits=%llu waitMs=%.3f",
            s->site,(unsigned long long)s->attempts,(unsigned long long)s->ready,
            (unsigned long long)(s->attempts-s->ready),s->query_ms,(unsigned long long)s->waits,s->wait_ms);
    }
    for(unsigned i=0;i<32;i++) {
        const report_wait_site *s=&perf_metrics.report_sites[i];if(!s->hits)continue;
        LOG("Report wait source: site=%s via=%s hits=%llu event=%llu timestamp=%llu samples=%llu pending=%llu drawSubmits=%llu timelineWaits=%llu timelineMs=%.3f fenceWaits=%llu fenceMs=%.3f",
            s->site,s->via,(unsigned long long)s->hits,(unsigned long long)s->events,(unsigned long long)s->timestamps,
            (unsigned long long)s->samples,(unsigned long long)s->pending,(unsigned long long)s->submits,
            (unsigned long long)s->timeline_waits,s->timeline_ms,(unsigned long long)s->fence_waits,s->fence_ms);
    }
    LOG("GPU completion poll reuse: emptyDrawPollSkips=%llu pressurePollReuse=%llu",
        (unsigned long long)perf_metrics.timeline_empty_draw_polls,(unsigned long long)perf_metrics.timeline_pressure_reused);
    LOG("GPU shared compute: recorded=%llu copies=%llu imageCopies=%llu batches=%llu waitSubmits=%llu pendingPolls=%llu",
        (unsigned long long)perf_metrics.shared_compute_dispatches,(unsigned long long)perf_metrics.shared_compute_copies,
        (unsigned long long)perf_metrics.shared_image_copies,(unsigned long long)perf_metrics.shared_compute_batches,
        (unsigned long long)perf_metrics.shared_compute_wait_submits,(unsigned long long)perf_metrics.shared_compute_pending_polls);
    LOG("GPU upload hazard polling: noOverlapSkips=%llu overlapPolls=%llu",
        (unsigned long long)perf_metrics.upload_no_overlap_polls_avoided,(unsigned long long)perf_metrics.upload_overlap_polls);
    LOG("GPU pass cache: hits=%llu misses=%llu capacityFallback=%llu allocationFallback=%llu",
        (unsigned long long)perf_metrics.pass_cache_hits,(unsigned long long)perf_metrics.pass_cache_misses,
        (unsigned long long)perf_metrics.pass_capacity_fallback,(unsigned long long)perf_metrics.pass_allocation_fallback);
    LOG("GPU attachment cache: hits=%llu misses=%llu capacityFallback=%llu uncachedPass=%llu allocationFallback=%llu cacheOnlyBatched=%llu",
        (unsigned long long)perf_metrics.attachment_cache_hits,(unsigned long long)perf_metrics.attachment_cache_misses,
        (unsigned long long)perf_metrics.attachment_capacity_fallback,(unsigned long long)perf_metrics.attachment_uncached_pass,
        (unsigned long long)perf_metrics.attachment_allocation_fallback,(unsigned long long)perf_metrics.cache_only_batched);
    LOG("GPU query segments: segments=%llu resetPrefixBatches=%llu sampleMarkersKeptPass=%llu eventMarkersKeptPass=%llu",
        (unsigned long long)perf_metrics.query_segments,(unsigned long long)perf_metrics.query_prefix_batches,
        (unsigned long long)perf_metrics.sample_pass_kept,(unsigned long long)perf_metrics.event_pass_kept);
    LOG("GPU full memory barriers: encoded=%llu consecutiveMerged=%llu",
        (unsigned long long)perf_metrics.full_barrier_encoded,(unsigned long long)perf_metrics.full_barrier_merged);
    LOG("GPU occlusion query pages: size=64 indices=%llu creates=%llu reuses=%llu resetCommands=%llu resetSlots=%llu timestampSingleResets=%llu standaloneSingleResets=%llu",
        (unsigned long long)perf_metrics.query_page_indices,(unsigned long long)perf_metrics.query_page_creates,
        (unsigned long long)perf_metrics.query_page_reuses,(unsigned long long)perf_metrics.query_page_resets,
        (unsigned long long)perf_metrics.query_page_reset_slots,(unsigned long long)perf_metrics.query_timestamp_resets,
        (unsigned long long)perf_metrics.query_standalone_resets);
    LOG("Command replay reads: calls=%llu bytes=%llu",(unsigned long long)perf_metrics.guest_reads,
        (unsigned long long)perf_metrics.guest_read_bytes);
    LOG("GPU uniform updates: commands=%llu bytes=%llu",(unsigned long long)perf_metrics.uniform_gpu_updates,
        (unsigned long long)perf_metrics.uniform_gpu_update_bytes);
    LOG("GPU event wait witness: hits=%llu unsubmitted=%llu noSource=%llu noPost=%llu reportAlias=%llu foreignSource=%llu add=%llu valueMismatch=%llu bufferAlias=%llu imageAlias=%llu",
        (unsigned long long)perf_metrics.event_witness_hits,(unsigned long long)perf_metrics.event_witness_pending,
        (unsigned long long)perf_metrics.event_witness_miss[0],(unsigned long long)perf_metrics.event_witness_miss[1],
        (unsigned long long)perf_metrics.event_witness_miss[2],(unsigned long long)perf_metrics.event_witness_miss[3],
        (unsigned long long)perf_metrics.event_witness_miss[4],(unsigned long long)perf_metrics.event_witness_miss[5],
        (unsigned long long)perf_metrics.event_witness_miss[6],(unsigned long long)perf_metrics.event_witness_miss[7]);
    LOG("CPU WSI calls: acquires=%llu acquireMs=%.3f acquireMaxMs=%.3f submits=%llu submitMs=%.3f submitMaxMs=%.3f presents=%llu presentMs=%.3f presentMaxMs=%.3f",
        (unsigned long long)perf_metrics.wsi_acquires,perf_metrics.wsi_acquire_ms,perf_metrics.wsi_acquire_max_ms,
        (unsigned long long)perf_metrics.wsi_submits,perf_metrics.wsi_submit_ms,perf_metrics.wsi_submit_max_ms,
        (unsigned long long)perf_metrics.wsi_presents,perf_metrics.wsi_present_ms,perf_metrics.wsi_present_max_ms);
    LOG("GPU draw command pool: allocations=%llu reuses=%llu",
        (unsigned long long)perf_metrics.draw_cb_alloc,(unsigned long long)perf_metrics.draw_cb_reuse);
    for(unsigned k=0;k<PERF_KINDS;k++)LOG("GPU sampled queue span: kind=%s stride=32 samples=%llu sumMs=%.3f maxMs=%.3f dropped=%llu",
        k==PERF_DRAW?"draw":k==PERF_COMPUTE?"compute":"mixed",(unsigned long long)perf_metrics.gpu[k].samples,
        perf_metrics.gpu[k].sum_ms,perf_metrics.gpu[k].max_ms,(unsigned long long)perf_metrics.gpu[k].dropped);
    LOG("CPU GPU completion: fenceWaits=%llu waitMs=%.3f timeouts=%llu polls=%llu ready=%llu notReady=%llu queueIdle=%llu queueIdleMs=%.3f deviceIdle=%llu deviceIdleMs=%.3f drawSubmits=%llu fullBatches=%llu nonFullBatches=%llu",
        (unsigned long long)perf_metrics.waits,perf_metrics.wait_ms,(unsigned long long)perf_metrics.timeouts,
        (unsigned long long)perf_metrics.polls,(unsigned long long)perf_metrics.ready,(unsigned long long)perf_metrics.not_ready,
        (unsigned long long)perf_metrics.queue_idle,perf_metrics.queue_idle_ms,
        (unsigned long long)perf_metrics.device_idle,perf_metrics.device_idle_ms,
        (unsigned long long)perf_metrics.draw_submits,(unsigned long long)perf_metrics.draw_capacity,
        (unsigned long long)(perf_metrics.draw_submits-perf_metrics.draw_capacity));
    for(unsigned i=0;i<8;i++)if(perf_metrics.idle_sites[i].function)
        LOG("CPU queue idle site: function=%s line=%u count=%llu ms=%.3f",perf_metrics.idle_sites[i].function,
            perf_metrics.idle_sites[i].line,(unsigned long long)perf_metrics.idle_sites[i].count,perf_metrics.idle_sites[i].ms);
    if(perf_metrics.idle_site_overflow)LOG("CPU queue idle site: overflowCount=%llu overflowMs=%.3f",
        (unsigned long long)perf_metrics.idle_site_overflow,perf_metrics.idle_site_overflow_ms);
    memset(&perf_metrics,0,sizeof perf_metrics);
}
static void gpu_performance_close(void) {
    for(unsigned k=0;k<PERF_KINDS;k++)for(unsigned i=0;i<PERF_SLOTS;i++)gpu_probe_destroy(&gpu_probes[k][i]);
    memset(gpu_probe_sequence,0,sizeof gpu_probe_sequence);gpu_probe_disabled=0;
}
// Only existing backend calls below this include are instrumented.
#define vkWaitForFences perf_wait
#define vkGetFenceStatus perf_poll
#define vkQueueWaitIdle(queue) perf_queue_idle((queue),__func__,__LINE__)
#define vkDeviceWaitIdle perf_device_idle
