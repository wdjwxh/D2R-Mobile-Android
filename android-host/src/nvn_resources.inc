// Portable NVN metadata, adapted from upstream 944e199. Vulkan resources are lazy;
// draw/readback remain explicit boundaries until resource consumption is implemented.
static _Atomic uint64_t next_serial=1;
static uint64_t nvn_new_serial(void) { uint64_t serial=atomic_fetch_add(&next_serial,1);if(!serial)die("NVN serial exhausted");return serial; }
#define TB(b) ((tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = f; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
static uint64_t g_layout_logged;
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("[nvn] 预排布贴图 %ux%ux%u 级%u 格式%#x 目标%d 布局字节 %02x%02x%02x%02x%02x%02x%02x%02x → 块高 log2=%u（取布局第 0 字节）尺寸 %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, (unsigned long long)L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) LOG("【红】TextureBuilderGetStorageSize 未知格式 %#x", ((const tex_builder_t *)b)->d.format);
    log_layout((const tex_builder_t *)b, &L);
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L); return L.align;
}
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = (const tex_builder_t *)b; tex_t *o = TX(t);
    nvn_texlayout layout;
    if(s->dev!=live_device || !live_device || !s->d.width || !s->d.height || !s->d.depth ||
       !s->d.levels || s->d.levels>16 ||
       (s->d.target!=NVN_TEXTURE_TARGET_BUFFER && s->d.width>16384) || s->d.height>16384 || s->d.depth>2048) {
        LOG("TextureInitialize rejected geometry: object=%p dev=%p target=%d size=%ux%ux%u levels=%u fmt=%#x",
            (void *)t,(void *)s->dev,s->d.target,s->d.width,s->d.height,s->d.depth,s->d.levels,s->d.format);
        return 0;
    }
    nvn_tex_layout(&s->d,&layout);
    if(!layout.bpb || !pool_range(s->pool,s->off,layout.size)) {
        LOG("TextureInitialize rejected storage: object=%p target=%d fmt=%#x bpb=%u offset=%lld size=%llu pool=%p pool_size=%llu",
            (void *)t,s->d.target,s->d.format,layout.bpb,(long long)s->off,(unsigned long long)layout.size,
            (void *)s->pool,(unsigned long long)(s->pool?s->pool->size:0));
        return 0;
    }
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8);
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    o->serial = nvn_new_serial();
    OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, (unsigned long long)o->L.size);
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture *t) {
    nvn_translate_drain();
    tex_t *o=TX(t);
    snap_drop(&o->snap,o->serial,offsetof(tex_t,serial));
    if(o->magic==MAGIC('T','E','X',0) && dm_vk_texture_retire(o->serial))die("Texture retire failed");
    o->magic=0;
}
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->L.size; }
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem;
}
// 头文件无原型；main 0x174899c 一类调用点把返回值与 1 比，不等则置某状态位 0x100。含义未核，先给 1
// No prototype in the header; call sites such as main 0x174899c compare the return value with 1 and set some status bit 0x100 if not equal. Meaning unverified; return 1 for now
#define VW(v) ((view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = t; }
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) {
    const view_t *o = (const view_t *)v; *min = o->min_layer; *n = o->nlayers; return o->set_layers;
}
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t); const view_t *w = (const view_t *)v;
    if (!w) return 0;
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * o->L.layer_size;
    return (ptrdiff_t)off;
}

#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;   // GL 默认 | GL defaults
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f; o->lod_max = 1000.f; o->aniso = 1.f;
}
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { ((samp_builder_t *)b)->dev = d; }
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { SB(b)->minf = mi; SB(b)->magf = ma; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->ws = s; SB(b)->wt = t; SB(b)->wr = r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float lo, float hi) { SB(b)->lod_min = lo; SB(b)->lod_max = hi; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->cmp_mode = m; SB(b)->cmp_func = f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->border, c, 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->aniso = v; }
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    if(((const samp_builder_t *)b)->dev!=live_device || !live_device) return 0;
    samp_t *o = (samp_t *)s; o->magic = MAGIC('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p; return 1;
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }

// ---------- 程序 ----------
// ---------- Programs ----------
NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    if(d!=live_device || !live_device) return 0; memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC('P', 'R', 'O', 'G'); return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram *p) { prog_t *o=(prog_t *)p;snap_drop(&o->snap,o->serial,offsetof(prog_t,serial));o->magic=0; }
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { LOG("【红】ProgramSetShaders 阶段数 %d", n); return 0; }
    snap_drop(&o->snap,o->serial,offsetof(prog_t,serial));
    o->nstage = n; memcpy(o->sd, sd, n * sizeof *sd); o->serial = nvn_new_serial();
    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    return 1;
}


// Retain optional sampler parameters for Vulkan consumption. Upstream Metal
// ignores LodSnap; retaining it here avoids silently losing the declaration.
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_snap=v; }
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction v) { SB(b)->reduction=v; }
NVN(nvnSamplerBuilderGetLodBias, float, const NVNsamplerBuilder *b) { return (float)(((const samp_builder_t *)b)->p).lod_bias; }
NVN(nvnSamplerBuilderGetMaxAnisotropy, float, const NVNsamplerBuilder *b) { return (float)(((const samp_builder_t *)b)->p).aniso; }
NVN(nvnSamplerBuilderGetReductionFilter, NVNsamplerReduction, const NVNsamplerBuilder *b) { return (NVNsamplerReduction)(((const samp_builder_t *)b)->p).reduction; }
NVN(nvnSamplerBuilderGetLodSnap, float, const NVNsamplerBuilder *b) { return (float)(((const samp_builder_t *)b)->p).lod_snap; }
NVN(nvnSamplerBuilderGetLodClamp, void, const NVNsamplerBuilder *b, float *lo, float *hi) { const samp_par *p=&(((const samp_builder_t *)b)->p); *lo=p->lod_min; *hi=p->lod_max; }
NVN(nvnSamplerBuilderGetCompare, void, const NVNsamplerBuilder *b, NVNcompareMode *mode, NVNcompareFunc *fn) { const samp_par *p=&(((const samp_builder_t *)b)->p); *mode=(NVNcompareMode)p->cmp_mode; *fn=(NVNcompareFunc)p->cmp_func; }
NVN(nvnSamplerBuilderGetBorderColor, void, const NVNsamplerBuilder *b, float *out) { memcpy(out,(((const samp_builder_t *)b)->p).border,16); }
NVN(nvnSamplerGetLodBias, float, const NVNsampler *b) { return (float)(((const samp_t *)b)->p).lod_bias; }
NVN(nvnSamplerGetMaxAnisotropy, float, const NVNsampler *b) { return (float)(((const samp_t *)b)->p).aniso; }
NVN(nvnSamplerGetReductionFilter, NVNsamplerReduction, const NVNsampler *b) { return (NVNsamplerReduction)(((const samp_t *)b)->p).reduction; }
NVN(nvnSamplerGetLodClamp, void, const NVNsampler *b, float *lo, float *hi) { const samp_par *p=&(((const samp_t *)b)->p); *lo=p->lod_min; *hi=p->lod_max; }
NVN(nvnSamplerGetCompare, void, const NVNsampler *b, NVNcompareMode *mode, NVNcompareFunc *fn) { const samp_par *p=&(((const samp_t *)b)->p); *mode=(NVNcompareMode)p->cmp_mode; *fn=(NVNcompareFunc)p->cmp_func; }
NVN(nvnSamplerGetBorderColor, void, const NVNsampler *b, float *out) { memcpy(out,(((const samp_t *)b)->p).border,16); }
