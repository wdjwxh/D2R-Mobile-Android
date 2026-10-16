/* A local P -> Q candidate, captured only once per canonical target segment.
 * The expensive vertex proof runs only after Q has a convertible fragment
 * shader. Failed candidates retain their safe owned prefix for ordinary replay. */
typedef struct {
    render_epoch proof;size_t record_capacity,byte_capacity;
    replay_state initial;p_set_rt attempted,qrt;int attempted_valid,phase;
    dm_fuse_plan plan;uint64_t token;int barrier;
} fuse_context;
static fuse_context *fuse_contexts[32];
static size_t fuse_memory;
static int fuse_barrier_supported(uint32_t bits) {
    // Primitive ordering adds no observable shader side effect in the proven
    // readonly P/Q scope; attachment order remains covered by subpass deps.
    return bits==0x12 || bits==0x13;
}
static int fuse_binding_op(unsigned op) {
    return render_binding_op(op) || op==OP_SET_TEX_POOL || op==OP_SET_SAMP_POOL;
}
static struct {uint64_t attempts,no_facts,proof,dependency,accepted;} fuse_stats;
static void fuse_report(void) {
    LOG("FUSE planner: attempts=%llu qNoFacts=%llu samePixelOrViewReject=%llu dependencyReject=%llu accepted=%llu",
        (unsigned long long)fuse_stats.attempts,(unsigned long long)fuse_stats.no_facts,
        (unsigned long long)fuse_stats.proof,(unsigned long long)fuse_stats.dependency,(unsigned long long)fuse_stats.accepted);
    memset(&fuse_stats,0,sizeof fuse_stats);
}
static fuse_context *fuse_context_get(unsigned depth) {
    if(fuse_contexts[depth])return fuse_contexts[depth];
    if(sizeof(fuse_context)>RE_MEMORY_BUDGET-fuse_memory)return NULL;
    fuse_context *f=calloc(1,sizeof *f);if(!f)return NULL;
    fuse_memory+=sizeof *f;return fuse_contexts[depth]=f;
}
static void replay_fuse_free(void) {
    for(unsigned i=0;i<32;i++)if(fuse_contexts[i]) {
        free(fuse_contexts[i]->proof.records);free(fuse_contexts[i]->proof.arena);free(fuse_contexts[i]);fuse_contexts[i]=NULL;
    }
    fuse_memory=0;
}
static render_record *fuse_record_append(fuse_context *f,uint64_t address,const uint8_t *bytes,op_hdr h,unsigned rt,p_scissor scissor) {
    render_epoch *e=&f->proof;
    if(e->count==RE_MAX || h.size>RE_ARENA_BYTES-e->arena_used)return NULL;
    if(e->count==f->record_capacity) {
        size_t capacity=f->record_capacity?f->record_capacity*2:128;if(capacity>RE_MAX)capacity=RE_MAX;
        size_t extra=(capacity-f->record_capacity)*sizeof(render_record);
        if(extra>RE_MEMORY_BUDGET-fuse_memory)return NULL;
        void *grown=realloc(e->records,capacity*sizeof(render_record));if(!grown)return NULL;
        e->records=grown;f->record_capacity=capacity;fuse_memory+=extra;
    }
    if(h.size>f->byte_capacity-e->arena_used) {
        size_t capacity=f->byte_capacity?f->byte_capacity*2:16384;
        while(capacity<e->arena_used+h.size)capacity*=2;if(capacity>RE_ARENA_BYTES)capacity=RE_ARENA_BYTES;
        if(capacity-f->byte_capacity>RE_MEMORY_BUDGET-fuse_memory)return NULL;
        uintptr_t old=(uintptr_t)e->arena;uint8_t *grown=realloc(e->arena,capacity);if(!grown)return NULL;
        for(unsigned i=0;i<e->count;i++)e->records[i].bytes=grown+((uintptr_t)e->records[i].bytes-old);
        e->arena=grown;fuse_memory+=capacity-f->byte_capacity;f->byte_capacity=capacity;
    }
    render_record *r=&e->records[e->count++];
    *r=(render_record){.address=address,.next=address+h.size,.h=h,.rt=rt,.scissor=scissor,.bytes=e->arena+e->arena_used};
    memcpy(r->bytes,bytes,h.size);e->arena_used+=h.size;
    if(h.op==OP_JUMP)memcpy(&r->next,bytes+8,8);
    return r;
}
static void fuse_end(fuse_context *f) {
    if(!f)return;
    if(f->token && dm_vk_fuse_end(f->token)<0)die("Vulkan FUSE end failed");
    f->token=0;f->phase=0;
    if(f->barrier && dm_vk_gpu_barrier())die("Vulkan FUSE terminal dependency failed");
    f->barrier=0;
}
static int fuse_plan_clear(fuse_context *f,render_record *r) {
    dm_compute_image image;if(!render_clear_image(&f->proof,r,&image))return 0;
    for(unsigned j=0;j<8;j++)if((f->plan.targets[0].color_mask&(1u<<j)) && render_overlap(&image,&f->plan.targets[0].color[j]))return 0;
    if(f->plan.targets[0].has_depth && render_overlap(&image,&f->plan.targets[0].depth))return 0;
    if(r->h.op==OP_CLEAR_COLOR) {
        p_clear_color p;memcpy(&p,r->bytes+8,sizeof p);f->plan.q_clear_colors|=1u<<p.index;memcpy(f->plan.q_clear_rgba[p.index],p.rgba,16);
    } else {
        p_clear_ds p;memcpy(&p,r->bytes+8,sizeof p);
        if(p.depth_mask){f->plan.q_clear_depth=1;f->plan.q_depth=p.depth;}
        if(p.stencil_mask&255){f->plan.q_clear_stencil=1;f->plan.q_stencil=p.stencil&255;}
    }
    return 1;
}
static int fuse_p_reads_safe(fuse_context *f,replay_state *s) {
    dm_compute_image reads[RE_IMAGES];unsigned n=0;
    if(!render_draw_reads(&f->proof,s,reads,&n))return 0;
    for(unsigned i=0;i<n;i++)for(unsigned pass=0;pass<2;pass++) {
        const dm_fuse_targets *targets=&f->plan.targets[pass];
        for(unsigned j=0;j<8;j++)if((targets->color_mask&(1u<<j)) && render_overlap(&reads[i],&targets->color[j]))return 0;
        if(targets->has_depth && render_overlap(&reads[i],&targets->depth))return 0;
    }
    return 1;
}
static void fuse_try_capture(fuse_context *f,replay_state *initial,uint64_t address,const uint8_t *first,op_hdr header,uint64_t budget) {
    fuse_stats.attempts++;
    render_epoch *e=&f->proof;
    e->count=e->index=e->arena_used=e->nr=e->nw=e->np=e->nt=e->nspan=0;e->nrt=2;
    e->catalog=dm_vk_graphics_generation();e->descriptors=atomic_load(&descriptor_generation);
    f->initial=*initial;f->attempted=initial->rt;f->attempted_valid=1;
    memset(&f->plan,0,sizeof f->plan);e->targets[0]=initial->rt;
    if(!fuse_targets(e,&initial->rt,&f->plan.targets[0]))return;
    replay_state s=*initial;replay_decoder decoder;replay_decoder_discard(&decoder);
    unsigned phase=0;int candidate=0;uint8_t record[2048];op_hdr h=header;
    replay_planning=1;
    for(unsigned guard=0;guard<RE_MAX && guard<=budget;guard++) {
        if(!guard){if(h.size<8 || h.size>sizeof record)break;memcpy(record,first,h.size);}
        else if(!replay_decoder_snapshot(&decoder,address,record,&h)) {
            if(dm_vk_guest_record_peek(address,record,sizeof record))break;
            memcpy(&h,record,8);
        }
        if(h.size<8 || (h.size&7) || h.size>sizeof record || address>UINT64_MAX-h.size)break;
        if(h.op==OP_JUMP && h.size!=16)break;
        int draw=h.op==OP_DRAW || h.op==OP_DRAW_IDX;
        int clear=h.op==OP_CLEAR_COLOR || h.op==OP_CLEAR_DS;
        if(h.op==OP_BARRIER) {uint32_t bits=0;if(h.size!=16)break;memcpy(&bits,record+8,4);if(!fuse_barrier_supported(bits))break;}
        else if(h.op!=OP_BEGIN && h.op!=OP_JUMP && !draw && !clear && !fuse_binding_op(h.op))break;
        if(h.op==OP_BEGIN && h.size!=8)break;
        if(clear && !phase)break;
        if(!render_span_add(e,address,address+h.size))break;
        int storage_ok=1;
        for(unsigned j=0;j<8;j++)if((f->plan.targets[0].color_mask&(1u<<j)) &&
            !render_storage_disjoint(&f->plan.targets[0].color[j],e,address,h.size))storage_ok=0;
        if(f->plan.targets[0].has_depth && !render_storage_disjoint(&f->plan.targets[0].depth,e,address,h.size))storage_ok=0;
        if(!storage_ok)break;
        replay_record.address=address;replay_record.size=h.size;replay_record.bytes=record;
        if(fuse_binding_op(h.op)) {
            if(h.op==OP_SET_RT) {
                p_set_rt rt;state_read(address+8,h.size,&rt,sizeof rt);
                if(!fuse_rt_equal(&s.rt,&rt)) {
                    if(phase)break;phase=1;e->targets[1]=rt;f->qrt=rt;
                    if(!fuse_targets(e,&rt,&f->plan.targets[1]))break;
                }
            }
            replay_binding(&s,h.op,address+8,h.size);
        }
        if(draw) {
            size_t payload=h.op==OP_DRAW?sizeof(p_draw):sizeof(p_draw_idx);
            if(h.size!=((8+payload+7)&~(size_t)7))break;
            int readonly=fuse_shader(e,&s,0)!=NULL && fuse_shader(e,&s,1)!=NULL;
            for(unsigned stage=2;readonly && stage<5;stage++)if(s.program[stage] && !render_program(e,s.program[stage]))readonly=0;
            if(!readonly)break;
        }
        render_record *r=fuse_record_append(f,address,record,h,phase,s.scissor);if(!r)break;
        if(clear && !fuse_plan_clear(f,r))break;
        if(draw && phase) {
            const dm_graphics_shader *fs=fuse_shader(e,&s,1);
            if(!fs || !fs->fuse || !fs->fuse->ninputs){fuse_stats.no_facts++;break;}
            p_draw dw;p_draw_idx di;
            if(h.op==OP_DRAW)memcpy(&dw,r->bytes+8,sizeof dw);else memcpy(&di,r->bytes+8,sizeof di);
            candidate=fuse_q_proof(e,&s,h.op==OP_DRAW?&dw:NULL,h.op==OP_DRAW_IDX?&di:NULL,&f->plan);
            if(!candidate)fuse_stats.proof++;
            break;
        }
        address=r->next;
    }
    int had_proof=candidate;
    if(candidate) {
        s=f->initial;
        for(unsigned i=0;candidate && i<e->count;i++) {
            render_record *r=&e->records[i];
            replay_record.address=r->address;replay_record.size=r->h.size;replay_record.bytes=r->bytes;
            if(fuse_binding_op(r->h.op))replay_binding(&s,r->h.op,r->address+8,r->h.size);
            if(!r->rt && (r->h.op==OP_DRAW || r->h.op==OP_DRAW_IDX))candidate=fuse_p_reads_safe(f,&s);
        }
        for(unsigned j=0;candidate && j<8;j++)if(f->plan.targets[1].color_mask&(1u<<j))
            candidate=render_storage_disjoint(&f->plan.targets[1].color[j],e,0,0);
        if(candidate && f->plan.targets[1].has_depth)candidate=render_storage_disjoint(&f->plan.targets[1].depth,e,0,0);
    }
    replay_planning=0;replay_record.bytes=NULL;
    if(had_proof && !candidate)fuse_stats.dependency++;
    if(!candidate || e->catalog!=dm_vk_graphics_generation() || e->descriptors!=atomic_load(&descriptor_generation))return;
    int result=dm_vk_fuse_begin(&f->plan,&f->token);
    if(result<0)die("Vulkan FUSE begin failed");
    if(result){f->token=0;return;}
    f->phase=1;fuse_stats.accepted++;int q_entered=0;
    for(unsigned i=0;i<e->count;i++) {
        render_record *r=&e->records[i];
        if(r->h.op==OP_SET_RT){r->skip=r->rt && !q_entered?3:4;if(r->rt)q_entered=1;}
        else if(r->h.op==OP_BARRIER)r->skip=2;
        else if(r->rt && (r->h.op==OP_CLEAR_COLOR || r->h.op==OP_CLEAR_DS))r->skip=1;
    }
}
static int fuse_q_matches(fuse_context *f,replay_state *s,const p_draw *dw,const p_draw_idx *di) {
    if(!fuse_rt_equal(&s->rt,&f->qrt))return 0;
    dm_fuse_plan next=f->plan;
    if(!fuse_q_proof(&f->proof,s,dw,di,&next) || next.q_shader!=f->plan.q_shader || next.q_mask!=f->plan.q_mask)return 0;
    for(unsigned i=0;i<8;i++)if((next.q_mask&(1u<<i)) && !fuse_image_equal(&next.inputs[i],&f->plan.inputs[i]))return 0;
    return 1;
}
