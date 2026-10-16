// Adapted from d2r-mobile 944e199 dm/host/hle_net.c; offline semantics unchanged.
// 联网类（排除范围：联机/好友/商城/云存档）：一律接成「网络不可用」，游戏走离线分支。
// 剪口：nifm 报无网络；socket 只接 Initialize（返回成功，不建会话），其余 socket/ssl/friends 用到时再接成失败。
// Networking (out of scope: online play/friends/store/cloud saves): all hooked up as "network unavailable", the game takes the offline branch.
// Cuts: nifm reports no network; socket only hooks up Initialize (returns success, creates no session), the rest of socket/ssl/friends are hooked up as failures when they come into use.
#include "dm_host.h"
typedef uint32_t Result;
#define RES_OK 0u
#define RES_FAIL 0x2a2u

static Result h_socket_Initialize(const void *cfg) { (void)cfg; return RES_OK; }
static Result h_nifm_Initialize(void) { return RES_OK; }
static void h_nifm_Submit(void) {}
static void h_nifm_Cancel(void) {}
static int h_nifm_OnHold(void) { return 0; }
static int h_nifm_Available(void) { return 0; }
static Result h_nifm_HandleErr(void) { return RES_FAIL; }
static Result h_nifm_GetIp(uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
static Result h_nifm_GetProxy(void *p) { (void)p; return RES_FAIL; }
static Result h_ssl_Initialize(void) { return RES_OK; }
static Result h_ssl_Finalize(void) { return RES_OK; }

// ---------- socket：离线，所有连服务的调用按 BSD 习惯回 -1、错误码 ENETDOWN ----------
// 不剪：Inet* / nnsocketInet* 字节序与地址文本转换（纯计算）。
// ---------- socket: offline, all calls that contact the service return -1 in BSD convention, error code ENETDOWN ----------
// Not cut: Inet* / nnsocketInet* byte order and address text conversion (pure computation).
#define SK_ENETDOWN 50                  // Horizon 套接字用 FreeBSD 错误码 | Horizon sockets use FreeBSD error codes
static __thread int t_skerr = SK_ENETDOWN;
static int sk_fail(void) { t_skerr = SK_ENETDOWN; return -1; }
static int h_sk_Socket(int a, int b, int c) { (void)a; (void)b; (void)c; return sk_fail(); }
static int h_sk_Bind(int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_sk_Connect(int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_sk_Listen(int s, int n) { (void)s; (void)n; return sk_fail(); }
static int h_sk_Accept(int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_sk_Close(int s) { (void)s; return sk_fail(); }
static int h_sk_GetPeerName(int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_sk_GetSockName(int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_sk_GetSockOpt(int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
static int h_sk_SetSockOpt(int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
static int h_sk_Fcntl(int s, int c) { (void)s; (void)c; return sk_fail(); }
static int h_sk_Poll(void *f, unsigned long n, int t) { (void)f; (void)n; (void)t; return sk_fail(); }
static int h_sk_Select(int n, void *r, void *w, void *e, void *t) { (void)n; (void)r; (void)w; (void)e; (void)t; return sk_fail(); }
static int h_sk_EventFd(unsigned long v, int f) { (void)v; (void)f; return sk_fail(); }
static int h_sk_EventFdRead(int s, unsigned long *v) { (void)s; (void)v; return sk_fail(); }
static int h_sk_EventFdWrite(int s, unsigned long v) { (void)s; (void)v; return sk_fail(); }
static int h_sk_ResGet(void *o, int k) { (void)o; (void)k; return sk_fail(); }
static int h_sk_ResSet(const void *o) { (void)o; return sk_fail(); }
static long h_sk_Recv(int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
static long h_sk_RecvFrom(int s, void *b, unsigned long n, int f, void *a, unsigned *al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return sk_fail(); }
static long h_sk_Send(int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
static int h_sk_GetAddrInfo(const char *h, const char *s, const void *hint, void **res) {
    (void)h; (void)s; (void)hint; if (res) *res = NULL; return 4;   // EAI_FAIL
}
static void h_sk_FreeAddrInfo(void *ai) { (void)ai; }
static void * h_sk_GetHostEnt(const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
static int h_sk_GetLastError(void) { return t_skerr; }
// C 接口
// C interface
static int h_nsk_Socket(int a, int b, int c) { (void)a; (void)b; (void)c; return sk_fail(); }
static int h_nsk_Bind(int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_nsk_Connect(int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_nsk_Close(int s) { (void)s; return sk_fail(); }
static int h_nsk_GetPeerName(int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_nsk_GetSockName(int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
static int h_nsk_GetSockOpt(int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
static int h_nsk_SetSockOpt(int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
static int h_nsk_Fcntl(int s, int c) { (void)s; (void)c; return sk_fail(); }
static int h_nsk_Poll(void *f, unsigned long n, int t) { (void)f; (void)n; (void)t; return sk_fail(); }
static int h_nsk_ResGet(void *o, int k) { (void)o; (void)k; return sk_fail(); }
static int h_nsk_ResSet(const void *o) { (void)o; return sk_fail(); }
static long h_nsk_Recv(int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
static long h_nsk_Send(int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
static void * h_nsk_GetHost(const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
static int h_nsk_GetErrno(void) { return t_skerr; }
static void h_nsk_SetErrno(int e) { t_skerr = e; }

// ---------- ssl：离线，Initialize 成功（网络库全局初始化依赖），其余连服务的一律失败 ----------
// 不剪：Context / Connection 构造析构、Context::GetContextId（只读写游戏对象）。
// ---------- ssl: offline, Initialize succeeds (the network library's global initialization depends on it), everything else that contacts the service fails ----------
// Not cut: Context / Connection construction and destruction, Context::GetContextId (only read and write game objects).
static Result h_ssl_InitCpp(void) { return RES_OK; }
static Result h_ssl_FinCpp(void) { return RES_OK; }
static Result h_ssl_CtxCreate(void *self, int v) { (void)self; (void)v; return RES_FAIL; }
static Result h_ssl_CtxDestroy(void *self) { (void)self; return RES_FAIL; }
static Result h_ssl_CtxRegPki(void *self, uint64_t *id, int p) { (void)self; (void)id; (void)p; return RES_FAIL; }
static Result h_ssl_CtxRemovePki(void *self, uint64_t id) { (void)self; (void)id; return RES_FAIL; }
static Result h_ssl_BiBufSize(uint32_t *o, void *ids, unsigned n) { (void)ids; (void)n; if (o) *o = 0; return RES_FAIL; }
static Result h_ssl_BiCerts(void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) { (void)o; (void)b; (void)bs; (void)ids; (void)n; return RES_FAIL; }
static Result h_ssl_CnCreate(void *self, void *ctx) { (void)self; (void)ctx; return RES_FAIL; }
static Result h_ssl_CnDestroy(void *self) { (void)self; return RES_FAIL; }
static Result h_ssl_CnHandshake(void *self) { (void)self; return RES_FAIL; }
static Result h_ssl_CnHost(void *self, const char *h, unsigned n) { (void)self; (void)h; (void)n; return RES_FAIL; }
static Result h_ssl_CnIoMode(void *self, int m) { (void)self; (void)m; return RES_FAIL; }
static Result h_ssl_CnCache(void *self, int m) { (void)self; (void)m; return RES_FAIL; }
static Result h_ssl_CnSock(void *self, int s) { (void)self; (void)s; return RES_FAIL; }
static Result h_ssl_CnLastErr(void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
static Result h_ssl_CnVerifyErr(void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
static Result h_ssl_CnPeek(void *self, char *b, int *o, unsigned n) { (void)self; (void)b; (void)n; if (o) *o = 0; return RES_FAIL; }
static Result h_ssl_CnPoll(void *self, void *o, void *i, unsigned t) { (void)self; (void)o; (void)i; (void)t; return RES_FAIL; }
static int h_ssl_CnRead(void *self, char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
static int h_ssl_CnWrite(void *self, const char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
// C 接口：全部返回 nnResult
// C interface: all return nnResult
static Result h_nssl_Create(void) { return RES_FAIL; } static Result h_nssl_Destroy(void) { return RES_FAIL; }
static Result h_nssl_Handshake(void) { return RES_FAIL; } static Result h_nssl_GetId(void) { return RES_FAIL; }
static Result h_nssl_GetAlpn(void) { return RES_FAIL; } static Result h_nssl_VerifyErr(void) { return RES_FAIL; }
static Result h_nssl_Peek(void) { return RES_FAIL; } static Result h_nssl_Pending(void) { return RES_FAIL; } static Result h_nssl_Read(void) { return RES_FAIL; }
static Result h_nssl_SetHost(void) { return RES_FAIL; } static Result h_nssl_SetIo(void) { return RES_FAIL; }
static Result h_nssl_SetAlpn(void) { return RES_FAIL; } static Result h_nssl_SetOpt(void) { return RES_FAIL; }
static Result h_nssl_SetCache(void) { return RES_FAIL; } static Result h_nssl_SetSock(void) { return RES_FAIL; }
static Result h_nssl_SetVerify(void) { return RES_FAIL; } static Result h_nssl_Write(void) { return RES_FAIL; }
static Result h_nssl_CtxDestroy(void) { return RES_FAIL; } static Result h_nssl_CtxGetId(void) { return RES_FAIL; }

typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *net_force_lookup(const char *name) {
    static const hle_ent entries[] = {
        {"_ZN2nn6socket10InitializeERKNS0_6ConfigE", (void *)h_socket_Initialize},
        {"_ZN2nn4nifm10InitializeEv", (void *)h_nifm_Initialize},
        {"_ZN2nn4nifm20SubmitNetworkRequestEv", (void *)h_nifm_Submit},
        {"_ZN2nn4nifm20CancelNetworkRequestEv", (void *)h_nifm_Cancel},
        {"_ZN2nn4nifm22IsNetworkRequestOnHoldEv", (void *)h_nifm_OnHold},
        {"_ZN2nn4nifm18IsNetworkAvailableEv", (void *)h_nifm_Available},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", (void *)h_nifm_HandleErr},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", (void *)h_nifm_GetIp},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", (void *)h_nifm_GetProxy},
        {"nnsslInitialize", (void *)h_ssl_Initialize},
        {"nnsslFinalize", (void *)h_ssl_Finalize},
        {"_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", (void *)h_sk_Socket},
        {"_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", (void *)h_sk_Bind},
        {"_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", (void *)h_sk_Connect},
        {"_ZN2nn6socket6ListenEii", (void *)h_sk_Listen},
        {"_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", (void *)h_sk_Accept},
        {"_ZN2nn6socket5CloseEi", (void *)h_sk_Close},
        {"_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", (void *)h_sk_GetPeerName},
        {"_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", (void *)h_sk_GetSockName},
        {"_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", (void *)h_sk_GetSockOpt},
        {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", (void *)h_sk_SetSockOpt},
        {"_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", (void *)h_sk_Fcntl},
        {"_ZN2nn6socket4PollEPNS0_6PollFdEmi", (void *)h_sk_Poll},
        {"_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", (void *)h_sk_Select},
        {"_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", (void *)h_sk_EventFd},
        {"_ZN2nn6socket11EventFdReadEiPm", (void *)h_sk_EventFdRead},
        {"_ZN2nn6socket12EventFdWriteEim", (void *)h_sk_EventFdWrite},
        {"_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", (void *)h_sk_ResGet},
        {"_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", (void *)h_sk_ResSet},
        {"_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", (void *)h_sk_Recv},
        {"_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", (void *)h_sk_RecvFrom},
        {"_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", (void *)h_sk_Send},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", (void *)h_sk_GetAddrInfo},
        {"_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", (void *)h_sk_FreeAddrInfo},
        {"_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", (void *)h_sk_GetHostEnt},
        {"_ZN2nn6socket12GetLastErrorEv", (void *)h_sk_GetLastError},
        {"nnsocketSocket", (void *)h_nsk_Socket},
        {"nnsocketBind", (void *)h_nsk_Bind},
        {"nnsocketConnect", (void *)h_nsk_Connect},
        {"nnsocketClose", (void *)h_nsk_Close},
        {"nnsocketGetPeerName", (void *)h_nsk_GetPeerName},
        {"nnsocketGetSockName", (void *)h_nsk_GetSockName},
        {"nnsocketGetSockOpt", (void *)h_nsk_GetSockOpt},
        {"nnsocketSetSockOpt", (void *)h_nsk_SetSockOpt},
        {"nnsocketFcntl", (void *)h_nsk_Fcntl},
        {"nnsocketPoll", (void *)h_nsk_Poll},
        {"nnsocketResolverGetOption", (void *)h_nsk_ResGet},
        {"nnsocketResolverSetOption", (void *)h_nsk_ResSet},
        {"nnsocketRecv", (void *)h_nsk_Recv},
        {"nnsocketSend", (void *)h_nsk_Send},
        {"nnsocketGetHostByNameWithOptions", (void *)h_nsk_GetHost},
        {"nnsocketGetLastErrno", (void *)h_nsk_GetErrno},
        {"nnsocketSetLastErrno", (void *)h_nsk_SetErrno},
        {"_ZN2nn3ssl10InitializeEv", (void *)h_ssl_InitCpp},
        {"_ZN2nn3ssl8FinalizeEv", (void *)h_ssl_FinCpp},
        {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", (void *)h_ssl_CtxCreate},
        {"_ZN2nn3ssl7Context7DestroyEv", (void *)h_ssl_CtxDestroy},
        {"_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", (void *)h_ssl_CtxRegPki},
        {"_ZN2nn3ssl7Context9RemovePkiEm", (void *)h_ssl_CtxRemovePki},
        {"_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", (void *)h_ssl_BiBufSize},
        {"_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", (void *)h_ssl_BiCerts},
        {"_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", (void *)h_ssl_CnCreate},
        {"_ZN2nn3ssl10Connection7DestroyEv", (void *)h_ssl_CnDestroy},
        {"_ZN2nn3ssl10Connection11DoHandshakeEv", (void *)h_ssl_CnHandshake},
        {"_ZN2nn3ssl10Connection11SetHostNameEPKcj", (void *)h_ssl_CnHost},
        {"_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", (void *)h_ssl_CnIoMode},
        {"_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", (void *)h_ssl_CnCache},
        {"_ZN2nn3ssl10Connection19SetSocketDescriptorEi", (void *)h_ssl_CnSock},
        {"_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", (void *)h_ssl_CnLastErr},
        {"_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", (void *)h_ssl_CnVerifyErr},
        {"_ZN2nn3ssl10Connection4PeekEPcPij", (void *)h_ssl_CnPeek},
        {"_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", (void *)h_ssl_CnPoll},
        {"_ZN2nn3ssl10Connection4ReadEPcj", (void *)h_ssl_CnRead},
        {"_ZN2nn3ssl10Connection5WriteEPKcj", (void *)h_ssl_CnWrite},
        {"nnsslConnectionCreate", (void *)h_nssl_Create},
        {"nnsslConnectionDestroy", (void *)h_nssl_Destroy},
        {"nnsslConnectionDoHandshakeWithCertBuffer", (void *)h_nssl_Handshake},
        {"nnsslConnectionGetConnectionId", (void *)h_nssl_GetId},
        {"nnsslConnectionGetNextAlpnProto", (void *)h_nssl_GetAlpn},
        {"nnsslConnectionGetVerifyCertError", (void *)h_nssl_VerifyErr},
        {"nnsslConnectionPeek", (void *)h_nssl_Peek},
        {"nnsslConnectionPending", (void *)h_nssl_Pending},
        {"nnsslConnectionRead", (void *)h_nssl_Read},
        {"nnsslConnectionSetHostName", (void *)h_nssl_SetHost},
        {"nnsslConnectionSetIoMode", (void *)h_nssl_SetIo},
        {"nnsslConnectionSetNextAlpnProto", (void *)h_nssl_SetAlpn},
        {"nnsslConnectionSetOption", (void *)h_nssl_SetOpt},
        {"nnsslConnectionSetSessionCacheMode", (void *)h_nssl_SetCache},
        {"nnsslConnectionSetSocketDescriptor", (void *)h_nssl_SetSock},
        {"nnsslConnectionSetVerifyOption", (void *)h_nssl_SetVerify},
        {"nnsslConnectionWrite", (void *)h_nssl_Write},
        {"nnsslContextDestroy", (void *)h_nssl_CtxDestroy},
        {"nnsslContextGetContextId", (void *)h_nssl_CtxGetId},
    };
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++)
        if(!strcmp(name,entries[i].name)) return &entries[i];
    return NULL;
}
