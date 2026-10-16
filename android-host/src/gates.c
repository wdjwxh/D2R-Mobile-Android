// 门表与分派 — 移植自上游 dm/host/hle.c（开地址哈希、按返回点查门）。
// 心跳版：svc_dispatch 只记录首次调用的 svc 号并返回 0；后续里程碑接 hle_svc.c 真实现。
#include "dm_host.h"
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

// 文件日志（logcat 限流的补充）：dm_log_open 由 bridge 在启动时调用
static FILE *g_logf;
static int g_in_gate;
static int g_logfd = -1;
// 非变参日志（翻译层下最可靠）：串 + 64 位值
void dm_log_kv(const char *tag, uint64_t v) {
    if (g_logfd < 0 && g_logf) g_logfd = fileno(g_logf);
    if (g_logfd < 0) return;
    (void)0;
    char b[256];
    int n = snprintf(b, sizeof b, "%s = %#llx", tag, (unsigned long long)v);
    b[n++] = 10;
    write(g_logfd, b, n);
    // 门内禁 logcat（变参调用毒翻译层，实测）；文件直写安全
    if (!g_in_gate) __android_log_print(ANDROID_LOG_INFO, "dmhost", "%s", b);
}

void dm_log_gate_done(void) { g_in_gate = 0; }

void dm_log_str(const char *s2) {
    if (g_logfd < 0 && g_logf) g_logfd = fileno(g_logf);
    if (g_logfd < 0) return;
    size_t n = strlen(s2);
    char nl = 10;
    write(g_logfd, s2, n);
    write(g_logfd, &nl, 1);
    if (!g_in_gate) __android_log_print(ANDROID_LOG_INFO, "dmhost", "%s", s2);
}

void dm_log_open(const char *path) {
    if (!g_logf) {
        g_logf = fopen(path, "w");
        if (g_logf) setvbuf(g_logf, NULL, _IOLBF, 8192);
        else __android_log_print(ANDROID_LOG_WARN, "dmhost", "日志文件打不开 %s errno=%d", path, errno);
    }
}
void dm_log_open(const char *path);
// 直写（无用户态缓冲）：崩溃也不丢行
void dm_log_file(const char *fmt, ...) {
    if (g_logfd < 0 && g_logf) g_logfd = fileno(g_logf);
    if (g_logfd < 0) return;
    char buf[2048];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int n = snprintf(buf, sizeof buf, "[%ld.%03ld] ", ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    buf[n++] = 10;
    write(g_logfd, buf, n);
}

enum { G_SVC, G_CNTPCT, G_CNTVCT, G_OBS };
#define OBS_MAX 64

static void (*g_obs_fn[OBS_MAX])(const uint64_t *regs);
static int g_obs_have[OBS_MAX];
void gate_obs_set(int id,void (*fn)(const uint64_t *)) {
    if(id<=0 || id>=OBS_MAX)die("Observation id out of range: %d",id);
    g_obs_fn[id]=fn;
}
int gate_obs_present(int id) { return id>0 && id<OBS_MAX && g_obs_have[id]; }


typedef struct { uint64_t ret; int kind, arg; uint64_t orig; } gate_t;
#define GATE_BITS 12
static gate_t g_gate[1u << GATE_BITS];
static int g_ngate;
static inline unsigned gate_h(uint64_t a) { return (unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - GATE_BITS)); }

void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig) {
    int k = !strcmp(kind, "svc") ? G_SVC : !strcmp(kind, "cntpct") ? G_CNTPCT : !strcmp(kind, "cntvct") ? G_CNTVCT : !strcmp(kind, "obs") ? G_OBS : -1;
    if (k < 0) die("未知门类 %s", kind);
    if (k == G_OBS) { if (arg <= 0 || arg >= OBS_MAX) die("观测门编号越界 %d", arg); g_obs_have[arg] = 1; }
    if (++g_ngate > (1 << GATE_BITS) / 4) die("gate overflow");
    unsigned i = gate_h(ret_abs);
    while (g_gate[i].ret) { if (g_gate[i].ret == ret_abs) die("门重复登记 %#llx", (unsigned long long)ret_abs); i = (i + 1) & ((1u << GATE_BITS) - 1); }
    g_gate[i] = (gate_t){ret_abs, k, arg, orig};
}

static uint64_t guest_ns(void) { return dm_guest_ns(); }

uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs, uint64_t guest_sp) {
    g_in_gate = 1;
    {
        static int n;
        if (n < 6) { n++; dm_log_kv("门时客体SP", guest_sp); }
    }
    (void)0;
    for (unsigned i = gate_h(ret_addr);; i = (i + 1) & ((1u << GATE_BITS) - 1)) {
        const gate_t *g = &g_gate[i];
        if (g->ret == ret_addr) switch (g->kind) {
            case G_CNTPCT: case G_CNTVCT: {
                uint64_t t = guest_ns() * 12 / 625;   // 19.2 MHz
                regs[g->arg] = t;
                return t;
            }
            case G_OBS: if(g->arg==5) { void dm_android_i18n_observe(const uint64_t *); dm_android_i18n_observe(regs); } if(g->arg==1) { void dm_android_touch_observe(const uint64_t *); dm_android_touch_observe(regs); } if (g_obs_fn[g->arg]) g_obs_fn[g->arg](regs); return 0;
            default: return svc_dispatch(g->arg, regs, g->orig);
        }
        if (!g->ret) die("unknown gate at %#llx", (unsigned long long)ret_addr);
    }
}

// 未实现导入的记名桩（entry.S 调）：返回值 = x0，直接用 C 返回
static char g_stub_names[4096][64];
void unimpl_register(const char *name, unsigned idx) {
    if (idx < 4096) snprintf(g_stub_names[idx], 64, "%s", name);
}
unsigned long unimpl_c(unsigned idx, uint64_t *regs) {
    static _Thread_local int depth;
    if (depth) return 0;   // 桩里又碰桩：只记一层
    depth = 1;
    LOG("【心跳】UNIMPL %s(x0=%#llx)", g_stub_names[idx], (unsigned long long)regs[0]);
    depth = 0;
    return 0;
}
