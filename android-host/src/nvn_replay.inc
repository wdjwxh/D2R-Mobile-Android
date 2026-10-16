/* Synchronous baseline. Implemented draw, dispatch and image operations finish
 * before the next command. Unsupported state remains an explicit boundary. */
static pthread_mutex_t replay_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct { uint64_t address;size_t size;const uint8_t *bytes; } replay_record;
static void replay_read(uint64_t address,void *out,size_t size) {
    if(replay_record.bytes && address>=replay_record.address && size<=replay_record.size &&
       address-replay_record.address<=replay_record.size-size) {
        if(size)memcpy(out,replay_record.bytes+(size_t)(address-replay_record.address),size);
        return;
    }
    if(dm_vk_guest_read(address,out,size)) die("NVN command references unregistered/out-of-range memory");
}
#include "nvn_replay_state.inc"
#include "nvn_dispatch.inc"
#include "nvn_image_copy.inc"
#include "nvn_draw.inc"
#include "nvn_command_decoder.inc"
#include "nvn_render_epoch.inc"
#include "nvn_render_hoist.inc"
#include "nvn_fuse_proof.inc"
#include "nvn_fuse_replay.inc"
// replay_lock serializes both worker and synchronous fallback execution.
static render_epoch *replay_epochs[32];
static void replay_epochs_free(void) {
    replay_fuse_free();
    for(unsigned i=0;i<32;i++)if(replay_epochs[i]) {
        free(replay_epochs[i]->records);free(replay_epochs[i]->arena);free(replay_epochs[i]);replay_epochs[i]=NULL;
    }
    render_memory_bytes=0;
}
static void replay_list(queue_t *queue,uint64_t address,unsigned depth,uint64_t *budget) {
    if(depth>=32) die("NVN command call depth exceeded");
    op_hdr h;uint8_t record[2048];int first=1;
    p_set_rt fuse_target={0};int fuse_target_valid=0;
    fuse_context *fuse=fuse_contexts[depth];
    if(fuse){fuse->proof.count=fuse->proof.index=0;fuse->attempted_valid=0;fuse->token=0;fuse->phase=fuse->barrier=0;}
    replay_decoder decoder;replay_decoder_discard(&decoder);
    render_epoch *epoch=NULL;
    if(RE_ENABLE_HOIST) {
        epoch=replay_epochs[depth];
        if(!epoch)replay_epochs[depth]=epoch=render_epoch_allocate();
    }
    if(epoch){epoch->count=epoch->index=0;epoch->deferred=0;}
    for(;;) {
        replay_record.bytes=NULL;
        if(epoch && epoch->index==epoch->count) {
            render_epoch_finish(epoch);
            render_epoch_collect(epoch,replay_state_get(queue),address,*budget);
            if(epoch->count)render_epoch_plan(epoch,replay_state_get(queue));
        }
        if(!(*budget)--) die("NVN command cycle or submission budget exceeded");
        int from_fuse=fuse && fuse->proof.index<fuse->proof.count;
        render_record *owned=from_fuse?&fuse->proof.records[fuse->proof.index++]:
            epoch && epoch->index<epoch->count?&epoch->records[epoch->index++]:NULL;
        int snapshot=0;
        if(owned) {
            if(owned->address!=address)die("Render IR execution address mismatch");
            h=owned->h;
        } else if(!replay_decoder_snapshot(&decoder,address,record,&h)) {
            snapshot=dm_vk_guest_record_read(address,record,sizeof record);
            // Materialization may execute earlier work. No previously peeked
            // bytes survive it, even when this record is a state operation.
            decoder.size=0;decoder.disabled=1;
        }
        if(snapshot<0)die("Invalid NVN record size or memory ownership");
        if(!owned)memcpy(&h,record,sizeof h);
        // Invalidate before executing side effects, especially before CALL can
        // enter a child list. This record itself is already an owned snapshot.
        if(!replay_decoder_preserve(h.op))replay_decoder_discard(&decoder);
        if(first) {
            if(h.op!=OP_BEGIN || h.size!=8) die("NVN command handle does not start with BEGIN");
            first=0;
        }
        if(!snapshot) {
            replay_record.address=address;replay_record.size=h.size;replay_record.bytes=owned?owned->bytes:record;
        }
        int is_draw=h.op==OP_DRAW || h.op==OP_DRAW_IDX;
        if(RE_ENABLE_FUSE && !RE_ENABLE_HOIST && !owned && !snapshot && is_draw && (!fuse || !fuse->token) &&
           (!fuse_target_valid || !fuse_rt_equal(&fuse_target,&replay_state_get(queue)->rt))) {
            replay_state *s=replay_state_get(queue);
            if(!fuse)fuse=fuse_context_get(depth);
            if(fuse && (!fuse->attempted_valid || !fuse_rt_equal(&fuse->attempted,&s->rt))) {
                fuse_try_capture(fuse,s,address,record,h,*budget);
                if(fuse->proof.count){owned=&fuse->proof.records[0];fuse->proof.index=1;from_fuse=1;}
                replay_record.address=address;replay_record.size=h.size;replay_record.bytes=owned?owned->bytes:record;
            }
        }
        if(from_fuse && owned->skip) {
            if(owned->skip==2)fuse->barrier=1;
            if(owned->skip==3 || owned->skip==4) {
                replay_state *s=replay_state_get(queue);state_read(address+8,h.size,&s->rt,sizeof s->rt);
                if(owned->skip==3){if(dm_vk_fuse_next(fuse->token)<0)die("Vulkan FUSE next failed");fuse->phase=2;}
            }
            address=owned->next;continue;
        }
        if(fuse && fuse->token && !from_fuse) {
            if(h.op==OP_BARRIER && h.size==16) {
                uint32_t bits;replay_read(address+8,&bits,4);
                if(fuse_barrier_supported(bits)){fuse->barrier=1;address+=h.size;continue;}
            }
            if(is_draw && fuse->phase==2) {
                p_draw dw;p_draw_idx di;
                if(h.op==OP_DRAW)state_read(address+8,h.size,&dw,sizeof dw);else state_read(address+8,h.size,&di,sizeof di);
                if(!fuse_q_matches(fuse,replay_state_get(queue),h.op==OP_DRAW?&dw:NULL,h.op==OP_DRAW_IDX?&di:NULL))fuse_end(fuse);
            } else if(!replay_decoder_preserve(h.op))fuse_end(fuse);
        }
        if(!replay_decoder_preserve(h.op)) {
            if(!is_draw && h.op!=OP_SET_RT && h.op!=OP_BARRIER) {
                fuse_target_valid=0;if(fuse)fuse->attempted_valid=0;
            }
        }
        if(is_draw){fuse_target=replay_state_get(queue)->rt;fuse_target_valid=1;}
        if(!from_fuse && owned && owned->skip) {
            if(owned->skip==2){epoch->deferred=1;render_stats.barriers++;}
            address=owned->next;continue;
        }
        if(!from_fuse && owned && is_draw)render_epoch_hoist(epoch,owned);
        uint64_t a[3]={0};
        // Each operation resolves its own dependency and exact CPU ranges.
        switch(h.op) {
        case OP_CLEAR_DS: {
            p_clear_ds p;state_read(address+8,h.size,&p,sizeof p);replay_state *s=replay_state_get(queue);
            if(!s->rt.depth || (!p.depth_mask && !p.stencil_mask))break;
            dm_compute_image d=replay_image(s->rt.depth,s->rt.has_dview?&s->rt.dview:NULL);int rect[4]={s->scissor.x,s->scissor.y,s->scissor.w,s->scissor.h};
            if(dm_vk_depth_attachment_clear(&d,rect,p.depth,p.depth_mask,p.stencil,p.stencil_mask))die("Vulkan depth/stencil clear failed");break;
        }
        case OP_DRAW: {p_draw p;state_read(address+8,h.size,&p,sizeof p);replay_draw(replay_state_get(queue),&p,NULL);break;}
        case OP_DRAW_IDX: {p_draw_idx p;state_read(address+8,h.size,&p,sizeof p);replay_draw(replay_state_get(queue),NULL,&p);break;}
        case OP_CLEAR_COLOR: {
            p_clear_color p;state_read(address+8,h.size,&p,sizeof p);replay_state *s=replay_state_get(queue);
            if(p.index<0 || p.index>=8)die("Invalid color attachment index");
            if(!p.mask || p.index>=s->rt.n || !s->rt.color[p.index])break;
            if(p.mask!=15)die("Masked color clear requires graphics clear shader");
            dm_compute_image d=replay_image(s->rt.color[p.index],s->rt.has_cview[p.index]?&s->rt.cview[p.index]:NULL);
            int rect[4]={s->scissor.x,s->scissor.y,s->scissor.w,s->scissor.h};
            if(dm_vk_color_attachment_clear(&d,rect,p.rgba))die("Vulkan render target clear failed");break;
        }
        case OP_WAIT_EVENT: {
            if(h.size!=24)die("Invalid event wait command size");replay_read(address+8,a,16);
            uint32_t current=0,wanted=a[1]>>32;int already_ready=0;
            if(dm_vk_event_wait_prepare(a[0],wanted,(uint32_t)a[1]!=NVN_EVENT_WAIT_MODE_EQUAL,queue,g_x_executing,&already_ready))die("Vulkan event predecessor completion failed");
            // Consume the successful observation once. Rechecking could see a
            // later CPU value and enter a wait without the progress submit.
            if(already_ready)break;
            for(;;) {
                if(dm_vk_guest_event_get(a[0],&current))die("Vulkan event wait read failed");
                int ready=(uint32_t)a[1]==NVN_EVENT_WAIT_MODE_EQUAL?current==wanted:(int32_t)(current-wanted)>=0;
                if(ready)break;
                // CPU EventSignal never drains translation, so it can satisfy
                // this wait. Release the backend lock between exact reads.
                struct timespec pause={0,100000};nanosleep(&pause,NULL);
            }
            break;
        }
        case OP_FENCE_SYNC:
            die("NVN recorded command fences are unsupported; use the implemented queue fence path");
            break;
        case OP_WAIT_SYNC: {
            if(h.size!=16)die("Invalid sync command size");replay_read(address+8,a,8);
            sync_t *sync=(sync_t *)(uintptr_t)a[0];
            sync_wait_complete(sync);
            break;
        }
        case OP_CLEAR_TEX: {
            p_clear_tex p;state_read(address+8,h.size,&p,sizeof p);
            if(p.mask!=15)die("Partial-channel texture clear requires draw pipeline");
            dm_compute_image d=replay_image(p.tex,p.has_view?&p.view:NULL);dm_image_region r=replay_region(&p.r);
            if(dm_vk_image_clear(&d,&r,&p.c))die("Vulkan texture clear failed");break;
        }
        case OP_COPY_B2T: case OP_COPY_T2B: {
            p_copy_b2t p;state_read(address+8,h.size,&p,sizeof p);
            replay_copy_buffer_image(replay_state_get(queue),&p,h.op==OP_COPY_T2B);break;
        }
        case OP_COPY_T2T: {
            p_copy_t2t p;state_read(address+8,h.size,&p,sizeof p);replay_copy_image(&p);break;
        }
        case OP_DISPATCH: {
            p_dispatch p;state_read(address+8,h.size,&p,sizeof p);
            replay_dispatch(replay_state_get(queue),&p);break;
        }
        case OP_BEGIN: case OP_END:
            if(h.size!=8) die("Invalid NVN delimiter size");
            if(h.op==OP_END) {replay_record.bytes=NULL;fuse_end(fuse);render_epoch_finish(epoch);return;}
            break;
        case OP_JUMP: case OP_CALL:
            if(h.size!=16) die("Invalid NVN branch size");
            replay_read(address+8,a,8);
            if(h.op==OP_JUMP) {address=a[0];continue;}
            replay_record.bytes=NULL;
            replay_list(queue,a[0],depth+1,budget);
            replay_record.bytes=NULL;break;
        case OP_RESET_COUNTER:
            if(h.size!=16) die("Invalid NVN counter reset record");
            replay_read(address+8,a,8);
            if(a[0]==NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                if(dm_vk_reset_samples(&queue->samples))die("Deferred query reset failed");
                queue->samples_counting=1;
            }
            break;
        case OP_REPORT_COUNTER: {
            if(h.size!=24) die("Invalid NVN counter report record");
            replay_read(address+8,a,16);
            if(a[0]!=NVN_COUNTER_TYPE_TIMESTAMP && a[0]!=NVN_COUNTER_TYPE_TIMESTAMP_TOP && a[0]!=NVN_COUNTER_TYPE_SAMPLES_PASSED)
                die("NVN pipeline statistics query not implemented");
            if(dm_vk_report_counter(a[1],a[0]==NVN_COUNTER_TYPE_SAMPLES_PASSED?&queue->samples:NULL,
                                    a[0]==NVN_COUNTER_TYPE_TIMESTAMP_TOP))die("Vulkan counter report failed");
            break;
        }
        case OP_COPY_B2B:
            if(h.size!=32) die("Invalid NVN copy record");
            replay_read(address+8,a,24);
            if(a[2] && dm_vk_copy(a[0],a[1],a[2])) die("Vulkan buffer copy failed");
            break;
        case OP_UPDATE_UB: {
            if(h.size<24) die("Invalid NVN uniform record");
            replay_read(address+8,a,16);
            if(a[1]>h.size-24) die("Truncated NVN uniform payload");
            void *data=malloc(a[1]?a[1]:1);
            if(!data) die("NVN uniform staging allocation failed");
            replay_read(address+24,data,a[1]);
            int result=dm_vk_guest_write(a[0],data,a[1]);free(data);
            if(result) die("Vulkan uniform upload failed");
            break;
        }
        case OP_SIGNAL_EVENT: {
            if(h.size!=24) die("Invalid NVN event record");
            replay_read(address+8,a,16);
            uint32_t mode=(uint32_t)a[1], value=(uint32_t)(a[1]>>32);
            if(mode!=NVN_EVENT_SIGNAL_MODE_ADD && mode!=NVN_EVENT_SIGNAL_MODE_WRITE) die("Invalid NVN event mode");
            if(dm_vk_event_record(a[0],value,mode==NVN_EVENT_SIGNAL_MODE_ADD,queue,g_x_executing)) die("Vulkan event recording failed");
            break;
        }
        case OP_BARRIER: {
            if(h.size!=16)die("Invalid NVN barrier record size");
            uint32_t bits=0;replay_read(address+8,&bits,sizeof bits);
            if(dm_vk_nvn_barrier(bits))die("Vulkan GPU barrier recording failed");
            if(!fuse_barrier_supported(bits)){fuse_target_valid=0;if(fuse)fuse->attempted_valid=0;}
            break;
        }
        default:
            if(replay_binding(replay_state_get(queue),h.op,address+8,h.size))break;
            LOG("Vulkan replay not implemented: op=%u size=%u command=%#llx",h.op,h.size,(unsigned long long)address);
            die("Vulkan rendering/query replay boundary");
        }
        address+=h.size;
    }
}
static void replay_submit(queue_t *queue,int n,const uint64_t *handles) {
    pthread_mutex_lock(&replay_lock);
    if(dm_vk_draw_batch(1))die("Draw batch begin failed");
    uint64_t budget=10000000;
    for(int i=0;i<n;i++) replay_list(queue,handles[i],0,&budget);
    if(dm_vk_draw_batch(0))die("Draw batch completion failed");
    pthread_mutex_unlock(&replay_lock);
}
NVN(nvnQueueSubmitCommands,void,NVNqueue *q,int n,const NVNcommandHandle *handles) {
    queue_t *queue=(queue_t *)q;
    if(queue->magic!=MAGIC('Q','U','E','U') || queue->dev!=live_device || n<0 || (n && !handles)) die("Invalid NVN submission");
    xjob j={.kind=XJ_SUBMIT,.queue=queue,.n=n};
    if(n) {
        if((size_t)n>SIZE_MAX/sizeof(*handles))die("NVN handle array overflow");
        j.handles=malloc((size_t)n*sizeof(*handles));
        if(!j.handles)die("NVN handle array allocation failed");
        memcpy(j.handles,handles,(size_t)n*sizeof(*handles));
    }
    __atomic_fetch_add(&queue->submits,1,__ATOMIC_RELAXED);
    x_push(&j);
}
