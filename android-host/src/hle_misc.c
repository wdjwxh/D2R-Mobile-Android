// 零散服务层：追加内容 / 商店 / 错误小程序 / 敏感词过滤 / 服务器名解析 / 家长控制 / 软键盘。
// 真机上各自走 IPC 或系统小程序；宿主按单机离线给结果。纯计算的 SDK 部分（hash、util、软件 Opus）不接管，直接用原实现。
// Miscellaneous service layer: add-on content / shop / error applet / profanity filter / server name resolution / parental controls / software keyboard.
// On the real device each goes through IPC or a system applet; the host gives results as single-player offline. Pure-computation SDK parts (hash, util, software Opus) are not taken over, the original implementation is used directly.
#include "dm_host.h"
typedef uint32_t Result;
#define RES_OK 0u
#define RES_FAIL 0x2a2u

// ---------- aoc 追加内容（术士君临，保留）/ ec 商店（排除）----------
// 术士君临（Reign of the Warlock）是 Switch 包里唯一的追加内容：AOC 0100726014353001 = 本体 0100726014352000 的追加内容序号 1。
// 该 DLC 只是约 100 KB 的解锁凭证，内容本体已在主 RomFS；main 不导入 MountAddOnContent，只按「有没有序号 1」判断。
// 复刻范围要求保留术士君临，所以报告已拥有序号 1；排除的是商店（ec），不是 DLC 本身。
// ---------- aoc add-on content (Reign of the Warlock, kept) / ec shop (excluded) ----------
// Reign of the Warlock is the only add-on content in the Switch package: AOC 0100726014353001 = add-on content index 1 of the base 0100726014352000.
// This DLC is only an unlock ticket of about 100 KB, the content itself is already in the main RomFS; main does not import MountAddOnContent and only judges by "is index 1 present".
// The reimplementation scope requires keeping Reign of the Warlock, so index 1 is reported as owned; what is excluded is the shop (ec), not the DLC itself.
enum { AOC_WARLOCK = 1 };
// DM_AOC_OWNED=0：测试开关，回到「无 DLC」前提（只用于对拍未装 DLC 时录的真值；缺省 = 拥有）
// DM_AOC_OWNED=0: test switch, back to the "no DLC" premise (only for lockstep comparison against ground truth recorded without the DLC installed; default = owned)
static int aoc_owned(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_AOC_OWNED"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_AOC_OWNED=0：aoc 报告 0 个追加内容（对拍测试用）"); } return v; }
static int h_aoc_Count(void) { return aoc_owned() ? 1 : 0; }
// ListAddOnContent(out, 起始下标, 最多个数)：按已拥有序号列表 {1} 从起始下标起写出，返回写出个数
// ListAddOnContent(out, start index, max count): writes out from the owned index list {1} starting at the start index, returns the number written
static int h_aoc_List(int *out, int off, int n) {
    static const int owned[] = {AOC_WARLOCK};
    int w = 0;
    if (!aoc_owned() || off<0) return 0;
    for (int i = off; i < (int)(sizeof owned / sizeof *owned) && w < n; i++) out[w++] = owned[i];
    return w;
}
// 事件对象按需由 nn::os 旁表建（未触发），游戏等它只会超时 / 取到未触发
// The event object is created on demand by the nn::os side table (not signaled), the game waiting on it only times out / gets not-signaled
static void h_aoc_GetEv(void *ev) { hle_bind_system_event(ev, 0); }
static void h_ec_ShowShop(void) { LOG("ec::ShowShopAddOnContentList（剪）"); }

// ---------- err 错误小程序：宿主打印 ----------
// ---------- err error applet: printed by the host ----------
static void h_err_ShowError(Result r) {
    LOG("err::ShowError Result=%#x（模块 %u，描述 %u）", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

// ---------- ngc 敏感词过滤：词库在系统数据存档（经 IPC 挂载）；单机无他人可见文本，恒判无敏感词 ----------
// ---------- ngc profanity filter: the word list is in a system data save (mounted via IPC); single-player has no text visible to others, always judged as no profanity ----------
static void h_ngc_Ctor(void *self) { (void)self; }
static void h_ngc_Dtor(void *self) { (void)self; }
static Result h_ngc_Init(void *self, void *work, uint64_t sz) { (void)self; (void)work; (void)sz; return RES_OK; }
static uint32_t h_ngc_Ver(void *self) { (void)self; return 0; }
static Result h_ngc_Check(void *self, uint32_t *masks, uint32_t pat, const char **words, uint64_t n) {
    (void)self; (void)pat; (void)words;
    for (uint64_t i = 0; i < n; i++) masks[i] = 0;
    return RES_OK;
}

// ---------- nsd 服务器名解析 / pctl 自由通信：联网，排除范围 ----------
// ---------- nsd server name resolution / pctl free communication: networking, out of scope ----------
static Result h_nsd_Resolve(void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
static Result h_nsd_ResolveEx(void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
// bool 参数只看低 8 位；返回 bool
// bool arguments only look at the low 8 bits; returns bool
static unsigned h_pctl_TryBegin(unsigned show_ui) { (void)show_ui; return 0; }
static void h_pctl_End(void) {}

// swkbd 软键盘：见 hle_swkbd.c
// swkbd software keyboard: see hle_swkbd.c

typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *misc_force_lookup(const char *name) {
    static const hle_ent entries[]={
        {"_ZN2nn3aoc17CountAddOnContentEv", (void *)h_aoc_Count},
        {"_ZN2nn3aoc16ListAddOnContentEPiii", (void *)h_aoc_List},
        {"_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", (void *)h_aoc_GetEv},
        {"_ZN2nn2ec24ShowShopAddOnContentListEv", (void *)h_ec_ShowShop},
        {"_ZN2nn3err9ShowErrorENS_6ResultE", (void *)h_err_ShowError},
        {"_ZN2nn3ngc15ProfanityFilterC1Ev", (void *)h_ngc_Ctor},
        {"_ZN2nn3ngc15ProfanityFilterD1Ev", (void *)h_ngc_Dtor},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", (void *)h_ngc_Init},
        {"_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", (void *)h_ngc_Ver},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", (void *)h_ngc_Check},
        {"_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_", (void *)h_nsd_Resolve},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", (void *)h_nsd_ResolveEx},
        {"_ZN2nn4pctl25TryBeginFreeCommunicationEb", (void *)h_pctl_TryBegin},
        {"_ZN2nn4pctl20EndFreeCommunicationEv", (void *)h_pctl_End},
    };
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++)
        if(!strcmp(name,entries[i].name)) return &entries[i];
    return NULL;
}
