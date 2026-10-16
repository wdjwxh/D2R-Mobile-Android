/* Persistent per-queue NVN bindings, ready for Vulkan pipeline/descriptor
 * consumption. Updating bindings is not a draw and issues no GPU work. */
typedef struct replay_state {
    struct replay_state *next;
    queue_t *owner;
    p_set_rt rt;
    float viewport[16][6]; p_scissor scissor; p_depth_range depth;
    const prog_t *program[6];
    uint64_t args_version[6];
    uint64_t args_static_version[6]; // Everything except ordinary UBO bindings.
    blend_st blend[8]; color_st color; chmask_st channels; ms_st multisample;
    poly_st polygon; ds_st depth_stencil;
    uint32_t attrib[16]; vs_st stream[16]; NVNbufferRange vertex[16]; int nattrib,nstream;
    p_bind_buf uniform[6][16],storage[6][16];
    uint64_t texture[6][128],sampler[6][128],image[6][32];
    p_stencil stencil[2]; p_poly_offset offset; p_blend_color blend_color;
    int64_t copy_stride;
    const dpool_t *texture_pool,*sampler_pool;
} replay_state;
static replay_state *replay_states;
static void replay_epochs_free(void);
// Like upstream g_args_dirty: compare only the binding being changed. A global
// serial also distinguishes queues and freed/recreated replay-state objects.
static uint64_t replay_args_clock;
static _Thread_local int replay_planning;
static void replay_args_changed(replay_state *s,unsigned stage) {
    if(replay_planning)return;
    if(!++replay_args_clock)die("NVN argument version exhausted");
    s->args_version[stage]=replay_args_clock;
    s->args_static_version[stage]=replay_args_clock;
}
static void replay_ubo_args_changed(replay_state *s,unsigned stage) {
    if(replay_planning)return;
    if(!++replay_args_clock)die("NVN argument version exhausted");
    s->args_version[stage]=replay_args_clock;
}
static void replay_args_changed_all(replay_state *s) {
    for(unsigned i=0;i<6;i++)replay_args_changed(s,i);
}
static void replay_state_free(queue_t *owner) {
    pthread_mutex_lock(&replay_lock);
    if(!owner)replay_epochs_free();
    replay_state **p=&replay_states;
    while(*p) {
        replay_state *s=*p;
        if(!owner || s->owner==owner) { *p=s->next;free(s); }
        else p=&s->next;
    }
    pthread_mutex_unlock(&replay_lock);
}
static replay_state *replay_state_get(queue_t *owner) {
    for(replay_state *s=replay_states;s;s=s->next) if(s->owner==owner)return s;
    replay_state *s=calloc(1,sizeof *s);if(!s)die("NVN replay state allocation failed");
    s->owner=owner;s->next=replay_states;replay_states=s;
    replay_args_changed_all(s);
    s->depth=(p_depth_range){0,1};
    for(int i=0;i<8;i++)s->blend[i]=(blend_st){i,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO,NVN_BLEND_EQUATION_ADD,NVN_BLEND_EQUATION_ADD};
    s->color=(color_st){0,NVN_LOGIC_OP_COPY,NVN_ALPHA_FUNC_ALWAYS};s->channels.mask=0xffffffffu;
    s->polygon=(poly_st){NVN_FACE_NONE,NVN_FRONT_FACE_CCW,NVN_POLYGON_MODE_FILL,0};
    s->depth_stencil=(ds_st){0,1,NVN_DEPTH_FUNC_LESS,0,NVN_STENCIL_FUNC_ALWAYS,NVN_STENCIL_FUNC_ALWAYS,1,1,1,1,1,1};
    for(int i=0;i<2;i++)s->stencil[i].value_mask=s->stencil[i].mask=0xff;
    for(int i=0;i<16;i++)s->viewport[i][5]=1;
    return s;
}
static void state_read(uint64_t a,uint32_t record_size,void *out,size_t n) {
    if(record_size!=((8+n+7)&~(size_t)7))die("Invalid NVN state record size");
    replay_read(a,out,n);
}
static void state_range(int first,int n,int cap) {
    if(first<0 || n<0 || first>cap || n>cap-first)die("NVN binding index out of range");
}
static int replay_binding(replay_state *s,uint32_t op,uint64_t a,uint32_t bytes) {
    switch(op) {
    case OP_SET_RT:
        if(!replay_planning && dm_vk_attachment_boundary())die("Vulkan render-target clear boundary failed");
        state_read(a,bytes,&s->rt,sizeof s->rt);state_range(0,s->rt.n,8);break;
    case OP_VIEWPORTS: {
        p_viewports p;state_read(a,bytes,&p,sizeof p);state_range(p.first,p.n,16);
        for(int i=0;i<p.n;i++)memcpy(s->viewport[p.first+i],p.v[i],4*sizeof(float));break;
    }
    case OP_SCISSOR: state_read(a,bytes,&s->scissor,sizeof s->scissor);break;
    case OP_DEPTH_RANGE:
        state_read(a,bytes,&s->depth,sizeof s->depth);
        for(int i=0;i<16;i++){s->viewport[i][4]=s->depth.n;s->viewport[i][5]=s->depth.f;}break;
    case OP_BIND_PROGRAM: {
        p_bind_program p;state_read(a,bytes,&p,sizeof p);
        for(int i=0;i<6;i++)if((p.stages&(1<<i)) && s->program[i]!=p.prog) {
            s->program[i]=p.prog;replay_args_changed(s,i);
        }
        break;
    }
    case OP_BIND_STATE: {
        p_bind_state p;state_read(a,bytes,&p,sizeof p);
        switch(p.kind) {
        case 0: {blend_st b;memcpy(&b,p.bytes,sizeof b);state_range(b.target,1,8);s->blend[b.target]=b;break;}
        case 1:memcpy(&s->color,p.bytes,sizeof s->color);break;
        case 2:memcpy(&s->channels,p.bytes,sizeof s->channels);break;
        case 3:memcpy(&s->multisample,p.bytes,sizeof s->multisample);break;
        case 4:memcpy(&s->polygon,p.bytes,sizeof s->polygon);break;
        case 5:memcpy(&s->depth_stencil,p.bytes,sizeof s->depth_stencil);break;
        default:die("Invalid NVN state kind");
        }break;
    }
    case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: {
        int32_t range[2];if(bytes<16)die("Truncated NVN vertex binding");replay_read(a,range,8);
        state_range(range[0],range[1],16);
        size_t element=op==OP_BIND_VATTR?4:op==OP_BIND_VSTREAM?sizeof(vs_st):sizeof(NVNbufferRange);
        uint8_t payload[8+16*sizeof(NVNbufferRange)];size_t n=8+range[1]*element;
        state_read(a,bytes,payload,n);
        void *dst=op==OP_BIND_VATTR?(void *)&s->attrib[range[0]]:op==OP_BIND_VSTREAM?(void *)&s->stream[range[0]]:(void *)&s->vertex[range[0]];
        memcpy(dst,payload+8,range[1]*element);
        if(op==OP_BIND_VATTR && s->nattrib<range[0]+range[1])s->nattrib=range[0]+range[1];
        if(op==OP_BIND_VSTREAM && s->nstream<range[0]+range[1])s->nstream=range[0]+range[1];
        break;
    }
    case OP_BIND_UBO: case OP_BIND_SSBO: {
        p_bind_buf p;state_read(a,bytes,&p,sizeof p);state_range(p.stage,1,6);state_range(p.index,1,16);
        p_bind_buf *dst=op==OP_BIND_UBO?&s->uniform[p.stage][p.index]:&s->storage[p.stage][p.index];
        if(memcmp(dst,&p,sizeof p)) {
            *dst=p;
            if(op==OP_BIND_UBO)replay_ubo_args_changed(s,p.stage);else replay_args_changed(s,p.stage);
        }break;
    }
    case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: {
        p_bind_handles p={0};
        if(bytes<8+offsetof(p_bind_handles,h))die("Truncated NVN descriptor binding");
        if((bytes&7) || bytes>8+sizeof p)die("Invalid NVN descriptor record size");
        // Materialize only this record's payload, including any GPU producers.
        // Validate and consume the same snapshot; a second payload read could
        // replace stage/count fields after their bounds had already been checked.
        replay_read(a,&p,bytes-8);
        state_range(p.stage,1,6);state_range(p.first,p.n,op==OP_BIND_IMG?32:128);
        if(p.n>32)die("NVN descriptor record exceeds payload capacity");
        size_t payload=offsetof(p_bind_handles,h)+(size_t)p.n*8;
        if(bytes!=((8+payload+7)&~(size_t)7))die("Invalid NVN state record size");
        uint64_t *dst=op==OP_BIND_TEX?s->texture[p.stage]:op==OP_BIND_SAMP?s->sampler[p.stage]:s->image[p.stage];
        if(memcmp(dst+p.first,p.h,p.n*8)) {
            memcpy(dst+p.first,p.h,p.n*8);replay_args_changed(s,p.stage);
        }break;
    }
    case OP_STENCIL: {
        p_stencil p;state_read(a,bytes,&p,sizeof p);
        if(p.face<NVN_FACE_FRONT || p.face>NVN_FACE_FRONT_AND_BACK || p.which<0 || p.which>2)die("Invalid NVN stencil binding");
        for(int i=0;i<2;i++)if(p.face==NVN_FACE_FRONT_AND_BACK || p.face==(i?NVN_FACE_BACK:NVN_FACE_FRONT)) {
            if(p.which==0)s->stencil[i].value_mask=p.value_mask;
            if(p.which==1)s->stencil[i].mask=p.mask;
            if(p.which==2)s->stencil[i].ref=p.ref;
        }
        break;
    }
    case OP_POLY_OFFSET:state_read(a,bytes,&s->offset,sizeof s->offset);break;
    case OP_BLEND_COLOR:state_read(a,bytes,&s->blend_color,sizeof s->blend_color);break;
    case OP_COPY_STRIDE:state_read(a,bytes,&s->copy_stride,sizeof s->copy_stride);break;
    case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL: {
        p_set_pool p;state_read(a,bytes,&p,sizeof p);
        const dpool_t **dst=op==OP_SET_TEX_POOL?&s->texture_pool:&s->sampler_pool;
        if(*dst!=p.pool){*dst=p.pool;replay_args_changed_all(s);}break;
    }
    default:return 0;
    }
    return 1;
}
