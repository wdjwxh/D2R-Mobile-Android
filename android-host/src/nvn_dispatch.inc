#include "shader_key.h"
#include "vk_compute.h"
// Upstream driver arena payload; GPU uniform descriptors still span 64 KiB.
enum { REPLAY_DRIVER_BYTES=0xA00 };
static const uint8_t replay_zero_storage[65536];
_Static_assert(0x510+16*16<=REPLAY_DRIVER_BYTES && 0x568+128*8<=REPLAY_DRIVER_BYTES,"driver ABI payload bounds");
// Caller holds descriptor_lock so a whole graphics-stage snapshot can resolve
// its descriptors consistently with one registration generation.
static void dispatch_image_locked(replay_state *s,const dm_shader_binding *b,dm_compute_image *out,int stage) {
    if(b->slot<0 || b->slot>=(b->kind==DM_BIND_IMAGE?32:128))die("Unmapped compute texture slot");
    uint64_t handle=b->kind==DM_BIND_IMAGE?s->image[stage][b->slot]:s->texture[stage][b->slot];
    unsigned tid=handle&0xfffff;
    descriptors *p=s->texture_pool?descriptor_find(s->texture_pool,0):NULL;
    texture_slot *t=p && tid<(unsigned)p->n && p->slots.textures[tid].present?&p->slots.textures[tid]:NULL;
    memset(out,0,sizeof *out);
    if(!t) {
        // Upstream has separate zero-read and discard-write dummy images.
        unsigned kind=b->type&255;int array=(b->type&256)!=0;
        if(b->type&512)die("Multisample dummy requires multisample image support");
        int format=(b->flags&0x20000)?0x30:(b->flags&0x10000)?0x2f:0x2e;
        if(b->type&1024)format=0x34;
        int target=kind==1?(array?3:0):kind==2?10:kind==4?2:kind==5?(array?9:8):(array?4:1);
        out->desc=(nvn_texdesc){.target=target,.format=format,.width=1,.height=1,.depth=1,.levels=1};
        out->serial=UINT64_C(0xff00000000000000)|((uint64_t)b->kind<<32)|((uint64_t)b->type<<16)|format;
        out->format=format;out->levels=1;out->layers=kind==5?6:1;
    } else {
        out->desc=t->tex.d;out->serial=t->tex.serial;out->address=(uintptr_t)t->tex.mem;
        out->packaged=t->tex.has_layout && t->tex.mem;
        out->levels=out->desc.levels;out->layers=t->tex.L.layers;out->format=out->desc.format;
    }
    if(t && t->has_view) {
        if(t->view.set_levels){out->base_level=t->view.base_level;out->levels=t->view.nlevels;}
        if(t->view.set_layers){out->base_layer=t->view.min_layer;out->layers=t->view.nlayers;}
        if(t->view.set_format)out->format=t->view.format;
    }
    out->lod_max=1000;out->aniso=1;out->minf=out->magf=t?1:0;out->ws=out->wt=out->wr=7;
    if(b->kind==DM_BIND_TEXTURE) {
        uint64_t sh=b->sampler_slot>=0?s->sampler[stage][b->sampler_slot]:handle;
        unsigned sid=(sh>>20)&4095;
        descriptors *sp=s->sampler_pool?descriptor_find(s->sampler_pool,1):NULL;
        if(sp && sid<(unsigned)sp->n && sp->slots.samplers[sid].present) {
            samp_par *a=&sp->slots.samplers[sid].sampler;
            out->minf=a->minf;out->magf=a->magf;out->ws=a->ws;out->wt=a->wt;out->wr=a->wr;
            out->compare=a->cmp_mode;out->compare_func=a->cmp_func;out->lod_min=a->lod_min;out->lod_max=a->lod_max;
            out->lod_bias=a->lod_bias;out->aniso=a->aniso;memcpy(out->border,a->border,sizeof out->border);
            if(a->reduction)die("Compute sampler reduction mode requires Vulkan extension");
        }
    }
}
static void dispatch_image(replay_state *s,const dm_shader_binding *b,dm_compute_image *out,int stage) {
    pthread_mutex_lock(&descriptor_lock);
    dispatch_image_locked(s,b,out,stage);
    pthread_mutex_unlock(&descriptor_lock);
}
static void replay_dispatch(replay_state *s,const p_dispatch *p) {
    const prog_t *program=s->program[5];
    if(!program || program->magic!=MAGIC('P','R','O','G'))die("Compute dispatch has no program");
    const dm_compute_shader *shader=NULL;char key[17]={0};
    for(unsigned i=0;i<program->nstage;i++) {
        if(!program->sd[i].control)die("Missing shader control block");
        if(dm_vk_cpu_read_address((uintptr_t)program->sd[i].control,0x880))die("Compute control visibility failed");
        shader_key_bytes(program->sd[i].control,0x880,key);
        const dm_compute_shader *candidate=dm_vk_compute_lookup(key);
        if(candidate){if(shader)die("Multiple compute stages in NVN program");shader=candidate;}
    }
    if(!shader)die("Compute shader %s missing from Android catalog",key);
    static unsigned logged;
    if(logged++<32)LOG("NVN compute %s builtin=%d groups=%d,%d,%d caller=%#llx",shader->key,shader->builtin,p->x,p->y,p->z,(unsigned long long)p->caller);
    if(p->x<0 || p->y<0 || p->z<0 || shader->nbindings>64)die("Invalid NVN dispatch");
    uint8_t *driver=calloc(2,REPLAY_DRIVER_BYTES);if(!driver)die("Compute driver block allocation failed");
    uint8_t *c0=driver,*c2=driver+REPLAY_DRIVER_BYTES;
    for(int i=0;i<16;i++) {
        memcpy(c0+0x210+i*16,&s->uniform[5][i].addr,8);memcpy(c0+0x218+i*16,&s->uniform[5][i].size,8);
        memcpy(c0+0x310+i*16,&s->storage[5][i].addr,8);memcpy(c0+0x318+i*16,&s->storage[5][i].size,8);
    }
    memcpy(c2+0x168,s->texture[5],128*8);memcpy(c2+0x568,s->sampler[5],128*8);
    // The upstream driver ABI has eight image handles before texture handles.
    memcpy(c2+0x120,s->image[5],8*8);
    uint8_t support[1392]={0};float one=1;
    for(int i=0;i<73;i++)memcpy(support+192+i*16,&one,4);
    dm_compute_buffer buffers[64]={0};dm_compute_image images[64];
    for(unsigned i=0;i<shader->nbindings;i++) {
        const dm_shader_binding *b=&shader->bindings[i];dm_compute_buffer *a=&buffers[i];a->binding=b->binding;
        const p_bind_buf *source=NULL;
        if(b->kind==DM_BIND_UNIFORM || b->kind==DM_BIND_UBO_POINTER || b->kind==DM_BIND_COUNT) {
            if(b->slot<0 || b->slot>=16)die("Compute uniform slot out of range");source=&s->uniform[5][b->slot];
        } else if(b->kind==DM_BIND_STORAGE) {
            // Ryujinx BufferUsageFlags.Write = 1 << 0 (not texture flags).
            if(b->slot<0 || b->slot>=16)die("Compute storage slot out of range");source=&s->storage[5][b->slot];a->writable=(b->flags&1)!=0;
        } else if(b->kind==DM_BIND_SUPPORT){a->host=support;a->size=sizeof support;}
        else if(b->kind==DM_BIND_DRIVER){if(b->slot!=0 && b->slot!=2)die("Unknown compute driver bank");a->host=b->slot==0?c0:c2;a->size=REPLAY_DRIVER_BYTES;}
        else if(b->kind==DM_BIND_CONSTANT){a->zero=shader->constants_size==0;a->host=a->zero?replay_zero_storage:shader->constants;a->size=a->zero?sizeof replay_zero_storage:shader->constants_size;}
        // TextureUsageFlags.ImageStore also covers ImageAtomic upstream.
        else if(b->kind==DM_BIND_TEXTURE || b->kind==DM_BIND_IMAGE){dispatch_image(s,b,&images[i],5);a->image=&images[i];a->writable=b->kind==DM_BIND_IMAGE && (b->flags&4)!=0;}
        else die("Unknown compute resource kind");
        if(source) {
            a->address=source->addr;a->size=source->size;
            if(!a->address || !a->size) {
                a->address=0;
                // UBOP/COUNT are storage descriptors too: preserve the full
                // missing-binding range independently of the driver payload.
                a->host=replay_zero_storage;a->size=sizeof replay_zero_storage;a->zero=1;
            }
        }
    }
    if(shader->builtin>=0) {
        if(p->y!=1 || p->z!=1 || !buffers[0].address || buffers[0].size<4)die("Invalid counter compute arguments");
        // counter.comp clamps against all descriptor capacities before loads
        // and stores. Keep the GPU-produced count on the GPU, as upstream.
    }
    int result=dm_vk_compute(shader,buffers,shader->nbindings,p->x,p->y,p->z);free(driver);
    if(result)die("Vulkan compute dispatch failed: %s",shader->key);
}
