// Horizon 内核系统调用 — 移植自上游 dm/host/hle_svc.c。
// Darwin 依赖替换：pthread_get_stackaddr_np/getsize_np → pthread_getattr_np。
// 线程和内核事件由 hle_thread.c 的原生调度后端处理。
#include "dm_host.h"
#include "vk_backend.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define RESERVE (5ull << 30)
static uint8_t *g_as;
static uint8_t *g_heap; static size_t g_heap_cur, g_heap_cap; static int g_heap_rejects;
static int g_heap_shared;
static uint8_t *g_alias; static size_t g_alias_cap;
static uint8_t *g_stack; static size_t g_stack_cap;

static uint64_t guest_ns(void) { return dm_guest_ns(); }

void svc_mem_preinit(void);

static void mem_init(void) {
    if (g_as) return;
    void *shared_heap = dm_vk_guest_heap_reserve(RESERVE - 0x40000000, 0x40000000);
    if (shared_heap) {
        g_as = (uint8_t *)shared_heap - 0x40000000;
        g_heap_shared = 1;
    } else {
        g_as = mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_as == MAP_FAILED) die("reserve guest AS");
    }
    g_alias = g_as + 0x10000000; g_alias_cap = 0x20000000;
    g_stack = g_as + 0x30000000; g_stack_cap = 0x10000000;
    g_heap  = g_as + 0x40000000; g_heap_cap  = RESERVE - 0x40000000;
    mprotect(g_stack, 16 << 20, PROT_READ | PROT_WRITE);   // 主线程栈区头 16MB 预开（rtld 语义）
    LOG("guest AS %p +%#llx heap %p alias %p stack %p（头16MB已RW）",
        g_as, (unsigned long long)RESERVE, g_heap, g_alias, g_stack);
}

#define APP_POOL 0xCD500000ull
static uint64_t app_pool(void) {
    static uint64_t v;
    if (!v) { const char *e = getenv("DM_APP_POOL_ADD_MB"); long add = e ? atol(e) : 512;
        v = APP_POOL + ((uint64_t)add << 20); }
    return v;
}
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) n += g_mod[i].size; return (n + 0x1fffff) & ~(size_t)0x1fffff; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23 };

static uint8_t g_seen[256];

// 客体主线程的宿主引导栈（bridge.c 装载后登记）：QueryMemory 按栈语义回报
uint8_t *g_boot_stack; size_t g_boot_stack_size;

#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_find(uint64_t a, uint64_t n) {
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {
    pthread_mutex_lock(&g_map_lock);
    if (g_nmap >= MAX_MAP) die("映射表满");
    g_map[g_nmap].base = b; g_map[g_nmap].size = n; g_map[g_nmap].src = src; g_map[g_nmap].state = st; g_nmap++;
    pthread_mutex_unlock(&g_map_lock);
}
static void map_del(int i) { pthread_mutex_lock(&g_map_lock); g_map[i] = g_map[--g_nmap]; pthread_mutex_unlock(&g_map_lock); }

int svc_thread(int num, uint64_t *regs);   // hle_thread.c：线程、同步、内核事件

static module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++)
        if ((const uint8_t *)p >= g_mod[i].base && (const uint8_t *)p < g_mod[i].base + g_mod[i].size)
            return &g_mod[i];
    return NULL;
}

static int g_trace_n = 0;   // 门热路径零日志：变参 LOG 在翻译层下破坏客体状态（Unicorn 验证引导链本身是通的）
const char *sym_of_addr(uint64_t a);   // loader.c：模块内符号定位
void dm_log_kv(const char *tag, uint64_t v);
void dm_log_str(const char *s);
static int g_snap;
void dm_log_str(const char *);   // 前几个 svc 的寄存器+栈快照（对拍用）
void svc_mem_preinit(void) { mem_init(); }

static int g_qa_n;
uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    if (g_qa_n < 48) {
        g_qa_n++;
        char tag[64];
        snprintf(tag, sizeof tag, "QA%02d a0", g_qa_n); dm_log_kv(tag, regs[0]);
        snprintf(tag, sizeof tag, "QA%02d a1", g_qa_n); dm_log_kv(tag, regs[1]);
        snprintf(tag, sizeof tag, "QA%02d a2", g_qa_n); dm_log_kv(tag, regs[2]);
        snprintf(tag, sizeof tag, "QA%02d PC", g_qa_n); dm_log_str(sym_of_addr(orig_addr));
    }
    if (g_snap < 3) {
        g_snap++;
        dm_log_kv("SNAP svc", num);
        for (int i = 0; i <= 12; i += 2)
            dm_log_kv(i == 0 ? "x0/x1" : i == 2 ? "x2/x3" : i == 4 ? "x4/x5" : i == 6 ? "x6/x7" : i == 8 ? "x8/x9" : i == 10 ? "x10/x11" : "x12",
                      (regs[i] & 0xffffffffffffull) | (regs[i + 1] << 48));
        dm_log_kv("lr", regs[30]);
        for (int i = -8; i < 8; i++)
            dm_log_kv("stk", ((uint64_t *)(uintptr_t)(regs[29]))[i]);   // x29 帧附近
    }
    if (g_trace_n > 0) {
        g_trace_n--;
        char t[64];
        snprintf(t, sizeof t, "svc#%x", num);
        dm_log_kv(t, regs[0]);
        dm_log_str(sym_of_addr(orig_addr));
    }
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    switch (num) {
    case 0x85: {
        int dm_touch_game_hide_own_glyph(uint64_t);
        regs[8]=dm_touch_game_hide_own_glyph(regs[19])?1:0;
        return regs[0];
    }
    case 0x86: {
        void svc_private_skill_aim(uint64_t *);
        svc_private_skill_aim(regs);
        return regs[0];
    }
    case 0x87: {
        // Original model-path call from dm_dropfilter.c. Its string result is
        // passed indirectly in x8, not as a third ordinary C argument.
        module_t *m=module_of((const void *)(uintptr_t)orig_addr);
        if(!m || strcmp(m->name,"main") || orig_addr-(uint64_t)m->base!=0x8fa590 || m->text_end<=0xa14dc4)
            die("Ground model host cut does not match guest build");
        register uint64_t x0 __asm__("x0")=regs[0],x1 __asm__("x1")=regs[1];
        register uint64_t x8 __asm__("x8")=regs[8],x16 __asm__("x16")=(uintptr_t)(m->base+0xa14dc4);
        __asm__ volatile("blr x16" : "+r"(x0),"+r"(x1),"+r"(x8),"+r"(x16) : :
            "x2","x3","x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","x17","x30",
            "v0","v1","v2","v3","v4","v5","v6","v7","v16","v17","v18","v19","v20","v21","v22","v23","v24","v25","v26","v27","v28","v29","v30","v31","cc","memory");
        return regs[0];
    }
    case 0x88: {
        module_t *m=module_of((const void *)(uintptr_t)orig_addr);
        if(!m || strcmp(m->name,"main") || orig_addr-(uint64_t)m->base!=0x468f2c || m->text_end<=0x470c20)
            die("Drop sound host cut does not match guest build");
        regs[0]=((uint32_t (*)(uint64_t,uint64_t))(m->base+0x470c20))(regs[0],regs[1]);
        return regs[0];
    }
    case 0x8a: case 0x8b: {
        void svc_join_spawn(uint64_t *,int);
        svc_join_spawn(regs,num==0x8b);
        return regs[0];
    }
    case 0x81: case 0x82: {
        // Upstream diag.c c2s_deliver/c2s_deliver_sys forward the offline
        // client's packets to these original server handlers. These cuts are
        // part of the packaged guest even when diagnostic replay is disabled.
        // Preserve the saved register frame, as the upstream void hooks do.
        module_t *m=module_of((const void *)(uintptr_t)orig_addr);
        size_t target=num==0x81?0x632510:0x6310b0;
        size_t callsite=num==0x81?0x5d3a88:0x5d39c8;
        if(!m || strcmp(m->name,"main") || orig_addr-(uint64_t)m->base!=callsite || m->text_end<=target)
            die("Client message host cut does not match guest build");
        static _Atomic unsigned delivered;
        unsigned trace=atomic_fetch_add(&delivered,1);
        if(trace<8) {
            dm_log_str("Client message forward begin");
            dm_log_kv("cut",num);dm_log_kv("target",(uintptr_t)(m->base+target));
            dm_log_kv("host frame",(uintptr_t)__builtin_frame_address(0));
            dm_log_kv("guest fp",regs[29]);dm_log_kv("guest lr",regs[30]);
            for(unsigned i=0;i<4;i++)dm_log_kv(i==0?"arg0":i==1?"arg1":i==2?"arg2":"arg3",regs[i]);
        }
        if(num==0x81) {
            ((void (*)(uint64_t,uint64_t,uint64_t,uint64_t))(m->base+target))(regs[0],regs[1],regs[2],regs[3]);
        } else {
            ((void (*)(uint64_t,uint64_t))(m->base+target))(regs[0],regs[1]);
        }
        if(trace<8)dm_log_str("Client message forward end");
        return regs[0];
    }
    case 0x89: case 0x8c: case 0x8d: case 0x8e:
        // dm_rift.c's branch hooks always clear x8 first. No Rift session can
        // be started by the Android shell yet, so use their non-Rift branch:
        // retain the original destination, saved act and equipment checks.
        // This is the original game's behavior, not Rift feature support.
        regs[8]=0;
        return regs[0];
    case 0x83: case 0x84: {
        // Upstream hle_granny.c's uncached path calls the original BitKnit2
        // decoder with the same chunk and context, then returns its w0.
        module_t *m=NULL;
        for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")){m=&g_mod[i];break;}
        if(!m || m->text_end<=0x11f4af0)die("Missing Granny decoder in main");
        typedef uint64_t (*granny_decode_fn)(uint64_t,uint64_t,uint64_t);
        granny_decode_fn decode=(granny_decode_fn)(m->base+0x11f4af0);
        regs[0]=(uint32_t)decode(regs[0],regs[1],(uint32_t)regs[2]);
        return regs[0];
    }
    case 0x80: { // 上游 TACT DecodeZlib 宿主剪口，不是 Horizon 内核 SVC
        uint32_t tact_decode_zlib(const uint64_t *);
        regs[0] = tact_decode_zlib(regs);
        return regs[0];
    }
    case 0x01: {  // SetHeapSize
        size_t size = regs[1];
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD401; return 0; }
        if (size + image_bytes() > app_pool()) { g_heap_rejects++; regs[0] = 0x10801; return 0; }
        int rc = 0;
        if (g_heap_shared) {
            if (dm_vk_guest_heap_resize(size)) { regs[0] = 0xD401; return 0; }
        }
        else if (size > g_heap_cur) rc = mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE);
        else if (size < g_heap_cur) rc = mprotect(g_heap + size, g_heap_cur - size, PROT_NONE);
        (void)rc;
        { static int said; if (!said && size) { said = 1; LOG("SetHeapSize：首次设堆 %zu MB（池 %llu MB − 映像 %zu MB；被拒 %d 次）",
            size >> 20, (unsigned long long)(app_pool() >> 20), image_bytes() >> 20, g_heap_rejects); } }
        g_heap_cur = size;
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
    case 0x02: regs[0] = 0; return 0;   // SetMemoryPermission
    case 0x03: regs[0] = 0; return 0;   // SetMemoryAttribute
    case 0x04: {  // MapMemory(dst, src, size)
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        if ((uint8_t *)dst < g_stack || (uint8_t *)dst + size > g_stack + g_stack_cap || map_find(dst, size)) {
            LOG("MapMemory 目标非法或已占用 %#llx+%#llx", (unsigned long long)dst, (unsigned long long)size); regs[0] = 0xDC01; return 0;
        }
        uint64_t lo = dst & ~0xFFFull, hi = (dst + size + 0xFFF) & ~0xFFFull;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        memcpy((void *)dst, (void *)src, size);
        map_add(dst, size, src, 0x0B);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            regs[0] = 0xD401; return 0;
        }
        memcpy((void *)src, (void *)dst, size);
        map_del(k - 1);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory
        uint64_t mi = regs[0], addr = regs[2];
        {
            void dm_log_kv(const char *, uint64_t);
            static int qn;
            if (qn < 24) { qn++; dm_log_kv("QM addr", addr); }
        }
        LOG("QueryMemory(addr=%#llx)", (unsigned long long)addr);
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        int k;
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)g_heap; size=g_heap_cur; state=5; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (a >= g_as && a < g_as + RESERVE) {
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + RESERVE;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0;
        } else if (g_boot_stack && a >= g_boot_stack && a < g_boot_stack + g_boot_stack_size) {
            base = (uint64_t)g_boot_stack; size = g_boot_stack_size; state = 0xB; perm = 3;
        } else {
            pthread_attr_t at;
            pthread_getattr_np(pthread_self(), &at);
            void *top_; size_t ssz;
            pthread_attr_getstack(&at, &top_, &ssz);
            uint64_t top = (uint64_t)top_;
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB; perm = 3; }
            else { base=addr & ~0xFFFull; size=0x1000; state=3; perm=5; }
        }
        if (mi) { uint64_t *m=(uint64_t*)mi; m[0]=base; m[1]=size; ((uint32_t*)mi)[4]=state; ((uint32_t*)mi)[5]=0; ((uint32_t*)mi)[6]=perm; ((uint32_t*)mi)[7]=0; ((uint32_t*)mi)[8]=0;
            void dm_log_kv(const char *, uint64_t);
            static int qn2;
            if (qn2 < 24) { qn2++; dm_log_kv("QM 答 base", base); dm_log_kv("QM 答 size", size); } }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x1F: {  // ConnectToNamedPort(name)：Result=0（成功）+ 假句柄在 W1（Unicorn 语义）
        dm_log_str((const char *)(uintptr_t)regs[1]);
        regs[0] = 0;
        regs[1] &= 0xffffffff00000000ull;
        regs[1] |= 0x2000;
        return 0;
    }
    case 0x21: {  // SendSyncRequest：成功 + 不动响应缓冲（Unicorn 同款行为）
        regs[0] = 0;
        return 0;
    }
    case 0x2C: case 0x2D: {  // (Un)MapPhysicalMemory
        uint8_t *a = (uint8_t *)regs[0]; size_t size = regs[1];
        if (a < g_alias || a + size > g_alias + g_alias_cap) { regs[0] = 0xD401; return 0; }
        int rc = num == 0x2C ? mprotect(a, size, PROT_READ | PROT_WRITE)
                             : (madvise(a, size, MADV_DONTNEED), mprotect(a, size, PROT_NONE));
        if (rc) die("SVC %#x mprotect errno=%d", num, errno);
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break
        uint32_t res = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        module_t *mm = module_of((void *)(uintptr_t)regs[30]);
        dm_log_str("guest Break");
        dm_log_kv("reason", regs[0]); dm_log_kv("Result", res);
        dm_log_str(mm ? mm->name : "?");
        dm_log_kv("svc offset", mm ? orig_addr - (uint64_t)mm->base : orig_addr);
        if (regs[0] & 0x80000000ull) return 0;
        // 仅在已登记的可读栈范围内走帧链；不读未提交的 guest AS。
        uint64_t fp = regs[29];
        for (int depth = 0; depth < 16 && !(fp & 15); depth++) {
            int readable = fp >= (uint64_t)g_boot_stack &&
                           fp + 16 >= fp && fp + 16 <= (uint64_t)g_boot_stack + g_boot_stack_size;
            for (int i = 0; !readable && i < g_nmap; i++)
                readable = fp >= g_map[i].base && fp + 16 >= fp && fp + 16 <= g_map[i].base + g_map[i].size;
            if (!readable) break;
            uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];
            module_t *m = module_of((void *)(uintptr_t)lr);
            dm_log_str(m ? m->name : "host");
            dm_log_kv("Break frame", m ? lr - (uint64_t)m->base : lr);
            if (next <= fp) break;
            fp = next;
        }
        _exit(42);
    }
    case 0x1E: regs[0] = 0; regs[1] = guest_ns() * 12 / 625; return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)：SDK 的诊断正文——直接打进日志
        dm_log_str((const char *)(uintptr_t)regs[0]);
        regs[0] = 0;
        return 0;
    }
    case 0x29: {  // GetInfo
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = RESERVE;     break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = app_pool(); break;
        case I_UsedMem:  case I_UsedNonSys:  v = g_heap_cur + image_bytes(); break;
        case I_SysResTotal: v = 0x2000000; break; case I_SysResUsed: v = 0x400000; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: v = 0; break;   // 与 Unicorn 语义同步（确定性模式）
        case I_ProgramId: v = 0x0100726014352000ull; break;
        case I_IsApplication: v = 1; break;
        default: v = 0; break;
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default:
        if (num >= 0x80) {
            // 这些是上游剪口，不是可忽略的系统查询；假成功会留下未填的输出。
            dm_log_str("未实现的宿主剪口");
            dm_log_kv("host call", num); dm_log_kv("original PC", orig_addr);
            _exit(43);
        }
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1; LOG("SVC %#x x0=%#llx x1=%#llx x2=%#llx（占位返回成功）",
            num, (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2]); }
        regs[0] = 0; return 0;
    }
}
