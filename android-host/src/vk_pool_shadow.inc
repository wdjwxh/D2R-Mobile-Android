/* CPU snapshots of resident pool pages. Backend lock owns this cache. The
 * arena is reserved lazily and pages are populated only on actual access;
 * neither metadata nor snapshots scale with registered pool sizes. */
// The game repeatedly binds a ~19.6 MB SSBO together with resident geometry.
// The former 4 MB range limit and 44.9 MiB page cache made unchanged data
// evict itself every frame on untracked/nativebridge memory. Keep both caches
// bounded, but large enough to retain that measured working set. Storage is
// still populated lazily; these limits never initialize whole guest pools.
enum { POOL_SHADOW_PAGE=4096, POOL_SHADOW_BUCKETS=32768, POOL_SHADOW_SLOTS=32768,
    POOL_RANGE_SLOTS=64, POOL_RANGE_MIN=64*1024, POOL_RANGE_MAX=32*1024*1024,
    POOL_RANGE_BUDGET=64*1024*1024 };

/* Exact uploaded snapshots avoid hash/LRU work for every 4 KiB of a repeated
 * large binding. Registered guest pages use the shared protected dirty-page
 * tracker; untracked memory retains exact byte comparison. */
typedef struct pool_range_shadow {
    dm_vk_pool *pool;
    size_t offset,size;
    uint8_t *bytes;
    uint64_t used,stamp;
    int valid;
} pool_range_shadow;
static pool_range_shadow pool_ranges[POOL_RANGE_SLOTS];
static size_t pool_range_bytes;
static uint64_t pool_range_clock;
static void pool_range_release(pool_range_shadow *e) {
    if(e->bytes) { pool_range_bytes-=e->size;free(e->bytes); }
    memset(e,0,sizeof *e);
}
static void pool_range_invalidate(dm_vk_pool *p,size_t offset,size_t size) {
    for(size_t i=0;i<POOL_RANGE_SLOTS;i++) {
        pool_range_shadow *e=&pool_ranges[i];
        if(e->pool==p && offset<e->offset+e->size && e->offset<offset+size) {
            e->valid=0;e->stamp=0;
        }
    }
}
// A successful exact CPU upload changes only these mapped bytes. Preserve
// other bytes in overlapping valid snapshots instead of forcing a full large
// range capture on the next binding. The source is the page's immutable stack
// capture, taken before uploading, never a fresh read of mutable guest RAM.
static void pool_range_patch_uploaded(dm_vk_pool *p,size_t offset,size_t size,const uint8_t *bytes) {
    for(size_t i=0;i<POOL_RANGE_SLOTS;i++) {
        pool_range_shadow *e=&pool_ranges[i];
        if(e->pool!=p || !e->valid || !e->bytes ||
           offset>=e->offset+e->size || e->offset>=offset+size)continue;
        size_t lo=offset>e->offset?offset:e->offset;
        size_t hi=offset+size<e->offset+e->size?offset+size:e->offset+e->size;
        memcpy(e->bytes+(lo-e->offset),bytes+(lo-offset),hi-lo);
        e->stamp=0; // A partial update cannot certify a whole guest range.
    }
}
static void pool_range_forget(dm_vk_pool *p) {
    for(size_t i=0;i<POOL_RANGE_SLOTS;i++)
        if(pool_ranges[i].pool==p)pool_range_release(&pool_ranges[i]);
}
/* First access records only the key. Allocate snapshots on repeated access,
 * with bounded total storage and no allocation proportional to pool size. */
static pool_range_shadow *pool_range_get(dm_vk_pool *p,size_t offset,size_t size) {
    if(size<POOL_RANGE_MIN || size>POOL_RANGE_MAX)return NULL;
    pool_range_shadow *oldest=&pool_ranges[0];
    for(size_t i=0;i<POOL_RANGE_SLOTS;i++) {
        pool_range_shadow *e=&pool_ranges[i];
        if(e->pool==p && e->offset==offset && e->size==size) {
            e->used=++pool_range_clock;
            if(!e->bytes) {
                while(pool_range_bytes+size>POOL_RANGE_BUDGET) {
                    pool_range_shadow *victim=NULL;
                    for(size_t j=0;j<POOL_RANGE_SLOTS;j++)
                        if(pool_ranges[j].bytes && &pool_ranges[j]!=e &&
                           (!victim || pool_ranges[j].used<victim->used))victim=&pool_ranges[j];
                    if(!victim)return NULL;
                    pool_range_release(victim);
                }
                e->bytes=malloc(size);
                if(!e->bytes)return NULL;
                pool_range_bytes+=size;
            }
            return e;
        }
        if(e->used<oldest->used)oldest=e;
    }
    pool_range_release(oldest);
    *oldest=(pool_range_shadow){.pool=p,.offset=offset,.size=size,.used=++pool_range_clock};
    return NULL;
}
typedef struct pool_shadow_page {
    struct pool_shadow_page *hash_next,*prev,*next;
    dm_vk_pool *pool;
    size_t page;
    uint64_t stamp;
    uint16_t cert_lo,cert_hi; // Exact byte interval, never the entire OS page.
    uint64_t known[POOL_SHADOW_PAGE/64]; // One bit per resident-matching byte.
    uint8_t bytes[POOL_SHADOW_PAGE];
} pool_shadow_page;
// Validity describes the CPU shadow, not guest dirty generations. Keep exact
// disjoint byte coverage; even one unknown byte requires the normal upload.
static int pool_shadow_known(const pool_shadow_page *e,size_t offset,size_t size) {
    for(size_t pos=offset,end=offset+size;pos<end;) {
        size_t bit=pos%64,n=64-bit;if(n>end-pos)n=end-pos;
        uint64_t mask=(UINT64_MAX>>(64-n))<<bit;
        if((e->known[pos/64]&mask)!=mask)return 0;
        pos+=n;
    }
    return 1;
}
static void pool_shadow_mark_known(pool_shadow_page *e,size_t offset,size_t size) {
    for(size_t pos=offset,end=offset+size;pos<end;) {
        size_t bit=pos%64,n=64-bit;if(n>end-pos)n=end-pos;
        uint64_t mask=(UINT64_MAX>>(64-n))<<bit;
        e->known[pos/64]|=mask;pos+=n;
    }
}
static struct {
    pool_shadow_page *arena,*buckets[POOL_SHADOW_BUCKETS],*head,*tail,*free;
    size_t used;
} pool_shadow;
/* Include page metadata and exact-range snapshots in the bounded RAM cap. */
_Static_assert(sizeof(pool_shadow)+sizeof(pool_shadow_page)*POOL_SHADOW_SLOTS+
    sizeof(pool_ranges)+POOL_RANGE_BUDGET < 224u*1024u*1024u,
    "pool shadow must fit the 224 MiB RAM budget");
static size_t pool_shadow_hash(dm_vk_pool *p,size_t page) {
    return (((uintptr_t)p>>4)^(page*UINT64_C(11400714819323198485)))&(POOL_SHADOW_BUCKETS-1);
}
static void pool_shadow_unlink(pool_shadow_page *e) {
    size_t h=pool_shadow_hash(e->pool,e->page);
    pool_shadow_page **link=&pool_shadow.buckets[h];
    while(*link && *link!=e)link=&(*link)->hash_next;
    if(*link)*link=e->hash_next;
    if(e->prev)e->prev->next=e->next;else pool_shadow.head=e->next;
    if(e->next)e->next->prev=e->prev;else pool_shadow.tail=e->prev;
}
static void pool_shadow_invalidate(dm_vk_pool *p,size_t offset,size_t size) {
    if(!size)return;
    pool_range_invalidate(p,offset,size);
    size_t first=offset/POOL_SHADOW_PAGE,last=(offset+size-1)/POOL_SHADOW_PAGE;
    // Compute outputs and CPU event words often invalidate only a few pages.
    // Find those exact keys instead of scanning every unrelated cached page.
    // Large ranges retain the bounded LRU walk rather than probing empty keys.
    if(last-first<64) {
        for(size_t page=first;page<=last;page++) {
            size_t h=pool_shadow_hash(p,page);
            pool_shadow_page *e=pool_shadow.buckets[h];
            while(e && (e->pool!=p || e->page!=page))e=e->hash_next;
            if(!e)continue;
            p->prepare[pool_prepare_consumer].invalidations++;
            e->stamp=0;memset(e->known,0,sizeof e->known);
            pool_shadow_unlink(e);e->hash_next=pool_shadow.free;pool_shadow.free=e;
        }
        return;
    }
    for(pool_shadow_page *e=pool_shadow.head,*next;e;e=next) {
        next=e->next;
        if(e->pool!=p || e->page<first || e->page>last)continue;
        p->prepare[pool_prepare_consumer].invalidations++;
        e->stamp=0;memset(e->known,0,sizeof e->known);
        pool_shadow_unlink(e);e->hash_next=pool_shadow.free;pool_shadow.free=e;
    }
}
/* A fresh entry has no known bytes. Upload establishes exact coverage without
 * reading mapped bytes. On failure the caller invalidates the range. */
static pool_shadow_page *pool_shadow_get(dm_vk_pool *p,size_t page,int *fresh) {
    size_t h=pool_shadow_hash(p,page);*fresh=0;
    pool_shadow_page *e=pool_shadow.buckets[h];
    while(e && (e->pool!=p || e->page!=page))e=e->hash_next;
    if(e)pool_shadow_unlink(e);
    else {
        if(!pool_shadow.arena) {
            pool_shadow.arena=calloc(POOL_SHADOW_SLOTS,sizeof *pool_shadow.arena);
            if(!pool_shadow.arena)return NULL;
        }
        if(pool_shadow.free){e=pool_shadow.free;pool_shadow.free=e->hash_next;*fresh=3;}
        else if(pool_shadow.used<POOL_SHADOW_SLOTS){e=&pool_shadow.arena[pool_shadow.used++];*fresh=1;}
        else {e=pool_shadow.tail;pool_shadow_unlink(e);*fresh=2;p->prepare[pool_prepare_consumer].evictions++;}
        e->pool=p;e->page=page;e->stamp=0;e->cert_lo=e->cert_hi=0;
        memset(e->known,0,sizeof e->known);
    }
    e->hash_next=pool_shadow.buckets[h];pool_shadow.buckets[h]=e;
    e->prev=NULL;e->next=pool_shadow.head;
    if(e->next)e->next->prev=e;else pool_shadow.tail=e;
    pool_shadow.head=e;return e;
}
static void pool_shadow_close(void) {
    for(size_t i=0;i<POOL_RANGE_SLOTS;i++)pool_range_release(&pool_ranges[i]);
    pool_range_clock=0;
    free(pool_shadow.arena);memset(&pool_shadow,0,sizeof pool_shadow);
}
