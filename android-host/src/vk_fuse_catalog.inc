/* SH4 typed facts and offline input-attachment modules. Included after
 * graphics_entry, before catalog lookup; all mutations occur under lock. */
static size_t graphics_fuse_bytes;
#define GRAPHICS_FUSE_BUDGET (64u*1024u*1024u)

static void graphics_fuse_free(graphics_entry *entry) {
    if(!entry->shader.fuse)return;
    const dm_fuse_shader_info *info=entry->shader.fuse;
    size_t bytes=sizeof(*info)+(size_t)info->nvariants*sizeof(dm_graphics_shader);
    if(graphics_fuse_bytes>=bytes)graphics_fuse_bytes-=bytes;
    free((void *)info->variants);free((void *)info);entry->shader.fuse=NULL;
}

static int graphics_fuse_parse(graphics_entry *entry,const unsigned char *raw,size_t size,size_t tail) {
    size_t aligned=(tail+3)&~(size_t)3;
    if(aligned>size || size-aligned<188)return -1;
    for(size_t i=tail;i<aligned;i++)if(raw[i])return -1;
    const unsigned char *data=raw+aligned;size_t available=size-aligned;
    uint32_t h[47];memcpy(h,data,sizeof h);
    if(h[0]!=0x31534656u || h[1]!=1 || h[2]!=available || h[3]>1 ||
       (int32_t)h[4]<-1 || (int32_t)h[4]>=16 || h[5]>1 ||
       h[41]>8 || h[42]!=((1u<<h[41])-1u) || h[43]>1 || h[44]>1 || h[45]>1 || h[46]>255)return -1;
    for(unsigned i=6;i<9;i++)if((int32_t)h[i]!=1 && (int32_t)h[i]!=-1)return -1;
    int useful=(int32_t)h[4]>=0 || h[41]!=0;
    for(unsigned i=9;i<41;i++){if((int32_t)h[i]<-1 || (int32_t)h[i]>=16)return -1;useful|=(int32_t)h[i]>=0;}
    if(entry->shader.stage==0 && h[41])return -1;
    if(entry->shader.stage==1){
        if((int32_t)h[4]!=-1)return -1;
        for(unsigned i=9;i<41;i++)if((int32_t)h[i]!=-1)return -1;
    }
    size_t table_end=188+(size_t)h[41]*16+(size_t)h[42]*12;
    if(table_end>available)return -1;
    dm_fuse_input inputs[8]={{0}};uint64_t selected=0;
    for(unsigned i=0;i<h[41];i++){
        uint32_t in[4];memcpy(in,data+188+i*16,sizeof in);
        if(in[0]>=entry->shader.nbindings || in[0]>=64 || in[1]!=i || !in[2] || in[3] ||
           (selected&(UINT64_C(1)<<in[0])) || entry->shader.bindings[in[0]].kind!=DM_BIND_TEXTURE)return -1;
        for(unsigned j=0;j<i;j++)if(entry->shader.bindings[inputs[j].binding_index].binding==entry->shader.bindings[in[0]].binding)return -1;
        selected|=UINT64_C(1)<<in[0];inputs[i]=(dm_fuse_input){in[0],in[1],in[2]};
    }
    /* Validate every offset/size before allocating or exposing any pointer. */
    size_t expected=table_end;
    for(unsigned i=0;i<h[42];i++){
        uint32_t v[3];memcpy(v,data+188+h[41]*16+i*12,sizeof v);
        if(v[0]!=i+1 || v[1]!=expected || v[2]<20 || (v[2]&3) || v[2]>available-expected)return -1;
        uint32_t magic;memcpy(&magic,data+v[1],4);if(magic!=0x07230203u)return -1;
        const uint32_t *code=(const uint32_t *)(data+v[1]);
        if(!code[3] || code[3]>1000000 || code[4])return -1;
        for(size_t at=5;at<v[2]/4;){unsigned count=code[at]>>16;if(!count || count>v[2]/4-at)return -1;at+=count;}
        expected+=v[2];
    }
    if(expected!=available)return -1;
    entry->shader.guarded_uniform_abi=h[3];
    if(!useful)return 0;
    size_t bytes=sizeof(dm_fuse_shader_info)+(size_t)h[42]*sizeof(dm_graphics_shader);
    if(bytes>GRAPHICS_FUSE_BUDGET || graphics_fuse_bytes>GRAPHICS_FUSE_BUDGET-bytes)return 0;
    dm_fuse_shader_info *info=calloc(1,sizeof *info);
    dm_graphics_shader *variants=h[42]?calloc(h[42],sizeof *variants):NULL;
    if(!info || (h[42] && !variants)){free(info);free(variants);return 0;}
    info->raster.pos_attr=(int32_t)h[4];info->raster.w_one=(int32_t)h[5];
    for(unsigned i=0;i<3;i++)info->raster.xyz_sign[i]=(int32_t)h[6+i];
    for(unsigned i=0;i<32;i++)info->raster.uv_attr[i]=(int32_t)h[9+i];
    info->raster.may_discard=h[43];info->raster.writes_sample_mask=h[44];
    info->raster.writes_frag_depth=h[45];info->raster.color_mask=h[46];
    info->ninputs=h[41];info->nvariants=h[42];memcpy(info->inputs,inputs,sizeof inputs);info->variants=variants;
    entry->shader.fuse=info;
    for(unsigned i=0;i<h[42];i++){
        uint32_t v[3];memcpy(v,data+188+h[41]*16+i*12,sizeof v);
        variants[i]=entry->shader;variants[i].code=(const uint32_t *)(data+v[1]);variants[i].code_size=v[2];variants[i].fuse_mask=v[0];
        for(unsigned n=0;n<info->ninputs;n++)if(v[0]&(1u<<n))variants[i].bindings[inputs[n].binding_index].kind=DM_BIND_INPUT_ATTACHMENT;
    }
    graphics_fuse_bytes+=bytes;return 0;
}

const dm_graphics_shader *dm_vk_graphics_fuse_variant(const dm_graphics_shader *base,uint32_t mask) {
    if(!base || base->fuse_mask)return NULL;
    if(!mask)return base;
    const dm_fuse_shader_info *info=base->fuse;
    if(!info || base->stage!=1 || mask>info->nvariants || !info->variants)return NULL;
    const dm_graphics_shader *variant=&info->variants[mask-1];
    return variant->fuse_mask==mask?variant:NULL;
}
