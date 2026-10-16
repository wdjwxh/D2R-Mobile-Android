#include "vk_fuse_shader.h"
#include <math.h>
/* Local FUSE proofs use the same immutable snapshot/catalog layer as replay.
 * Guest vertex data is inspected only for a structurally eligible Q draw. */
static const dm_graphics_shader *fuse_shader(render_epoch *proof,replay_state *s,unsigned stage) {
    if(!render_program(proof,s->program[stage]))return NULL;
    for(unsigned i=0;i<proof->np;i++)if(proof->programs[i]==s->program[stage])return proof->shaders[i][stage];
    return NULL;
}
static int fuse_targets(render_epoch *proof,const p_set_rt *rt,dm_fuse_targets *out) {
    memset(out,0,sizeof *out);if(rt->n<1 || rt->n>8)return 0;
    for(int i=0;i<rt->n;i++)if(rt->color[i]) {
        if(!render_image(proof,rt->color[i],rt->has_cview[i]?&rt->cview[i]:NULL,&out->color[i],1))return 0;
        if(out->color[i].desc.samples>1)return 0;out->color_mask|=1u<<i;
    }
    if(rt->depth) {
        if(!render_image(proof,rt->depth,rt->has_dview?&rt->dview:NULL,&out->depth,1) || out->depth.desc.samples>1)return 0;
        out->has_depth=1;
    }
    return out->color_mask!=0;
}
static int fuse_image_equal(const dm_compute_image *a,const dm_compute_image *b) {
    return a->serial==b->serial && a->base_level==b->base_level && a->base_layer==b->base_layer && a->format==b->format;
}
static int fuse_rt_equal(const p_set_rt *a,const p_set_rt *b) {
    if(a->n!=b->n || a->n<0 || a->n>8)return 0;
    for(int i=0;i<=a->n;i++) {
        const tex_t *x=i==a->n?a->depth:a->color[i],*y=i==a->n?b->depth:b->color[i];
        if(!x || !y){if(x!=y)return 0;continue;}
        const view_t *xv=i==a->n?(a->has_dview?&a->dview:NULL):(a->has_cview[i]?&a->cview[i]:NULL);
        const view_t *yv=i==b->n?(b->has_dview?&b->dview:NULL):(b->has_cview[i]?&b->cview[i]:NULL);
        dm_compute_image xi=replay_image(x,xv),yi=replay_image(y,yv);
        if(!fuse_image_equal(&xi,&yi) || xi.layers!=yi.layers)return 0;
    }
    return 1;
}
static int fuse_attr(replay_state *s,int attribute,const p_draw *dw,const p_draw_idx *di,float values[64][4]) {
    int n=dw?dw->count:di->count;
    if(n<1 || n>64 || (dw && dw->first<0) || (di && (di->itype<1 || di->itype>2 || di->instances!=1)) ||
       attribute<0 || attribute>=s->nattrib || attribute>=16)return 0;
    va_st a;memcpy(&a,&s->attrib[attribute],sizeof a);
    if(a.stream>=16)return 0;
    const vs_st *stream=&s->stream[a.stream];const NVNbufferRange *vb=&s->vertex[a.stream];
    unsigned components=a.fmt==NVN_FORMAT_RG32F?2:a.fmt==NVN_FORMAT_RGB32F?3:a.fmt==NVN_FORMAT_RGBA32F?4:0;
    if(!components || !stream->stride || stream->divisor || !vb->address)return 0;
    uint8_t indices[64*4];unsigned size=di?(di->itype==1?2:4):0;
    if(di && dm_vk_guest_range_peek(di->ibuf,indices,(size_t)n*size))return 0;
    for(int i=0;i<n;i++) {
        uint32_t index=0;if(di)memcpy(&index,indices+i*size,size);
        int64_t vertex=dw?(int64_t)dw->first+i:(int64_t)index+di->base_vertex;
        if(vertex<0 || (uint64_t)vertex>(UINT64_MAX-a.offset)/stream->stride)return 0;
        uint64_t offset=(uint64_t)vertex*stream->stride+a.offset,bytes=components*4;
        if(offset>vb->size || bytes>vb->size-offset || vb->address>UINT64_MAX-offset)return 0;
        values[i][0]=values[i][1]=values[i][2]=0;values[i][3]=1;
        if(dm_vk_guest_range_peek(vb->address+offset,values[i],bytes))return 0;
        for(unsigned j=0;j<4;j++)if(!isfinite(values[i][j]))return 0;
    }
    return 1;
}
static int fuse_samepixel(replay_state *s,const dm_graphics_shader *vs,uint32_t varyings,
                          const p_draw *dw,const p_draw_idx *di,unsigned width,unsigned height) {
    if(!vs || !vs->fuse || !varyings)return 0;
    const dm_fuse_raster *f=&vs->fuse->raster;
    int primitive=dw?dw->prim:di->prim;
    if(primitive!=NVN_DRAW_PRIMITIVE_TRIANGLES && primitive!=NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP)return 0;
    if(s->viewport[0][0]!=0 || s->viewport[0][1]!=0 || s->viewport[0][2]!=(float)width || s->viewport[0][3]!=(float)height ||
       s->scissor.x>0 || s->scissor.y>0 || (int64_t)s->scissor.x+s->scissor.w<width || (int64_t)s->scissor.y+s->scissor.h<height)return 0;
    if((f->xyz_sign[0]!=1 && f->xyz_sign[0]!=-1) || (f->xyz_sign[1]!=1 && f->xyz_sign[1]!=-1))return 0;
    float position[64][4],uv[64][4];if(!fuse_attr(s,f->pos_attr,dw,di,position))return 0;
    int n=dw?dw->count:di->count;
    for(unsigned location=0;location<32;location++)if(varyings&(1u<<location)) {
        if(!fuse_attr(s,f->uv_attr[location],dw,di,uv))return 0;
        for(int i=0;i<n;i++) {
            float x=position[i][0]*f->xyz_sign[0],y=position[i][1]*f->xyz_sign[1];
            float w=f->w_one?1:position[i][3];
            // SPIR-V facts describe final clip position. Negative Vulkan
            // viewport height maps final +Y to the top of the framebuffer.
            if(w!=1 || uv[i][0]!=(x+1)*0.5f || uv[i][1]!=(1-y)*0.5f)return 0;
        }
    }
    return 1;
}
static int fuse_q_proof(render_epoch *proof,replay_state *s,const p_draw *dw,const p_draw_idx *di,dm_fuse_plan *plan) {
    const dm_graphics_shader *vs=fuse_shader(proof,s,0),*fs=fuse_shader(proof,s,1);
    if(!vs || !fs || !fs->fuse || !fs->fuse->ninputs || fs->fuse->ninputs>8 || fs->nbindings>64 ||
       s->multisample.a2c || (s->multisample.enable && s->multisample.samples>1))return 0;
    for(unsigned stage=2;stage<5;stage++)if(s->program[stage] && !render_program(proof,s->program[stage]))return 0;
    uint32_t mask=0,varyings=0;dm_compute_image inputs[8]={0};int valid=1;
    pthread_mutex_lock(&descriptor_lock);
    for(unsigned stage=0;valid && stage<2;stage++) {
        const dm_graphics_shader *sh=stage?fs:vs;
        for(unsigned k=0;k<sh->nbindings;k++) {
            const dm_shader_binding *binding=&sh->bindings[k];
            if(binding->kind!=DM_BIND_TEXTURE && binding->kind!=DM_BIND_IMAGE)continue;
            if(binding->slot<0 || binding->slot>=(binding->kind==DM_BIND_IMAGE?32:128) || binding->sampler_slot>=128){valid=0;break;}
            dm_compute_image image;dispatch_image_locked(s,binding,&image,stage);
            if(binding->kind==DM_BIND_IMAGE)image.levels=1;
            if((binding->type&255)==4)image.layers=1;
            else if(!(binding->type&256))image.layers=(binding->type&255)==5?6:1;
            for(unsigned j=0;j<8;j++)if((plan->targets[1].color_mask&(1u<<j)) && render_overlap(&image,&plan->targets[1].color[j]))valid=0;
            if(plan->targets[1].has_depth && render_overlap(&image,&plan->targets[1].depth))valid=0;
            if(!valid)break;
            int attachment=-1;
            for(unsigned j=0;j<8;j++)if((plan->targets[0].color_mask&(1u<<j)) && render_overlap(&image,&plan->targets[0].color[j])){attachment=(int)j;break;}
            if(plan->targets[0].has_depth && render_overlap(&image,&plan->targets[0].depth)){valid=0;break;}
            if(attachment<0)continue;
            if(!stage || binding->kind!=DM_BIND_TEXTURE || (binding->type&255)!=3 || (binding->type&~255u) ||
               image.base_level || image.base_layer || image.compare || image.lod_bias!=0 || image.lod_min>0 ||
               !isfinite(image.lod_min) || !isfinite(image.lod_max) || !isfinite(image.aniso) || image.aniso>1 ||
               (image.minf>=2 && image.lod_max<0) || !fuse_image_equal(&image,&plan->targets[0].color[attachment])){valid=0;break;}
            if((image.format>=0x69 && image.format<=0x78) || image.format==0x37 || image.format==0x99 || image.format==0x9b){valid=0;break;}
            const dm_fuse_input *input=NULL;
            for(unsigned j=0;j<fs->fuse->ninputs;j++)if(fs->fuse->inputs[j].binding_index==k)input=&fs->fuse->inputs[j];
            if(!input || input->input_index>=8 || !input->varying_mask){valid=0;break;}
            mask|=1u<<input->input_index;varyings|=input->varying_mask;inputs[input->input_index]=plan->targets[0].color[attachment];
        }
    }
    pthread_mutex_unlock(&descriptor_lock);
    if(!valid || !mask || !dm_vk_graphics_fuse_variant(fs,mask))return 0;
    unsigned first=0;while(first<8 && !(mask&(1u<<first)))first++;
    unsigned width=inputs[first].desc.width,height=inputs[first].desc.height;
    if(!width || !height || !fuse_samepixel(s,vs,varyings,dw,di,width,height))return 0;
    for(unsigned j=0;j<8;j++)if(mask&(1u<<j))if(inputs[j].base_level || inputs[j].base_layer || inputs[j].desc.width!=width || inputs[j].desc.height!=height)return 0;
    plan->q_shader=fs;plan->q_mask=mask;memcpy(plan->inputs,inputs,sizeof inputs);return 1;
}
