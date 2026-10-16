/* Upstream b94670c translation queue, with Vulkan completion kept separate
 * from CPU translation completion. Command memory follows NVN fence lifetime;
 * the caller's handle array and present parameters are owned by each job. */
#include <time.h>
#include <errno.h>
enum { XJ_SUBMIT, XJ_PRESENT, XJ_FENCE, XJ_SYNC_READY };
typedef struct {
    int kind,n,flags;
    queue_t *queue;
    uint64_t *handles,sub;
    sync_t *sync;
    uint64_t generation;
    dm_compute_image image;
    int crop[4];
    double enqueued_ms; // Diagnostic only; never used for scheduling/deadlines.
} xjob;
#define XQ 32
#define XTK 1024
static xjob g_xq[XQ];
static struct {uint64_t job,serial;} g_xtickets[XTK];
static uint64_t g_xq_in,g_xq_done;
static pthread_mutex_t g_xmu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_xcv=PTHREAD_COND_INITIALIZER;
static pthread_once_t g_xonce=PTHREAD_ONCE_INIT;
static int g_x_sync;
static _Thread_local uint64_t g_x_executing;
static pthread_mutex_t g_sync_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sync_cv=PTHREAD_COND_INITIALIZER;
static uint64_t g_sync_generation;
// g_xmu owns these counters. Worker busy/empty time is wall time, including
// descheduling/driver waits; caller wait totals may overlap on several threads.
static struct {
    uint64_t jobs[4],idle_episodes,idle_polls,backpressure,wait_calls[2],wait_blocks[2],wait_timeouts[2];
    double job_ms[4],queue_ms[4],queue_max_ms[4],idle_ms,backpressure_ms,wait_ms[2];
    uint64_t max_depth;
} g_xperf;
static double g_xperf_start;
static void render_epoch_report(void);
static double x_perf_now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1000.0+t.tv_nsec/1e6;
}
static void x_perf_report_locked(double now) {
    if(now-g_xperf_start<5000)return;
    __typeof__(g_xperf) s=g_xperf;memset(&g_xperf,0,sizeof g_xperf);
    double span=now-g_xperf_start;g_xperf_start=now;
    pthread_mutex_unlock(&g_xmu);
    render_epoch_report();
    LOG("Translation occupancy: wallMs=%.3f idleMs=%.3f idleEpisodes=%llu idlePolls=%llu maxDepth=%llu backpressure=%llu backpressureMs=%.3f mode=worker",
        span,s.idle_ms,(unsigned long long)s.idle_episodes,(unsigned long long)s.idle_polls,
        (unsigned long long)s.max_depth,(unsigned long long)s.backpressure,s.backpressure_ms);
    for(unsigned i=0;i<4;i++)LOG("Translation job: kind=%s jobs=%llu wallMs=%.3f queueMs=%.3f queueMaxMs=%.3f",
        i==XJ_SUBMIT?"submit":i==XJ_PRESENT?"present":i==XJ_FENCE?"fence":"sync-ready",
        (unsigned long long)s.jobs[i],s.job_ms[i],s.queue_ms[i],s.queue_max_ms[i]);
    for(unsigned i=0;i<2;i++)LOG("Translation caller wait: kind=%s calls=%llu blocked=%llu timeouts=%llu wallMs=%.3f totalsMayOverlap=1",
        i?"drain":"present-reuse",(unsigned long long)s.wait_calls[i],(unsigned long long)s.wait_blocks[i],
        (unsigned long long)s.wait_timeouts[i],s.wait_ms[i]);
    pthread_mutex_lock(&g_xmu);
}
static uint64_t sync_begin(sync_t *s) {
    pthread_mutex_lock(&g_sync_mu);
    uint64_t generation=++g_sync_generation;
    if(!generation)die("NVN sync generation overflow");
    s->generation=generation;
    dm_vk_sync_release(s->ticket);s->ticket=NULL;
    s->fenced=1;s->published=0;s->fence=0;
    pthread_cond_broadcast(&g_sync_cv);
    pthread_mutex_unlock(&g_sync_mu);return generation;
}
static void sync_publish(sync_t *s,uint64_t generation,dm_vk_sync_ticket *ticket) {
    pthread_mutex_lock(&g_sync_mu);
    if(s->generation==generation) {
        s->ticket=ticket;s->published=generation;
        pthread_cond_broadcast(&g_sync_cv);
    } else dm_vk_sync_release(ticket);
    pthread_mutex_unlock(&g_sync_mu);
}
static void sync_worker_publish(sync_t *s,uint64_t generation,dm_vk_sync_ticket *ticket) {
    if(!g_x_executing || !generation)die("NVN sync worker publication outside translation FIFO");
    pthread_mutex_lock(&g_sync_mu);
    if(s->magic!=MAGIC('S','Y','N','C'))die("NVN sync worker publication after finalization");
    // Own a separate reference before CPU publication may reject/release an
    // obsolete generation. This function borrows its caller's ticket.
    dm_vk_sync_retain(ticket);
    dm_vk_sync_release(s->worker_ticket);
    s->worker_ticket=ticket;s->worker_published=generation;
    pthread_mutex_unlock(&g_sync_mu);
}
static void replay_submit(queue_t *queue,int n,const uint64_t *handles);
static void x_run(const xjob *j,uint64_t job) {
    g_x_executing=job;
    if(j->kind==XJ_SUBMIT) {
        replay_submit(j->queue,j->n,j->handles);
        free(j->handles);
        snap_tick(j->sub);
    } else if(j->kind==XJ_PRESENT) {
        uint64_t serial=0;
        if(dm_vk_present_texture(&j->image,j->crop,&serial))die("Vulkan queued texture present failed");
        pthread_mutex_lock(&g_xmu);
        uint64_t old=g_xtickets[job%XTK].serial;
        pthread_mutex_unlock(&g_xmu);
        // An overwritten ticket may still belong to an inactive NVN window.
        // Prove its GPU copy complete before forgetting its exact serial.
        if(old && dm_vk_present_wait(old))die("Vulkan old present ticket retirement failed");
        pthread_mutex_lock(&g_xmu);
        g_xtickets[job%XTK].job=job;g_xtickets[job%XTK].serial=serial;
        pthread_mutex_unlock(&g_xmu);
        extern void dm_android_input_frame_tick(void);
        dm_android_input_frame_tick();
    } else if(j->kind==XJ_FENCE) {
        dm_vk_sync_ticket *ticket=dm_vk_sync_submit(j->flags);
        if(!ticket)die("Vulkan queued fence submission failed");
        sync_worker_publish(j->sync,j->generation,ticket);
        sync_publish(j->sync,j->generation,ticket);
    } else if(j->kind==XJ_SYNC_READY) {
        // Window acquisition completed on the CPU. Publish its ready condition
        // here so it cannot overwrite a wait earlier in the translation FIFO.
        sync_worker_publish(j->sync,j->generation,NULL);
    } else die("Invalid translation job");
    // Synchronous fallback has no worker to publish events after returning.
    // Normal translation only polls completed fences, without waiting.
    if(g_x_sync?dm_vk_events_drain():dm_vk_completion_poll())die("Vulkan event completion progress failed");
    g_x_executing=0;
}
static void *x_thread(void *arg) {
    (void)arg;
    extern void crash_thread_init(void);
    crash_thread_init();
    pthread_setname_np(pthread_self(),"nvn-translate");
    dm_host_thread_priority(-4,"nvn-translate"); // Display-critical translation worker.
    pthread_mutex_lock(&g_xmu);
    g_xperf_start=x_perf_now();
    for(;;) {
        double idle_started=0;
        if(g_xq_done==g_xq_in){idle_started=x_perf_now();g_xperf.idle_episodes++;}
        while(g_xq_done==g_xq_in) {
            int result;
            if(dm_vk_completion_pending()) {
                struct timespec deadline;
                if(clock_gettime(CLOCK_REALTIME,&deadline))die("Translation event deadline failed");
                deadline.tv_nsec+=1000000;
                if(deadline.tv_nsec>=1000000000){deadline.tv_nsec-=1000000000;deadline.tv_sec++;}
                result=pthread_cond_timedwait(&g_xcv,&g_xmu,&deadline);
                if(result && result!=ETIMEDOUT)die("Translation event completion wait failed");
                // Never acquire the backend lock while holding the job mutex.
                pthread_mutex_unlock(&g_xmu);
                if(dm_vk_completion_poll())die("Vulkan idle event completion poll failed");
                pthread_mutex_lock(&g_xmu);
                g_xperf.idle_polls++;
            } else {
                result=pthread_cond_wait(&g_xcv,&g_xmu);
                if(result)die("Translation queue wait failed");
            }
        }
        uint64_t job=g_xq_done+1;
        xjob j=g_xq[job%XQ];
        double started=x_perf_now();
        if(idle_started)g_xperf.idle_ms+=started-idle_started;
        double age=started-j.enqueued_ms;g_xperf.queue_ms[j.kind]+=age;
        if(age>g_xperf.queue_max_ms[j.kind])g_xperf.queue_max_ms[j.kind]=age;
        pthread_mutex_unlock(&g_xmu);
        x_run(&j,job);
        double ended=x_perf_now();
        pthread_mutex_lock(&g_xmu);
        g_xperf.jobs[j.kind]++;g_xperf.job_ms[j.kind]+=ended-started;
        memset(&g_xq[job%XQ],0,sizeof(xjob));
        g_xq_done=job;
        pthread_cond_broadcast(&g_xcv);
        if(j.kind==XJ_PRESENT)x_perf_report_locked(ended);
    }
    return NULL;
}
static void x_start(void) {
    const char *e=getenv("DM_NVN_SYNC");g_x_sync=e && atoi(e);
    if(!g_x_sync) {
        pthread_attr_t attr;pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr,8u<<20);
        pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        if(pthread_create(&thread,&attr,x_thread,NULL))g_x_sync=1;
        pthread_attr_destroy(&attr);
    }
    LOG("[nvn] translation %s; bounded queue=%u",g_x_sync?"synchronous":"worker",XQ);
}
static uint64_t x_push(xjob *j) {
    if(g_x_executing)die("Recursive translation enqueue");
    pthread_once(&g_xonce,x_start);
    pthread_mutex_lock(&g_xmu);
    // In synchronous mode only one caller executes at a time, preserving FIFO
    // even when several guest threads submit concurrently.
    double blocked_started=0;
    if(g_xq_in-g_xq_done>=XQ || (g_x_sync && g_xq_in!=g_xq_done)) {
        blocked_started=x_perf_now();g_xperf.backpressure++;
    }
    while(g_xq_in-g_xq_done>=XQ || (g_x_sync && g_xq_in!=g_xq_done))
        pthread_cond_wait(&g_xcv,&g_xmu);
    if(blocked_started)g_xperf.backpressure_ms+=x_perf_now()-blocked_started;
    if(j->kind==XJ_SUBMIT)j->sub=atomic_fetch_add(&g_x_subs,1)+1;
    uint64_t job=++g_xq_in;
    if(g_x_sync) {
        pthread_mutex_unlock(&g_xmu);x_run(j,job);pthread_mutex_lock(&g_xmu);
        g_xq_done=job;pthread_cond_broadcast(&g_xcv);
    } else {
        j->enqueued_ms=x_perf_now();
        g_xq[job%XQ]=*j;pthread_cond_broadcast(&g_xcv);
        uint64_t depth=g_xq_in-g_xq_done;if(depth>g_xperf.max_depth)g_xperf.max_depth=depth;
    }
    pthread_mutex_unlock(&g_xmu);return job;
}
static uint64_t x_frontier(void) {
    pthread_mutex_lock(&g_xmu);uint64_t job=g_xq_in;
    pthread_mutex_unlock(&g_xmu);return job;
}
// Return 1 on completion, 0 on timeout. A worker may only wait for earlier
// jobs, which are already complete; waiting for itself would deadlock.
static int x_wait_timeout(uint64_t job,uint64_t timeout,unsigned reason) {
    if(g_x_executing) {
        if(job>=g_x_executing)die("Translation worker attempted to wait on itself or a later job");
        return 1;
    }
    struct timespec deadline={0};
    if(timeout && timeout!=UINT64_MAX) {
        clock_gettime(CLOCK_REALTIME,&deadline);
        uint64_t seconds=timeout/1000000000ull;
        if(seconds>315360000ull)seconds=315360000ull;
        uint64_t ns=(uint64_t)deadline.tv_nsec+timeout%1000000000ull;
        deadline.tv_sec+=(time_t)(seconds+ns/1000000000ull);
        deadline.tv_nsec=(long)(ns%1000000000ull);
    }
    pthread_mutex_lock(&g_xmu);
    g_xperf.wait_calls[reason]++;
    double blocked_started=0;
    int completed=1;
    if(timeout && g_xq_done<job){blocked_started=x_perf_now();g_xperf.wait_blocks[reason]++;}
    while(g_xq_done<job) {
        if(!timeout){completed=0;break;}
        int r=timeout==UINT64_MAX?pthread_cond_wait(&g_xcv,&g_xmu):pthread_cond_timedwait(&g_xcv,&g_xmu,&deadline);
        if(r==ETIMEDOUT && g_xq_done<job){completed=0;break;}
        if(r && r!=ETIMEDOUT)die("Translation completion wait failed");
    }
    if(blocked_started)g_xperf.wait_ms[reason]+=x_perf_now()-blocked_started;
    if(!completed)g_xperf.wait_timeouts[reason]++;
    pthread_mutex_unlock(&g_xmu);return completed;
}
static void x_wait(uint64_t job) { (void)x_wait_timeout(job,UINT64_MAX,1); }
static void x_present_wait(uint64_t job) {
    if(!job)return;
    (void)x_wait_timeout(job,UINT64_MAX,0);
    pthread_mutex_lock(&g_xmu);
    uint64_t stored=g_xtickets[job%XTK].job,serial=g_xtickets[job%XTK].serial;
    pthread_mutex_unlock(&g_xmu);
    if(stored==job) {
        if(dm_vk_present_wait(serial))die("Vulkan NVN image reuse wait failed");
    } else if(stored<job)die("NVN presentation ticket was not published");
    // A newer occupant explicitly completed this ticket before replacing it.
}
static void nvn_translate_drain(void) {
    if(g_x_executing)return;
    x_wait(x_frontier());
}
