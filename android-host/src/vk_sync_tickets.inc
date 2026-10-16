/* Hardware fences are bounded independently from live guest sync objects.
 * Completed tokens keep their result after the corresponding slot is reused.
 * All state is protected by the backend lock; no backend path takes g_sync_mu. */
#define SYNC_TICKET_SLOTS 64
typedef struct sync_ticket_slot sync_ticket_slot;
struct dm_vk_sync_ticket {
    uint64_t epoch,serial,draw_frontier,completion_frontier;
    unsigned refs;
    int gpu_done,cpu_visible,materialized,error;
    sync_ticket_slot *slot;
};
struct sync_ticket_slot {VkFence fence;dm_vk_sync_ticket *ticket;};
static sync_ticket_slot sync_ticket_slots[SYNC_TICKET_SLOTS];
static uint64_t sync_ticket_epoch=1,sync_ticket_serial;
static uint64_t sync_now_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static void sync_ticket_release_locked(dm_vk_sync_ticket *t) {
    if(!t)return;
    if(!t->refs)die("Vulkan sync token reference underflow");
    if(!--t->refs)free(t);
}
void dm_vk_sync_retain(dm_vk_sync_ticket *t) {
    if(!t)return;
    pthread_mutex_lock(&lock);
    if(!t->refs || t->refs==~0u)die("Invalid Vulkan sync token retain");
    t->refs++;
    pthread_mutex_unlock(&lock);
}
void dm_vk_sync_release(dm_vk_sync_ticket *t) {
    if(!t)return;
    pthread_mutex_lock(&lock);sync_ticket_release_locked(t);pthread_mutex_unlock(&lock);
}
static void sync_ticket_complete_locked(sync_ticket_slot *slot) {
    dm_vk_sync_ticket *t=slot->ticket;
    if(!t)return;
    if(t->epoch!=sync_ticket_epoch)die("Vulkan sync fence completion epoch mismatch");
    completion_certify_fence(t->completion_frontier);
    t->gpu_done=1;t->slot=NULL;slot->ticket=NULL;
    sync_ticket_release_locked(t); // Drop the independent in-flight reference.
}
dm_vk_sync_ticket *dm_vk_sync_submit(int cpu_visible) {
    pthread_mutex_lock(&lock);
    if(!v.device || draw_pending_submit()){pthread_mutex_unlock(&lock);return NULL;}
    sync_ticket_slot *idle=NULL,*oldest=NULL;
    for(unsigned i=0;i<SYNC_TICKET_SLOTS;i++) {
        sync_ticket_slot *slot=&sync_ticket_slots[i];
        if(slot->ticket) {
            VkResult r=vkGetFenceStatus(v.device,slot->fence);
            if(r==VK_SUCCESS)sync_ticket_complete_locked(slot);
            else if(r!=VK_NOT_READY)die("Vulkan sync token status failed; retaining pending resources");
        }
        if(!slot->ticket && !idle)idle=slot;
        if(slot->ticket && (!oldest || slot->ticket->serial<oldest->ticket->serial))oldest=slot;
    }
    if(!idle) {
        gpu_metrics.sync_fence_backpressure++;
        // Bounded backpressure waits only for submitted GPU work, never for a
        // guest object to release its reference (which could require this worker).
        if(!oldest || vkWaitForFences(v.device,1,&oldest->fence,VK_TRUE,UINT64_MAX))
            die("Vulkan sync token backpressure failed; retaining resources");
        sync_ticket_complete_locked(oldest);idle=oldest;
    }
    if(!idle->fence) {
        VkFenceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence=VK_NULL_HANDLE;
        if(vkCreateFence(v.device,&ci,NULL,&fence)){pthread_mutex_unlock(&lock);return NULL;}
        idle->fence=fence;
        gpu_metrics.sync_fence_creates++;
    }
    dm_vk_sync_ticket *t=calloc(1,sizeof(*t));
    if(!t){pthread_mutex_unlock(&lock);return NULL;}
    if(sync_ticket_serial==UINT64_MAX)die("Vulkan sync token serial overflow");
    *t=(dm_vk_sync_ticket){.epoch=sync_ticket_epoch,.serial=++sync_ticket_serial,
        .draw_frontier=draw_submitted_serial,.completion_frontier=completion.submitted,
        .refs=2,.cpu_visible=!!cpu_visible,.slot=idle};
    idle->ticket=t;
    if(vkResetFences(v.device,1,&idle->fence))die("Vulkan sync fence reset failed; retaining resources");
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if(vkQueueSubmit(v.queue,1,&submit,idle->fence))die("Vulkan sync fence submit failed; retaining resources");
    gpu_metrics.sync_fence_submits++;
    pthread_mutex_unlock(&lock);return t;
}
int dm_vk_sync_wait(dm_vk_sync_ticket *t,uint64_t timeout,int *waited) {
    if(!t)return 0; // Acquired NVN textures are already complete.
    uint64_t started=sync_now_ns();
    pthread_mutex_lock(&lock);
    int result=-1;
    if(!timeout)gpu_metrics.sync_cpu_polls++;
    if(t->epoch!=sync_ticket_epoch || !v.device || t->error)goto done;
    if(!t->gpu_done) {
        if(!t->slot || t->slot->ticket!=t)die("Vulkan sync token lost its hardware fence");
        VkResult r=vkGetFenceStatus(v.device,t->slot->fence);
        if(r==VK_NOT_READY) {
            uint64_t remaining=timeout;
            if(timeout!=UINT64_MAX) {
                uint64_t elapsed=sync_now_ns()-started;
                remaining=elapsed>=timeout?0:timeout-elapsed;
            }
            if(!remaining){result=1;goto done;}
            if(waited)*waited=1;
            gpu_metrics.sync_cpu_gpu_waits++;
            r=vkWaitForFences(v.device,1,&t->slot->fence,VK_TRUE,remaining);
            if(r==VK_TIMEOUT){result=1;goto done;}
        }
        if(r!=VK_SUCCESS){t->error=1;goto done;}
        sync_ticket_complete_locked(t->slot);
    }
    // The GPU fence also covers all earlier report markers. Publish their CPU
    // records before any successful CPU wait, independently of pool dirty flags.
    if(draw_wait_serial(t->draw_frontier)){t->error=1;goto done;}
    if(t->cpu_visible && !t->materialized) {
        uint64_t before=gpu_metrics.buffer_materialize_bytes;
        // Once the token's GPU work is complete, explicit CPU consumers need
        // real bytes. The existing image fallback may conservatively wait for
        // later resource hazards; zero-timeout polls must still make progress.
        // The lock serializes this housekeeping across all concurrent waiters.
        int failure=images_flush_all() || pools_cpu_read_all_after_fence_locked();
        gpu_materialize_account(DM_VK_MATERIALIZE_FENCE,before);
        if(failure){t->error=1;goto done;}
        t->materialized=1;
        gpu_metrics.sync_cpu_materializations++;
    }
    result=0;
done:
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_sync_ordered(dm_vk_sync_ticket *t) {
    pthread_mutex_lock(&lock);
    int result=!v.device || (t && (t->epoch!=sync_ticket_epoch || t->error))?-1:0;
    pthread_mutex_unlock(&lock);return result;
}
static void sync_tickets_close(void) {
    // close_locked has already successfully waited for the device. Every
    // surviving CPU token is invalidated by epoch before a new device opens.
    for(unsigned i=0;i<SYNC_TICKET_SLOTS;i++) {
        sync_ticket_slot *slot=&sync_ticket_slots[i];
        sync_ticket_complete_locked(slot);
        if(slot->fence)vkDestroyFence(v.device,slot->fence,NULL);
    }
    memset(sync_ticket_slots,0,sizeof sync_ticket_slots);
    if(++sync_ticket_epoch==0)die("Vulkan sync token epoch overflow");
}
