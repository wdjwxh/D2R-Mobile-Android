// D2R Mobile Android 宿主 — 公共头
// 移植自上游 jhaizhou-ops/d2r-mobile dm/host（MIT），Darwin 特有部分改为 bionic 等价物。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MOD 4
#ifdef __ANDROID__
#include <android/log.h>
void dm_log_file(const char *fmt, ...);
// logcat 有行数限流：同时落文件（/data/data/local.d2rm/files/dmhost.log），崩溃后可完整拉取
#define die(...) do { dm_log_file(__VA_ARGS__); __android_log_print(ANDROID_LOG_FATAL, "dmhost", __VA_ARGS__); exit(1); } while (0)
#define LOG(...) do { dm_log_file(__VA_ARGS__); __android_log_print(ANDROID_LOG_INFO, "dmhost", __VA_ARGS__); } while (0)
#else
#define die(...) do { fprintf(stderr, "[dmhost] " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define LOG(...) do { fprintf(stderr, "[dmhost] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_KRES0, KEY_KRES1, KEY_KASPECT, KEY_KASPECTUI, KEY_N };

struct Elf64_Dyn_ { int64_t tag; uint64_t val; };
struct Elf64_Sym_ { uint32_t name, info; uint64_t value, size; };
struct Elf64_Rela_ { uint64_t off, info; int64_t add; };

typedef struct {
    const char *name;
    uint8_t *base;         // 映像基址（整映像一段映射，先 RW，重定位后 text 设 RX）
    uint64_t size;
    uint64_t text_end, ro_off, data_off, patch_off, key_page_off, key_page_size;
    unsigned key_idx[KEY_N];
    const struct Elf64_Dyn_ *dyn;
    const struct Elf64_Sym_ *symtab; const char *strtab; unsigned nsym;
    uint64_t init, init_array, init_arraysz, fini_array, fini_arraysz;
    unsigned ngate;
    int from_dlopen;
} module_t;

extern module_t g_mod[MAX_MOD];
extern int g_nmod;

// 装载器（loader.c）
void load_module(const char *image_path, const char *tables_dir, const char *name);
void resolve_and_relocate(void);
void install_key_pages(void);
void protect_modules(void);
void *guest_sym(const char *name);
void hle_bind_system_event(void *event, int autoclear);
void hle_signal_system_event(void *event);
void svc_thread_perf_report(void);
uint64_t dm_guest_ns(void);
void guest_clock_freeze_for(unsigned reason, int on);
int front_text_input(const char *title,const char *sub,const char *guide,const char *initial,int max_chars,char *out,int out_cap);

// 门（gates.c）
void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig);
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs, uint64_t guest_sp);
uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);

// 线程槽（tsd.c）：自校准 bionic TSD，返回第 k 键的槽偏移（字节）
void tsd_init(void);
unsigned long tsd_slot_offset(unsigned k);
void tsd_guest_thread_init(void);
void dm_host_thread_priority(int priority,const char *role);
