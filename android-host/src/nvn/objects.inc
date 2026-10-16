// Adapted from upstream 944e199: portable object and recording layouts.
#include "nvn_tex.h"
typedef struct { const pool_t *pool; int64_t off; int32_t n; } dpool_t;                       // 贴图 / 采样器描述符池 | Texture / sampler descriptor pool
FITS(dpool_t, NVNtexturePool); FITS(dpool_t, NVNsamplerPool);

typedef struct {
    NVNdevice *dev; nvn_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
} tex_builder_t;                                                                             FITS(tex_builder_t, NVNtextureBuilder);
typedef struct {
    uint32_t magic; nvn_texdesc d; nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off; uint8_t has_layout, layout[8];
    uint64_t serial;                     // 初始化序号：回放层按它在宿主表里找 MTLTexture（游戏可能按值拷贝对象，不能存对象指针） | Initialization serial: the replay layer uses it to find the MTLTexture in the host table (the game may copy the object by value, so object pointers cannot be stored)
    const void *snap; // Immutable record-time object snapshot.
} tex_t;                                                                                     FITS(tex_t, NVNtexture);
typedef struct { uint8_t set_levels, set_layers, set_format, set_target; int32_t base_level, nlevels, min_layer, nlayers, format, target; } view_t;
FITS(view_t, NVNtextureView);

// 采样器参数（builder 与对象共用一份）
// Sampler parameters (one copy shared by builder and object)
typedef struct { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction; float lod_min, lod_max, lod_bias, aniso, border[4], lod_snap; } samp_par;   // 枚举值都 < 256 | Enum values are all < 256
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                                FITS(samp_builder_t, NVNsamplerBuilder);
typedef struct { uint32_t magic; samp_par p; } samp_t;                                        FITS(samp_t, NVNsampler);

typedef struct { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; const void *snap; } prog_t;     FITS(prog_t, NVNprogram);   // serial：回放层解析结果的表键 | serial: table key for the replay layer's resolved result

typedef struct {
    uint32_t magic,fenced;uint64_t fence,generation,published;dm_vk_sync_ticket *ticket;
    // Queue-facing condition follows translation FIFO, independently of a
    // newer condition already issued by the guest CPU. NULL can mean ready.
    dm_vk_sync_ticket *worker_ticket;uint64_t worker_published;
} sync_t;
FITS(sync_t, NVNsync);
typedef struct { const pool_t *pool; int64_t off; } ev_builder_t;                             FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; } ev_t;                              FITS(ev_t, NVNevent);

typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval; NVNtexture *tex[4]; } win_builder_t;
FITS(win_builder_t, NVNwindowBuilder);
// avail[i]：交换链第 i 张可再次获取的时刻（ns，单调时钟）= 它的显示时刻 + 一个出图周期；pace：本窗口的出图节拍器目标时刻
// avail[i]: time at which swapchain image i can be acquired again (ns, monotonic clock) = its display time + one present period; pace: target time of this window's present pacer
typedef struct { uint32_t magic; void *native; int32_t ntex, interval, next, crop[4]; NVNtexture *tex[4]; uint64_t presents, avail[4], pace, ticket[4]; } win_t;   // ticket[i]：第 i 张最近一次出图的票号 | ticket[i]: ticket of the most recent present of image i
FITS(win_t, NVNwindow);

// 命令缓冲：命令写进游戏给的命令内存（AddCommandMemory），句柄 = 段首地址；用完回调游戏补内存
// Command buffer: commands are written into the command memory given by the game (AddCommandMemory), handle = segment start address; when it runs out, the game is called back for more memory
typedef struct {
    uint32_t magic, recording;
    uint8_t *cur, *end;                  // 当前命令内存段的写指针 / 段尾 | Write pointer / end of the current command memory segment
    uint8_t *begin;                      // 本次录制的 BEGIN 记录 | BEGIN record of this recording
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    NVNdevice *dev;
} cmd_t;                                                                                     FITS(cmd_t, NVNcommandBuffer);

typedef struct { uint32_t op, size; } op_hdr;
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,                 // CPU 可见副作用 | CPU-visible side effects
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_WAIT_SYNC, OP_FENCE_SYNC, OP_WAIT_EVENT, OP_COUNT_
};
// 载荷（紧跟 op_hdr）
// Payload (immediately follows op_hdr)
typedef struct { int32_t n; const tex_t *color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t *depth; view_t dview; uint8_t has_dview; } p_set_rt;
typedef struct { int32_t index, mask; float rgba[4]; } p_clear_color;
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } p_clear_ds;
typedef struct { int32_t first, n; float v[16][6]; } p_viewports;          // x, y, w, h, 近, 远（近远由 DEPTH_RANGE 覆盖） | x, y, w, h, near, far (near and far are overridden by DEPTH_RANGE)
typedef struct { int32_t x, y, w, h; } p_scissor;
typedef struct { float n, f; } p_depth_range;
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
typedef struct { int32_t kind; uint8_t bytes[0x18]; } p_bind_state;      // kind：0 blend 1 color 2 chmask 3 ms 4 poly 5 ds
typedef struct { int32_t first, n; uint32_t st[16]; } p_bind_vattr;
typedef struct { int32_t first, n; vs_st st[16]; } p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } p_bind_vbuf;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;  // UBO / SSBO
typedef struct { int32_t stage, first, n; uint64_t h[32]; } p_bind_handles; // 分离贴图 / 采样器句柄 | Separate texture / sampler handles
typedef struct { int32_t face, value_mask, mask, ref, which; } p_stencil;  // which：0 值掩码 1 写掩码 2 参考值 | which: 0 value mask 1 write mask 2 reference value
typedef struct { float factor, units, clamp; } p_poly_offset;
typedef struct { int32_t prim, first, count; } p_draw;
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf; } p_draw_idx;
typedef struct { int32_t x, y, z; uint64_t caller; } p_dispatch;   // caller：游戏调用点返回地址（定位派发者） | caller: return address of the game call site (to locate the dispatcher)
typedef struct { const dpool_t *pool; } p_set_pool;
typedef struct { uint64_t src; const tex_t *tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; } p_copy_b2t;
typedef struct { const tex_t *src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } p_copy_t2t;   // T2B 复用：src 存目标缓冲地址 | Reused for T2B: src stores the destination buffer address
typedef struct { const tex_t *tex; view_t view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; } p_clear_tex;
typedef struct { int64_t stride; } p_copy_stride;
typedef struct { float rgba[4]; } p_blend_color;
