/* Upstream hoist_try: P draws -> <=4 clear-only groups -> P. All decisions
 * refer to owned IR; descriptor and shader dependencies were closed before
 * execution. The entry barrier stays in place for preceding GPU producers. */
static int render_target_slot_same(render_epoch *e,const tex_t *a,const view_t *av,const tex_t *b,const view_t *bv) {
    if(!a || !b)return a==b;
    dm_compute_image x,y;
    if(!render_image(e,a,av,&x,1) || !render_image(e,b,bv,&y,1))return 0;
    return x.serial==y.serial && x.base_level==y.base_level && x.base_layer==y.base_layer && x.format==y.format;
}
static int render_targets_same(render_epoch *e,const p_set_rt *a,const p_set_rt *b) {
    if(a->n!=b->n || a->n<0 || a->n>8)return 0;
    for(int i=0;i<a->n;i++)if(!render_target_slot_same(e,a->color[i],a->has_cview[i]?&a->cview[i]:NULL,
        b->color[i],b->has_cview[i]?&b->cview[i]:NULL))return 0;
    return render_target_slot_same(e,a->depth,a->has_dview?&a->dview:NULL,b->depth,b->has_dview?&b->dview:NULL);
}
static int render_clear_image(render_epoch *e,render_record *r,dm_compute_image *image) {
    const p_set_rt *rt=&e->targets[r->rt];
    if(r->h.op==OP_CLEAR_COLOR) {
        p_clear_color p;if(r->h.size!=((8+sizeof p+7)&~(size_t)7))return 0;memcpy(&p,r->bytes+8,sizeof p);
        if(p.mask!=15 || p.index<0 || p.index>=rt->n ||
           !render_image(e,rt->color[p.index],rt->has_cview[p.index]?&rt->cview[p.index]:NULL,image,1))return 0;
    } else {
        p_clear_ds p;if(r->h.size!=((8+sizeof p+7)&~(size_t)7))return 0;memcpy(&p,r->bytes+8,sizeof p);
        if((!p.depth_mask && !(p.stencil_mask&255)) || ((p.stencil_mask&255) && (p.stencil_mask&255)!=255) ||
           !render_image(e,rt->depth,rt->has_dview?&rt->dview:NULL,image,1))return 0;
    }
    unsigned mip=(unsigned)image->base_level;
    if(mip>=32)return 0;
    unsigned width=image->desc.width>>mip,height=image->desc.height>>mip;if(!width)width=1;if(!height)height=1;
    const p_scissor *s=&r->scissor;
    return s->x<=0 && s->y<=0 && (int64_t)s->x+s->w>=width && (int64_t)s->y+s->h>=height;
}
static void render_epoch_plan(render_epoch *e,replay_state *initial) {
    /* Most prefixes contain no P -> clear group -> P pattern. Establish
     * barrier skips with a cheap opcode walk before rebuilding any bindings. */
    unsigned target_changes=0;int entered=0,saw_draw=0;
    for(unsigned i=0;i<e->count;i++) {
        render_record *r=&e->records[i];
        if(r->h.op==OP_BARRIER) {
            if(entered)r->skip=2;else entered=1;
        } else if(entered && (r->h.op==OP_DRAW || r->h.op==OP_DRAW_IDX))saw_draw=1;
        else if(saw_draw && r->h.op==OP_SET_RT)target_changes++;
    }
    if(target_changes<2)return;
    /* Upstream checks every bound slot, including unused shader slots, only
     * from the anchor draw through the return to P. Later consumers must not
     * retroactively invalidate a completed candidate. */
    dm_compute_image bindings[RE_IMAGES],current[RE_IMAGES];unsigned nb=0,nc=0;
    replay_state s=*initial;
    unsigned anchor=UINT32_MAX,sets[4],clears[32],ng=0,nclear=0,group_clears=0;int entry=0;
    replay_planning=1;
    for(unsigned i=0;i<e->count;i++) {
        render_record *r=&e->records[i];
        replay_record.address=r->address;replay_record.size=r->h.size;replay_record.bytes=r->bytes;
        if(render_binding_op(r->h.op))replay_binding(&s,r->h.op,r->address+8,r->h.size);
        if(anchor!=UINT32_MAX && (r->h.op==OP_BIND_TEX || r->h.op==OP_BIND_IMG)) {
            int binding_valid=render_bound_images(&s,current,&nc);
            for(unsigned j=0;binding_valid && j<nc;j++)binding_valid=render_add_image(bindings,&nb,&current[j]);
            if(!binding_valid){anchor=UINT32_MAX;ng=nclear=group_clears=nb=0;}
        }
        if(r->h.op==OP_BARRIER) {
            if(!entry){entry=1;anchor=UINT32_MAX;ng=nclear=group_clears=0;}
            else r->skip=2;
            continue;
        }
        if(!entry)continue;
        if(r->h.op==OP_DRAW || r->h.op==OP_DRAW_IDX) {
            if(anchor==UINT32_MAX || ng) {
                anchor=i;ng=nclear=group_clears=0;
                if(!render_bound_images(&s,bindings,&nb))anchor=UINT32_MAX;
            }
            continue;
        }
        if(anchor==UINT32_MAX)continue;
        if(r->h.op==OP_SET_RT) {
            if(render_targets_same(e,&e->targets[r->rt],&e->targets[e->records[anchor].rt])) {
                if(!ng)continue;
                if(!group_clears){anchor=UINT32_MAX;continue;}
                dm_compute_image primary[9];unsigned np=0;int valid=render_attachments(e,&e->targets[r->rt],primary,&np);
                for(unsigned j=0;valid && j<nclear;j++) {
                    dm_compute_image image;valid=render_clear_image(e,&e->records[clears[j]],&image);
                    for(unsigned k=0;valid && k<np;k++)if(render_overlap(&image,&primary[k]))valid=0;
                    for(unsigned k=0;valid && k<nb;k++)if(image.serial==bindings[k].serial)valid=0;
                }
                if(valid) {
                    e->records[anchor].hoist=clears[0]+1;
                    for(unsigned j=0;j<nclear;j++) {
                        render_record *c=&e->records[clears[j]];c->skip=1;c->hoist=j+1<nclear?clears[j+1]+1:0;
                    }
                    for(unsigned j=0;j<ng;j++)e->records[sets[j]].skip=1;
                    r->skip=1;render_stats.hoists++;render_stats.clears+=nclear;
                }
                anchor=UINT32_MAX;ng=nclear=group_clears=nb=0;
            } else {
                if(ng==4 || (ng && !group_clears)){anchor=UINT32_MAX;continue;}
                sets[ng++]=i;group_clears=0;
            }
        } else if(r->h.op==OP_CLEAR_COLOR || r->h.op==OP_CLEAR_DS) {
            dm_compute_image image;
            if(!ng || group_clears==8 || nclear==32 || !render_clear_image(e,r,&image)) {
                anchor=UINT32_MAX;ng=nclear=group_clears=0;continue;
            }
            clears[nclear++]=i;group_clears++;
        }
    }
    replay_planning=0;replay_record.bytes=NULL;
}
static void render_epoch_hoist(render_epoch *e,const render_record *anchor) {
    unsigned at=anchor->hoist,previous=UINT32_MAX;
    while(at) {
        const render_record *r=&e->records[at-1];const p_set_rt *rt=&e->targets[r->rt];
        if(previous!=r->rt && dm_vk_attachment_boundary())die("Hoisted target boundary failed");previous=r->rt;
        int rect[4]={r->scissor.x,r->scissor.y,r->scissor.w,r->scissor.h};
        if(r->h.op==OP_CLEAR_COLOR) {
            p_clear_color p;memcpy(&p,r->bytes+8,sizeof p);
            dm_compute_image image=replay_image(rt->color[p.index],rt->has_cview[p.index]?&rt->cview[p.index]:NULL);
            if(dm_vk_color_attachment_clear(&image,rect,p.rgba))die("Hoisted color clear failed");
        } else {
            p_clear_ds p;memcpy(&p,r->bytes+8,sizeof p);
            dm_compute_image image=replay_image(rt->depth,rt->has_dview?&rt->dview:NULL);
            if(dm_vk_depth_attachment_clear(&image,rect,p.depth,p.depth_mask,p.stencil,p.stencil_mask))die("Hoisted depth clear failed");
        }
        at=r->hoist;
    }
    if(anchor->hoist && dm_vk_attachment_boundary())die("Hoisted clear materialization failed");
}
