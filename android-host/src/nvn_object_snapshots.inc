/* Upstream b94670c record-time tex/program snapshots. Recorded command memory
 * retains its NVN fence lifetime; these slots protect the mutable NVN objects.
 * Serial numbers are process-unique. Slots stay allocated so stale by-value
 * copies can check their serial safely; retirement uses enqueued submits and
 * reclamation uses completed translation, never wall-clock time. */
#include <stddef.h>
#define SNAP_KEEP 8
typedef struct snap_slot {
    union { tex_t t; prog_t p; };
    struct snap_slot *next;
    uint8_t *controls;
    unsigned retired;
} snap_slot;
static pthread_mutex_t g_snap_mu=PTHREAD_MUTEX_INITIALIZER;
static snap_slot *g_snap_free;
typedef struct snap_block { struct snap_block *next;snap_slot *slots; } snap_block;
static snap_block *g_snap_blocks;
static struct { snap_slot *s;uint64_t at; } *g_snap_ret;
static size_t g_snap_nret,g_snap_capret,g_snap_live,g_snap_slots;
static uint64_t g_snap_sub;
static _Atomic uint64_t g_x_subs;
static snap_slot *snap_alloc(void) {
    if(!g_snap_free) {
        snap_slot *block=calloc(256,sizeof *block);
        if(!block)die("NVN object snapshot allocation failed");
        snap_block *owned=malloc(sizeof *owned);
        if(!owned)die("NVN snapshot block registry allocation failed");
        *owned=(snap_block){g_snap_blocks,block};g_snap_blocks=owned;
        for(int i=255;i>=0;i--){block[i].next=g_snap_free;g_snap_free=&block[i];}
        g_snap_slots+=256;
        LOG("NVN object snapshot slots=%zu live=%zu retired=%zu",g_snap_slots,g_snap_live,g_snap_nret);
    }
    snap_slot *slot=g_snap_free;g_snap_free=slot->next;
    memset(slot,0,sizeof *slot);g_snap_live++;return slot;
}
static void snap_retire(snap_slot *slot) {
    // Copied NVN objects can Finalize the same snapshot more than once.
    if(slot->retired)return;
    if(g_snap_nret==g_snap_capret) {
        size_t cap=g_snap_capret?g_snap_capret*2:256;
        if(cap<g_snap_capret || cap>SIZE_MAX/sizeof *g_snap_ret)die("NVN snapshot retirement overflow");
        void *grown=realloc(g_snap_ret,cap*sizeof *g_snap_ret);
        if(!grown)die("NVN snapshot retirement allocation failed");
        g_snap_ret=grown;g_snap_capret=cap;
    }
    slot->retired=1;
    g_snap_ret[g_snap_nret].s=slot;
    g_snap_ret[g_snap_nret++].at=atomic_load(&g_x_subs);
}
static void snap_tick(uint64_t done) {
    pthread_mutex_lock(&g_snap_mu);
    if(done<g_snap_sub)die("NVN snapshot execution clock regressed");
    g_snap_sub=done;size_t keep=0;
    for(size_t i=0;i<g_snap_nret;i++) {
        uint64_t at=g_snap_ret[i].at;
        if(done>=at && done-at>=SNAP_KEEP) {
            snap_slot *slot=g_snap_ret[i].s;free(slot->controls);
            memset(slot,0,sizeof *slot);slot->next=g_snap_free;g_snap_free=slot;g_snap_live--;
        } else g_snap_ret[keep++]=g_snap_ret[i];
    }
    g_snap_nret=keep;pthread_mutex_unlock(&g_snap_mu);
}
static const tex_t *snap_tex(const NVNtexture *texture) {
    tex_t *object=(tex_t *)texture;if(!object || !object->serial)return object;
    // Hold the mutex even for hits: reclaim zeroes and reuses slot storage.
    pthread_mutex_lock(&g_snap_mu);
    snap_slot *slot=(snap_slot *)object->snap;
    if(!slot || slot->t.serial!=object->serial || slot->retired) {
        slot=snap_alloc();slot->t=*object;slot->t.snap=NULL;object->snap=&slot->t;
    }
    pthread_mutex_unlock(&g_snap_mu);return &slot->t;
}
static const prog_t *snap_prog(const NVNprogram *program) {
    prog_t *object=(prog_t *)program;if(!object || !object->serial)return object;
    pthread_mutex_lock(&g_snap_mu);
    snap_slot *slot=(snap_slot *)object->snap;
    if(!slot || slot->p.serial!=object->serial || slot->retired) {
        if(object->nstage>6)die("NVN snapshot invalid shader count");
        slot=snap_alloc();slot->p=*object;slot->p.snap=NULL;
        // Android resolves catalog keys at replay, unlike Metal's program
        // object creation. Own the control bytes that replay hashes; shader
        // binaries are offline catalog entries and sd.data is not read here.
        if(object->nstage) {
            slot->controls=malloc((size_t)object->nstage*0x880);
            if(!slot->controls)die("NVN shader control snapshot allocation failed");
            for(unsigned i=0;i<object->nstage;i++) {
                const void *control=object->sd[i].control;
                if(!control || dm_vk_cpu_read_address((uintptr_t)control,0x880))die("NVN shader control snapshot visibility failed");
                memcpy(slot->controls+(size_t)i*0x880,control,0x880);
                slot->p.sd[i].control=slot->controls+(size_t)i*0x880;
            }
        }
        object->snap=&slot->p;
    }
    pthread_mutex_unlock(&g_snap_mu);return &slot->p;
}
static void snap_drop(const void **address,uint64_t serial,uint64_t serial_offset) {
    pthread_mutex_lock(&g_snap_mu);
    snap_slot *slot=(snap_slot *)*address;uint64_t stored=0;
    if(slot)memcpy(&stored,(const uint8_t *)slot+serial_offset,sizeof stored);
    if(slot && serial && stored==serial)snap_retire(slot);
    *address=NULL;pthread_mutex_unlock(&g_snap_mu);
}
