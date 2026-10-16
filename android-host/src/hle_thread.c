// Horizon 线程与同步 SVC — 移植自上游 dm/host/hle_thread.c。
// 剥离：mach（线程端口/挂起/回溯）、QoS、bootprof、detclock、现场导出诊断。
// 保留：全局锁 G + 每线程条件变量的等待模型、锁交接、条件变量「先置键后放锁」顺序、地址仲裁器语义。
#include "dm_host.h"
#include <errno.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

// Host scheduling hint corresponding to upstream's interactive QoS. Horizon
// priorities/core masks remain independent bookkeeping; never force affinity.
void dm_host_thread_priority(int priority,const char *role) {
    int result=setpriority(PRIO_PROCESS,0,priority),error=result?errno:0;
    LOG("Android thread priority: role=%s tid=%d requested=%d result=%d errno=%d",role,gettid(),priority,result,error);
}

#define R_TIMEOUT     0xEA01u
#define R_CANCELLED   0xEC01u
#define R_BAD_HANDLE  0xE401u
#define R_BAD_STATE   0xFA01u
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W };
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    int started, exited, cancel, closed;
    pthread_t pt; pthread_cond_t cv;
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;
    uint32_t sync_handles[64];
    unsigned sync_count;
    int sync_active, sync_snapshot, sync_invalidated;
    struct thr *next;
    struct thr *all;
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static int g_keyed;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;
typedef struct {
    uint64_t calls,polls,blocks,ready,timeouts,cancelled,invalidated,rechecks,rewaits;
    uint64_t signals[2],candidates[2],matched[2],skipped[2],closes;
} sync_perf_t;
static sync_perf_t sync_perf;

void svc_thread_perf_report(void) {
    pthread_mutex_lock(&G);sync_perf_t s=sync_perf;memset(&sync_perf,0,sizeof sync_perf);pthread_mutex_unlock(&G);
    LOG("Host synchronization: calls=%llu polls=%llu blocks=%llu ready=%llu timeouts=%llu cancelled=%llu invalidated=%llu rechecks=%llu noReadyRewait=%llu closeInvalidations=%llu",
        (unsigned long long)s.calls,(unsigned long long)s.polls,(unsigned long long)s.blocks,(unsigned long long)s.ready,
        (unsigned long long)s.timeouts,(unsigned long long)s.cancelled,(unsigned long long)s.invalidated,
        (unsigned long long)s.rechecks,(unsigned long long)s.rewaits,(unsigned long long)s.closes);
    for(unsigned i=0;i<2;i++)LOG("Host synchronization wake: source=%s signals=%llu candidates=%llu matched=%llu skipped=%llu",
        i?"thread-exit":"event",(unsigned long long)s.signals[i],(unsigned long long)s.candidates[i],
        (unsigned long long)s.matched[i],(unsigned long long)s.skipped[i]);
}

static uint64_t now_ns_(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }

void guest_thread_full_init(void);   // tsd.c：门槽 + TLS 块 + TLR（含线程对象）
static void se_forget(uint32_t handle);

static uint32_t h_new(int type, void *obj) {
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}

static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv); }
static int64_t block(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    if (ns > 0) {
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    }
    while (!t->woke) {
        int rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// Match canonical objects under G, so event read/write handle aliases agree.
// Unsupported counts retain the old conservative wake behavior.
static int sync_interested(thr_t *t,int type,const void *object) {
    if(!t->sync_snapshot)return 1;
    for(unsigned i=0;i<t->sync_count;i++)if(h_peek(t->sync_handles[i],type)==object)return 1;
    return 0;
}
static void wake_sync_object(int type,void *object,unsigned reason) {
    sync_perf.signals[reason]++;
    for(thr_t *w=g_wait;w;) {
        thr_t *next=w->next;
        if(w->wkind==4) {
            sync_perf.candidates[reason]++;
            if(sync_interested(w,type,object)){sync_perf.matched[reason]++;wake(w,-1);}
            else sync_perf.skipped[reason]++;
        }
        w=next;
    }
}
static void sync_handle_closing(uint32_t handle) {
    // Include calls already signalled but still waiting to reacquire G. A
    // numeric handle may be reused before they run; remember invalidation.
    for(thr_t *w=g_all;w;w=w->all)if(w->sync_active) {
        int found=!w->sync_snapshot;
        for(unsigned i=0;!found && i<w->sync_count;i++)found=w->sync_handles[i]==handle;
        if(!found)continue;
        if(w->sync_snapshot){w->sync_invalidated=1;sync_perf.closes++;}
        if(w->wkind==4)wake(w,-1);
    }
}

static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;
    t->wseq = ++g_wseq;
}

extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);
uint64_t dm_gate_stack(void);   // entry.S
static void *thread_main(void *p) {
    thr_t *t = p;
    dm_host_thread_priority(-2,"guest"); // Android THREAD_PRIORITY_FOREGROUND.
    pthread_setspecific(g_keythr, t);
    guest_thread_full_init();
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    dm_thread_enter(t->entry, t->arg, t->stack_top);
    die("客体线程入口返回");
}
static void thr_free(thr_t *t) {
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}
static void thread_exit(thr_t *t) {
    pthread_mutex_lock(&G);
    t->exited = 1;
    wake_sync_object(O_THREAD,t,1);
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}

uint32_t svc_thread_main_init(void) {
    dm_host_thread_priority(-2,"guest-main");
    if (!g_keyed) { pthread_key_create(&g_keythr, NULL); g_keyed = 1; }
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self();
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t);
    return t->handle;
}

static int sig_obj(uint32_t h, int *bad) {
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static int64_t now_mono_ns(void) { return (int64_t)now_ns_(); }

int svc_thread(int num, uint64_t *regs) {
    switch (num) {
    case 0x08: {
        thr_t *t = calloc(1, sizeof *t);
        pthread_cond_init(&t->cv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        LOG("CreateThread h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d", t->handle,
            (unsigned long long)t->entry, (unsigned long long)t->arg, (unsigned long long)t->stack_top, t->prio);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {
        int64_t ns = (int64_t)regs[0];
        if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
        else sched_yield();
        return 1;
    }
    case 0x0C: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; return 1; }
    case 0x0D: { thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE; return 1; }
    case 0x0E: { thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; return 1; }
    case 0x0F: {
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2]; }
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x10: regs[0] = (uint64_t)(uint32_t)cur()->core; return 1;
    case 0x25: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; return 1; }
    case 0x1A: {
        thr_t *t = cur(); uint64_t addr = regs[1];
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr);
        int64_t r = block(t, 2, (int64_t)regs[3]);
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    case 0x34: {
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x45: {
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {
        pthread_mutex_lock(&G);
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_object(O_EVENT_R,e,0); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)(uintptr_t)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        int64_t deadline = ns > 0 ? now_mono_ns() + ns : 0;
        pthread_mutex_lock(&G);
        sync_perf.calls++;sync_perf.polls+=ns==0;
        t->sync_snapshot=n>=0 && n<=64;t->sync_count=t->sync_snapshot?(unsigned)n:0;
        if(t->sync_count)memcpy(t->sync_handles,hs,t->sync_count*sizeof *hs);
        if(t->sync_snapshot)hs=t->sync_handles;
        t->sync_active=1;t->sync_invalidated=0;
        int recheck=0;
        int64_t r; int idx = -1;
        for (;;) {
            if(recheck)sync_perf.rechecks++;
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED;sync_perf.cancelled++;break; }
            if(t->sync_invalidated){r=R_BAD_HANDLE;sync_perf.invalidated++;break;}
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0;sync_perf.ready++;break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - now_mono_ns();
            if (ns > 0 && left <= 0) { r = R_TIMEOUT;sync_perf.timeouts++;break; }
            if(left!=0){sync_perf.blocks++;if(recheck)sync_perf.rewaits++;}
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT;sync_perf.timeouts++;break; }
            recheck=1;
        }
        t->sync_active=t->sync_snapshot=t->sync_invalidated=0;t->sync_count=0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) t->cancel = 1;
        for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w == t) wake(w, R_CANCELLED); w = n; }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {
        pthread_mutex_lock(&G);
        uint32_t h = (uint32_t)regs[0];
        unsigned i = h - 0x1000;
        int64_t r = 0;
        if (i == 0 || i >= MAX_H || !g_h[i].type) r = R_BAD_HANDLE;
        else if (g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            sync_handle_closing(h);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        } else if (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W) {
            event_t *e = g_h[i].obj;
            sync_handle_closing(h);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            if (--e->refs <= 0) free(e);
        }
        pthread_mutex_unlock(&G);
        if(!r) se_forget(h);
        regs[0] = (uint64_t)r; return 1;
    }
    default: return 0;
    }
}

static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_se_bind_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
void hle_bind_system_event(void *ev, int autoclear) {      // 必须在客体线程上调（要执行 SDK 代码） | Must be called on a guest thread (it has to execute SDK code)
    if(!ev) die("NULL SystemEvent");
    pthread_mutex_lock(&g_se_bind_lock);
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) { pthread_mutex_unlock(&g_se_bind_lock); return; }
    if (!g_keyed || !pthread_getspecific(g_keythr)) die("hle_bind_system_event 须在客体线程上调用");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    if(!create || !getw) die("Missing SDK SystemEvent symbols");
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent 失败 %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 绑定表满");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
    pthread_mutex_unlock(&g_se_bind_lock);
}
void hle_signal_system_event(void *ev) {   // 未绑定的按手动清除即时绑定（只能在客体线程上发生） | An unbound one is bound on the spot as manual-clear (can only happen on a guest thread)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) { hle_bind_system_event(ev, 0); pthread_mutex_lock(&g_se_lock); wh = se_find(ev); pthread_mutex_unlock(&g_se_lock); }
    pthread_mutex_lock(&G);
    event_t *e = h_get(wh, O_EVENT_W);
    if (!e) die("SystemEvent 写端句柄失效 %#x", wh);
    e->signaled = 1; wake_sync_object(O_EVENT_R,e,0);
    pthread_mutex_unlock(&G);
}
// A destroyed/reused guest SystemEvent must get a fresh binding, even if the
// numeric kernel handle is later reused. Called after dropping G.
static void se_forget(uint32_t handle) {
    pthread_mutex_lock(&g_se_lock);
    for(int i=0;i<g_nse;i++) if(g_se[i].wh==handle) { g_se[i]=g_se[--g_nse];break; }
    pthread_mutex_unlock(&g_se_lock);
}
