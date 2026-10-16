// CPU descriptor registry, mirroring upstream's registration before lazy GPU use.
// Key by pool storage identity, so copying a guest descriptor-pool object works.
typedef struct { tex_t tex; view_t view; uint8_t present,has_view; } texture_slot;
typedef struct { samp_par sampler; uint8_t present; } sampler_slot;
typedef struct descriptors {
    struct descriptors *next; const pool_t *pool; int64_t off; int32_t n,kind;
    union { texture_slot *textures; sampler_slot *samplers; void *data; } slots;
} descriptors;
static descriptors *descriptor_pools;
static pthread_mutex_t descriptor_lock=PTHREAD_MUTEX_INITIALIZER;
// CPU argument caches must see re-registration even when all NVN binding
// handles are unchanged. Mutations publish under descriptor_lock.
static _Atomic uint64_t descriptor_generation=1;
static void descriptor_changed(void) {
    atomic_fetch_add_explicit(&descriptor_generation,1,memory_order_release);
}
static void descriptors_close(void) {
    pthread_mutex_lock(&descriptor_lock);
    while(descriptor_pools) {
        descriptors *p=descriptor_pools;descriptor_pools=p->next;
        free(p->slots.data);free(p);
    }
    descriptor_changed();pthread_mutex_unlock(&descriptor_lock);
}
static descriptors *descriptor_find(const dpool_t *p,int kind) {
    for(descriptors *it=descriptor_pools;it;it=it->next)
        if(it->pool==p->pool && it->off==p->off && it->n==p->n && it->kind==kind) return it;
    return NULL;
}
static NVNboolean descriptor_init(dpool_t *out,const pool_t *pool,ptrdiff_t off,int n,int kind) {
    nvn_translate_drain();
    // Texture handles have 20 index bits; only sampler indices have 12.
    if(n<0 || n>(kind?4096:1048576) || !pool_range(pool,off,(size_t)n*32)) {
        LOG("NVN descriptor pool rejected: kind=%d count=%d offset=%lld",kind,n,(long long)off);
        return 0;
    }
    dpool_t key={pool,off,n};
    pthread_mutex_lock(&descriptor_lock);
    if(descriptor_find(&key,kind)) { pthread_mutex_unlock(&descriptor_lock);return 0; }
    descriptors *p=calloc(1,sizeof *p);
    if(p) p->slots.data=calloc(n?n:1,kind?sizeof(sampler_slot):sizeof(texture_slot));
    if(!p || !p->slots.data) { free(p);pthread_mutex_unlock(&descriptor_lock);return 0; }
    p->pool=pool;p->off=off;p->n=n;p->kind=kind;p->next=descriptor_pools;descriptor_pools=p;
    *out=key;descriptor_changed();pthread_mutex_unlock(&descriptor_lock);
    LOG("NVN descriptor pool initialized: kind=%d count=%d",kind,n);return 1;
}
static void descriptor_fini(dpool_t *key,int kind) {
    nvn_translate_drain();
    pthread_mutex_lock(&descriptor_lock);
    descriptors **p=&descriptor_pools;
    while(*p && !((*p)->pool==key->pool && (*p)->off==key->off && (*p)->kind==kind)) p=&(*p)->next;
    if(*p) { descriptors *old=*p;*p=old->next;free(old->slots.data);free(old); }
    memset(key,0,sizeof *key);descriptor_changed();pthread_mutex_unlock(&descriptor_lock);
}
NVN(nvnTexturePoolInitialize,NVNboolean,NVNtexturePool *tp,const NVNmemoryPool *p,ptrdiff_t off,int n) { return descriptor_init((dpool_t *)tp,(const pool_t *)p,off,n,0); }
NVN(nvnSamplerPoolInitialize,NVNboolean,NVNsamplerPool *sp,const NVNmemoryPool *p,ptrdiff_t off,int n) { return descriptor_init((dpool_t *)sp,(const pool_t *)p,off,n,1); }
NVN(nvnTexturePoolFinalize,void,NVNtexturePool *tp) { descriptor_fini((dpool_t *)tp,0); }
NVN(nvnSamplerPoolFinalize,void,NVNsamplerPool *sp) { descriptor_fini((dpool_t *)sp,1); }
NVN(nvnTexturePoolRegisterTexture,void,const NVNtexturePool *tp,int id,const NVNtexture *t,const NVNtextureView *v) {
    nvn_translate_drain();
    pthread_mutex_lock(&descriptor_lock);descriptors *p=descriptor_find((const dpool_t *)tp,0);
    if(!p || id<0 || id>=p->n || (t && ((const tex_t *)t)->magic!=MAGIC('T','E','X',0))) {
        LOG("NVN descriptor failure: registry=%p id=%d capacity=%d texture=%p",(void *)p,id,p?p->n:0,(void *)t);
        if(t) LOG("NVN descriptor texture magic=%#x caller=%p",((const tex_t *)t)->magic,__builtin_return_address(0));
        die("Invalid NVN texture descriptor");
    }
    texture_slot value={0},*s=&value;
    // Upstream mtl_reg_tex explicitly supports NULL to clear a slot.
    if(t) { s->tex=*(const tex_t *)t;s->tex.snap=NULL; }
    s->present=t!=NULL;s->has_view=v!=NULL;
    if(v) s->view=*(const view_t *)v;
    if(memcmp(&p->slots.textures[id],s,sizeof *s)) {
        p->slots.textures[id]=*s;descriptor_changed();
    }
    pthread_mutex_unlock(&descriptor_lock);
}
NVN(nvnSamplerPoolRegisterSampler,void,const NVNsamplerPool *sp,int id,const NVNsampler *s) {
    nvn_translate_drain();
    pthread_mutex_lock(&descriptor_lock);descriptors *p=descriptor_find((const dpool_t *)sp,1);
    if(!p || id<0 || id>=p->n || !s || ((const samp_t *)s)->magic!=MAGIC('S','M','P',0)) die("Invalid NVN sampler descriptor");
    sampler_slot value={0};value.sampler=((const samp_t *)s)->p;value.present=1;
    if(memcmp(&p->slots.samplers[id],&value,sizeof value)) {
        p->slots.samplers[id]=value;descriptor_changed();
    }
    pthread_mutex_unlock(&descriptor_lock);
}
