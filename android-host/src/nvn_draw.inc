#include "vk_draw.h"
typedef struct {uint8_t driver[2*REPLAY_DRIVER_BYTES];dm_compute_buffer buffers[64];dm_compute_image images[64];} draw_arguments;
static uint8_t draw_support[1392];
static int draw_support_initialized;
// Like upstream prog_get: resolve controls once for a ProgramSetShaders serial.
// The replay lock owns this bounded four-way cache. Entries borrow catalog
// objects, whose generation changes before the catalog releases its storage.
typedef struct {
    uint64_t serial,generation,used;unsigned count;
    const dm_graphics_shader *graphics[6];const dm_compute_shader *compute[6];
    char keys[6][17];
} replay_program_resolution;
static replay_program_resolution replay_program_cache[64][4],replay_program_uncached;
static replay_program_resolution *replay_program_last;
static uint64_t replay_program_clock;
static const replay_program_resolution *replay_program_resolve_impl(const prog_t *p,int owned_controls) {
    uint64_t generation=dm_vk_graphics_generation();
    replay_program_resolution *entry=replay_program_last;
    if(p->serial && entry && entry->serial==p->serial && entry->generation==generation) {
        entry->used=++replay_program_clock;return entry;
    }
    if(p->nstage>6)die("Invalid graphics stage count");
    if(p->serial) {
        unsigned bucket=(unsigned)(p->serial^(p->serial>>32))&63;
        replay_program_resolution *set=replay_program_cache[bucket];entry=&set[0];
        for(unsigned i=0;i<4;i++) {
            if(set[i].serial==p->serial && set[i].generation==generation) {
                set[i].used=++replay_program_clock;replay_program_last=&set[i];return &set[i];
            }
            if(set[i].used<entry->used)entry=&set[i];
        }
    } else entry=&replay_program_uncached; // Unconfigured objects have no stable identity.
    memset(entry,0,sizeof *entry);entry->count=p->nstage;
    for(unsigned i=0;i<p->nstage;i++) {
        if(!p->sd[i].control)die("Missing graphics control");
        if(!owned_controls && dm_vk_cpu_read_address((uintptr_t)p->sd[i].control,0x880))die("Graphics control visibility failed");
        shader_key_bytes(p->sd[i].control,0x880,entry->keys[i]);
        entry->graphics[i]=dm_vk_graphics_lookup(entry->keys[i]);
        if(!entry->graphics[i])entry->compute[i]=dm_vk_compute_lookup(entry->keys[i]);
    }
    entry->serial=p->serial;entry->generation=generation;entry->used=++replay_program_clock;
    replay_program_last=p->serial?entry:NULL;return entry;
}
static const replay_program_resolution *replay_program_resolve(const prog_t *p) {
    return replay_program_resolve_impl(p,0);
}
static const dm_graphics_shader *replay_graphics_shader(const prog_t *p,int stage) {
    if(!p || p->magic!=MAGIC('P','R','O','G'))die("Missing graphics program at stage %d",stage);
    const dm_graphics_shader *found=NULL;
    const replay_program_resolution *resolved=replay_program_resolve(p);
    for(unsigned i=0;i<resolved->count;i++) {
        const dm_graphics_shader *shader=resolved->graphics[i];
        if(shader && shader->stage==(unsigned)stage){if(found)die("Duplicate graphics stage");found=shader;}
    }
    if(!found)die("Graphics stage %d missing from shader archive",stage);return found;
}
static void draw_arguments_build(replay_state *s,const dm_graphics_shader *shader,int stage,draw_arguments *args) {
    memset(args,0,sizeof *args);
    uint8_t *driver=args->driver;
    uint8_t *c0=driver,*c2=driver+REPLAY_DRIVER_BYTES;
    int base=stage==0?0x110:0x510;
    for(int i=0;i<16;i++) {
        memcpy(c0+base+i*16,&s->storage[stage][i].addr,8);memcpy(c0+base+8+i*16,&s->storage[stage][i].size,8);
    }
    memcpy(c2+0x168,s->texture[stage],128*8);memcpy(c2+0x568,s->sampler[stage],128*8);
    // The upstream driver ABI has eight image handles before texture handles.
    memcpy(c2+0x120,s->image[stage],8*8);
    if(!draw_support_initialized) {
        float one=1;
        for(int i=0;i<73;i++)memcpy(draw_support+192+i*16,&one,4);
        draw_support_initialized=1;
    }
    dm_compute_buffer *buffers=args->buffers;dm_compute_image *images=args->images;
    for(unsigned i=0;i<shader->nbindings;i++) {
        const dm_shader_binding *b=&shader->bindings[i];dm_compute_buffer *a=&buffers[i];a->binding=b->binding;
        const p_bind_buf *source=NULL;
        if(b->kind==DM_BIND_UNIFORM || b->kind==DM_BIND_UBO_POINTER || b->kind==DM_BIND_COUNT) {
            if(b->slot<0 || b->slot>=16)die("Compute uniform slot out of range");source=&s->uniform[stage][b->slot];
        } else if(b->kind==DM_BIND_STORAGE) {
            // Ryujinx BufferUsageFlags.Write = 1 << 0 (not texture flags).
            if(b->slot<0 || b->slot>=16)die("Compute storage slot out of range");source=&s->storage[stage][b->slot];a->writable=(b->flags&1)!=0;
        } else if(b->kind==DM_BIND_SUPPORT){a->host=draw_support;a->size=sizeof draw_support;}
        else if(b->kind==DM_BIND_DRIVER){if(b->slot!=0 && b->slot!=2)die("Unknown compute driver bank");a->host=b->slot==0?c0:c2;a->size=REPLAY_DRIVER_BYTES;}
        else if(b->kind==DM_BIND_CONSTANT){a->zero=shader->constants_size==0;a->host=a->zero?replay_zero_storage:shader->constants;a->size=a->zero?sizeof replay_zero_storage:shader->constants_size;}
        // TextureUsageFlags.ImageStore also covers ImageAtomic upstream.
        else if(b->kind==DM_BIND_TEXTURE || b->kind==DM_BIND_IMAGE){dispatch_image_locked(s,b,&images[i],stage);a->image=&images[i];a->writable=b->kind==DM_BIND_IMAGE && (b->flags&4)!=0;}
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
}
/* Like upstream g_args_dirty/encode_args, retain CPU argument descriptions
 * across equal bindings. The replay lock serializes both slots and dm_vk_draw
 * consumes them synchronously; deferred Vulkan work owns its own snapshots.
 * Bind handlers advance args_version only on a changed value; this avoids a
 * second full-state scan/copy on every draw. Driver banks here depend only on
 * storage/texture/sampler/image bindings. Viewport and draw base-instance are
 * still rebuilt below and passed separately to Vulkan for every draw.
 * This caches identities, never guest UBO/SSBO contents: backend validation
 * still runs on every draw, including under native-bridge translation. */
typedef struct {
    int valid,has_texture_pool,has_sampler_pool;
    uint64_t program_serial,catalog_generation,descriptor_generation,args_version,args_static_version;
    const dm_graphics_shader *shader;
    dpool_t texture_pool,sampler_pool;
    draw_arguments args;
} draw_stage_arguments;
static draw_stage_arguments draw_stages[2];
static struct {uint64_t hits,full,ubo_only,ubo_bindings;} draw_argument_stats;
static void draw_arguments_report(void) {
    LOG("Replay argument cache: hits=%llu fullBuilds=%llu uboOnlyBuilds=%llu uboBindings=%llu",
        (unsigned long long)draw_argument_stats.hits,(unsigned long long)draw_argument_stats.full,
        (unsigned long long)draw_argument_stats.ubo_only,(unsigned long long)draw_argument_stats.ubo_bindings);
    memset(&draw_argument_stats,0,sizeof draw_argument_stats);
}
static void draw_arguments_update_ubo(replay_state *s,const dm_graphics_shader *shader,int stage,draw_arguments *args) {
    // Only these reflected kinds read s->uniform. Driver c0 uses storage and
    // c2 uses descriptor handles, so neither changes for an ordinary UBO bind.
    for(unsigned i=0;i<shader->nbindings;i++) {
        const dm_shader_binding *b=&shader->bindings[i];
        if(b->kind!=DM_BIND_UNIFORM && b->kind!=DM_BIND_UBO_POINTER && b->kind!=DM_BIND_COUNT)continue;
        if(b->slot<0 || b->slot>=16)die("Compute uniform slot out of range");
        const p_bind_buf *source=&s->uniform[stage][b->slot];
        dm_compute_buffer *a=&args->buffers[i];
        *a=(dm_compute_buffer){.binding=b->binding,.address=source->addr,.size=source->size};
        if(!a->address || !a->size) {
            a->address=0;a->host=replay_zero_storage;a->size=sizeof replay_zero_storage;a->zero=1;
        }
        draw_argument_stats.ubo_bindings++;
    }
}
static int draw_pool_equal(const dpool_t *a,const dpool_t *b) {
    return a->pool==b->pool && a->off==b->off && a->n==b->n;
}
static draw_arguments *draw_arguments_get(replay_state *s,const dm_graphics_shader *shader,int stage) {
    draw_stage_arguments *entry=&draw_stages[stage];
    uint64_t catalog=dm_vk_graphics_generation(),serial=s->program[stage]->serial;
    // One registry lock per stage, instead of per texture. Keeping it through
    // a miss also prevents a mixed descriptor snapshot being certified valid.
    pthread_mutex_lock(&descriptor_lock);
    uint64_t registration=atomic_load_explicit(&descriptor_generation,memory_order_acquire);
    int same_identity=entry->valid && entry->shader==shader && entry->program_serial==serial &&
        entry->catalog_generation==catalog && entry->descriptor_generation==registration &&
        entry->has_texture_pool==!!s->texture_pool && entry->has_sampler_pool==!!s->sampler_pool;
    if(same_identity && s->texture_pool)same_identity=draw_pool_equal(&entry->texture_pool,s->texture_pool);
    if(same_identity && s->sampler_pool)same_identity=draw_pool_equal(&entry->sampler_pool,s->sampler_pool);
    int same=same_identity && entry->args_version==s->args_version[stage];
    if(!same) {
        entry->valid=0;
        if(shader->nbindings>64)die("Graphics binding count out of range");
        if(same_identity && entry->args_static_version==s->args_static_version[stage]) {
            draw_arguments_update_ubo(s,shader,stage,&entry->args);draw_argument_stats.ubo_only++;
        } else {
            draw_arguments_build(s,shader,stage,&entry->args);draw_argument_stats.full++;
        }
        entry->shader=shader;entry->program_serial=serial;
        entry->catalog_generation=catalog;entry->descriptor_generation=registration;
        entry->args_version=s->args_version[stage];
        entry->args_static_version=s->args_static_version[stage];
        entry->has_texture_pool=!!s->texture_pool;entry->has_sampler_pool=!!s->sampler_pool;
        if(s->texture_pool)entry->texture_pool=*s->texture_pool;
        if(s->sampler_pool)entry->sampler_pool=*s->sampler_pool;
        entry->valid=1;
    } else draw_argument_stats.hits++;
    pthread_mutex_unlock(&descriptor_lock);
    return &entry->args;
}
static void replay_draw(replay_state *s,const p_draw *plain,const p_draw_idx *indexed) {
    // BindProgram's mask selects stages to update, not stages present in the
    // program. Games commonly bind a VS/PS program with all graphics bits set.
    for(int i=2;i<5;i++)if(s->program[i]) {
        const prog_t *p=s->program[i];
        if(p->magic!=MAGIC('P','R','O','G'))die("Invalid graphics program");
        const replay_program_resolution *resolved=replay_program_resolve(p);
        for(unsigned j=0;j<resolved->count;j++) {
            const dm_graphics_shader *sh=resolved->graphics[j];
            if(!sh && !resolved->compute[j])die("Unknown graphics stage control %s",resolved->keys[j]);
            if(sh && sh->stage>=2 && sh->stage<=4)die("Geometry/tessellation stage needs graphics backend support");
        }
    }
    if(s->multisample.cov_mod || s->multisample.cov2color_en || s->multisample.sl_en)die("Coverage/sample-location state requires Vulkan extension");
    dm_draw *d=calloc(1,sizeof *d);if(!d)die("Draw argument allocation failed");
    for(int stage=0;stage<2;stage++){d->shader[stage]=replay_graphics_shader(s->program[stage],stage);d->resources[stage]=draw_arguments_get(s,d->shader[stage],stage)->buffers;}
    for(int i=0;i<s->rt.n;i++)if(s->rt.color[i]){d->color[i]=replay_image(s->rt.color[i],s->rt.has_cview[i]?&s->rt.cview[i]:NULL);d->color_mask|=1u<<i;}
    d->has_depth=s->rt.depth!=NULL;d->depth_test=s->depth_stencil.dt;d->stencil_test=s->depth_stencil.st;
    if(s->rt.depth)d->depth=replay_image(s->rt.depth,s->rt.has_dview?&s->rt.dview:NULL);
    d->depth_write=s->depth_stencil.dw;d->depth_compare=s->depth_stencil.df;
    d->stencil[0]=(dm_stencil){s->depth_stencil.fail_f,s->depth_stencil.zfail_f,s->depth_stencil.zpass_f,s->depth_stencil.sf_f,s->stencil[0].value_mask,s->stencil[0].mask,s->stencil[0].ref};
    d->stencil[1]=(dm_stencil){s->depth_stencil.fail_b,s->depth_stencil.zfail_b,s->depth_stencil.zpass_b,s->depth_stencil.sf_b,s->stencil[1].value_mask,s->stencil[1].mask,s->stencil[1].ref};
    d->nattributes=s->nattrib;
    for(int i=0;i<16;i++) {
        va_st a;memcpy(&a,&s->attrib[i],sizeof a);d->attributes[i]=(dm_vertex_attribute){a.fmt,a.offset,a.stream};
        d->streams[i]=(dm_vertex_stream){s->vertex[i].address,s->vertex[i].size,s->stream[i].stride,s->stream[i].divisor};
    }
    for(int i=0;i<8;i++){blend_st *b=&s->blend[i];d->blend[i]=(dm_blend){(s->color.blend_en>>i)&1,b->src_rgb,b->dst_rgb,b->src_a,b->dst_a,b->eq_rgb,b->eq_a,(s->channels.mask>>(i*4))&15};}
    memcpy(d->blend_constant,s->blend_color.rgba,16);memcpy(d->viewport,s->viewport[0],sizeof d->viewport);
    d->scissor[0]=s->scissor.x;d->scissor[1]=s->scissor.y;d->scissor[2]=s->scissor.w;d->scissor[3]=s->scissor.h;
    d->cull=s->polygon.cull;d->front=s->polygon.front;d->polygon=s->polygon.mode;d->logic=s->color.logic_op;d->alpha=s->color.alpha_func;
    d->raster_samples=s->multisample.enable?s->multisample.samples:1;d->alpha_to_coverage=s->multisample.a2c;
    d->depth_bias=(s->polygon.offset_en&4)!=0;d->bias_factor=s->offset.factor;d->bias_units=s->offset.units;d->bias_clamp=s->offset.clamp;
    d->samples=s->owner->samples_counting?&s->owner->samples:NULL;
    if(indexed){d->indexed=1;d->primitive=indexed->prim;d->index_type=indexed->itype;d->count=indexed->count;d->base_vertex=indexed->base_vertex;d->base_instance=indexed->base_instance;d->instances=indexed->instances;d->index_address=indexed->ibuf;}
    else {d->primitive=plain->prim;d->first=plain->first;d->count=plain->count;d->instances=1;}
    static unsigned logged;if(logged++<16)LOG("NVN draw vs=%s ps=%s count=%d indexed=%d",d->shader[0]->key,d->shader[1]->key,d->count,d->indexed);
    int result=dm_vk_draw(d);free(d);if(result)die("Vulkan graphics draw failed");
}
