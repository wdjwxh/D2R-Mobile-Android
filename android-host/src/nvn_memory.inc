// NVN memory pool semantics adapted from upstream; Vulkan owns backing memory.
typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;
typedef struct { uint32_t magic,flags; uint8_t *mem; uint64_t size; dm_vk_pool *backing; } pool_t;
typedef struct { NVNdevice *dev; NVNmemoryPool *pool; ptrdiff_t offset; size_t size; } buffer_builder_t;
typedef struct { uint32_t magic; NVNmemoryPool *pool; ptrdiff_t offset; size_t size; } buffer_t;
FITS(pool_builder_t,NVNmemoryPoolBuilder); FITS(pool_t,NVNmemoryPool);
FITS(buffer_builder_t,NVNbufferBuilder); FITS(buffer_t,NVNbuffer);
static int pool_range(const pool_t *p, ptrdiff_t off, size_t n) {
    return p && p->magic==MAGIC('P','O','O','L') && off>=0 && (uint64_t)off<=p->size && n<=p->size-(uint64_t)off;
}
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) { memset(b,0,sizeof(pool_builder_t)); }
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { ((pool_builder_t *)b)->dev=d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { ((pool_builder_t *)b)->mem=m; ((pool_builder_t *)b)->size=n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { ((pool_builder_t *)b)->flags=f; }
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s=(const pool_builder_t *)b;
    if(!live_device || s->dev!=live_device || !s->mem || !s->size) return 0;
    if(s->flags&0x300) { LOG("NVN physical/virtual pools require sparse mapping backend"); return 0; }
    if((uintptr_t)s->mem%4096 || s->size%4096) return 0;
    dm_vk_pool *backing=dm_vk_pool_create(s->mem,s->size,s->flags);
    if(!backing) return 0;
    *(pool_t *)p=(pool_t){MAGIC('P','O','O','L'),s->flags,s->mem,s->size,backing};
    LOG("NVN memory pool flags=%#x size=%zu",s->flags,(size_t)s->size); return 1;
}
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) {
    nvn_translate_drain();
    pool_t *o=(pool_t *)p;
    if(!pool_range(o,0,0) || dm_vk_pool_destroy(o->backing)) die("Invalid NVN pool finalize");
    memset(o,0,sizeof *o);
}
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) {
    nvn_translate_drain();
    const pool_t *o=(const pool_t *)p;
    if(!pool_range(o,0,0) || (o->flags&1))return NULL;
    if(dm_vk_pool_cpu_read(o->backing,0,o->size))die("NVN pool map visibility failed");
    return o->mem;
}
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    nvn_translate_drain();
    const pool_t *o=(const pool_t *)p;
    if(!pool_range(o,off,n) || dm_vk_pool_sync(o->backing,off,n,0)) die("NVN pool flush failed or out of bounds");
}
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    nvn_translate_drain();
    const pool_t *o=(const pool_t *)p;
    if(!pool_range(o,off,n) || dm_vk_pool_sync(o->backing,off,n,1)) die("NVN pool invalidate failed or out of bounds");
}
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) {
    const pool_t *o=(const pool_t *)p; return pool_range(o,0,0) && !(o->flags&8) ? (uintptr_t)o->mem : 0;
}
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return ((const pool_t *)p)->size; }
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { return ((const pool_t *)p)->flags; }
NVN(nvnMemoryPoolBuilderGetSize, size_t, const NVNmemoryPoolBuilder *b) { return ((const pool_builder_t *)b)->size; }
NVN(nvnMemoryPoolBuilderGetFlags, NVNmemoryPoolFlags, const NVNmemoryPoolBuilder *b) { return ((const pool_builder_t *)b)->flags; }

NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) {
    buffer_builder_t *o=(buffer_builder_t *)b; NVNdevice *d=o->dev; memset(o,0,sizeof *o); o->dev=d;
}
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { ((buffer_builder_t *)b)->dev=d; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    buffer_builder_t *o=(buffer_builder_t *)b; o->pool=p; o->offset=off; o->size=n;
}
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *b, const NVNbufferBuilder *builder) {
    const buffer_builder_t *o=(const buffer_builder_t *)builder;
    if(!live_device || o->dev!=live_device || !pool_range((const pool_t *)o->pool,o->offset,o->size)) return 0;
    *(buffer_t *)b=(buffer_t){MAGIC('B','U','F','F'),o->pool,o->offset,o->size}; return 1;
}
static const buffer_t *buffer_valid(const NVNbuffer *b, ptrdiff_t off, size_t n) {
    const buffer_t *o=(const buffer_t *)b;
    if(o->magic!=MAGIC('B','U','F','F') || off<0 || (size_t)off>o->size || n>o->size-(size_t)off ||
       !pool_range((const pool_t *)o->pool,o->offset,o->size)) die("Invalid NVN buffer or range");
    return o;
}
NVN(nvnBufferFinalize, void, NVNbuffer *b) { memset(b,0,sizeof(buffer_t)); }
NVN(nvnBufferMap, void *, const NVNbuffer *b) {
    const buffer_t *o=buffer_valid(b,0,0); uint8_t *base=h_nvnMemoryPoolMap(o->pool); return base ? base+o->offset : NULL;
}
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *b) {
    const buffer_t *o=buffer_valid(b,0,0); uint64_t base=h_nvnMemoryPoolGetBufferAddress(o->pool); return base ? base+o->offset : 0;
}
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t n) {
    const buffer_t *o=buffer_valid(b,off,n); h_nvnMemoryPoolFlushMappedRange(o->pool,o->offset+off,n);
}
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t n) {
    const buffer_t *o=buffer_valid(b,off,n); h_nvnMemoryPoolInvalidateMappedRange(o->pool,o->offset+off,n);
}
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *b) { return buffer_valid(b,0,0)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *b) { return buffer_valid(b,0,0)->offset; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *b) { return buffer_valid(b,0,0)->size; }
NVN(nvnBufferBuilderGetMemoryOffset, ptrdiff_t, const NVNbufferBuilder *b) { return ((const buffer_builder_t *)b)->offset; }
NVN(nvnBufferBuilderGetSize, size_t, const NVNbufferBuilder *b) { return ((const buffer_builder_t *)b)->size; }
