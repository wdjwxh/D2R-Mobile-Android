/* Owned, bounded render IR. Collection never materializes guest bytes. A
 * producer, observer, unknown record or unprovable draw ends the prefix before
 * that operation. The interpreter consumes these exact bytes, not a rescan. */
#define RE_MAX 200000
#define RE_ARENA_BYTES (16u*1024u*1024u)
#define RE_MEMORY_BUDGET (128u*1024u*1024u)
#define RE_IMAGES 512
typedef struct {
    uint64_t address,next;op_hdr h;unsigned rt,skip,hoist;
    p_scissor scissor;uint8_t *bytes;
} render_record;
typedef struct {
    render_record *records;uint8_t *arena;size_t arena_used;
    unsigned count,index,nrt;
    replay_decoder decoder;
    p_set_rt targets[128];
    dm_compute_image reads[RE_IMAGES],writes[RE_IMAGES];unsigned nr,nw;
    struct {uint64_t start,end;} spans[128];unsigned nspan;
    uint64_t write_end[RE_IMAGES];
    struct {const tex_t *source;tex_t value;} textures[256];unsigned nt;
    const prog_t *programs[64];const dm_graphics_shader *shaders[64][2];unsigned np;
    uint64_t catalog,descriptors;int deferred;
} render_epoch;
// replay_lock owns the shared depth caches and this complete allocation budget.
static size_t render_memory_bytes;
static render_epoch *render_epoch_allocate(void) {
    size_t bytes=sizeof(render_epoch)+(size_t)RE_MAX*sizeof(render_record)+RE_ARENA_BYTES;
    if(bytes>RE_MEMORY_BUDGET-render_memory_bytes)return NULL;
    render_epoch *e=malloc(sizeof *e);if(!e)return NULL;
    e->records=malloc((size_t)RE_MAX*sizeof *e->records);e->arena=malloc(RE_ARENA_BYTES);
    if(!e->records || !e->arena){free(e->records);free(e->arena);free(e);return NULL;}
    render_memory_bytes+=bytes;return e;
}
static int render_record_snapshot(render_epoch *e,uint64_t address,uint8_t record[2048],op_hdr *header) {
    if(address&7)return 0;
    if(replay_decoder_snapshot(&e->decoder,address,record,header))return 1;
    if(dm_vk_guest_record_peek(address,record,2048))return 0;
    memcpy(header,record,sizeof *header);
    return header->size>=8 && !(header->size&7) && header->size<=2048 && address<=UINT64_MAX-header->size;
}
static struct {uint64_t epochs,records,hoists,clears,barriers,dependency,peek,limit;
    uint64_t program,read_view,attachment,command_span,resource_conflict;
} render_stats;
static void fuse_report(void);
static void render_epoch_report(void) {
    draw_arguments_report();
    fuse_report();
    if(!RE_ENABLE_HOIST)return;
    LOG("Render IR: epochs=%llu records=%llu hoists=%llu clears=%llu mergedBarriers=%llu dependencyStops=%llu peekStops=%llu limitStops=%llu",
        (unsigned long long)render_stats.epochs,(unsigned long long)render_stats.records,
        (unsigned long long)render_stats.hoists,(unsigned long long)render_stats.clears,
        (unsigned long long)render_stats.barriers,(unsigned long long)render_stats.dependency,
        (unsigned long long)render_stats.peek,(unsigned long long)render_stats.limit);
    LOG("Render IR rejects: program=%llu readView=%llu attachment=%llu commandSpan=%llu resourceConflict=%llu countsIncludeRetry=1",
        (unsigned long long)render_stats.program,(unsigned long long)render_stats.read_view,
        (unsigned long long)render_stats.attachment,(unsigned long long)render_stats.command_span,
        (unsigned long long)render_stats.resource_conflict);
    memset(&render_stats,0,sizeof render_stats);
}
static int render_snapshot_owned(const void *p) {
    uintptr_t a=(uintptr_t)p;
    for(snap_block *b=g_snap_blocks;b;b=b->next) {
        uintptr_t start=(uintptr_t)b->slots;
        if(a>=start && a-start<256*sizeof(snap_slot) && !((a-start)%sizeof(snap_slot)))return 1;
    }
    return 0;
}
static int render_image(render_epoch *e,const tex_t *p,const view_t *v,dm_compute_image *out,int attachment) {
    if(!p)return 0;
    const tex_t *owned=NULL;
    for(unsigned i=0;i<e->nt;i++)if(e->textures[i].source==p){owned=&e->textures[i].value;break;}
    if(!owned) {
        if(e->nt==256)return 0;
        pthread_mutex_lock(&g_snap_mu);
        int valid=render_snapshot_owned(p) && p->magic==MAGIC('T','E','X',0) && p->serial;
        if(valid){e->textures[e->nt].source=p;e->textures[e->nt].value=*p;owned=&e->textures[e->nt++].value;}
        pthread_mutex_unlock(&g_snap_mu);
        if(!valid)return 0;
    }
    *out=replay_image(owned,v);
    if(out->base_level<0 || out->levels<=0 || out->base_layer<0 || out->layers<=0 ||
       (unsigned)out->base_level>=out->desc.levels || (unsigned)out->levels>out->desc.levels-(unsigned)out->base_level)return 0;
    if((unsigned)out->base_layer>=owned->L.layers || (unsigned)out->layers>owned->L.layers-(unsigned)out->base_layer)return 0;
    if(attachment) {if(out->packaged || out->layers!=1 || out->desc.target==2)return 0;out->levels=1;}
    return 1;
}
static int render_overlap(const dm_compute_image *a,const dm_compute_image *b) {
    if(a->serial!=b->serial)return 0;
    return a->base_level<(int64_t)b->base_level+b->levels && b->base_level<(int64_t)a->base_level+a->levels &&
           a->base_layer<(int64_t)b->base_layer+b->layers && b->base_layer<(int64_t)a->base_layer+a->layers;
}
static int render_add_image(dm_compute_image *list,unsigned *n,const dm_compute_image *image) {
    for(unsigned i=0;i<*n;i++)if(list[i].serial==image->serial && list[i].base_level==image->base_level &&
        list[i].levels==image->levels && list[i].base_layer==image->base_layer && list[i].layers==image->layers)return 1;
    if(*n==RE_IMAGES)return 0;list[(*n)++]=*image;return 1;
}
static int render_program(render_epoch *e,const prog_t *p) {
    for(unsigned i=0;i<e->np;i++)if(e->programs[i]==p)return 1;
    if(!p || e->np==64)return 0;
    pthread_mutex_lock(&g_snap_mu);
    int valid=render_snapshot_owned(p) && p->magic==MAGIC('P','R','O','G') && p->serial && p->nstage && p->nstage<=6;
    e->shaders[e->np][0]=e->shaders[e->np][1]=NULL;
    if(valid)for(unsigned i=0;i<p->nstage;i++) {
        const snap_slot *slot=(const snap_slot *)p;
        if(!slot->controls || p->sd[i].control!=slot->controls+(size_t)i*0x880){valid=0;break;}
    }
    const replay_program_resolution *resolved=valid?replay_program_resolve_impl(p,1):NULL;
    if(valid)for(unsigned i=0;i<resolved->count;i++) {
        const dm_graphics_shader *sh=resolved->graphics[i];
        if(!sh || sh->stage>1 || !sh->reflection || sh->nbindings>64){valid=0;break;}
        if(e->shaders[e->np][sh->stage]){valid=0;break;}e->shaders[e->np][sh->stage]=sh;
        for(unsigned j=0;j<sh->nbindings;j++) {
            const dm_shader_binding *b=&sh->bindings[j];
            if(b->kind>DM_BIND_COUNT || (b->kind==DM_BIND_STORAGE && (b->flags&1)) ||
               (b->kind==DM_BIND_IMAGE && (b->flags&4))){valid=0;break;}
        }
        if(!valid)break;
    }
    pthread_mutex_unlock(&g_snap_mu);
    if(valid)e->programs[e->np++]=p;return valid;
}
static int render_draw_reads(render_epoch *e,replay_state *s,dm_compute_image *images,unsigned *count) {
    *count=0;int valid=1;
    pthread_mutex_lock(&descriptor_lock);
    for(unsigned stage=0;valid && stage<2;stage++) {
        const dm_graphics_shader *sh=NULL;
        for(unsigned i=0;i<e->np;i++)if(e->programs[i]==s->program[stage])sh=e->shaders[i][stage];
        if(!sh){valid=0;break;}
        for(unsigned i=0;i<sh->nbindings;i++) {
            const dm_shader_binding *b=&sh->bindings[i];
            if(b->kind!=DM_BIND_TEXTURE && b->kind!=DM_BIND_IMAGE)continue;
            if(b->slot<0 || b->slot>=(b->kind==DM_BIND_IMAGE?32:128) || b->sampler_slot>=128){valid=0;break;}
            dm_compute_image image;dispatch_image_locked(s,b,&image,stage);
            if(b->kind==DM_BIND_IMAGE)image.levels=1;
            if((b->type&255)==4) {
                // Match graphics_image_view_range: a readonly 3D view has one
                // Vulkan array layer. Keep its full sampled mip range in the
                // dependency set; do not reinterpret a nonzero layer view.
                if(image.desc.target!=2 || image.base_layer!=0){valid=0;break;}
                image.layers=1;
            } else if(!(b->type&256))image.layers=(b->type&255)==5?6:1;
            nvn_texlayout layout;nvn_tex_layout(&image.desc,&layout);
            if(image.base_level<0 || image.levels<=0 || image.base_layer<0 || image.layers<=0 ||
               (unsigned)image.base_level>=image.desc.levels || (unsigned)image.levels>image.desc.levels-(unsigned)image.base_level ||
               (unsigned)image.base_layer>=layout.layers || (unsigned)image.layers>layout.layers-(unsigned)image.base_layer ||
               !render_add_image(images,count,&image)){valid=0;break;}
        }
    }
    pthread_mutex_unlock(&descriptor_lock);return valid;
}
static int render_bound_images(replay_state *s,dm_compute_image *images,unsigned *count) {
    *count=0;int valid=1;
    pthread_mutex_lock(&descriptor_lock);
    descriptors *pool=s->texture_pool?descriptor_find(s->texture_pool,0):NULL;
    for(unsigned stage=0;valid && stage<6;stage++)for(unsigned i=0;valid && i<160;i++) {
        uint64_t handle=i<128?s->texture[stage][i]:s->image[stage][i-128];
        unsigned id=(unsigned)handle&0xfffff;
        if(!pool || id>=(unsigned)pool->n || !pool->slots.textures[id].present)continue;
        texture_slot *slot=&pool->slots.textures[id];
        dm_compute_image image=replay_image(&slot->tex,slot->has_view?&slot->view:NULL);
        if(image.base_level<0 || image.levels<=0 || image.base_layer<0 || image.layers<=0 ||
           (unsigned)image.base_level>=image.desc.levels || (unsigned)image.levels>image.desc.levels-(unsigned)image.base_level ||
           !render_add_image(images,count,&image)){valid=0;break;}
    }
    pthread_mutex_unlock(&descriptor_lock);return valid;
}
static int render_attachments(render_epoch *e,const p_set_rt *rt,dm_compute_image *images,unsigned *n) {
    *n=0;if(rt->n<0 || rt->n>8)return 0;
    for(int i=0;i<rt->n;i++)if(rt->color[i]) {
        if(!render_image(e,rt->color[i],rt->has_cview[i]?&rt->cview[i]:NULL,&images[*n],1))return 0;(*n)++;
    }
    if(rt->depth){if(!render_image(e,rt->depth,rt->has_dview?&rt->dview:NULL,&images[*n],1))return 0;(*n)++;}
    return 1;
}
static int render_binding_op(unsigned op) {
    switch(op) {
    case OP_SET_RT:case OP_VIEWPORTS:case OP_SCISSOR:case OP_DEPTH_RANGE:
    case OP_BIND_PROGRAM:case OP_BIND_STATE:case OP_BIND_VATTR:case OP_BIND_VSTREAM:case OP_BIND_VBUF:
    case OP_BIND_UBO:case OP_BIND_SSBO:case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:
    case OP_STENCIL:case OP_POLY_OFFSET:case OP_BLEND_COLOR:case OP_COPY_STRIDE:return 1;
    default:return 0;
    }
}
static int render_storage_disjoint(const dm_compute_image *image,const render_epoch *e,uint64_t address,unsigned bytes) {
    if(!image->address)return 1;
    for(unsigned i=0;i<e->nw;i++)if(e->writes[i].serial==image->serial && e->writes[i].address==image->address)return 1;
    nvn_texlayout layout;nvn_tex_layout(&image->desc,&layout);
    if(!layout.size || image->address>UINT64_MAX-layout.size)return 0;
    uint64_t end=image->address+layout.size;
    if(address<end && image->address<address+bytes)return 0;
    for(unsigned i=0;i<e->nspan;i++)if(e->spans[i].start<end && image->address<e->spans[i].end)return 0;
    return 1;
}
static int render_span_add(render_epoch *e,uint64_t start,uint64_t end) {
    for(unsigned i=0;i<e->nspan;i++)if(start<e->spans[i].end && e->spans[i].start<end)return 0;
    for(unsigned i=0;i<e->nspan;i++) {
        if(start==e->spans[i].end){e->spans[i].end=end;return 1;}
        if(end==e->spans[i].start){e->spans[i].start=start;return 1;}
    }
    if(e->nspan==128)return 0;
    e->spans[e->nspan].start=start;e->spans[e->nspan++].end=end;return 1;
}
static void render_epoch_collect(render_epoch *e,replay_state *initial,uint64_t address,uint64_t budget) {
    e->count=e->index=e->nrt=e->nr=e->nw=e->np=e->nspan=e->nt=0;e->deferred=0;
    e->arena_used=0;replay_decoder_discard(&e->decoder);
    e->catalog=dm_vk_graphics_generation();e->descriptors=atomic_load(&descriptor_generation);
    replay_state s=*initial;dm_compute_image bound[RE_IMAGES];unsigned nb=0;int bindings_dirty=1;
    unsigned rt=0;e->targets[e->nrt++]=s.rt;
    replay_planning=1;
    while(e->count<RE_MAX && e->count<budget) {
        render_record *r=&e->records[e->count];memset(r,0,sizeof *r);
        uint8_t record[2048];
        if(!render_record_snapshot(e,address,record,&r->h)){render_stats.peek++;break;}
        if(r->h.size>RE_ARENA_BYTES-e->arena_used){render_stats.limit++;break;}
        r->bytes=e->arena+e->arena_used;memcpy(r->bytes,record,r->h.size);e->arena_used+=r->h.size;r->address=address;
        if(address>UINT64_MAX-r->h.size)break;r->next=address+r->h.size;
        int command_overlap=0;for(unsigned i=0;i<e->nw;i++)if(e->writes[i].address &&
            address<e->write_end[i] && e->writes[i].address<r->next){command_overlap=1;break;}
        if(command_overlap){render_stats.command_span++;render_stats.dependency++;break;}
        if(!render_span_add(e,address,r->next))break;
        replay_record.address=address;replay_record.size=r->h.size;replay_record.bytes=r->bytes;
        if(r->h.op==OP_JUMP) {
            if(r->h.size!=16)break;memcpy(&r->next,r->bytes+8,8);
        } else if(r->h.op==OP_BEGIN) {if(r->h.size!=8)break;
        } else if(r->h.op==OP_BARRIER) {
            uint32_t bits=0;if(r->h.size!=16)break;memcpy(&bits,r->bytes+8,4);if(bits!=0x12)break;
        } else if(render_binding_op(r->h.op)) {
            if(r->h.op==OP_SET_RT && e->nrt==128)break;
            replay_binding(&s,r->h.op,address+8,r->h.size);
            if(r->h.op==OP_SET_RT){rt=e->nrt;e->targets[e->nrt++]=s.rt;}
            if(r->h.op==OP_BIND_TEX || r->h.op==OP_BIND_IMG || r->h.op==OP_BIND_PROGRAM)bindings_dirty=1;
        } else if(r->h.op==OP_DRAW || r->h.op==OP_DRAW_IDX || r->h.op==OP_CLEAR_COLOR || r->h.op==OP_CLEAR_DS) {
            size_t payload=r->h.op==OP_DRAW?sizeof(p_draw):r->h.op==OP_DRAW_IDX?sizeof(p_draw_idx):
                r->h.op==OP_CLEAR_COLOR?sizeof(p_clear_color):sizeof(p_clear_ds);
            if(r->h.size!=((8+payload+7)&~(size_t)7))break;
            if(r->h.op==OP_CLEAR_COLOR) {
                p_clear_color p;memcpy(&p,r->bytes+8,sizeof p);
                if(p.mask!=15 || p.index<0 || p.index>=s.rt.n)break;
            }
            if(r->h.op==OP_CLEAR_DS) {
                p_clear_ds p;memcpy(&p,r->bytes+8,sizeof p);
                /* Partial stencil clears perform CPU read/modify/write in the
                 * backend and therefore are a producer/observer boundary. */
                if((p.stencil_mask&255) && (p.stencil_mask&255)!=255)break;
            }
            int draw=r->h.op==OP_DRAW || r->h.op==OP_DRAW_IDX,valid=1;
            if(draw)for(unsigned stage=0;stage<5;stage++)if((stage<2 || s.program[stage]) && !render_program(e,s.program[stage])){valid=0;break;}
            if(!valid){render_stats.program++;render_stats.dependency++;break;}
            if(draw && bindings_dirty){valid=render_draw_reads(e,&s,bound,&nb);bindings_dirty=0;}
            if(!valid){render_stats.read_view++;render_stats.dependency++;break;}
            dm_compute_image attachments[9];unsigned na=0;
            valid=render_attachments(e,&s.rt,attachments,&na);
            if(!valid){render_stats.attachment++;render_stats.dependency++;break;}
            for(unsigned i=0;i<na;i++) {
                if(!render_storage_disjoint(&attachments[i],e,address,r->h.size)){valid=0;break;}
            }
            if(!valid){render_stats.command_span++;render_stats.dependency++;break;}
            for(unsigned i=0;i<na;i++) {
                for(unsigned j=0;j<e->nr;j++)if(render_overlap(&attachments[i],&e->reads[j]))valid=0;
                if(draw)for(unsigned j=0;j<nb;j++)if(render_overlap(&attachments[i],&bound[j]))valid=0;
            }
            if(draw && valid)for(unsigned i=0;i<nb;i++)for(unsigned j=0;j<e->nw;j++)if(render_overlap(&bound[i],&e->writes[j]))valid=0;
            if(!valid){render_stats.resource_conflict++;render_stats.dependency++;break;}
            /* Capacity failure ends before this operation; partially accumulated
             * proof sets only make the accepted prefix more conservative. */
            for(unsigned i=0;i<na;i++) {
                unsigned before=e->nw;
                if(!render_add_image(e->writes,&e->nw,&attachments[i]))valid=0;
                if(before!=e->nw) {
                    nvn_texlayout storage;nvn_tex_layout(&attachments[i].desc,&storage);
                    e->write_end[before]=attachments[i].address+storage.size;
                }
            }
            if(draw)for(unsigned i=0;i<nb;i++)if(!render_add_image(e->reads,&e->nr,&bound[i]))valid=0;
            if(!valid)break;
        } else break;
        r->rt=rt;r->scissor=s.scissor;e->count++;address=r->next;
    }
    if(e->count==RE_MAX)render_stats.limit++;
    replay_planning=0;replay_record.bytes=NULL;
    replay_decoder_discard(&e->decoder); // No bytes survive a producer/observer edge.
    if(e->catalog!=dm_vk_graphics_generation() || e->descriptors!=atomic_load(&descriptor_generation))e->count=0;
    if(e->count){render_stats.epochs++;render_stats.records+=e->count;}
}
static void render_epoch_finish(render_epoch *e) {
    if(!e)return;
    if(e->deferred) {
        if(dm_vk_attachment_boundary() || dm_vk_gpu_barrier())die("Render IR terminal barrier failed");
        e->deferred=0;
    }
    e->count=e->index=0;
}
