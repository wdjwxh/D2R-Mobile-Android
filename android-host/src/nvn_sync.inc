// Translation publishes a retained GPU token; only CPU SyncWait waits for it.
NVN(nvnCommandBufferWaitEvent,void,NVNcommandBuffer *c,const NVNevent *event,NVNeventWaitMode mode,uint32_t value) {
    const ev_t *e=(const ev_t *)event;
    if(!e || e->magic!=MAGIC('E','V','N','T') || (mode!=NVN_EVENT_WAIT_MODE_EQUAL && mode!=NVN_EVENT_WAIT_MODE_GEQUAL_WRAP))die("Invalid NVN event wait");
    uint8_t *p=cmd_alloc(CB(c),24);put_hdr(p,OP_WAIT_EVENT,24);
    uint64_t address=(uintptr_t)e->val,comparison=(uint32_t)mode|((uint64_t)value<<32);
    memcpy(p+8,&address,8);memcpy(p+16,&comparison,8);
}
static uint64_t nvn_sync_now_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static NVNsyncWaitResult sync_wait_result(const sync_t *s,uint64_t timeout,int cpu_wait) {
    if(!s || s->magic!=MAGIC('S','Y','N','C'))return NVN_SYNC_WAIT_RESULT_FAILED;
    uint64_t started=nvn_sync_now_ns();
    struct timespec deadline={0};
    if(timeout && timeout!=UINT64_MAX) {
        clock_gettime(CLOCK_REALTIME,&deadline);
        uint64_t seconds=timeout/1000000000ull;
        if(seconds>315360000ull)seconds=315360000ull;
        uint64_t ns=(uint64_t)deadline.tv_nsec+timeout%1000000000ull;
        deadline.tv_sec+=(time_t)(seconds+ns/1000000000ull);
        deadline.tv_nsec=(long)(ns%1000000000ull);
    }
    pthread_mutex_lock(&g_sync_mu);
    if(!s->fenced){pthread_mutex_unlock(&g_sync_mu);return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;}
    uint64_t generation=s->generation;
    int waited=0;
    while(s->published!=generation) {
        if(s->generation!=generation){pthread_mutex_unlock(&g_sync_mu);return NVN_SYNC_WAIT_RESULT_FAILED;}
        if(!timeout){pthread_mutex_unlock(&g_sync_mu);return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;}
        if(g_x_executing)die("Translation worker cannot wait for a later fence producer");
        waited=1;
        int r=timeout==UINT64_MAX?pthread_cond_wait(&g_sync_cv,&g_sync_mu):pthread_cond_timedwait(&g_sync_cv,&g_sync_mu,&deadline);
        if(r==ETIMEDOUT && s->published!=generation){pthread_mutex_unlock(&g_sync_mu);return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;}
        if(r && r!=ETIMEDOUT)die("NVN sync completion wait failed");
    }
    dm_vk_sync_ticket *ticket=s->ticket;
    dm_vk_sync_retain(ticket); // Same lock as generation publication/replacement.
    pthread_mutex_unlock(&g_sync_mu);
    uint64_t remaining=timeout;
    if(timeout!=UINT64_MAX) {
        uint64_t elapsed=nvn_sync_now_ns()-started;
        remaining=elapsed>=timeout?0:timeout-elapsed;
    }
    int result=cpu_wait?dm_vk_sync_wait(ticket,remaining,&waited):dm_vk_sync_ordered(ticket);
    dm_vk_sync_release(ticket);
    // Retaining the published token captures this wait's fence condition.
    // Refencing/reacquiring the sync changes later waits, not this one: both
    // CPU waits and queue ordering keep the old token alive independently.
    // Before publication the generation check above remains necessary, since
    // there is no captured condition yet. Backend errors (including device
    // epoch invalidation) still fail even after a condition was captured.
    if(result<0) {
        LOG("NVN sync wait failed: sync=%p cpu=%d captured=%llu backend=%d worker=%llu",
            (void *)s,cpu_wait,(unsigned long long)generation,result,(unsigned long long)g_x_executing);
        return NVN_SYNC_WAIT_RESULT_FAILED;
    }
    if(result>0)return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    return waited?NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED:NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
}
static void sync_wait_complete(const sync_t *s) {
    NVNsyncWaitResult result;
    if(g_x_executing) {
        pthread_mutex_lock(&g_sync_mu);
        if(!s || s->magic!=MAGIC('S','Y','N','C') || !s->worker_published)
            die("NVN translation wait has no preceding sync producer: sync=%p worker=%llu",
                (void *)s,(unsigned long long)g_x_executing);
        dm_vk_sync_ticket *ticket=s->worker_ticket;
        dm_vk_sync_retain(ticket);
        pthread_mutex_unlock(&g_sync_mu);
        int ordered=dm_vk_sync_ordered(ticket);
        dm_vk_sync_release(ticket);
        result=ordered==0?NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED:NVN_SYNC_WAIT_RESULT_FAILED;
    } else result=sync_wait_result(s,UINT64_MAX,0);
    if(result!=NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED && result!=NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED)
        die("NVN wait on invalid or unsubmitted fence: sync=%p result=%d magic=%#x fenced=%d generation=%llu published=%llu worker=%llu",
            (void *)s,result,s?s->magic:0,s?s->fenced:0,(unsigned long long)(s?s->generation:0),
            (unsigned long long)(s?s->published:0),(unsigned long long)g_x_executing);
    // The producer has actually entered the same Vulkan queue. A GPU barrier
    // orders subsequent work; neither CPU completion nor materialization is
    // required for a queue/command wait (unlike the CPU SyncWait API).
    if(dm_vk_gpu_barrier())die("Vulkan sync GPU ordering failed");
}
NVN(nvnCommandBufferWaitSync,void,NVNcommandBuffer *c,const NVNsync *s) {
    uint8_t *p=cmd_alloc(CB(c),16);put_hdr(p,OP_WAIT_SYNC,16);memcpy(p+8,&s,8);
}
NVN(nvnCommandBufferFenceSync,void,NVNcommandBuffer *c,NVNsync *s,NVNsyncCondition condition,int flags) {
    (void)c;(void)s;(void)condition;(void)flags;
    // Upstream b94670c implements QueueFenceSync only; this unused extension
    // needs per-execution fence instances for repeated/called command lists.
    // Do not record a fence that can publish a previous execution's result.
    die("NVN CommandBufferFenceSync is unsupported; per-execution command fence instances required");
}
NVN(nvnQueueWaitSync,NVNboolean,NVNqueue *q,const NVNsync *s) {
    if(((queue_t *)q)->magic!=MAGIC('Q','U','E','U'))return 0;
    sync_wait_complete((const sync_t *)s);return 1;
}
NVN(nvnSyncInitialize,NVNboolean,NVNsync *s,NVNdevice *d) {
    if(!live_device || d!=live_device) return 0;
    nvn_translate_drain();
    pthread_mutex_lock(&g_sync_mu);
    if(((sync_t *)s)->magic==MAGIC('S','Y','N','C')) {
        dm_vk_sync_release(((sync_t *)s)->ticket);
        dm_vk_sync_release(((sync_t *)s)->worker_ticket);
    }
    *(sync_t *)s=(sync_t){.magic=MAGIC('S','Y','N','C')};
    pthread_mutex_unlock(&g_sync_mu);return 1;
}
NVN(nvnSyncFinalize,void,NVNsync *s) {
    nvn_translate_drain();pthread_mutex_lock(&g_sync_mu);
    dm_vk_sync_release(((sync_t *)s)->ticket);
    dm_vk_sync_release(((sync_t *)s)->worker_ticket);
    memset(s,0,sizeof(sync_t));pthread_cond_broadcast(&g_sync_cv);pthread_mutex_unlock(&g_sync_mu);
}
NVN(nvnQueueFenceSync,void,NVNqueue *q,NVNsync *s,NVNsyncCondition condition,int flags) {
    if(((queue_t *)q)->magic!=MAGIC('Q','U','E','U') || ((sync_t *)s)->magic!=MAGIC('S','Y','N','C')) die("Invalid NVN fence");
    if(condition!=NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE && condition!=NVN_SYNC_CONDITION_GRAPHICS_WORLD_SPACE_COMPLETE)die("Invalid NVN fence condition");
    // Conservatively honor every nonzero flag, including FLUSH_FOR_CPU, even
    // though the imported header omits the flag enum. Zero means GPU ordering.
    xjob j={.kind=XJ_FENCE,.flags=flags,.sync=(sync_t *)s,.generation=sync_begin((sync_t *)s)};
    uint64_t job=x_push(&j);
    pthread_mutex_lock(&g_sync_mu);
    if(((sync_t *)s)->generation==j.generation)((sync_t *)s)->fence=job;
    pthread_mutex_unlock(&g_sync_mu);
}
NVN(nvnSyncWait,NVNsyncWaitResult,const NVNsync *s,uint64_t timeout) {
    return sync_wait_result((const sync_t *)s,timeout,1);
}
NVN(nvnEventBuilderSetDefaults,void,NVNeventBuilder *b) { memset(b,0,sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage,void,NVNeventBuilder *b,const NVNmemoryPool *p,int64_t off) { *(ev_builder_t *)b=(ev_builder_t){(const pool_t *)p,off}; }
NVN(nvnEventInitialize,NVNboolean,NVNevent *e,const NVNeventBuilder *b) {
    const ev_builder_t *s=(const ev_builder_t *)b;
    if(!pool_range(s->pool,s->off,4) || s->off%4) return 0;
    *(ev_t *)e=(ev_t){MAGIC('E','V','N','T'),(volatile uint32_t *)(s->pool->mem+s->off)};return 1;
}
NVN(nvnEventFinalize,void,NVNevent *e) { nvn_translate_drain();memset(e,0,sizeof(ev_t)); }
NVN(nvnEventGetValue,uint32_t,const NVNevent *e) {
    const ev_t *o=(const ev_t *)e;if(o->magic!=MAGIC('E','V','N','T')) die("Invalid NVN event");
    uint32_t value;if(dm_vk_guest_event_get((uintptr_t)o->val,&value))die("NVN event read visibility failed");
    return value;
}
NVN(nvnEventSignal,void,NVNevent *e,NVNeventSignalMode mode,uint32_t value) {
    ev_t *o=(ev_t *)e;if(o->magic!=MAGIC('E','V','N','T')) die("Invalid NVN event");
    if(mode!=NVN_EVENT_SIGNAL_MODE_ADD && mode!=NVN_EVENT_SIGNAL_MODE_WRITE)die("Invalid NVN event signal mode");
    if(dm_vk_guest_event((uintptr_t)o->val,value,mode==NVN_EVENT_SIGNAL_MODE_ADD))die("NVN event signal visibility failed");
}
