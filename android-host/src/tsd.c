// 线程槽 — 直接在 tpidr_el0 指向的 TLS 块里用固定高位槽（不用 pthread key）。
// 为什么：键页机制是「槽偏移取值」，客体经 [tpidr + 偏移] 读。真机 bionic 与
// x86 模拟器的 ARM 翻译层各有自己的 tpidr 块，pthread_setspecific 在翻译环境下
// 写不进客体可见的那块（实测）——所以改为：宿主在每个客体线程初始化时，直接把
// 门入口等写进本线程 tpidr 块的固定高位槽。槽位选在映射边界内的末尾（离常用
// 低位槽尽量远），写入前核对映射边界（/proc/self/maps）。
#include "dm_host.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <stddef.h>

// Shared lifecycle from upstream dm/host/crash.c. Install before switching to
// the guest's small stack; SA_ONSTACK alone does not allocate a signal stack.
static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;
static void altstack_free(void *p) {
    stack_t off = {.ss_flags = SS_DISABLE};
    // Do not free memory still registered with the kernel if disabling fails.
    if (!sigaltstack(&off, NULL)) free(p);
}
static void altstack_key_init(void) {
    if (pthread_key_create(&g_altstack_key, altstack_free)) die("altstack key");
}
void crash_thread_init(void) {
    pthread_once(&g_altstack_once, altstack_key_init);
    if (pthread_getspecific(g_altstack_key)) return;
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536};
    if (!ss.ss_sp) die("altstack allocation");
    if (sigaltstack(&ss, NULL)) { free(ss.ss_sp); die("altstack install"); }
    if (pthread_setspecific(g_altstack_key, ss.ss_sp)) {
        altstack_free(ss.ss_sp);
        die("altstack thread registration");
    }
}

#define SLOT_BASE_OFF 8   // 从（边界内）最后一个可用槽往回排 8 个

static uint64_t g_slot_off[KEY_N];   // 字节偏移
static int g_calibrated;
/* entry.S reads these three 64-bit fields through translation-only TLS400.
 * Depth is authoritative: a deep guest callback must never be mistaken for
 * an outer gate merely because its SP crossed the old stack bounds. */
typedef struct {
    uint64_t top,low,depth;
} gate_stack_descriptor;
_Static_assert(offsetof(gate_stack_descriptor,top)==0 &&
    offsetof(gate_stack_descriptor,low)==8 &&
    offsetof(gate_stack_descriptor,depth)==16 && sizeof(gate_stack_descriptor)==24,
    "entry.S translation gate descriptor layout");
enum { GATE_STACK_BYTES=1<<20, GATE_STACK_GUARD=64<<10 };
static __thread gate_stack_descriptor t_gate;
extern unsigned char dm_no_fp_state;
uint64_t dm_gate_stack(void) { return t_gate.top; }

static inline uint64_t read_tpidr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

// TLS 块所在映射的可读边界
static uint64_t tls_readable_end(uint64_t base) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return base + 0x1000;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, e;
        if (sscanf(line, "%llx-%llx", &s, &e) == 2 && base >= s && base < e) {
            fclose(f);
            return e;
        }
    }
    fclose(f);
    return base + 0x1000;
}

unsigned long tsd_slot_offset(unsigned k) { return g_slot_off[k]; }

extern void dm_gate(void);

// 每线程：把门入口 / TLS 块 / TLR 写进本线程固定槽（对应上游 thread.c 的 guest_thread_init）
// KTLR = Horizon ThreadLocalRegion（0x200）：SDK 内部临界区从 TLR[0x1f8] 取当前线程对象
// KRES0/1 = 分辨率参数（掌机 1280x720 / 底座 1920x1080）；KASPECT = 19/9
void guest_thread_full_init(void) {
    static __thread uint8_t tlr[0x200] __attribute__((aligned(16)));
    if (!g_calibrated) return;
    uint64_t base = read_tpidr();
    *(uint64_t *)(base + g_slot_off[KEY_KGATE]) = (uint64_t)dm_gate;
    if (dm_no_fp_state) {
        if (!t_gate.top) {
            // The offline join callback main+0x5d0bd0 alone needs 0x13860
            // bytes; other guest frames reach 0x8d000. Reserve 1 MiB plus
            // inaccessible guards, rather than allowing overflow into TLS or
            // adjacent anonymous mappings. Physical pages remain demand-paged.
            size_t bytes=GATE_STACK_BYTES+2*GATE_STACK_GUARD;
            uint8_t *mapping=mmap(NULL,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
            if(mapping==MAP_FAILED)die("translation gate stack mmap");
            uint8_t *low=mapping+GATE_STACK_GUARD;
            if(mprotect(low,GATE_STACK_BYTES,PROT_READ|PROT_WRITE)) {
                munmap(mapping,bytes);die("translation gate stack mprotect");
            }
            t_gate.low=(uintptr_t)low;t_gate.top=(uintptr_t)low+GATE_STACK_BYTES;
            t_gate.depth=0;
            LOG("Translation gate stack: low=%p top=%p usable=%u guard=%u",low,(void *)(uintptr_t)t_gate.top,GATE_STACK_BYTES,GATE_STACK_GUARD);
        }
        if (tls_readable_end(base) < base + 401 * 8) die("translation gate TLS slot out of range");
        *(uint64_t *)(base + 400 * 8) = (uintptr_t)&t_gate;
    }
    LOG("槽写入：KGATE=%p KTLR=%p 门栈=%p（tpidr=%#llx）", (void *)dm_gate, tlr, (void *)(uintptr_t)t_gate.low, (unsigned long long)base);
    static __thread void *tlr_obj;
    if (!tlr_obj) { tlr_obj = calloc(1, 0x400); *(void **)(tlr + 0x1f8) = tlr_obj; }
    *(uint64_t *)(base + g_slot_off[KEY_KTLR]) = (uint64_t)tlr;
    *(uint64_t *)(base + g_slot_off[KEY_K18]) = 0;
    static __thread void *elf_tls;
    if (!elf_tls) elf_tls = calloc(1, 8192);
    // nnosInitialize 的 ThreadManager 构造器从 TLR+0x1f0 重载 TPIDR_EL0。
    // 改写后它写 KTLS 槽；缺失此字段会把有效 ELF TLS 指针清成 NULL。
    *(void **)(tlr + 0x1f0) = elf_tls;
    *(uint64_t *)(base + g_slot_off[KEY_KTLS]) = (uint64_t)elf_tls;
    *(uint64_t *)(base + g_slot_off[KEY_KRES0]) = 1280 | ((uint64_t)720 << 32);
    *(uint64_t *)(base + g_slot_off[KEY_KRES1]) = 1920 | ((uint64_t)1080 << 32);
    uint32_t bits; float a = 19.0f / 9.0f;
    memcpy(&bits, &a, 4);
    *(uint64_t *)(base + g_slot_off[KEY_KASPECT]) = bits;
    *(uint64_t *)(base + g_slot_off[KEY_KASPECTUI]) = bits;
}

void tsd_guest_thread_init(void) { guest_thread_full_init(); }

static int detect_translation(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int t = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, "libndk_translation")) { t = 1; break; }
    fclose(f);
    return t;
}

void tsd_init(void) {
    if (detect_translation()) {
        dm_no_fp_state = 1;
        LOG("检测到 ARM 翻译层：门内跳过 nzcv/fpsr/fpcr 存取（x86 模拟器）");
    }
    uint64_t base = read_tpidr();
    uint64_t end = tls_readable_end(base);
    unsigned nslots = (unsigned)((end - base) / 8);
    if (nslots < SLOT_BASE_OFF + KEY_N + 4)
        die("TLS 块可读槽不足：%u（基址 %#llx 边界 %#llx）", nslots, (unsigned long long)base, (unsigned long long)end);
    unsigned first = 0;
    for (unsigned i = 64; i + KEY_N <= nslots && i < 4096; i++) {
        int ok = 1;
        for (unsigned k = 0; k < KEY_N; k++) if (*(uint64_t *)(base + (i + k) * 8)) { ok = 0; i += k; break; }
        if (ok) { first = i; break; }
    }
    if (!first) die("TLS 无可用连续零槽（%u 槽内）", nslots);
    for (unsigned k = 0; k < KEY_N; k++) g_slot_off[k] = (first + k) * 8;
    g_calibrated = 1;
    LOG("TSD：tpidr=%#llx 映射边界 +%u 槽，键页槽位 %u..%u",
        (unsigned long long)base, nslots, first, first + KEY_N - 1);
}
