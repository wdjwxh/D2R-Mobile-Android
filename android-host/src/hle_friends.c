// Adapted from upstream 944e199, using native Horizon event handles.
// nn::friends（好友 / 在线状态 / 邀请，排除范围）：连 friends 服务的接口一律剪成「不可用」。
// 不剪：Friend / Profile / FriendPresence 的取值函数、Friend / Profile / UserPresence 构造（只读写游戏自己的对象，不走服务）。
// 返回约定：Result 回 RES_FAIL；bool 回假；事件指针给一个已绑定内核事件、永不触发的 SystemEvent。
// nn::friends (friends / presence / invitations, out of scope): every interface that connects to the friends service is cut to "unavailable".
// Not cut: the getters of Friend / Profile / FriendPresence and the constructors of Friend / Profile / UserPresence (they only read and write the game's own objects and do not go through the service).
// Return convention: Result returns RES_FAIL; bool returns false; event pointers get a SystemEvent that is bound to a kernel event and never fires.
#include "dm_host.h"
typedef uint32_t Result;
#define RES_OK 0u
#define RES_FAIL 0x2a2u

// 永不触发的通知事件（GetSystemEvent 类返回指针；首次在客体线程上绑定）
// Notification event that never fires (GetSystemEvent-style calls return a pointer; bound on the guest thread on first use)
static _Alignas(16) uint8_t g_ev_notify[0x100], g_ev_invite[0x100];
static void *quiet_event(uint8_t *ev) { hle_bind_system_event(ev, 0); return ev; }

static void h_fr_Initialize(void) {}
// AsyncContext：不发请求；完成事件立即触发、结果失败（同 hle_account.c）
// AsyncContext: no request is sent; the completion event fires immediately and the result is failure (same as hle_account.c)
static void h_fr_AcCtor(void *self) { (void)self; }
static void h_fr_AcDtor(void *self) { (void)self; }
static Result h_fr_AcGetEv(void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
static Result h_fr_AcResult(const void *self) { (void)self; return RES_FAIL; }
// 列表 / 资料 / 游玩记录
// Lists / profiles / play history
static Result h_fr_GetFriendList(int *count, void *out, int off, int n, const void *filt) {
    (void)out; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL;
}
static Result h_fr_GetProfileList(void *ctx, void *out, const void *uid, const void *ids, int n) { (void)ctx; (void)out; (void)uid; (void)ids; (void)n; return RES_FAIL; }
static Result h_fr_UpdateFriendInfo(void *f, const void *uid, const void *ids, int n) { (void)f; (void)uid; (void)ids; (void)n; return RES_FAIL; }
static Result h_fr_AddPlayHistory(const void *k, const void *a, const void *b) { (void)k; (void)a; (void)b; return RES_FAIL; }
static Result h_fr_GetPHKey(void *k, const void *uid, int local) { (void)k; (void)uid; (void)local; return RES_FAIL; }
// 通知队列
// Notification queue
static void h_fr_NqCtor(void *self) { (void)self; }
static void h_fr_NqDtor(void *self) { (void)self; }
static Result h_fr_NqInit(void *self, const void *uid) { (void)self; (void)uid; return RES_FAIL; }
static void * h_fr_NqGetEv(void *self) { (void)self; return quiet_event(g_ev_notify); }
static Result h_fr_NqPop(void *self, void *info) { (void)self; (void)info; return RES_FAIL; }
// 邀请
// Invitations
static void * h_fr_InvEv(void) { return quiet_event(g_ev_invite); }
static int h_fr_InvPop(void *uid, uint64_t *sz, void *buf, size_t n) {
    (void)uid; (void)sz; (void)buf; (void)n; return 0;
}
static Result h_fr_StartInv(const void *uid, int n, const void *d, const void *p, size_t sz) { (void)uid; (void)n; (void)d; (void)p; (void)sz; return RES_FAIL; }
static Result h_fr_SendInv(const void *uid, const void *ids, int n, const void *d, const void *p, size_t sz) { (void)uid; (void)ids; (void)n; (void)d; (void)p; (void)sz; return RES_FAIL; }
static Result h_fr_ShowDetail(const void *uid, const void *id, const void *a, const void *b) { (void)uid; (void)id; (void)a; (void)b; return RES_FAIL; }
// 在线状态
// Presence
static Result h_fr_UpInit(void *self, const void *uid) { (void)self; (void)uid; return RES_FAIL; }
static Result h_fr_UpDesc(void *self, const char *s) { (void)self; (void)s; return RES_FAIL; }
static Result h_fr_UpCommit(void *self) { (void)self; return RES_FAIL; }

typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *friends_force_lookup(const char *name) {
    static const hle_ent entries[]={
        {"_ZN2nn7friends10InitializeEv", (void *)h_fr_Initialize},
        {"_ZN2nn7friends12AsyncContextC1Ev", (void *)h_fr_AcCtor},
        {"_ZN2nn7friends12AsyncContextD1Ev", (void *)h_fr_AcDtor},
        {"_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", (void *)h_fr_AcGetEv},
        {"_ZNK2nn7friends12AsyncContext9GetResultEv", (void *)h_fr_AcResult},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", (void *)h_fr_GetFriendList},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", (void *)h_fr_GetProfileList},
        {"_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", (void *)h_fr_UpdateFriendInfo},
        {"_ZN2nn7friends14AddPlayHistoryERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameES6_", (void *)h_fr_AddPlayHistory},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", (void *)h_fr_GetPHKey},
        {"_ZN2nn7friends17NotificationQueueC1Ev", (void *)h_fr_NqCtor},
        {"_ZN2nn7friends17NotificationQueueD1Ev", (void *)h_fr_NqDtor},
        {"_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", (void *)h_fr_NqInit},
        {"_ZN2nn7friends17NotificationQueue14GetSystemEventEv", (void *)h_fr_NqGetEv},
        {"_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE", (void *)h_fr_NqPop},
        {"_ZN2nn7friends36GetFriendInvitationNotificationEventEv", (void *)h_fr_InvEv},
        {"_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", (void *)h_fr_InvPop},
        {"_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", (void *)h_fr_StartInv},
        {"_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", (void *)h_fr_SendInv},
        {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", (void *)h_fr_ShowDetail},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", (void *)h_fr_UpInit},
        {"_ZN2nn7friends12UserPresence14SetDescriptionEPKc", (void *)h_fr_UpDesc},
        {"_ZN2nn7friends12UserPresence6CommitEv", (void *)h_fr_UpCommit},
    };
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++)
        if(!strcmp(name,entries[i].name)) return &entries[i];
    return NULL;
}
