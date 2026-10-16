// Adapted device/queue semantics from upstream 944e199; Vulkan owns execution.
#include "dm_host.h"
#include <stdatomic.h>
#include <pthread.h>
#include "vk_backend.h"
#include "vk_compute.h"
#include "nvn/nvn_protos.h"
#define MAGIC(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
#define B(x) ((x)&0xff)
#define OLOG(...) LOG(__VA_ARGS__)
#define FITS(T,H) _Static_assert(sizeof(T)<=sizeof(H), #T " NVN ABI overflow")
typedef struct { uint32_t flags; } dev_builder_t;
typedef struct { uint32_t magic,flags,origin,depth_mode; void *dbg_cb,*dbg_data; } dev_t_;
typedef struct { NVNdevice *dev; uint32_t flags; uint64_t cmd_sz,comp_sz,ctl_sz,flush; void *mem; uint64_t mem_sz; } queue_builder_t;
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits, samples; int samples_counting; } queue_t;
FITS(dev_builder_t,NVNdeviceBuilder); FITS(dev_t_,NVNdevice);
FITS(queue_builder_t,NVNqueueBuilder); FITS(queue_t,NVNqueue);
static NVNdevice *live_device;
static void nvn_translate_drain(void);
void dm_nvn_translate_drain(void) { nvn_translate_drain(); }
static void descriptors_close(void);
static void replay_state_free(queue_t *owner);
static void *nvn_lookup(const char *name);
#define NVN(name,ret,...) static ret h_##name(__VA_ARGS__); \
    _Static_assert(__builtin_types_compatible_p(__typeof__(&h_##name),NVNT_##name), #name " signature"); \
    static ret h_##name(__VA_ARGS__)
NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = f; }
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    if (live_device && live_device != d) return 0;
    if (dm_vk_open()) return 0;
    live_device = d;
    dev_t_ *o = (dev_t_ *)d; memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0); o->flags = ((const dev_builder_t *)b)->flags;
    OLOG("DeviceInitialize %p flags=%#x", (void *)d, o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { nvn_translate_drain(); ((dev_t_ *)d)->magic = 0; if(live_device == d) { replay_state_free(NULL); descriptors_close(); dm_vk_close(); live_device = NULL; } }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d; return (PFNNVNGENERICFUNCPTRPROC)nvn_lookup(name);
}

// 设备属性：Tegra X1 实测值（nnheaders nvn_DeviceConstantsNX.h，在 SMO 上逐项调 nvnDeviceGetInteger 得到）
// Device properties: values measured on Tegra X1 (nnheaders nvn_DeviceConstantsNX.h, obtained by calling nvnDeviceGetInteger item by item on SMO)
static const int g_devinfo[] = {
    [0x00] = 53, [0x01] = 311,   // API 版本：游戏读了不比较（main 0x173cbd8），此值未核 | API version: the game reads it without comparing (main 0x173cbd8); this value is unverified
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    unsigned w = (unsigned)what;
    // 0x5F 分离采样器绑定数、0x60/0x61 在 SMO 实测表里没有：0x5F 按采样器池上限给 4096 未核，0x60/0x61 报红给 0
    // 0x5F separate sampler binding count and 0x60/0x61 are not in the table measured on SMO: 0x5F is given 4096 per the sampler pool limit, unverified; 0x60/0x61 report a failure and return 0
    if (w >= sizeof g_devinfo / sizeof *g_devinfo) { LOG("【红】nvnDeviceGetInteger 未知项 %#x，给 0", w); *out = 0; return; }
    *out = g_devinfo[w];
    OLOG("DeviceGetInteger %#x -> %d", w, *out);
}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (NVNimageHandle)id; }
// 分离贴图 / 采样器句柄：值 = 描述符号（采样器号放 20 位以上，与组合句柄的位布局一致）
// Separate texture / sampler handles: value = descriptor index (the sampler index goes in bit 20 and above, consistent with the bit layout of combined handles)
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d; o->dbg_cb = B(enable) ? (void *)cb : NULL; o->dbg_data = data;
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) { ((dev_t_ *)d)->origin = m; LOG("[nvn] 窗口原点 %s", m ? "左上（同 D3D / Metal）" : "左下（GL）"); }
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) { ((dev_t_ *)d)->depth_mode = m; LOG("[nvn] 深度模式 %s", m ? "近=0（同 D3D / Metal）" : "近=-w（GL）"); }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; (void)h; }

#define QB(b) ((queue_builder_t *)(b))
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
    QB(b)->cmd_sz = 65536; QB(b)->comp_sz = 262144; QB(b)->ctl_sz = 16384; QB(b)->flush = 4096;   // 设备默认值 | Device defaults
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_sz = n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_sz = n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_sz = n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = n; }
// 队列内存：命令 + 计算 + 控制三块之和按 4 KiB 取整（宿主不使用，只需游戏按此大小分配）
// Queue memory: the sum of the command + compute + control blocks rounded to 4 KiB (the host does not use it; the game only needs to allocate this size)
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    return (o->cmd_sz + o->comp_sz + o->ctl_sz + 4095) & ~(size_t)4095;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_sz = n; }
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    if (((const queue_builder_t *)b)->dev != live_device || !live_device) return 0;
    replay_state_free((queue_t *)q);
    dm_vk_samples_forget(&((queue_t *)q)->samples);
    *(queue_t *)q = (queue_t){.magic=MAGIC('Q', 'U', 'E', 'U'), .dev=((const queue_builder_t *)b)->dev};
    OLOG("QueueInitialize %p", (void *)q); return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) { nvn_translate_drain();if(dm_vk_barrier())die("Queue finalize GPU wait failed"); dm_vk_samples_forget(&((queue_t *)q)->samples);replay_state_free((queue_t *)q); ((queue_t *)q)->magic = 0; }
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    (void)q; if (info) memset(info, 0, sizeof *info); return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnQueueFlush, void, NVNqueue *q) { (void)q; }
NVN(nvnQueueFinish, void, NVNqueue *q) { (void)q; nvn_translate_drain();if(dm_vk_finish()) die("Vulkan queue finish failed"); }


typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;            FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;            /* 每目标 4 位：r=1 g=2 b=4 a=8，置位 = 写 | 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ FITS(chmask_st, NVNchannelMaskState);
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3; } blend_st;
FITS(blend_st, NVNblendState);
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } ms_st;
FITS(ms_st, NVNmultisampleState);
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly_st;           FITS(poly_st, NVNpolygonState);
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} ds_st;                                                                                     FITS(ds_st, NVNdepthStencilState);
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } va_st;                         FITS(va_st, NVNvertexAttribState);
typedef struct { uint32_t stride, divisor; } vs_st;                                          FITS(vs_st, NVNvertexStreamState);


#define ST(T, x) ((T *)(x))
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG("【红】顶点属性偏移 %td 超出打包范围", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }


#include "nvn_memory.inc"
#include "nvn/objects.inc"
#include "nvn_object_snapshots.inc"
#include "nvn_resources.inc"
#include "nvn_commands.inc"
#include "nvn_descriptors.inc"
#include "nvn_translate_queue.inc"
#include "nvn_sync.inc"
#include "nvn_window.inc"
#include "nvn_replay.inc"
#include "nvn/missing.inc"
static void *nvn_lookup(const char *name) {
    if (!name) return NULL;
    static const struct { const char *name; void *fn; } impl[] = {
        {"nvnDeviceBuilderSetDefaults", (void *)h_nvnDeviceBuilderSetDefaults},
        {"nvnDeviceBuilderSetFlags", (void *)h_nvnDeviceBuilderSetFlags},
        {"nvnDeviceInitialize", (void *)h_nvnDeviceInitialize},
        {"nvnDeviceFinalize", (void *)h_nvnDeviceFinalize},
        {"nvnDeviceGetProcAddress", (void *)h_nvnDeviceGetProcAddress},
        {"nvnDeviceGetInteger", (void *)h_nvnDeviceGetInteger},
        {"nvnDeviceGetImageHandle", (void *)h_nvnDeviceGetImageHandle},
        {"nvnDeviceGetSeparateTextureHandle", (void *)h_nvnDeviceGetSeparateTextureHandle},
        {"nvnDeviceGetSeparateSamplerHandle", (void *)h_nvnDeviceGetSeparateSamplerHandle},
        {"nvnDeviceInstallDebugCallback", (void *)h_nvnDeviceInstallDebugCallback},
        {"nvnDeviceSetWindowOriginMode", (void *)h_nvnDeviceSetWindowOriginMode},
        {"nvnDeviceSetDepthMode", (void *)h_nvnDeviceSetDepthMode},
        {"nvnDeviceFinalizeCommandHandle", (void *)h_nvnDeviceFinalizeCommandHandle},
        {"nvnQueueBuilderSetDefaults", (void *)h_nvnQueueBuilderSetDefaults},
        {"nvnQueueBuilderSetDevice", (void *)h_nvnQueueBuilderSetDevice},
        {"nvnQueueBuilderSetFlags", (void *)h_nvnQueueBuilderSetFlags},
        {"nvnQueueBuilderSetCommandMemorySize", (void *)h_nvnQueueBuilderSetCommandMemorySize},
        {"nvnQueueBuilderSetComputeMemorySize", (void *)h_nvnQueueBuilderSetComputeMemorySize},
        {"nvnQueueBuilderSetControlMemorySize", (void *)h_nvnQueueBuilderSetControlMemorySize},
        {"nvnQueueBuilderSetCommandFlushThreshold", (void *)h_nvnQueueBuilderSetCommandFlushThreshold},
        {"nvnQueueBuilderGetQueueMemorySize", (void *)h_nvnQueueBuilderGetQueueMemorySize},
        {"nvnQueueBuilderSetQueueMemory", (void *)h_nvnQueueBuilderSetQueueMemory},
        {"nvnQueueInitialize", (void *)h_nvnQueueInitialize},
        {"nvnQueueFinalize", (void *)h_nvnQueueFinalize},
        {"nvnQueueGetError", (void *)h_nvnQueueGetError},
        {"nvnQueueFlush", (void *)h_nvnQueueFlush},
        {"nvnQueueFinish", (void *)h_nvnQueueFinish},
        {"nvnBlendStateSetDefaults", (void *)h_nvnBlendStateSetDefaults},
        {"nvnBlendStateSetBlendTarget", (void *)h_nvnBlendStateSetBlendTarget},
        {"nvnBlendStateSetBlendFunc", (void *)h_nvnBlendStateSetBlendFunc},
        {"nvnBlendStateSetBlendEquation", (void *)h_nvnBlendStateSetBlendEquation},
        {"nvnColorStateSetDefaults", (void *)h_nvnColorStateSetDefaults},
        {"nvnColorStateSetBlendEnable", (void *)h_nvnColorStateSetBlendEnable},
        {"nvnChannelMaskStateSetDefaults", (void *)h_nvnChannelMaskStateSetDefaults},
        {"nvnChannelMaskStateSetChannelMask", (void *)h_nvnChannelMaskStateSetChannelMask},
        {"nvnMultisampleStateSetDefaults", (void *)h_nvnMultisampleStateSetDefaults},
        {"nvnMultisampleStateSetMultisampleEnable", (void *)h_nvnMultisampleStateSetMultisampleEnable},
        {"nvnMultisampleStateSetSamples", (void *)h_nvnMultisampleStateSetSamples},
        {"nvnMultisampleStateSetAlphaToCoverageEnable", (void *)h_nvnMultisampleStateSetAlphaToCoverageEnable},
        {"nvnMultisampleStateSetSampleLocationsEnable", (void *)h_nvnMultisampleStateSetSampleLocationsEnable},
        {"nvnPolygonStateSetDefaults", (void *)h_nvnPolygonStateSetDefaults},
        {"nvnPolygonStateSetCullFace", (void *)h_nvnPolygonStateSetCullFace},
        {"nvnPolygonStateSetFrontFace", (void *)h_nvnPolygonStateSetFrontFace},
        {"nvnPolygonStateSetPolygonMode", (void *)h_nvnPolygonStateSetPolygonMode},
        {"nvnPolygonStateSetPolygonOffsetEnables", (void *)h_nvnPolygonStateSetPolygonOffsetEnables},
        {"nvnDepthStencilStateSetDefaults", (void *)h_nvnDepthStencilStateSetDefaults},
        {"nvnDepthStencilStateSetDepthTestEnable", (void *)h_nvnDepthStencilStateSetDepthTestEnable},
        {"nvnDepthStencilStateSetDepthWriteEnable", (void *)h_nvnDepthStencilStateSetDepthWriteEnable},
        {"nvnDepthStencilStateSetDepthFunc", (void *)h_nvnDepthStencilStateSetDepthFunc},
        {"nvnDepthStencilStateSetStencilTestEnable", (void *)h_nvnDepthStencilStateSetStencilTestEnable},
        {"nvnDepthStencilStateSetStencilFunc", (void *)h_nvnDepthStencilStateSetStencilFunc},
        {"nvnDepthStencilStateSetStencilOp", (void *)h_nvnDepthStencilStateSetStencilOp},
        {"nvnVertexAttribStateSetDefaults", (void *)h_nvnVertexAttribStateSetDefaults},
        {"nvnVertexAttribStateSetFormat", (void *)h_nvnVertexAttribStateSetFormat},
        {"nvnVertexAttribStateSetStreamIndex", (void *)h_nvnVertexAttribStateSetStreamIndex},
        {"nvnVertexStreamStateSetDefaults", (void *)h_nvnVertexStreamStateSetDefaults},
        {"nvnVertexStreamStateSetStride", (void *)h_nvnVertexStreamStateSetStride},
        {"nvnVertexStreamStateSetDivisor", (void *)h_nvnVertexStreamStateSetDivisor},
        {"nvnMemoryPoolBuilderSetDefaults", (void *)h_nvnMemoryPoolBuilderSetDefaults},
        {"nvnMemoryPoolBuilderSetDevice", (void *)h_nvnMemoryPoolBuilderSetDevice},
        {"nvnMemoryPoolBuilderSetStorage", (void *)h_nvnMemoryPoolBuilderSetStorage},
        {"nvnMemoryPoolBuilderSetFlags", (void *)h_nvnMemoryPoolBuilderSetFlags},
        {"nvnMemoryPoolInitialize", (void *)h_nvnMemoryPoolInitialize},
        {"nvnMemoryPoolFinalize", (void *)h_nvnMemoryPoolFinalize},
        {"nvnMemoryPoolMap", (void *)h_nvnMemoryPoolMap},
        {"nvnMemoryPoolFlushMappedRange", (void *)h_nvnMemoryPoolFlushMappedRange},
        {"nvnMemoryPoolInvalidateMappedRange", (void *)h_nvnMemoryPoolInvalidateMappedRange},
        {"nvnMemoryPoolGetBufferAddress", (void *)h_nvnMemoryPoolGetBufferAddress},
        {"nvnMemoryPoolGetSize", (void *)h_nvnMemoryPoolGetSize},
        {"nvnMemoryPoolGetFlags", (void *)h_nvnMemoryPoolGetFlags},
        {"nvnMemoryPoolBuilderGetSize", (void *)h_nvnMemoryPoolBuilderGetSize},
        {"nvnMemoryPoolBuilderGetFlags", (void *)h_nvnMemoryPoolBuilderGetFlags},
        {"nvnBufferBuilderSetDefaults", (void *)h_nvnBufferBuilderSetDefaults},
        {"nvnBufferBuilderSetDevice", (void *)h_nvnBufferBuilderSetDevice},
        {"nvnBufferBuilderSetStorage", (void *)h_nvnBufferBuilderSetStorage},
        {"nvnBufferInitialize", (void *)h_nvnBufferInitialize},
        {"nvnBufferFinalize", (void *)h_nvnBufferFinalize},
        {"nvnBufferMap", (void *)h_nvnBufferMap},
        {"nvnBufferGetAddress", (void *)h_nvnBufferGetAddress},
        {"nvnBufferFlushMappedRange", (void *)h_nvnBufferFlushMappedRange},
        {"nvnBufferInvalidateMappedRange", (void *)h_nvnBufferInvalidateMappedRange},
        {"nvnBufferGetMemoryPool", (void *)h_nvnBufferGetMemoryPool},
        {"nvnBufferGetMemoryOffset", (void *)h_nvnBufferGetMemoryOffset},
        {"nvnBufferGetSize", (void *)h_nvnBufferGetSize},
        {"nvnBufferBuilderGetMemoryOffset", (void *)h_nvnBufferBuilderGetMemoryOffset},
        {"nvnBufferBuilderGetSize", (void *)h_nvnBufferBuilderGetSize},
        {"nvnTextureBuilderSetDefaults", (void *)h_nvnTextureBuilderSetDefaults},
        {"nvnTextureReadTexels", (void *)h_nvnTextureReadTexels},
        {"nvnTextureReadTexelsStrided", (void *)h_nvnTextureReadTexelsStrided},
        {"nvnTextureInvalidateTexels", (void *)h_nvnTextureInvalidateTexels},
        {"nvnTextureBuilderSetDevice", (void *)h_nvnTextureBuilderSetDevice},
        {"nvnTextureBuilderSetFlags", (void *)h_nvnTextureBuilderSetFlags},
        {"nvnTextureBuilderSetTarget", (void *)h_nvnTextureBuilderSetTarget},
        {"nvnTextureBuilderSetWidth", (void *)h_nvnTextureBuilderSetWidth},
        {"nvnTextureBuilderSetHeight", (void *)h_nvnTextureBuilderSetHeight},
        {"nvnTextureBuilderSetDepth", (void *)h_nvnTextureBuilderSetDepth},
        {"nvnTextureBuilderSetSize1D", (void *)h_nvnTextureBuilderSetSize1D},
        {"nvnTextureBuilderSetLevels", (void *)h_nvnTextureBuilderSetLevels},
        {"nvnTextureBuilderSetFormat", (void *)h_nvnTextureBuilderSetFormat},
        {"nvnTextureBuilderSetSamples", (void *)h_nvnTextureBuilderSetSamples},
        {"nvnTextureBuilderSetStride", (void *)h_nvnTextureBuilderSetStride},
        {"nvnTextureBuilderSetStorage", (void *)h_nvnTextureBuilderSetStorage},
        {"nvnTextureBuilderSetPackagedTextureLayout", (void *)h_nvnTextureBuilderSetPackagedTextureLayout},
        {"nvnTextureBuilderGetFlags", (void *)h_nvnTextureBuilderGetFlags},
        {"nvnTextureBuilderGetStorageSize", (void *)h_nvnTextureBuilderGetStorageSize},
        {"nvnTextureBuilderGetStorageAlignment", (void *)h_nvnTextureBuilderGetStorageAlignment},
        {"nvnTextureInitialize", (void *)h_nvnTextureInitialize},
        {"nvnTextureFinalize", (void *)h_nvnTextureFinalize},
        {"nvnTextureGetFlags", (void *)h_nvnTextureGetFlags},
        {"nvnTextureGetWidth", (void *)h_nvnTextureGetWidth},
        {"nvnTextureGetHeight", (void *)h_nvnTextureGetHeight},
        {"nvnTextureGetDepth", (void *)h_nvnTextureGetDepth},
        {"nvnTextureGetStorageSize", (void *)h_nvnTextureGetStorageSize},
        {"nvnTextureCompare", (void *)h_nvnTextureCompare},
        {"nvnTextureViewSetDefaults", (void *)h_nvnTextureViewSetDefaults},
        {"nvnTextureViewSetLevels", (void *)h_nvnTextureViewSetLevels},
        {"nvnTextureViewSetLayers", (void *)h_nvnTextureViewSetLayers},
        {"nvnTextureViewSetFormat", (void *)h_nvnTextureViewSetFormat},
        {"nvnTextureViewSetTarget", (void *)h_nvnTextureViewSetTarget},
        {"nvnTextureViewGetLayers", (void *)h_nvnTextureViewGetLayers},
        {"nvnTextureGetViewOffset", (void *)h_nvnTextureGetViewOffset},
        {"nvnSamplerBuilderSetDefaults", (void *)h_nvnSamplerBuilderSetDefaults},
        {"nvnSamplerBuilderSetDevice", (void *)h_nvnSamplerBuilderSetDevice},
        {"nvnSamplerBuilderSetMinMagFilter", (void *)h_nvnSamplerBuilderSetMinMagFilter},
        {"nvnSamplerBuilderSetWrapMode", (void *)h_nvnSamplerBuilderSetWrapMode},
        {"nvnSamplerBuilderSetLodClamp", (void *)h_nvnSamplerBuilderSetLodClamp},
        {"nvnSamplerBuilderSetLodBias", (void *)h_nvnSamplerBuilderSetLodBias},
        {"nvnSamplerBuilderSetCompare", (void *)h_nvnSamplerBuilderSetCompare},
        {"nvnSamplerBuilderSetBorderColor", (void *)h_nvnSamplerBuilderSetBorderColor},
        {"nvnSamplerBuilderSetMaxAnisotropy", (void *)h_nvnSamplerBuilderSetMaxAnisotropy},
        {"nvnSamplerBuilderGetMinMagFilter", (void *)h_nvnSamplerBuilderGetMinMagFilter},
        {"nvnSamplerBuilderGetWrapMode", (void *)h_nvnSamplerBuilderGetWrapMode},
        {"nvnSamplerBuilderSetLodSnap", (void *)h_nvnSamplerBuilderSetLodSnap},
        {"nvnSamplerBuilderSetReductionFilter", (void *)h_nvnSamplerBuilderSetReductionFilter},
        {"nvnSamplerBuilderGetLodBias", (void *)h_nvnSamplerBuilderGetLodBias},
        {"nvnSamplerBuilderGetMaxAnisotropy", (void *)h_nvnSamplerBuilderGetMaxAnisotropy},
        {"nvnSamplerBuilderGetReductionFilter", (void *)h_nvnSamplerBuilderGetReductionFilter},
        {"nvnSamplerBuilderGetLodSnap", (void *)h_nvnSamplerBuilderGetLodSnap},
        {"nvnSamplerBuilderGetLodClamp", (void *)h_nvnSamplerBuilderGetLodClamp},
        {"nvnSamplerBuilderGetCompare", (void *)h_nvnSamplerBuilderGetCompare},
        {"nvnSamplerBuilderGetBorderColor", (void *)h_nvnSamplerBuilderGetBorderColor},
        {"nvnSamplerGetLodBias", (void *)h_nvnSamplerGetLodBias},
        {"nvnSamplerGetMaxAnisotropy", (void *)h_nvnSamplerGetMaxAnisotropy},
        {"nvnSamplerGetReductionFilter", (void *)h_nvnSamplerGetReductionFilter},
        {"nvnSamplerGetLodClamp", (void *)h_nvnSamplerGetLodClamp},
        {"nvnSamplerGetCompare", (void *)h_nvnSamplerGetCompare},
        {"nvnSamplerGetBorderColor", (void *)h_nvnSamplerGetBorderColor},
        {"nvnSamplerInitialize", (void *)h_nvnSamplerInitialize},
        {"nvnSamplerFinalize", (void *)h_nvnSamplerFinalize},
        {"nvnProgramInitialize", (void *)h_nvnProgramInitialize},
        {"nvnProgramFinalize", (void *)h_nvnProgramFinalize},
        {"nvnProgramSetShaders", (void *)h_nvnProgramSetShaders},
        {"nvnCommandBufferSetShaderScratchMemory", (void *)h_nvnCommandBufferSetShaderScratchMemory},
        {"nvnCommandBufferInitialize", (void *)h_nvnCommandBufferInitialize},
        {"nvnCommandBufferFinalize", (void *)h_nvnCommandBufferFinalize},
        {"nvnCommandBufferSetMemoryCallback", (void *)h_nvnCommandBufferSetMemoryCallback},
        {"nvnCommandBufferSetMemoryCallbackData", (void *)h_nvnCommandBufferSetMemoryCallbackData},
        {"nvnCommandBufferAddCommandMemory", (void *)h_nvnCommandBufferAddCommandMemory},
        {"nvnCommandBufferAddControlMemory", (void *)h_nvnCommandBufferAddControlMemory},
        {"nvnCommandBufferBeginRecording", (void *)h_nvnCommandBufferBeginRecording},
        {"nvnCommandBufferEndRecording", (void *)h_nvnCommandBufferEndRecording},
        {"nvnCommandBufferIsRecording", (void *)h_nvnCommandBufferIsRecording},
        {"nvnCommandBufferCopyCommands", (void *)h_nvnCommandBufferCopyCommands},
        {"nvnCommandBufferSignalEvent", (void *)h_nvnCommandBufferSignalEvent},
        {"nvnCommandBufferResetCounter", (void *)h_nvnCommandBufferResetCounter},
        {"nvnCommandBufferReportCounter", (void *)h_nvnCommandBufferReportCounter},
        {"nvnCommandBufferCopyBufferToBuffer", (void *)h_nvnCommandBufferCopyBufferToBuffer},
        {"nvnCommandBufferUpdateUniformBuffer", (void *)h_nvnCommandBufferUpdateUniformBuffer},
        {"nvnCommandBufferSetRenderTargets", (void *)h_nvnCommandBufferSetRenderTargets},
        {"nvnCommandBufferClearColor", (void *)h_nvnCommandBufferClearColor},
        {"nvnCommandBufferClearDepthStencil", (void *)h_nvnCommandBufferClearDepthStencil},
        {"nvnCommandBufferSetViewports", (void *)h_nvnCommandBufferSetViewports},
        {"nvnCommandBufferSetScissor", (void *)h_nvnCommandBufferSetScissor},
        {"nvnCommandBufferSetDepthRange", (void *)h_nvnCommandBufferSetDepthRange},
        {"nvnCommandBufferBindProgram", (void *)h_nvnCommandBufferBindProgram},
        {"nvnCommandBufferBindBlendState", (void *)h_nvnCommandBufferBindBlendState},
        {"nvnCommandBufferBindColorState", (void *)h_nvnCommandBufferBindColorState},
        {"nvnCommandBufferBindChannelMaskState", (void *)h_nvnCommandBufferBindChannelMaskState},
        {"nvnCommandBufferBindMultisampleState", (void *)h_nvnCommandBufferBindMultisampleState},
        {"nvnCommandBufferBindPolygonState", (void *)h_nvnCommandBufferBindPolygonState},
        {"nvnCommandBufferBindDepthStencilState", (void *)h_nvnCommandBufferBindDepthStencilState},
        {"nvnCommandBufferBindVertexAttribState", (void *)h_nvnCommandBufferBindVertexAttribState},
        {"nvnCommandBufferBindVertexStreamState", (void *)h_nvnCommandBufferBindVertexStreamState},
        {"nvnCommandBufferBindVertexBuffers", (void *)h_nvnCommandBufferBindVertexBuffers},
        {"nvnCommandBufferBindUniformBuffer", (void *)h_nvnCommandBufferBindUniformBuffer},
        {"nvnCommandBufferBindUniformBuffers", (void *)h_nvnCommandBufferBindUniformBuffers},
        {"nvnCommandBufferBindStorageBuffer", (void *)h_nvnCommandBufferBindStorageBuffer},
        {"nvnCommandBufferBindStorageBuffers", (void *)h_nvnCommandBufferBindStorageBuffers},
        {"nvnCommandBufferBindImages", (void *)h_nvnCommandBufferBindImages},
        {"nvnCommandBufferBindSeparateTextures", (void *)h_nvnCommandBufferBindSeparateTextures},
        {"nvnCommandBufferBindSeparateSamplers", (void *)h_nvnCommandBufferBindSeparateSamplers},
        {"nvnCommandBufferBindSeparateSampler", (void *)h_nvnCommandBufferBindSeparateSampler},
        {"nvnCommandBufferSetStencilValueMask", (void *)h_nvnCommandBufferSetStencilValueMask},
        {"nvnCommandBufferSetStencilMask", (void *)h_nvnCommandBufferSetStencilMask},
        {"nvnCommandBufferSetStencilRef", (void *)h_nvnCommandBufferSetStencilRef},
        {"nvnCommandBufferSetPolygonOffsetClamp", (void *)h_nvnCommandBufferSetPolygonOffsetClamp},
        {"nvnCommandBufferDrawArrays", (void *)h_nvnCommandBufferDrawArrays},
        {"nvnCommandBufferDrawElementsInstanced", (void *)h_nvnCommandBufferDrawElementsInstanced},
        {"nvnCommandBufferDispatchCompute", (void *)h_nvnCommandBufferDispatchCompute},
        {"nvnCommandBufferBarrier", (void *)h_nvnCommandBufferBarrier},
        {"nvnCommandBufferSetTexturePool", (void *)h_nvnCommandBufferSetTexturePool},
        {"nvnCommandBufferSetSamplerPool", (void *)h_nvnCommandBufferSetSamplerPool},
        {"nvnCommandBufferCopyBufferToTexture", (void *)h_nvnCommandBufferCopyBufferToTexture},
        {"nvnCommandBufferCopyTextureToBuffer", (void *)h_nvnCommandBufferCopyTextureToBuffer},
        {"nvnCommandBufferClearTexture", (void *)h_nvnCommandBufferClearTexture},
        {"nvnCommandBufferClearTextureui", (void *)h_nvnCommandBufferClearTextureui},
        {"nvnCommandBufferCopyTextureToTexture", (void *)h_nvnCommandBufferCopyTextureToTexture},
        {"nvnCommandBufferSetCopyRowStride", (void *)h_nvnCommandBufferSetCopyRowStride},
        {"nvnCommandBufferSetBlendColor", (void *)h_nvnCommandBufferSetBlendColor},
        {"nvnTexturePoolInitialize", (void *)h_nvnTexturePoolInitialize},
        {"nvnSamplerPoolInitialize", (void *)h_nvnSamplerPoolInitialize},
        {"nvnTexturePoolFinalize", (void *)h_nvnTexturePoolFinalize},
        {"nvnSamplerPoolFinalize", (void *)h_nvnSamplerPoolFinalize},
        {"nvnTexturePoolRegisterTexture", (void *)h_nvnTexturePoolRegisterTexture},
        {"nvnSamplerPoolRegisterSampler", (void *)h_nvnSamplerPoolRegisterSampler},
        {"nvnSyncInitialize", (void *)h_nvnSyncInitialize},
        {"nvnSyncFinalize", (void *)h_nvnSyncFinalize},
        {"nvnQueueFenceSync", (void *)h_nvnQueueFenceSync},
        {"nvnQueueWaitSync", (void *)h_nvnQueueWaitSync},
        {"nvnCommandBufferWaitSync", (void *)h_nvnCommandBufferWaitSync},
        {"nvnCommandBufferWaitEvent", (void *)h_nvnCommandBufferWaitEvent},
        {"nvnCommandBufferFenceSync", (void *)h_nvnCommandBufferFenceSync},
        {"nvnSyncWait", (void *)h_nvnSyncWait},
        {"nvnEventBuilderSetDefaults", (void *)h_nvnEventBuilderSetDefaults},
        {"nvnEventBuilderSetStorage", (void *)h_nvnEventBuilderSetStorage},
        {"nvnEventInitialize", (void *)h_nvnEventInitialize},
        {"nvnEventFinalize", (void *)h_nvnEventFinalize},
        {"nvnEventGetValue", (void *)h_nvnEventGetValue},
        {"nvnEventSignal", (void *)h_nvnEventSignal},
        {"nvnWindowBuilderSetDefaults", (void *)h_nvnWindowBuilderSetDefaults},
        {"nvnWindowBuilderSetDevice", (void *)h_nvnWindowBuilderSetDevice},
        {"nvnWindowBuilderSetNativeWindow", (void *)h_nvnWindowBuilderSetNativeWindow},
        {"nvnWindowBuilderSetTextures", (void *)h_nvnWindowBuilderSetTextures},
        {"nvnWindowBuilderSetPresentInterval", (void *)h_nvnWindowBuilderSetPresentInterval},
        {"nvnWindowInitialize", (void *)h_nvnWindowInitialize},
        {"nvnWindowSetPresentInterval", (void *)h_nvnWindowSetPresentInterval},
        {"nvnWindowSetCrop", (void *)h_nvnWindowSetCrop},
        {"nvnWindowFinalize", (void *)h_nvnWindowFinalize},
        {"nvnWindowAcquireTexture", (void *)h_nvnWindowAcquireTexture},
        {"nvnQueuePresentTexture", (void *)h_nvnQueuePresentTexture},
        {"nvnQueueSubmitCommands", (void *)h_nvnQueueSubmitCommands},
    };
    for(unsigned i=0;i<sizeof impl/sizeof impl[0];i++)
        if(!strcmp(name,impl[i].name)) return impl[i].fn;
    for(unsigned i=0;i<sizeof missing/sizeof missing[0];i++)
        if(!strcmp(name,missing[i].name)) return missing[i].fn;
    LOG("Unknown NVN lookup: %s",name);
    return NULL;
}
void *dm_nvn_lookup(const char *name) { return nvn_lookup(name); }
typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *nvn_force_lookup(const char *name) {
    static const hle_ent e={"nvnBootstrapLoader",(void *)dm_nvn_lookup};
    return !strcmp(name,e.name) ? &e : NULL;
}
