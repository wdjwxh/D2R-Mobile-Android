// Upstream window objects, Android swapchain. Present requires actual texture replay.
#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) { NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1; }
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0 || n > 4) { LOG("【红】WindowBuilderSetTextures 数量 %d", n); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    if(s->dev!=live_device || !live_device || !s->native || s->ntex<1 || s->ntex>4) return 0;
    for(int i=0;i<s->ntex;i++) if(!s->tex[i] || CTX(s->tex[i])->magic!=MAGIC('T','E','X',0)) return 0;
    nvn_translate_drain();
    if(dm_vk_prepare_window()) return 0;
    memset(o, 0, sizeof *o); o->magic = MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    LOG("[nvn] WindowInitialize 交换链 %d 张 间隔 %d 首张 %dx%d 格式 %#x", o->ntex, o->interval,
        o->ntex ? (int)CTX(o->tex[0])->d.width : 0, o->ntex ? (int)CTX(o->tex[0])->d.height : 0, o->ntex ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int v) { if (((win_t *)w)->interval != v) LOG("[nvn] 游戏设出图间隔 %d → %d", ((win_t *)w)->interval, v); ((win_t *)w)->interval = v; }
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    win_t *o = (win_t *)w; int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) LOG("[nvn] WindowCrop %d,%d %dx%d", x, y, ww, hh);
    memcpy(o->crop, v, sizeof v);
}

NVN(nvnWindowFinalize,void,NVNwindow *w) { nvn_translate_drain();if(dm_vk_finish()) die("Window finalize queue failure");memset(w,0,sizeof(win_t)); }
NVN(nvnWindowAcquireTexture,NVNwindowAcquireTextureResult,NVNwindow *w,NVNsync *sync,int *idx) {
    win_t *o=(win_t *)w;sync_t *s=(sync_t *)sync;
    if(o->magic!=MAGIC('W','I','N','D') || !o->ntex || !idx || !s || s->magic!=MAGIC('S','Y','N','C')) die("Invalid NVN window acquire");
    // Wait only for this texture's previous queued presentation. The Vulkan
    // presenter owns swapchain completion; acquire requests no CPU readback.
    x_present_wait(o->ticket[o->next]);
    *idx=o->next;o->next=(o->next+1)%o->ntex;
    uint64_t generation=sync_begin(s);
    sync_publish(s,generation,NULL);
    xjob ready={.kind=XJ_SYNC_READY,.sync=s,.generation=generation};
    (void)x_push(&ready);
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN(nvnQueuePresentTexture,void,NVNqueue *q,NVNwindow *w,int index) {
    win_t *window=(win_t *)w;
    if(((queue_t *)q)->magic!=MAGIC('Q','U','E','U') || window->magic!=MAGIC('W','I','N','D') || index<0 || index>=window->ntex)die("Invalid NVN present");
    const tex_t *texture=snap_tex(window->tex[index]);
    if(texture->magic!=MAGIC('T','E','X',0))die("Present texture has been finalized");
    dm_compute_image image={.desc=texture->d,.serial=texture->serial,.address=(uintptr_t)texture->mem,
        .packaged=texture->has_layout && texture->mem,
        .levels=texture->d.levels,.layers=texture->L.layers,.format=texture->d.format};
    xjob j={.kind=XJ_PRESENT,.image=image};
    memcpy(j.crop,window->crop,sizeof(j.crop));
    window->ticket[index]=x_push(&j);
    window->presents++;
    if(window->presents<=8 || !(window->presents%60))LOG("NVN guest present #%u serial=%llu crop=%d,%d %dx%d",(unsigned)window->presents,(unsigned long long)image.serial,window->crop[0],window->crop[1],window->crop[2],window->crop[3]);
}
