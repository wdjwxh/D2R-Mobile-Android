// nn::account：真机经 acc:u0 服务管理用户；宿主固定一个本地用户。
// 联网账号（网络服务账号 / 任天堂账号授权 / 令牌）属排除范围，统一回 RES_FAIL，异步上下文的完成事件立即触发。
// 环境开关：DM_NICK=昵称（默认 Player）。
// nn::account: the real device manages users via the acc:u0 service; the host has one fixed local user.
// Online accounts (network service account / Nintendo Account authorization / tokens) are out of scope and uniformly return RES_FAIL; the async context's completion event fires immediately.
// Environment switch: DM_NICK=nickname (default Player).
#include "dm_host.h"
typedef uint32_t Result;
#define RES_OK 0
#define RES_FAIL 0x2a2

typedef struct { uint64_t v[2]; } Uid;                 // nn::account::Uid，16 字节 | nn::account::Uid, 16 bytes
typedef struct { uint64_t v[3]; } UserHandle;          // nn::account::UserHandle，24 字节 | nn::account::UserHandle, 24 bytes
_Static_assert(sizeof(Uid) == 16, "Uid");
static const Uid k_uid = {{0x78626c0000000001ull, 0x53574e4154495645ull}};   // 非零即有效 | Non-zero means valid
#define HANDLE_MAGIC 0x55534552484e444cull                                    // "USERHNDL"

static void h_acc_Init(void) { LOG("Account initialized: local Player"); }

static Result h_acc_ListOpen(int *n, Uid *out, int max) {
    *n = 0;
    if (max > 0) { out[0] = k_uid; *n = 1; }
    return RES_OK;
}
static Result h_acc_ShowSel(Uid *out, const void *settings) {
    (void)settings; *out = k_uid; return RES_OK;
}
static unsigned h_acc_TryOpenPre(UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; return 1;
}
static Result h_acc_OpenUser(UserHandle *h, const Uid *uid) {
    h->v[0] = uid->v[0]; h->v[1] = uid->v[1]; h->v[2] = HANDLE_MAGIC; return RES_OK;
}
static Result h_acc_GetUserId(Uid *out, const UserHandle *h) {
    if (h->v[2] != HANDLE_MAGIC) die("account::GetUserId 句柄不是宿主发的（%#llx）", (unsigned long long)h->v[2]);
    out->v[0] = h->v[0]; out->v[1] = h->v[1]; return RES_OK;
}
// nn::account::Nickname = struct { char name[33]; }，UTF-8
static Result h_acc_GetNick(char *nick, const Uid *uid) {
    (void)uid;
    const char *e = getenv("DM_NICK");
    memset(nick, 0, 33); strncpy(nick, e ? e : "Player", 32);
    return RES_OK;
}

// ---------- 联网账号：剪 ----------
// ---------- Online accounts: cut ----------
static Result h_acc_EnsureNsa(const UserHandle *h) { (void)h; return RES_FAIL; }
static Result h_acc_GetNsaId(void *id, const UserHandle *h) { (void)id; (void)h; return RES_FAIL; }
static Result h_acc_LoadTok(uint64_t *n, char *buf, uint64_t sz, const UserHandle *h) {
    (void)buf; (void)sz; (void)h; if (n) *n = 0; return RES_FAIL;
}
static Result h_acc_ShowLic(const UserHandle *h) { (void)h; return RES_FAIL; }


static void h_acc_AcCtor(void *self) { (void)self; }
static Result h_acc_EnsureTokAsync(void *ctx, const UserHandle *h) { (void)ctx; (void)h; return RES_FAIL; }
static Result h_acc_AcGetEv(void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
static Result h_acc_AcGetRes(void *self) { (void)self; return RES_FAIL; }
static void h_acc_NaCtor(void *self) { (void)self; }
static void h_acc_NaDtor(void *self) { (void)self; }
static Result h_acc_NaCreate(void *ctx, const UserHandle *h, const void *prm, void *buf, uint64_t sz) {
    (void)ctx; (void)h; (void)prm; (void)buf; (void)sz; return RES_FAIL;
}
static Result h_acc_NaGetEv(void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
static Result h_acc_NaGetRes(void *self) { (void)self; return RES_FAIL; }
static Result h_acc_NaGetTok(void *self, uint64_t *n, char *buf, uint64_t sz) {
    (void)self; (void)buf; (void)sz; if (n) *n = 0; return RES_FAIL;
}

typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *account_force_lookup(const char *name) {
    static const hle_ent entries[] = {
        {"_ZN2nn7account10InitializeEv", (void *)h_acc_Init},
        {"_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", (void *)h_acc_ListOpen},
        {"_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", (void *)h_acc_ShowSel},
        {"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", (void *)h_acc_TryOpenPre},
        {"_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", (void *)h_acc_OpenUser},
        {"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", (void *)h_acc_GetUserId},
        {"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", (void *)h_acc_GetNick},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", (void *)h_acc_EnsureNsa},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", (void *)h_acc_GetNsaId},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", (void *)h_acc_LoadTok},
        {"_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", (void *)h_acc_ShowLic},
        {"_ZN2nn7account12AsyncContextC1Ev", (void *)h_acc_AcCtor},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", (void *)h_acc_EnsureTokAsync},
        {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", (void *)h_acc_AcGetEv},
        {"_ZN2nn7account12AsyncContext9GetResultEv", (void *)h_acc_AcGetRes},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContextC1Ev", (void *)h_acc_NaCtor},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev", (void *)h_acc_NaDtor},
        {"_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm", (void *)h_acc_NaCreate},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext14GetSystemEventEPNS_2os11SystemEventE", (void *)h_acc_NaGetEv},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext34GetResultWithInteractionIfRequiredEv", (void *)h_acc_NaGetRes},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext10GetIdTokenEPmPcm", (void *)h_acc_NaGetTok},
    };
    for (unsigned i=0;i<sizeof entries/sizeof entries[0];i++)
        if (!strcmp(name,entries[i].name)) return &entries[i];
    return NULL;
}
