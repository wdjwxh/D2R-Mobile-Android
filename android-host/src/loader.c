// 装载器 — 移植自上游 dm/host/loader.c 的 Mac 分支，改为 Android：
// 整映像 mmap 一段（无 W^X 限制，不需要 vm_remap 花活），重定位后把 text/补丁区设 RX。
#include "dm_host.h"
#include "i18n_android.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

module_t g_mod[MAX_MOD];
int g_nmod;

typedef struct Elf64_Dyn_ Elf64_Dyn_;
typedef struct Elf64_Sym_ Elf64_Sym_;
typedef struct Elf64_Rela_ Elf64_Rela_;
#define DT_SYMTAB 6
#define DT_STRTAB 5
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_JMPREL 0x17
#define DT_PLTRELSZ 2
#define DT_INIT 0xc
#define DT_INIT_ARRAY 0x19
#define DT_INIT_ARRAYSZ 0x1b
#define DT_FINI_ARRAY 0x1a
#define DT_FINI_ARRAYSZ 0x1c
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_JUMP_SLOT 1026
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((unsigned)(i) & 0xffffffff)

// 未实现桩表（entry.S 的 unimpl_stubs，每桩 8 字节）
extern char unimpl_stubs;
void unimpl_register(const char *name, unsigned idx);
static unsigned g_stub_n;
static unsigned stub_for(const char *name) {
    if (g_stub_n >= 4096) die("桩表满");
    unimpl_register(name, g_stub_n);
    return g_stub_n++;
}

static void read_fixups(module_t *m, const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[8] = {"K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI"};
    char line[256], key[32];
    unsigned long long v;
    unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %31s %llx", key, &v) != 2) die("%s: 旧格式产物", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else for (int k = 0; k < 8; k++)
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], strlen(kn[k])) && !strcmp(key + 4 + strlen(kn[k]), "_IDX")) {
                char *tab = strrchr(line, '\t');
                m->key_idx[k] = (unsigned)strtoul(tab + 1, NULL, 10);
                seen |= 1u << k;
            }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != 0xff)
        die("%s: fixups 不完整", path);
}

static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    const Elf64_Dyn_ *d = (const Elf64_Dyn_ *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
    for (; d->tag; d++) switch (d->tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym_ *)(m->base + d->val); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->val); break;
        case DT_RELA: rela = d->val; break;
        case DT_RELASZ: relasz = d->val; break;
        case DT_JMPREL: jmprel = d->val; break;
        case DT_PLTRELSZ: pltsz = d->val; break;
        case DT_INIT: m->init = d->val; break;
        case DT_INIT_ARRAY: m->init_array = d->val; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->val; break;
        case DT_FINI_ARRAY: m->fini_array = d->val; break;
        case DT_FINI_ARRAYSZ: m->fini_arraysz = d->val; break;
    }
    if (!m->symtab || !m->strtab) die("%s: 动态表缺符号/字符串表", m->name);
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym_));
    LOG("%s: %llu 字节 符号 %u MOD0@%#x rela=%#llx(+%#llx) jmprel=%#llx(+%#llx) init_array=%#llx[%llu]",
        m->name, (unsigned long long)m->size, m->nsym, mod0,
        (unsigned long long)rela, (unsigned long long)relasz, (unsigned long long)jmprel,
        (unsigned long long)pltsz, (unsigned long long)m->init_array, (unsigned long long)m->init_arraysz);
}

#include <dlfcn.h>
#include <link.h>
// 符号哈希（开地址）
#define SYMH_BITS 16
typedef struct { const char *name; uint64_t val; } syment_t;
static syment_t g_symh[1u << SYMH_BITS];
static unsigned sym_h(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return (unsigned)(h >> (64 - SYMH_BITS));
}
static void symhash_add(const char *name, uint64_t val) {
    unsigned i = sym_h(name);
    while (g_symh[i].name) {
        if (!strcmp(g_symh[i].name, name)) return;   // 同名保留首个
        i = (i + 1) & ((1u << SYMH_BITS) - 1);
    }
    g_symh[i] = (syment_t){name, val};
}
static uint64_t symhash_get(const char *name) {
    unsigned i = sym_h(name);
    unsigned probes = 0;
    while (g_symh[i].name) {
        if (!strcmp(g_symh[i].name, name)) return g_symh[i].val;
        i = (i + 1) & ((1u << SYMH_BITS) - 1);
        if (++probes > 200) break;
    }
    if (!g_symh[i].name && probes == 0) {
        // 插桩：首槽即空 = 哈希入口被清/串被毁
        extern void dm_log_kv(const char *, uint64_t);
        dm_log_kv("HASHMISS 槽空 name@=", (uint64_t)(uintptr_t)name);
        dm_log_kv("HASHMISS 首字符", (uint64_t)(unsigned char)name[0]);
        dm_log_kv("HASHMISS i", i);
    }
    return 0;
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256];
    snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    unsigned long ret, orig;
    char kind[16];
    int arg, n = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) {
            gate_register((uint64_t)m->base + ret, kind, arg, (uint64_t)m->base + orig);
            n++;
        }
    fclose(f);
    m->ngate = n;
    LOG("%s: 基址 %p 注册门 %d 个（补丁区 %#llx 键页 %#llx）", m->name, m->base, n,
        (unsigned long long)m->patch_off, (unsigned long long)m->key_page_off);
}

void load_module(const char *image_path, const char *tables_dir, const char *name) {
    module_t *m = &g_mod[g_nmod];
    memset(m, 0, sizeof *m);
    m->name = name;
    {
        // The wrapper exports the image origin, independently of ELF load bias
        // and its leading header page. Loading it does not imply AOT execution.
        char lib[64];
        snprintf(lib, sizeof lib, "libguest_%s.so", name);
        void *h = dlopen(lib, RTLD_NOW);
        if (!h) LOG("%s: dlopen(%s) failed: %s (using anonymous image)", name, lib, dlerror());
        if (h) {
            uint8_t *base = dlsym(h, "dm_guest_image");
            if (base) {
                struct stat st;
                if (stat(image_path, &st) || st.st_size <= 0) die("stat %s", image_path);
                m->size = st.st_size;
                m->base = base;
                m->from_dlopen = 1;
                LOG("%s: ELF image loaded from %s at %p", name, lib, m->base);
            } else {
                LOG("%s: wrapper has no dm_guest_image export; using anonymous image", name);
                dlclose(h);
            }
        }
    }

    if (!m->base) {
        int fd = open(image_path, O_RDONLY);
        if (fd < 0) die("open %s", image_path);
        struct stat st;
        fstat(fd, &st);
        m->size = st.st_size;
        // 匿名映射再读入：/data 文件直接 PROT_EXEC 会撞 SELinux（Android 16 实测）
        m->base = mmap(NULL, (m->size + 0xFFFF) & ~0xFFFFULL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m->base == MAP_FAILED) die("mmap %s", image_path);
        if (read(fd, m->base, m->size) != (ssize_t)m->size) die("read %s", image_path);
        close(fd);
    }
    read_fixups(m, tables_dir);
    {
        // 陷阱梯：brk.txt 每行「模块名 偏移hex 编号」，替换为 brk #编号（SIGTRAP 暴露到达点）
        char pp[1024];
        snprintf(pp, sizeof pp, "%s/brk.txt", tables_dir);
        FILE *bf = fopen(pp, "r");
        if (bf) {
            char bn[32];
            unsigned long off;
            int id;
            while (fscanf(bf, "%31s %lx %d", bn, &off, &id) == 3)
                if (!strcmp(bn, name)) {
                    uint32_t brk = 0xD4200000u | ((unsigned)id << 5);
                    memcpy(m->base + off, &brk, 4);
                    LOG("brk#%d 打在 %s+%#lx", id, name, off);
                }
            fclose(bf);
        }
    }
    parse_dynamic(m);
    register_gates(m, tables_dir);
    // 把有值符号登进全局哈希（shndx 在 Switch NSO 符号表里恒 0，按 value 非零判定义）
    for (unsigned i = 0; i < m->nsym; i++) {
        const Elf64_Sym_ *s = &m->symtab[i];
        if (s->name && s->value)
            symhash_add(m->strtab + s->name, (uint64_t)m->base + s->value);
    }
    g_nmod++;
}

static int g_unresolved, g_resolved;
// 模块内地址 → 最近符号名（诊断用）
const char *sym_of_addr(uint64_t a) {
    static char buf[128];
    const module_t *bm = NULL;
    for (int i = 0; i < g_nmod; i++)
        if (a >= (uint64_t)g_mod[i].base && a < (uint64_t)g_mod[i].base + g_mod[i].size) { bm = &g_mod[i]; break; }
    if (!bm) return "宿主";
    uint64_t off = a - (uint64_t)bm->base;
    const Elf64_Sym_ *best = NULL;
    for (unsigned i = 0; i < bm->nsym; i++) {
        const Elf64_Sym_ *y = &bm->symtab[i];
        if (y->name && y->value && y->value <= off && (!best || y->value > best->value)) best = y;
    }
    if (!best) return "未知";
    snprintf(buf, sizeof buf, "%s+%#llx", bm->strtab + best->name, (unsigned long long)(off - best->value));
    return buf;
}

// 金丝雀：映像哈希与复核（涂改检测）
static uint64_t img_hash(int k) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = g_mod[k].base;
    for (uint64_t i = 0; i + 8 <= g_mod[k].size; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}
static uint64_t g_ihash[MAX_MOD];

void canary_install(void) {
    for (int k = 0; k < g_nmod; k++) g_ihash[k] = img_hash(k);
}

int canary_verify(char *report, size_t repcap) {
    for (int k = 0; k < g_nmod; k++) {
        if (img_hash(k) != g_ihash[k]) {
            // 二分找第一个差异
            uint64_t lo = 0, hi = g_mod[k].size;
            while (lo < hi) {
                uint64_t mid = (lo + hi) / 2 & ~7ull;
                uint64_t h1 = 0xcbf29ce484222325ull, h2 = h1;
                for (uint64_t i = 0; i < mid; i += 8) {
                    uint64_t v; memcpy(&v, g_mod[k].base + i, 8);
                    h1 = (h1 ^ v) * 0x100000001b3ull;
                }
                if (h1 == g_ihash[k] && mid > 0) lo = mid; else hi = mid == lo ? lo + 8 : mid;
                if (hi - lo <= 8) break;
            }
            snprintf(report, repcap, "模块%s 被改！首异约 +%#llx", g_mod[k].name, (unsigned long long)lo);
            return k + 1;
        }
    }
    return 0;
}

uint64_t symhash_get_probe(void) { return symhash_get("nnosInitialize"); }

void run_guest_inits(void) {
    {
        // DT_INIT 自重定位跳过标志（main+0x21260d0，cbnz w8 检查；我们已应用过绝对值重定位，
        // SDK 的二次重放会产生双基址野指针——置 1 让它跳过）
        for (int k = 0; k < g_nmod; k++)
            if (!strcmp(g_mod[k].name, "main")) {
                uint32_t *flag = (uint32_t *)(g_mod[k].base + 0x21260d0);
                LOG("DT_INIT 跳过标志原值 %#x → 1", *flag);
                *flag = 1;
            }
    }
    extern void dm_log_kv(const char *, uint64_t);
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        typedef void (*fn0)(void);
        dm_log_kv("init_array 模块", k);
        if (m->init) {
            ((fn0)(m->base + m->init))();
            // 钉死版本 3.3.94012：main/sdk 的 DT_INIT 自己遍历完整
            // DT_INIT_ARRAY（+0x12c..0x150），不能在宿主再执行一次。
            continue;
        }
        uint64_t *arr = (uint64_t *)(m->base + m->init_array);
        for (uint64_t i = 0; i * 8 < m->init_arraysz; i++)
            if (arr[i]) {
                if (i % 32 == 0) dm_log_kv("ctor 进度", i);   // 死前最后序号=凶手
                ((fn0)(arr[i]))();
            }
    }
}

void *guest_sym(const char *name) {
    uint64_t v = symhash_get(name);
    return (void *)v;
}

// 强制接管（上游 hle_sdkinit.c）：服务层初始化→空操作，绕过 Horizon IPC
typedef struct { const char *name; void *fn; } hle_ent;
static void hle_noop_init(void) { LOG("init 接管（空操作）"); }
// LanguageCode 是 8 字节值类型，在 x0 返回。oe::Initialize 已接管，
// 语言查询也必须接管，否则 SDK 会解引用未创建的 applet 服务对象。
// Match upstream language policy, taking Android's preferred locale at launch.
static uint64_t hle_desired_language(void) { return dm_android_desired_language(); }
static uint32_t hle_time_initialize(void) { return 0; }
// 上游 hle_net.c 的离线路径：不创建 Horizon bsd 服务会话。
static int hle_time_is_initialized(void) { return 1; }
static uint32_t hle_time_current(int64_t *out) {
    *out = (int64_t)time(NULL);
    return 0;
}
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } hle_calendar;
typedef struct { uint32_t wday, yday; char zone[8]; uint32_t dst; int32_t offset; } hle_calendar_info;
_Static_assert(sizeof(hle_calendar) == 8, "CalendarTime ABI");
_Static_assert(sizeof(hle_calendar_info) == 24, "CalendarAdditionalInfo ABI");
static uint32_t hle_to_calendar(hle_calendar *out, hle_calendar_info *info, const int64_t *posix) {
    time_t t = *posix;
    struct tm tm;
    if (!localtime_r(&t, &tm)) return 0x3e2;
    if (out) *out = (hle_calendar){tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                  tm.tm_hour, tm.tm_min, tm.tm_sec, 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->wday = tm.tm_wday; info->yday = tm.tm_yday;
        strncpy(info->zone, tm.tm_zone ? tm.tm_zone : "UTC", sizeof info->zone - 1);
        info->dst = tm.tm_isdst > 0; info->offset = tm.tm_gmtoff;
    }
    return 0;
}
int64_t dm_android_tz_now(uint64_t caller);
static int64_t hle_time_now(void) { return dm_android_tz_now((uint64_t)__builtin_return_address(0)); }
static int64_t hle_time_to_time_t(const int64_t *value) { return *value; }
// nn::os::GetDefaultUserExceptionStackForApplication(uint64_t *out)：SDK 装异常栈用，
// 桩不写出参→客体把垃圾装进 SP（真机/模拟器均实证炸点）。给真栈。
static void hle_exc_stack(uint64_t *out) {
    static void *stk;
    if (!stk) stk = mmap(NULL, 64 << 10, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (out) *out = (uint64_t)stk + (60 << 10);
}
extern const hle_ent *fs_force_lookup(const char *);   // hle_fs.c

extern const hle_ent *hid_force_lookup(const char *);
extern const hle_ent *vi_force_lookup(const char *);
extern const hle_ent *account_force_lookup(const char *);
extern const hle_ent *net_force_lookup(const char *);
extern const hle_ent *oe_force_lookup(const char *);
extern const hle_ent *friends_force_lookup(const char *);
extern const hle_ent *misc_force_lookup(const char *);
extern const hle_ent *audio_force_lookup(const char *);
extern const hle_ent *codec_force_lookup(const char *);
extern const hle_ent *movie_force_lookup(const char *);
extern const hle_ent *swkbd_force_lookup(const char *);
extern const hle_ent *charname_force_lookup(const char *);
extern const hle_ent *nv_force_lookup(const char *);
extern const hle_ent *nvn_force_lookup(const char *);

static const hle_ent *force_lookup(const char *name) {
    const hle_ent *hid = hid_force_lookup(name);
    if (hid) return hid;
    const hle_ent *vi = vi_force_lookup(name);
    if (vi) return vi;
    const hle_ent *account = account_force_lookup(name);
    if (account) return account;
    const hle_ent *net = net_force_lookup(name);
    if (net) return net;
    const hle_ent *oe = oe_force_lookup(name);
    if (oe) return oe;
    const hle_ent *friends = friends_force_lookup(name);
    if (friends) return friends;
    const hle_ent *misc = misc_force_lookup(name);
    if(misc) return misc;
    const hle_ent *audio = audio_force_lookup(name);
    if(audio) return audio;
    const hle_ent *codec = codec_force_lookup(name);
    if(codec) return codec;
    const hle_ent *movie = movie_force_lookup(name);
    if(movie) return movie;
    const hle_ent *swkbd = swkbd_force_lookup(name);
    if(swkbd) return swkbd;
    const hle_ent *charname = charname_force_lookup(name);
    if(charname) return charname;
    const hle_ent *nv = nv_force_lookup(name);
    if (nv) return nv;
    const hle_ent *nvn = nvn_force_lookup(name);
    if (nvn) return nvn;
    {
        const hle_ent *f = fs_force_lookup(name);
        if (f) return f;
    }
    static const hle_ent F[] = {
        {"time", (void *)time},
        {"localtime", (void *)localtime},
        {"localtime_r", (void *)localtime_r},
        {"mktime", (void *)mktime},
        {"nndiagStartup", (void *)hle_noop_init},
        {"_ZN2nn2oe10InitializeEv", (void *)hle_noop_init},
        {"_ZN2nn2oe18GetDesiredLanguageEv", (void *)hle_desired_language},
        {"_ZN2nn4time10InitializeEv", (void *)hle_time_initialize},
        {"_ZN2nn4time13IsInitializedEv", (void *)hle_time_is_initialized},
        {"_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", (void *)hle_time_current},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", (void *)hle_to_calendar},
        {"_ZN2nn4time23StandardUserSystemClock3nowEv", (void *)hle_time_now},
        {"_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE", (void *)hle_time_to_time_t},
        {"_ZN2nn3aoc10InitializeEv", (void *)hle_noop_init},
        {"_ZN2nn3err26InitializeContextWriterApiEv", (void *)hle_noop_init},
        {"_ZN2nn2fs24InitializeForApplicationEv", (void *)hle_noop_init},
        {"_ZN2nn4pctl10InitializeEv", (void *)hle_noop_init},
        {"_ZN2nn4diag34InitializeApplicationAbortObserverEv", (void *)hle_noop_init},
        {"nnosGetDefaultUserExceptionStackForApplication", (void *)hle_exc_stack},
    };
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++)
        if (!strcmp(F[i].name, name)) return &F[i];
    return NULL;
}

// libc 直通表（移植上游 hle_table.c；bionic 同名同 ABI）
#include <math.h>
#include <ctype.h>
#include <time.h>
static const hle_ent g_hle[] = {
    {"memcpy", (void *)memcpy}, {"memset", (void *)memset}, {"memmove", (void *)memmove},
    {"memcmp", (void *)memcmp}, {"memchr", (void *)memchr}, {"strlen", (void *)strlen},
    {"strnlen", (void *)strnlen}, {"strcmp", (void *)strcmp}, {"strncmp", (void *)strncmp},
    {"strcpy", (void *)strcpy}, {"strncpy", (void *)strncpy}, {"strcat", (void *)strcat},
    {"strncat", (void *)strncat}, {"strchr", (void *)strchr}, {"strrchr", (void *)strrchr},
    {"strstr", (void *)strstr}, {"strspn", (void *)strspn}, {"strcspn", (void *)strcspn},
    {"strpbrk", (void *)strpbrk}, {"strcasecmp", (void *)strcasecmp}, {"strncasecmp", (void *)strncasecmp},
    {"atoi", (void *)atoi}, {"atof", (void *)atof}, {"strtol", (void *)strtol}, {"strtoul", (void *)strtoul},
    {"strtoll", (void *)strtoll}, {"strtoull", (void *)strtoull}, {"strtod", (void *)strtod}, {"strtof", (void *)strtof},
    {"sqrtf", (void *)sqrtf}, {"cbrtf", (void *)cbrtf}, {"expf", (void *)expf}, {"acosf", (void *)acosf},
    {"cosf", (void *)cosf}, {"sinf", (void *)sinf}, {"atan2f", (void *)atan2f}, {"fmodf", (void *)fmodf},
    {"cos", (void *)cos}, {"sin", (void *)sin}, {"powf", (void *)powf}, {"atanf", (void *)atanf},
    {"tanf", (void *)tanf}, {"asinf", (void *)asinf}, {"log10", (void *)log10}, {"pow", (void *)pow},
    {"logf", (void *)logf}, {"ldexpf", (void *)ldexpf}, {"log10f", (void *)log10f}, {"log", (void *)log},
    {"ldexp", (void *)ldexp}, {"frexp", (void *)frexp}, {"atan2", (void *)atan2}, {"modff", (void *)modff},
    {"exp2f", (void *)exp2f}, {"exp2", (void *)exp2}, {"log2f", (void *)log2f}, {"log2", (void *)log2},
    {"lroundf", (void *)lroundf},
    {"mktime", (void *)mktime}, {"timegm", (void *)timegm}, {"gmtime_r", (void *)gmtime_r},
    {"localtime_r", (void *)localtime_r}, {"strftime", (void *)strftime},
    {"__nnmusl_memcpy", (void *)memcpy}, {"__nnmusl_memset", (void *)memset},
    {"getentropy", (void *)getentropy},
};
static void *hle_lookup(const char *name) {
    for (unsigned i = 0; i < sizeof g_hle / sizeof g_hle[0]; i++)
        if (!strcmp(g_hle[i].name, name)) return g_hle[i].fn;
    return NULL;
}

void resolve_and_relocate(void) {
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
        uint32_t mod0 = *(uint32_t *)(m->base + 4);
        const Elf64_Dyn_ *d = (const Elf64_Dyn_ *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
        for (; d->tag; d++) switch (d->tag) {
            case DT_RELA: rela = d->val; break;
            case DT_RELASZ: relasz = d->val; break;
            case DT_JMPREL: jmprel = d->val; break;
            case DT_PLTRELSZ: pltsz = d->val; break;
        }
        const Elf64_Rela_ *tabs[2] = {(const Elf64_Rela_ *)(m->base + rela), (const Elf64_Rela_ *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz};
        int cnt[3] = {0};
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela_); i++) {
                const Elf64_Rela_ *r = &tabs[t][i];
                uint64_t *where = (uint64_t *)(m->base + r->off);
                unsigned type = ELF64_R_TYPE(r->info), si = ELF64_R_SYM(r->info);
                if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->add; cnt[0]++; continue; }
                const char *nm = si ? m->strtab + m->symtab[si].name : "?";
                const hle_ent *fe = force_lookup(nm);
                uint64_t v = fe ? (uint64_t)fe->fn : (si ? symhash_get(nm) : 0);
                if (!v) v = (uint64_t)hle_lookup(nm);
                if (!v) {
                    // rtld 语义：__rel_dyn/__rel_plt 标记 = 本模块的重定位表界（SDK 自举依赖）
                    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
                    {
                        uint32_t mod0 = *(uint32_t *)(m->base + 4);
                        const Elf64_Dyn_ *dd = (const Elf64_Dyn_ *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
                        for (; dd->tag; dd++) switch (dd->tag) {
                            case DT_RELA: rela = dd->val; break; case DT_RELASZ: relasz = dd->val; break;
                            case DT_JMPREL: jmprel = dd->val; break; case DT_PLTRELSZ: pltsz = dd->val; break;
                        }
                    }
                    if (!strcmp(nm, "__rel_dyn_start")) v = (uint64_t)m->base + rela;
                    else if (!strcmp(nm, "__rel_dyn_end")) v = (uint64_t)m->base + rela + relasz;
                    else if (!strcmp(nm, "__rel_plt_start")) v = (uint64_t)m->base + jmprel;
                    else if (!strcmp(nm, "__rel_plt_end")) v = (uint64_t)m->base + jmprel + pltsz;
                }
                if (!v) {
                    v = (uint64_t)&unimpl_stubs + stub_for(nm) * 8;
                    g_unresolved++;
                    if (g_unresolved <= 10) LOG("  未解析导入（先接桩）：%s", nm);
                } else g_resolved++;
                *where = v + r->add;
                cnt[1 + (type == R_AARCH64_JUMP_SLOT)]++;
            }
        LOG("%s: 重定位 relative=%d abs/glob=%d jump=%d", m->name, cnt[0], cnt[1], cnt[2]);
    }
    LOG("导入解析：成功 %d，未解析接桩 %d", g_resolved, g_unresolved);
    {
        extern void dm_log_kv(const char *, uint64_t);
        dm_log_kv("自检 nnosInitialize", symhash_get("nnosInitialize"));
        dm_log_kv("自检 nnMain", symhash_get("nnMain"));
    }
}

void install_key_pages(void) {
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
        memset(page, 0, m->key_page_size);
        for (unsigned i = 0; i < KEY_N; i++) {
            page[m->key_idx[i]] = tsd_slot_offset(i);
            if (!page[m->key_idx[i]]) die("%s 键页第 %u 项为 0", m->name, i);
        }
        // 实验期键页保持可写
    }
    LOG("键页已装并设只读");
}

void protect_modules(void) {
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        // 真机 icache 维护：映像经匿名拷贝+重定位写入后执行，必须冲指令缓存
        __builtin___clear_cache((char *)m->base, (char *)m->base + m->size);
    }
    for (int k = 0; k < g_nmod; k++)
        if (g_mod[k].from_dlopen) LOG("%s: ELF 装入，段权限由 PT_LOAD 给定", g_mod[k].name);
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        if (m->from_dlopen) continue;
        if (mprotect(m->base, (m->text_end + 0x3FFF) & ~0x3FFFULL, PROT_READ | PROT_EXEC)) die("%s text mprotect", m->name);
        uint64_t pa = m->patch_off, pa_end = (m->size + 0x3FFF) & ~0x3FFFULL;
        pa &= ~0x3FFFULL;
        if (pa_end > pa)
            if (mprotect(m->base + pa, pa_end - pa, PROT_READ | PROT_EXEC)) die("%s patch mprotect", m->name);
        // W 段（ro 余部 + data + bss）显式重断言 RW：x86 翻译层会把大段降级成 r--，
        // 而 SDK 自举（区域管理器/自重定位）要写这段（实测 fault=sdk+0xB20000 落在 [TEXT_END,PATCH_OFF)）
        uint64_t we = ((m->text_end + 0x3FFF) & ~0x3FFFULL);
        if (pa > we)
            if (mprotect(m->base + we, pa - we, PROT_READ | PROT_WRITE))
                die("%s W 段 mprotect", m->name);
        LOG("%s: text[0,%#llx) 补丁区[%#llx,%#llx) RX，W 段[%#llx,%#llx) RW", m->name,
            (unsigned long long)m->text_end, (unsigned long long)pa, (unsigned long long)pa_end,
            (unsigned long long)we, (unsigned long long)pa);
    }
}
