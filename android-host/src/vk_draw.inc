#include "vk_draw.h"
#include <time.h>
enum { DRAW_TIMING_STRIDE=DM_VK_DETAIL_TIMING?1:32 };
// Owned by the backend lock throughout dm_vk_draw, including nested drains.
static uint64_t draw_timing_sequence;
static int draw_timing_sample;
static double draw_time_ms(void) { return draw_timing_sample?gpu_time_ms():0; }
static VkImageView draw_fuse_input_view(const dm_compute_image *view,gpu_image **image);
#include "vk_graphics_resources.inc"
#include "vk_graphics_stage_cache.inc"
#include "vk_draw_queries.inc"
// Device-owned render passes and attachment bundles. Entries are immutable;
// image retirement flushes pending work before calling the eviction hook.
enum { DRAW_PASS_LIMIT=256, DRAW_ATTACHMENT_LIMIT=512, DRAW_CACHE_BUCKETS=257 };
typedef struct {
    uint32_t count,subpasses,flags;
    VkAttachmentDescription descriptions[9];
    struct {uint32_t colors,has_depth,inputs,preserves,flags,bindpoint;
        VkAttachmentReference references[8],depth,input[8];uint32_t preserve[9];} sub[2];
    VkSubpassDependency dependencies[5];uint32_t dependency_count;
} draw_pass_key;
typedef struct draw_cached_pass {
    struct draw_cached_pass *next;draw_pass_key key;VkRenderPass pass;
} draw_cached_pass;
typedef struct {
    VkRenderPass pass;uint64_t count,width,height,framebuffer_layers;
    VkImage images[9];uint64_t formats[9],aspects[9],mips[9],layers[9],layer_counts[9],view_types[9];
} draw_attachment_key;
typedef struct draw_cached_attachments {
    struct draw_cached_attachments *next;draw_attachment_key key;
    VkFramebuffer fb;VkImageView views[9];
} draw_cached_attachments;
static draw_cached_pass *draw_passes[DRAW_CACHE_BUCKETS],*draw_last_pass;
static draw_cached_attachments *draw_attachments[DRAW_CACHE_BUCKETS],*draw_last_attachments;
static unsigned draw_pass_count,draw_attachment_count;
static unsigned draw_cache_hash(const void *data,size_t size) {
    const unsigned char *p=data;uint32_t h=2166136261u;
    for(size_t i=0;i<size;i++)h=(h^p[i])*16777619u;
    return h%DRAW_CACHE_BUCKETS;
}
static VkResult draw_pass_get(const VkRenderPassCreateInfo *info,VkRenderPass *pass,int *cached) {
    if(info->attachmentCount>9 || !info->subpassCount || info->subpassCount>2 || info->dependencyCount>5 || info->pNext)return VK_ERROR_INITIALIZATION_FAILED;
    draw_pass_key key={0};key.count=info->attachmentCount;key.subpasses=info->subpassCount;key.flags=info->flags;
    memcpy(key.descriptions,info->pAttachments,key.count*sizeof key.descriptions[0]);
    for(unsigned i=0;i<info->subpassCount;i++) {
        const VkSubpassDescription *sub=&info->pSubpasses[i];
        if(sub->colorAttachmentCount>8 || sub->inputAttachmentCount>8 || sub->preserveAttachmentCount>9 || sub->pResolveAttachments)return VK_ERROR_INITIALIZATION_FAILED;
        key.sub[i].colors=sub->colorAttachmentCount;key.sub[i].inputs=sub->inputAttachmentCount;
        key.sub[i].preserves=sub->preserveAttachmentCount;key.sub[i].has_depth=sub->pDepthStencilAttachment!=NULL;
        key.sub[i].flags=sub->flags;key.sub[i].bindpoint=sub->pipelineBindPoint;
        if(sub->colorAttachmentCount)memcpy(key.sub[i].references,sub->pColorAttachments,sub->colorAttachmentCount*sizeof(VkAttachmentReference));
        if(sub->inputAttachmentCount)memcpy(key.sub[i].input,sub->pInputAttachments,sub->inputAttachmentCount*sizeof(VkAttachmentReference));
        if(sub->preserveAttachmentCount)memcpy(key.sub[i].preserve,sub->pPreserveAttachments,sub->preserveAttachmentCount*sizeof(uint32_t));
        if(sub->pDepthStencilAttachment)key.sub[i].depth=*sub->pDepthStencilAttachment;
    }
    key.dependency_count=info->dependencyCount;
    if(info->dependencyCount)memcpy(key.dependencies,info->pDependencies,info->dependencyCount*sizeof key.dependencies[0]);
    unsigned bucket=0;
    draw_cached_pass *entry=draw_last_pass;
    if(!entry || memcmp(&entry->key,&key,sizeof key)) {
        bucket=draw_cache_hash(&key,sizeof key);
        for(entry=draw_passes[bucket];entry;entry=entry->next)if(!memcmp(&entry->key,&key,sizeof key))break;
    }
    if(entry){perf_metrics.pass_cache_hits++;draw_last_pass=entry;*pass=entry->pass;*cached=1;return VK_SUCCESS;}
    perf_metrics.pass_cache_misses++;
    VkResult result=vkCreateRenderPass(v.device,info,NULL,pass);
    if(result!=VK_SUCCESS)return result;
    if(draw_pass_count<DRAW_PASS_LIMIT && (entry=calloc(1,sizeof *entry))) {
        entry->key=key;entry->pass=*pass;entry->next=draw_passes[bucket];draw_passes[bucket]=entry;
        draw_last_pass=entry;draw_pass_count++;*cached=1;
    } else if(draw_pass_count>=DRAW_PASS_LIMIT)perf_metrics.pass_capacity_fallback++;
    else perf_metrics.pass_allocation_fallback++;
    return VK_SUCCESS;
}
static void draw_attachment_destroy(draw_cached_attachments *entry) {
    vkDestroyFramebuffer(v.device,entry->fb,NULL);
    for(unsigned i=0;i<entry->key.count;i++)graphics_destroy_image_view(entry->views[i]);
    free(entry);
}
static void draw_attachments_retire_image(VkImage image) {
    for(unsigned b=0;b<DRAW_CACHE_BUCKETS;b++) {
        draw_cached_attachments **link=&draw_attachments[b];
        while(*link) {
            draw_cached_attachments *entry=*link;unsigned i;
            for(i=0;i<entry->key.count;i++)if(entry->key.images[i]==image)break;
            if(i==entry->key.count){link=&entry->next;continue;}
            *link=entry->next;if(draw_last_attachments==entry)draw_last_attachments=NULL;
            draw_attachment_destroy(entry);draw_attachment_count--;
        }
    }
}
static void draw_attachments_close(void) {
    for(unsigned b=0;b<DRAW_CACHE_BUCKETS;b++)while(draw_attachments[b]) {
        draw_cached_attachments *entry=draw_attachments[b];draw_attachments[b]=entry->next;draw_attachment_destroy(entry);
    }
    draw_last_attachments=NULL;draw_attachment_count=0;
}
static void draw_passes_close(void) {
    draw_attachments_close();
    for(unsigned b=0;b<DRAW_CACHE_BUCKETS;b++)while(draw_passes[b]) {
        draw_cached_pass *entry=draw_passes[b];draw_passes[b]=entry->next;
        vkDestroyRenderPass(v.device,entry->pass,NULL);free(entry);
    }
    draw_last_pass=NULL;draw_pass_count=0;
}
static VkResult draw_attachments_get(VkRenderPass pass,int pass_cached,const VkImageViewCreateInfo *views,
        unsigned count,unsigned width,unsigned height,unsigned layers,VkImageView *out,VkFramebuffer *fb,int *cached) {
    draw_attachment_key key={0};key.pass=pass;key.count=count;key.width=width;key.height=height;key.framebuffer_layers=layers;
    for(unsigned i=0;i<count;i++) {
        key.images[i]=views[i].image;key.formats[i]=views[i].format;key.aspects[i]=views[i].subresourceRange.aspectMask;
        key.mips[i]=views[i].subresourceRange.baseMipLevel;key.layers[i]=views[i].subresourceRange.baseArrayLayer;
        key.layer_counts[i]=views[i].subresourceRange.layerCount;key.view_types[i]=views[i].viewType;
    }
    // Identity-swizzled, one-mip views; clears may cover multiple array layers.
    unsigned bucket=0;
    draw_cached_attachments *entry=draw_last_attachments;
    if(!entry || memcmp(&entry->key,&key,sizeof key)) {
        bucket=draw_cache_hash(&key,sizeof key);
        for(entry=draw_attachments[bucket];entry;entry=entry->next)if(!memcmp(&entry->key,&key,sizeof key))break;
    }
    if(entry){perf_metrics.attachment_cache_hits++;draw_last_attachments=entry;memcpy(out,entry->views,sizeof entry->views);*fb=entry->fb;*cached=1;return VK_SUCCESS;}
    perf_metrics.attachment_cache_misses++;
    for(unsigned i=0;i<count;i++) {
        VkResult result=vkCreateImageView(v.device,&views[i],NULL,&out[i]);if(result!=VK_SUCCESS)return result;
    }
    VkFramebufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=count,.pAttachments=out,.width=width,.height=height,.layers=layers};
    VkResult result=vkCreateFramebuffer(v.device,&info,NULL,fb);if(result!=VK_SUCCESS)return result;
    // Full caches and allocation failures use caller-owned temporary objects;
    // never force a GPU wait merely to evict a still-referenced cache entry.
    if(pass_cached && draw_attachment_count<DRAW_ATTACHMENT_LIMIT && (entry=calloc(1,sizeof *entry))) {
        entry->key=key;entry->fb=*fb;memcpy(entry->views,out,sizeof entry->views);
        entry->next=draw_attachments[bucket];draw_attachments[bucket]=entry;
        draw_last_attachments=entry;draw_attachment_count++;*cached=1;
    } else if(!pass_cached)perf_metrics.attachment_uncached_pass++;
    else if(draw_attachment_count>=DRAW_ATTACHMENT_LIMIT)perf_metrics.attachment_capacity_fallback++;
    else perf_metrics.attachment_allocation_fallback++;
    return VK_SUCCESS;
}
// Retain each draw's immutable resources until the whole submission completes,
// matching upstream's deferred command-buffer lifetime. Writable descriptors
// stay on the synchronous path until their visibility can be deferred as well.
typedef struct {
    VkCommandBuffer cmd;VkQueryPool query;VkRenderPass pass;VkFramebuffer fb;
    unsigned query_index;int query_paged;
    VkImageView attachments[9];graphics_resources *resources[2];compute_scratch index;
    int pass_cached,attachments_cached,gpu_barrier,pass_begin,attachment_clear,pure_barrier;
    sample_interval *samples;counter_report *report;
    uint64_t vertex_address[16];size_t vertex_size[16];
    uint64_t index_address;size_t index_size; // Borrowed resident-pool read.
    uint64_t compute_serial; // Shared dispatch owner in the compute FIFO.
} pending_draw;
// Keep the measured camp working set in fewer submissions. FIFO ownership and
// uniform/cache pressure still impose independent bounded backpressure.
enum { DRAW_BATCH_CAPACITY=512,DRAW_SUBMISSION_SLOTS=VK_DRAW_FIFO_CAPACITY,DRAW_BATCH_COMMANDS=DRAW_SUBMISSION_SLOTS+1 };
_Static_assert(DRAW_SUBMISSION_SLOTS<=PERF_SLOTS,"draw FIFO must fit timing probe slots");
static pending_draw pending_draws[DRAW_BATCH_CAPACITY];
static unsigned pending_draw_count;
// A batch owns one recording command buffer across render-pass boundaries.
// It is never owned by a local draw or by active_draw_pass. End/submit failure
// terminates the backend process: recorded references must not be freed and
// then retried as though the partially finalized batch were still recordable.
static VkCommandBuffer draw_batch_cmd;
static int draw_batch_sealed;
// Only batch command buffers enter this pool, after their real fence retires.
// At most eight submitted batches and one recording batch can own a buffer.
static VkCommandBuffer draw_batch_idle[DRAW_BATCH_COMMANDS];
static unsigned draw_batch_idle_count,draw_batch_allocated;
// Query resets cannot be encoded inside a render pass on Vulkan 1.1. Collect
// them in a prefix CB submitted before this batch's body. Every query lease is
// unique until that same fence retires, so no reset can erase a live result.
// vkCmdResetQueryPool orders references to the same query by submission order;
// the prefix therefore needs no host reset extension or extra queue submit.
static VkCommandBuffer draw_reset_cmd,draw_reset_idle[DRAW_BATCH_COMMANDS];
static unsigned draw_reset_idle_count,draw_reset_allocated;
static int draw_reset_sealed;
static int draw_batch_query_reset_range(VkQueryPool pool,unsigned count) {
    if(draw_reset_sealed)die("Vulkan attempted to record into sealed query reset prefix");
    if(!draw_reset_cmd) {
        VkCommandBuffer cmd=VK_NULL_HANDLE;int reused=draw_reset_idle_count!=0;
        if(reused) {
            cmd=draw_reset_idle[draw_reset_idle_count-1];
        } else {
            if(draw_reset_allocated==DRAW_BATCH_COMMANDS)die("Vulkan query reset prefix ownership overflow");
            VkCommandBufferAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool=v.commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
            if(vkAllocateCommandBuffers(v.device,&alloc,&cmd))return -1;
        }
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        // v.commands allows individual resets: Begin implicitly resets a
        // completed ONE_TIME buffer. Avoid a second reset through the driver.
        if(vkBeginCommandBuffer(cmd,&begin)) {
            // Do not retry a failed Begin with an unspecified buffer state.
            vkFreeCommandBuffers(v.device,v.commands,1,&cmd);
            if(reused){draw_reset_idle[--draw_reset_idle_count]=VK_NULL_HANDLE;draw_reset_allocated--;}
            return -1;
        }
        if(reused)draw_reset_idle[--draw_reset_idle_count]=VK_NULL_HANDLE;
        else draw_reset_allocated++;
        draw_reset_cmd=cmd;
    }
    vkCmdResetQueryPool(draw_reset_cmd,pool,0,count);return 0;
}
static int draw_batch_query_reset(VkQueryPool pool) {
    int result=draw_batch_query_reset_range(pool,1);
    if(!result)perf_metrics.query_timestamp_resets++;
    return result;
}
// These four states survive render-pass/subpass boundaries in the same CB.
// Every graphics pipeline uses dynamic viewport/scissor. The only writers of
// these states are dm_vk_draw below; clear/copy/compute encoders do not alter
// them. Any future graphics encoder/ExecuteCommands must update or invalidate.
typedef struct {
    VkViewport viewport;VkRect2D scissor;
    VkBuffer vertex[16],index;VkDeviceSize vertex_offsets[16],index_offset;
    VkIndexType index_type;unsigned vertex_valid;
    unsigned viewport_valid,scissor_valid,index_valid;
} draw_fixed_binding_state;
static draw_fixed_binding_state draw_batch_bindings;
static int draw_batch_begin(void) {
    if(draw_batch_cmd) {
        if(draw_batch_sealed)die("Vulkan attempted to record into a sealed draw batch");
        return 0;
    }
    VkCommandBuffer cmd=VK_NULL_HANDLE;
    int reused=draw_batch_idle_count!=0;
    if(reused) {
        cmd=draw_batch_idle[draw_batch_idle_count-1];
    } else {
        if(draw_batch_allocated==DRAW_BATCH_COMMANDS)die("Vulkan draw batch command ownership overflow");
        VkCommandBufferAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=v.commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        if(vkAllocateCommandBuffers(v.device,&alloc,&cmd))return -1;
    }
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    // Idle membership already proves completion. Begin performs the reset
    // required by this RESET_COMMAND_BUFFER_BIT pool, including invalid state.
    if(vkBeginCommandBuffer(cmd,&begin)) {
        vkFreeCommandBuffers(v.device,v.commands,1,&cmd);
        if(reused){draw_batch_idle[--draw_batch_idle_count]=VK_NULL_HANDLE;draw_batch_allocated--;}
        return -1;
    }
    if(reused){draw_batch_idle[--draw_batch_idle_count]=VK_NULL_HANDLE;perf_metrics.draw_cb_reuse++;}
    else {draw_batch_allocated++;perf_metrics.draw_cb_alloc++;}
    // Begin resets state even when the recycled handle equals the last CB.
    memset(&draw_batch_bindings,0,sizeof draw_batch_bindings);
    draw_batch_cmd=cmd;draw_batch_sealed=0;return 0;
}
// Read-only-resource draws without feedback share the batch, including
// temporary attachment objects. Matching pass/framebuffer handles can join
// the physical pass. Each pending draw retains
// its own descriptors/index/vertex references but no shared command handle.
// Four immutable bundles per stage, MRU first. The eight cache leases are
// separate from pending-draw references and never survive a pass boundary.
enum { DRAW_STAGE_CACHE_COUNT=4 };
// Command-encoder state only, never a resource cache. A new pass starts with
// pass-local descriptor/push validity clear. Only the four fixed states below
// can additionally inherit the current recording batch command buffer state.
typedef struct {
    draw_fixed_binding_state fixed;
    VkPipelineLayout descriptor_layout;VkDescriptorSet sets[2];
    uint32_t dynamic_offsets[64];unsigned dynamic_count;
    unsigned descriptor_valid;
    unsigned uniform_counts_valid;uint32_t uniform_counts[32];
} draw_binding_state;
static struct { VkCommandBuffer cmd;VkRenderPass pass;VkFramebuffer fb;VkPipeline pipeline;
    draw_binding_state bindings;
    VkQueryPool query;unsigned query_index;uint64_t *samples;graphics_resources *stages[2][DRAW_STAGE_CACHE_COUNT];
} active_draw_pass;
static uint64_t active_draw_epoch;
// One token is owned by the serialized host replay; resources stay cache/fence owned.
static struct {
    dm_fuse_plan plan;uint64_t token,sequence;unsigned requested,physical,started;
    VkRenderPass pass;VkFramebuffer fb;VkImageView views[9];
    gpu_image *images[9];dm_compute_image union_views[9];unsigned count,width,height;
    VkCommandBuffer cmd;
} draw_fuse;

static void draw_active_query_end(void) {
    if(active_draw_pass.query) {
        if(!active_draw_pass.cmd)die("Vulkan active query has no render pass");
        vkCmdEndQuery(active_draw_pass.cmd,active_draw_pass.query,active_draw_pass.query_index);
        active_draw_pass.query=VK_NULL_HANDLE;
        active_draw_pass.query_index=0;
    }
}
static int draw_active_end(void) {
    // Invalidate before callbacks can retire resources or prepare other work.
    int finish_fuse=draw_fuse.started && active_draw_pass.cmd==draw_fuse.cmd;
    if(draw_fuse.token)gpu_metrics.fuse_abandoned++;
    draw_fuse.token=0;
    if(!active_draw_pass.cmd){draw_fuse.started=0;return 0;}
    active_draw_epoch++;
    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<DRAW_STAGE_CACHE_COUNT;j++) {
        graphics_resources *r=active_draw_pass.stages[i][j];active_draw_pass.stages[i][j]=NULL;
        graphics_resources_free(r);
    }
    VkCommandBuffer cmd=active_draw_pass.cmd;
    draw_active_query_end();
    if(finish_fuse && !draw_fuse.physical)vkCmdNextSubpass(cmd,VK_SUBPASS_CONTENTS_INLINE);
    draw_fuse.started=0;draw_fuse.cmd=VK_NULL_HANDLE;
    vkCmdEndRenderPass(cmd);
    // Read-only descriptor batches keep attachment results on the GPU. The
    // render pass's outgoing external dependency orders subsequent GPU use;
    // actual image readback establishes its own transfer-to-host dependency.
    memset(&active_draw_pass,0,sizeof active_draw_pass);
    return 0;
}
// Each fence owns every transient resource referenced by its submitted batch.
// Use a bounded FIFO, recycling only completed slots; a full ring waits for its
// oldest fence rather than idling the entire queue.
struct draw_submission {
    VkFence fence;uint64_t serial,completion_value;unsigned count;VkCommandBuffer batch_cmd,reset_cmd;
    unsigned probe_kind;
    pending_draw draws[DRAW_BATCH_CAPACITY];
    draw_query_page *query_pages;
};
static draw_submission draw_submissions[DRAW_SUBMISSION_SLOTS];
static unsigned draw_submission_head,draw_submission_count;
static uint64_t draw_submitted_serial,draw_completed_serial;
static int draw_submission_retire(draw_submission *slot) {
    gpu_probe_retire(slot->probe_kind,(unsigned)(slot-draw_submissions));
    int result=0;
    VkCommandBuffer commands[DRAW_BATCH_CAPACITY+1];unsigned command_count=0;
    for(unsigned i=0;i<slot->count;i++)if(slot->draws[i].cmd)commands[command_count++]=slot->draws[i].cmd;
    for(unsigned i=0;i<slot->count;i++) {
        pending_draw *p=&slot->draws[i];uint64_t samples=0;
        if(p->report) {
            if(report_publish(p->report))die("Vulkan report publication failed; retaining resources");
            continue;
        }
        if(p->query) {
            // Query references preserve lifetime, not observability. Once an
            // interval was reset and its last report published, discard later
            // results just as upstream vis_clear discards unobserved slots.
            if(p->samples && (p->samples->current || p->samples->pending_reports)) {
                gpu_metrics.report_samples_queried++;
                if(vkGetQueryPoolResults(v.device,p->query,p->query_index,1,sizeof samples,&samples,sizeof samples,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT))result=-1;
                else p->samples->total+=samples;
            } else gpu_metrics.report_samples_discarded++;
            if(!p->query_paged)draw_query_release(p->query);
        }
        sample_release(p->samples);
        for(unsigned j=0;j<2;j++)graphics_resources_free(p->resources[j]);
        scratch_free(&p->index);
        if(!p->attachments_cached) {
            // Ordering-only records own no pass/FB. Avoid sending legal but
            // unnecessary null-handle destroys through the Vulkan driver.
            if(p->fb)vkDestroyFramebuffer(v.device,p->fb,NULL);
            for(unsigned j=0;j<9;j++)if(p->attachments[j])vkDestroyImageView(v.device,p->attachments[j],NULL);
        }
        if(!p->pass_cached && p->pass)vkDestroyRenderPass(v.device,p->pass,NULL);
    }
    if(command_count)vkFreeCommandBuffers(v.device,v.commands,command_count,commands);
    if(slot->batch_cmd) {
        if(draw_batch_idle_count==DRAW_BATCH_COMMANDS)die("Vulkan draw batch idle pool overflow");
        draw_batch_idle[draw_batch_idle_count++]=slot->batch_cmd;
    }
    if(slot->reset_cmd) {
        if(draw_reset_idle_count==DRAW_BATCH_COMMANDS)die("Vulkan query reset idle pool overflow");
        draw_reset_idle[draw_reset_idle_count++]=slot->reset_cmd;
    }
    draw_query_pages_retire(slot->query_pages);slot->query_pages=NULL;
    draw_completed_serial=slot->serial;
    memset(slot->draws,0,sizeof slot->draws);slot->count=0;slot->serial=0;slot->batch_cmd=VK_NULL_HANDLE;slot->reset_cmd=VK_NULL_HANDLE;
    draw_submission_head=(draw_submission_head+1)%DRAW_SUBMISSION_SLOTS;
    draw_submission_count--;
    return result;
}
static int draw_submissions_poll_cached(void) {
    while(draw_submission_count) {
        draw_submission *slot=&draw_submissions[draw_submission_head];
        int ready=completion_ready(slot->completion_value,slot->fence);
        if(!ready)return 0;
        if(ready<0)return -1;
        if(draw_submission_retire(slot))return -1;
    }
    return 0;
}
static int draw_submissions_poll(void) {
    if(!draw_submission_count){perf_metrics.timeline_empty_draw_polls++;return 0;}
    if(completion_poll())return -1;
    return draw_submissions_poll_cached();
}
static int draw_wait_serial_at(uint64_t serial,const char *site) {
    if(serial>draw_submitted_serial)return -1;
    if(completion.semaphore && draw_completed_serial<serial) {
        uint64_t value=0;
        for(unsigned i=0;i<draw_submission_count;i++) {
            const draw_submission *target=&draw_submissions[(draw_submission_head+i)%DRAW_SUBMISSION_SLOTS];
            if(target->serial==serial){value=target->completion_value;break;}
        }
        // One queue-wide timeline certificate proves the whole earlier FIFO
        // prefix complete. Wait for the requested target, not each oldest slot.
        if(!value || completion_wait_at(value,VK_NULL_HANDLE,site))return -1;
    }
    while(draw_completed_serial<serial) {
        if(!draw_submission_count)return -1;
        draw_submission *slot=&draw_submissions[draw_submission_head];
        if(completion.semaphore) {
            if(completion_ready(slot->completion_value,VK_NULL_HANDLE)!=1)return -1;
        } else if(completion_wait_at(slot->completion_value,slot->fence,site))return -1;
        if(draw_submission_retire(slot))return -1;
    }
    return 0;
}
static int draw_serial_ready(uint64_t serial) {
    if(serial>draw_submitted_serial)return -1;
    return draw_completed_serial>=serial;
}
static int draw_compute_pending_valid(uint64_t serial,uint64_t draw_serial) {
    if(!serial || !draw_serial || draw_submitted_serial==UINT64_MAX ||
       draw_serial!=draw_submitted_serial+1 || !draw_batch_cmd || draw_batch_sealed)return 0;
    for(unsigned i=0;i<pending_draw_count;i++)if(pending_draws[i].compute_serial==serial)return 1;
    return 0;
}
static int draw_compute_begin_locked(VkCommandBuffer *cmd,uint64_t *draw_serial,size_t staging_bytes) {
    for(;;) {
        if(attachment_clears_flush() || draw_active_end())return -1;
        if(pending_draw_count==DRAW_BATCH_CAPACITY || (pending_draw_count && !draw_batch_cmd)) {
            if(draw_pending_submit())return -1;
            continue;
        }
        // Clear preparation can upload textures and consume compute slots.
        // Finish every possible callback before creating an empty shared CB.
        // Clear preparation can itself record uploads. Recheck both budgets
        // here, before acquiring a body CB which has no ownership marker yet.
        if(staging_bytes?compute_texture_room(staging_bytes):compute_fifo_room())return -1;
        if(attachment_clear_count || active_draw_pass.cmd || pending_draw_count==DRAW_BATCH_CAPACITY ||
           (pending_draw_count && !draw_batch_cmd))continue;
        break;
    }
    if(draw_submitted_serial==UINT64_MAX)die("Vulkan shared compute draw serial exhausted");
    if(draw_batch_begin())return -1;
    *cmd=draw_batch_cmd;*draw_serial=draw_submitted_serial+1;return 0;
}
static void draw_compute_record_locked(VkCommandBuffer cmd,uint64_t serial,uint64_t draw_serial) {
    if(!cmd || cmd!=draw_batch_cmd || draw_batch_sealed || active_draw_pass.cmd ||
       pending_draw_count==DRAW_BATCH_CAPACITY || draw_serial!=draw_submitted_serial+1)
        die("Vulkan shared compute recording owner changed");
    pending_draws[pending_draw_count++]=(pending_draw){.gpu_barrier=1,.compute_serial=serial};
}
static void draw_compute_report_record_locked(VkCommandBuffer cmd,uint64_t serial,uint64_t draw_serial,counter_report *report) {
    draw_compute_record_locked(cmd,serial,draw_serial);
    pending_draws[pending_draw_count-1].report=report;
}
// Complete every operation which may recurse, retire callbacks or wait before
// the caller selects its compute FIFO slot. Commit below performs none of them.
static int draw_submit_prepare(draw_submission **prepared) {
    *prepared=NULL;
    if(attachment_clears_flush())return -1;
    if(draw_active_end())return -1;
    if(!pending_draw_count) {
        if(draw_batch_cmd || draw_reset_cmd || draw_batch_query_pages)die("Vulkan draw batch has no resource ownership records");
        return 0;
    }
    // Retire for capacity here; explicit CPU/event visibility boundaries keep
    // their own polls/waits and must not depend on this FIFO becoming full.
    if(draw_submission_count==DRAW_SUBMISSION_SLOTS) {
        if(draw_submissions_poll())return -1;
        if(draw_submission_count==DRAW_SUBMISSION_SLOTS &&
           draw_wait_serial(draw_submissions[draw_submission_head].serial))return -1;
    }
    if(draw_submitted_serial==UINT64_MAX)die("Vulkan draw submission serial exhausted");
    draw_submission *slot=&draw_submissions[(draw_submission_head+draw_submission_count)%DRAW_SUBMISSION_SLOTS];
    if(completion_prepare(&slot->fence))return -1;
    *prepared=slot;return 0;
}
static uint64_t draw_submit_commit_at(draw_submission *slot,const char *function,unsigned line) {
    if(!pending_draw_count || draw_submission_count==DRAW_SUBMISSION_SLOTS ||
       slot!=&draw_submissions[(draw_submission_head+draw_submission_count)%DRAW_SUBMISSION_SLOTS])
        die("Vulkan draw submission changed after preparation");
    if(draw_batch_cmd && !draw_batch_sealed) {
        // Mark first, so this buffer can never accidentally be ended twice.
        draw_batch_sealed=1;
        VkResult result=vkEndCommandBuffer(draw_batch_cmd);
        if(result)die("Vulkan draw batch end failed result=%d; terminating",result);
    }
    // Used indices are known from our own CPU encoding metadata. Append one
    // reset per page to the prefix, which executes before every body command.
    // No guest command lookahead, extra submission, or host query reset.
    for(draw_query_page *p=draw_batch_query_pages;p;p=p->next) {
        if(!p->used || p->used>DRAW_QUERY_PAGE_SIZE)die("Vulkan invalid query page usage");
        if(draw_batch_query_reset_range(p->pool,p->used))die("Vulkan query page reset prefix failed");
        perf_metrics.query_page_resets++;perf_metrics.query_page_reset_slots+=p->used;
    }
    if(draw_reset_cmd && !draw_reset_sealed) {
        draw_reset_sealed=1;
        VkResult result=vkEndCommandBuffer(draw_reset_cmd);
        if(result)die("Vulkan query reset prefix end failed result=%d; terminating",result);
    }
    VkCommandBuffer commands[DRAW_BATCH_CAPACITY+3];unsigned command_count=0,draw_count=0,pass_count=0,compute_count=0;
    if(draw_reset_cmd)commands[command_count++]=draw_reset_cmd;
    for(unsigned i=0;i<pending_draw_count;i++) {
        pending_draw *p=&pending_draws[i];
        if(p->cmd)commands[command_count++]=p->cmd;
        compute_count+=p->compute_serial!=0;
        if(!p->gpu_barrier && !p->report){draw_count+=!p->attachment_clear;pass_count+=p->pass_begin;}
    }
    if(draw_batch_cmd)commands[command_count++]=draw_batch_cmd;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=command_count,.pCommandBuffers=commands};
    VkCommandBuffer measured_commands[DRAW_BATCH_CAPACITY+5];
    unsigned probe_kind=compute_count?PERF_MIXED:PERF_DRAW;
    gpu_probe_prepare(probe_kind,(unsigned)(slot-draw_submissions),&submit,measured_commands);
    int timing_sample=gpu_metrics.batches%CPU_OPERATION_TIMING_STRIDE==0;
    double started=timing_sample?gpu_time_ms():0;
    VkResult submitted=completion_submit(&submit,slot->fence,&slot->completion_value);
    if(submitted)die("Vulkan draw batch submit failed result=%d; terminating",submitted);
    gpu_submit_site_record(function,line,draw_count,pending_draw_count,compute_count!=0,
        pending_draw_count==DRAW_BATCH_CAPACITY);
    perf_metrics.draw_submits++;perf_metrics.draw_capacity+=pending_draw_count==DRAW_BATCH_CAPACITY;
    slot->serial=++draw_submitted_serial;slot->count=pending_draw_count;slot->probe_kind=probe_kind;
    if(compute_count)perf_metrics.shared_compute_batches++;
    for(unsigned i=0;i<pending_draw_count;i++)if(pending_draws[i].compute_serial)
        compute_draw_batch_submitted(pending_draws[i].compute_serial,slot->serial);
    for(unsigned i=0;i<pending_draw_count;i++)if(pending_draws[i].report)pending_draws[i].report->serial=slot->serial;
    slot->batch_cmd=draw_batch_cmd;draw_batch_cmd=VK_NULL_HANDLE;draw_batch_sealed=0;
    memset(&draw_batch_bindings,0,sizeof draw_batch_bindings);
    slot->reset_cmd=draw_reset_cmd;draw_reset_cmd=VK_NULL_HANDLE;draw_reset_sealed=0;
    if(slot->query_pages)die("Vulkan query page submission owner was not retired");
    slot->query_pages=draw_batch_query_pages;draw_batch_query_pages=NULL;
    perf_metrics.query_prefix_batches+=slot->reset_cmd!=VK_NULL_HANDLE;
    memcpy(slot->draws,pending_draws,pending_draw_count*sizeof *pending_draws);
    draw_submission_count++;
    gpu_metrics.batches++;gpu_metrics.draws+=draw_count;gpu_metrics.commands+=pass_count;
    if(timing_sample){gpu_metrics.batch_ms+=gpu_time_ms()-started;gpu_metrics.batch_timing_samples++;}
    memset(pending_draws,0,sizeof pending_draws);pending_draw_count=0;
    return slot->serial;
}
static int draw_pending_submit_at(const char *function,unsigned line) {
    draw_submission *slot=NULL;
    if(draw_submit_prepare(&slot))return -1;
    if(slot)(void)draw_submit_commit_at(slot,function,line);
    return 0;
}
// Queue an ordering dependency, not a CPU completion point. End only the
// render pass and append to the same batch CB and fence-owned resource FIFO.
static int draw_gpu_barrier_locked(void) {
    if(attachment_clears_flush())return -1;
    if(!v.device || draw_active_end())return -1;
    // Only this helper marks a pure barrier. All successful body work leaves
    // a draw/clear/compute/report record; standalone alias paths flush first.
    // Thus there is no intervening GPU work after the last identical barrier.
    // Keep this check before capacity submission and never carry it across CBs.
    // New body encoders must publish their record before returning, or clear
    // this proof. If the barrier mapping changes, compare its full signature.
    if(draw_batch_cmd && !draw_batch_sealed && pending_draw_count &&
       pending_draws[pending_draw_count-1].pure_barrier) {
        perf_metrics.full_barrier_merged++;return 0;
    }
    if(pending_draw_count==DRAW_BATCH_CAPACITY && draw_pending_submit())return -1;
    if(pending_draw_count && !draw_batch_cmd && draw_pending_submit())return -1;
    if(draw_batch_begin())return -1;
    VkCommandBuffer cmd=draw_batch_cmd;
    // Conservatively cover all NVN barrier bits. Attachment final layouts stay
    // GENERAL; this command orders memory without inventing layout changes.
    VkMemoryBarrier memory={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&memory,0,NULL,0,NULL);
    pending_draw *entry=&pending_draws[pending_draw_count++];
    *entry=(pending_draw){.gpu_barrier=1,.pure_barrier=1};
    perf_metrics.full_barrier_encoded++;
    return 0;
}
// Clears use the same command-buffer/fence ownership as draws. All resource
// preparation precedes recording; after recording no fallible operation remains.
static int draw_attachment_clear_record(const VkRenderPassCreateInfo *rp,
        const VkImageViewCreateInfo *vi,unsigned width,unsigned height,unsigned layers,
        const VkClearRect *rect,VkImageAspectFlags aspects,const VkClearValue *value) {
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer fb=VK_NULL_HANDLE;
    VkImageView views[9]={0};int pass_cached=0,attachments_cached=0;
    if(pending_draw_count==DRAW_BATCH_CAPACITY && draw_pending_submit())return -1;
    if(pending_draw_count && !draw_batch_cmd && draw_pending_submit())return -1;
    if(draw_pass_get(rp,&pass,&pass_cached))goto fail;
    if(draw_attachments_get(pass,pass_cached,vi,1,width,height,layers,views,&fb,&attachments_cached))goto fail;
    int joined=active_draw_pass.cmd && active_draw_pass.pass==pass && active_draw_pass.fb==fb;
    if(!joined && draw_active_end())goto fail;
    if(draw_batch_begin())goto fail;
    VkCommandBuffer cmd=draw_batch_cmd;
    if(!joined) {
        VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{width,height}}};
        vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
    }
    VkClearAttachment clear={.aspectMask=aspects,.colorAttachment=0,.clearValue=*value};
    vkCmdClearAttachments(cmd,1,&clear,1,rect);
    pending_draw *entry=&pending_draws[pending_draw_count++];
    *entry=(pending_draw){.pass=pass,.fb=fb,.pass_cached=pass_cached,
        .attachments_cached=attachments_cached,.attachment_clear=1,.pass_begin=!joined};
    memcpy(entry->attachments,views,sizeof views);
    gpu_metrics.attachment_clears++;if(joined)gpu_metrics.attachment_clear_joins++;
    if(!joined) {
        if(pass_cached && attachments_cached) {
            active_draw_epoch++;active_draw_pass.cmd=cmd;active_draw_pass.pass=pass;active_draw_pass.fb=fb;
        } else vkCmdEndRenderPass(cmd);
    }
    return 0;
fail:
    if(!attachments_cached) {
        if(fb)vkDestroyFramebuffer(v.device,fb,NULL);
        for(unsigned i=0;i<9;i++)if(views[i])vkDestroyImageView(v.device,views[i],NULL);
    }
    if(!pass_cached && pass)vkDestroyRenderPass(v.device,pass,NULL);
    return -1;
}
// Preserve completion semantics for existing CPU readback/destruction callers.
static int draw_pending_flush_at(const char *function,unsigned line) {
    if(draw_pending_submit_at(function,line))return -1;
    return draw_wait_serial(draw_submitted_serial);
}
// Reports retain the old interval across resets. FIFO retirement reaches each
// report marker before adding any later draw, preserving its exact snapshot.
int dm_vk_reset_samples(uint64_t *samples) {
    if(!samples)return -1;
    pthread_mutex_lock(&lock);
    if(attachment_clears_flush()){pthread_mutex_unlock(&lock);return -1;}
    if(active_draw_pass.cmd)perf_metrics.sample_pass_kept++;
    draw_active_query_end();
    sample_owner *o=sample_owner_get(samples);
    sample_interval *next=calloc(1,sizeof *next);
    if(!o || !next){free(next);pthread_mutex_unlock(&lock);return -1;}
    next->refs=1;next->current=1;o->interval->current=0;sample_release(o->interval);o->interval=next;
    gpu_metrics.report_resets++;
    *samples=0;
    pthread_mutex_unlock(&lock);return 0;
}
static int report_record_locked(uint64_t address,uint64_t *samples,int top) {
    if(!v.device || !v.timestamp_bits || !address || address>UINT64_MAX-16)return -1;
    if(attachment_clears_flush())return -1;
    // A preceding packaged image writer must not later overwrite this report.
    // This exact alias fallback is needed only when the report aliases image RAM.
    // Consecutive reports to the same bytes are ordered markers, not CPU/GPU
    // consumers. Do not force retirement merely to enqueue the next report.
    if(images_flush_alias_range(address,16))return -1;
    size_t offset;dm_vk_pool *pool=pool_address_locked(address,16,&offset);if(!pool)return -1;
    if(!samples) {
        int gpu=timestamp_gpu_record_locked(pool,offset,top);
        if(gpu<=0)return gpu;
    }
    if(pending_draw_count==DRAW_BATCH_CAPACITY && draw_pending_submit())return -1;
    if(pending_draw_count && !draw_batch_cmd && draw_pending_submit())return -1;
    if(draw_batch_begin())return -1;
    counter_report *r=calloc(1,sizeof *r);if(!r)return -1;
    r->pool=pool;r->offset=offset;r->size=16;r->samples=sample_capture(samples);
    if(samples && !r->samples){free(r);return -1;}
    r->timestamp=timestamp_acquire();
    if(!r->timestamp){sample_release(r->samples);free(r);return -1;}
    if(draw_batch_query_reset(r->timestamp->pool)) {
        r->timestamp->leased=0;sample_release(r->samples);free(r);return -1;
    }
    // The query's owning draw record precedes this report in the FIFO. End
    // only that counting segment; later draws receive a new query lease.
    if(samples) {
        if(active_draw_pass.cmd)perf_metrics.sample_pass_kept++;
        draw_active_query_end();
    }
    vkCmdWriteTimestamp(draw_batch_cmd,top?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r->timestamp->pool,0);
    pending_draws[pending_draw_count++]=(pending_draw){.report=r};
    if(r->samples)r->samples->pending_reports++;
    report_track(r);
    if(samples)gpu_metrics.report_samples++;
    else if(top)gpu_metrics.report_top++;
    else gpu_metrics.report_timestamp++;
    return 0;
}
static int reports_range_drain_filtered(uint64_t address,size_t size,int include_events,int gpu_consumer,const char *site,const char *via) {
    if(!counter_reports || !size)return 0;
    if(size>UINT64_MAX-address)return -1;
    gpu_metrics.report_page_checks++;
    if(!(counter_report_pages&report_page_mask(address,size))) {
        gpu_metrics.report_page_skips++;return 0;
    }
    uint64_t serial=0;int pending=0,found=0;
    uint64_t events=0,timestamps=0,samples=0;
    for(counter_report *r=counter_reports;r;r=r->next) {
        if(r->event && !include_events)continue;
        uint64_t dst=(uintptr_t)r->pool->guest+r->offset;
        if(address<dst+r->size && dst<address+size){
            if(gpu_consumer && r->gpu_published) {gpu_metrics.report_gpu_reads++;continue;}
            found=1;if(!r->serial)pending=1;else if(r->serial>serial)serial=r->serial;
            if(r->event)events++;else if(r->samples)samples++;else timestamps++;
        }
    }
    if(!found)return 0;
    gpu_metrics.report_range_waits++;
    int previous=report_wait_active;
    report_wait_active=report_wait_site_get(site,via);
    report_wait_site *stats=&perf_metrics.report_sites[report_wait_active];
    stats->hits++;stats->events+=events;stats->timestamps+=timestamps;stats->samples+=samples;stats->pending+=pending;
    int result=0;
    if(pending) {
        if(draw_pending_submit()){result=-1;goto done;}
        // Submission may already poll/retire earlier reports. The newly
        // submitted frontier includes every previously unsubmitted marker.
        serial=draw_submitted_serial;
    }
    result=draw_wait_serial(serial);
done:
    report_wait_active=previous;return result;
}
static int reports_range_drain_at(uint64_t address,size_t size,const char *site,const char *via) {
    return reports_range_drain_filtered(address,size,1,0,site,via);
}
static int reports_gpu_range_prepare(uint64_t address,size_t size,const char *site) {
    // Encoded GPU reports carry their own transfer->consumer dependency.
    // CPU callback reports still require their original completion boundary.
    return reports_range_drain_filtered(address,size,1,1,site,"gpu-buffer-read");
}
static int reports_cpu_only_drain(uint64_t address,size_t size) {
    return reports_range_drain_filtered(address,size,1,1,__func__,"report-predecessor");
}
static int event_word_prepare_locked(dm_vk_pool *p,size_t offset) {
    uint64_t address=(uintptr_t)p->guest+offset;
    // Consecutive posts to this word accumulate at retirement. Only unrelated
    // report/GPU-byte producers require CPU materialization before recording.
    if(reports_range_drain_filtered(address,4,0,0,__func__,"event-word-prep") || images_flush_alias_range(address,4))return -1;
    return pool_cpu_read_dirty_locked(p,offset,4,"event");
}
int dm_vk_event_record(uint64_t address,uint32_t value,int add,const void *queue,uint64_t job) {
    pthread_mutex_lock(&lock);
    int result=-1;size_t offset=0;
    dm_vk_pool *pool=pool_address_locked(address,4,&offset);
    if(!v.device || !pool || (address&3) || attachment_clears_flush() || event_word_prepare_locked(pool,offset))goto done;
    if(pending_draw_count==DRAW_BATCH_CAPACITY && draw_pending_submit())goto done;
    if(pending_draw_count && !draw_batch_cmd && draw_pending_submit())goto done;
    if(draw_batch_begin())goto done;
    if(active_draw_pass.cmd)perf_metrics.event_pass_kept++;
    counter_report *r=calloc(1,sizeof *r);
    if(!r)die("Vulkan event ownership allocation failed");
    r->pool=pool;r->offset=offset;r->size=4;r->event=1;r->value=value;r->add=add;
    r->event_queue=queue;r->event_job=job;
    pending_draws[pending_draw_count++]=(pending_draw){.report=r};
    report_track(r);
    __atomic_fetch_add(&pending_event_count,1,__ATOMIC_RELEASE);result=0;
done:
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_completion_poll(void) {
    if(!dm_vk_completion_pending())return 0;
    pthread_mutex_lock(&lock);int result=draw_submissions_poll();pthread_mutex_unlock(&lock);return result;
}
// In this backend event stores publish at FIFO retirement. For a definite
// preceding WRITE, the one-shot wait observation can be placed at that store:
// earlier CPU signals are overwritten by it, later signals do not revoke it.
// A real GPU dependency still orders all work after this point. This is not a
// speculative read of guest RAM or permission to retire the callback early.
static int event_wait_witness_locked(dm_vk_pool *pool,size_t offset,uint32_t wanted,int gequal,const void *queue,uint64_t job) {
    unsigned miss=0;
    if(!queue || !job)goto reject;
    uint64_t address=(uintptr_t)pool->guest+offset;
    int found=0,unsubmitted=0;uint32_t posted=0;
    // Newest first. Never accept an older satisfying write past a newer one.
    // Reject the whole alias set for ADD/partial/non-event producers.
    for(counter_report *r=counter_reports;r;r=r->next) {
        uint64_t dst=(uintptr_t)r->pool->guest+r->offset;
        if(address>=dst+r->size || dst>=address+4)continue;
        miss=2;if(!r->event || r->size!=4 || r->pool!=pool || r->offset!=offset)goto reject;
        miss=3;if(r->event_queue!=queue || !r->event_job || r->event_job>job)goto reject;
        miss=4;if(r->add)goto reject;
        if(!found){posted=r->value;unsubmitted=!r->serial;found=1;}
    }
    miss=1;if(!found)goto reject;
    miss=5;if(!(gequal?(int32_t)(posted-wanted)>=0:posted==wanted))goto reject;
    miss=6;
    for(pool_gpu_range *r=pool->gpu_dirty;r;r=r->next)
        if(offset<r->hi && r->lo<offset+4)goto reject;
    miss=7;
    for(gpu_image *p=gpu_images;p;p=p->next)
        if(p->address && address<p->address+p->storage.size && p->address<address+4 &&
           (p->draw_dirty || p->pending_clear || p->last_compute_use>compute_completed_serial))goto reject;
    // This can flush clears/submit/retire callbacks. Only copied proof values
    // survive it; no counter_report pointer is kept across the call.
    if(draw_gpu_barrier_locked())return -1;
    perf_metrics.event_witness_hits++;perf_metrics.event_witness_pending+=unsubmitted;
    return 1;
reject:
    perf_metrics.event_witness_miss[miss]++;return 0;
}
int dm_vk_event_wait_prepare(uint64_t address,uint32_t value,int gequal,const void *queue,uint64_t job,int *ready) {
    if(!ready)return -1;
    *ready=0;
    pthread_mutex_lock(&lock);size_t offset=0;int result=-1;
    dm_vk_pool *pool=pool_address_locked(address,4,&offset);
    if(pool && !(address&3)) {
        int witness=event_wait_witness_locked(pool,offset,value,gequal,queue,job);
        if(witness<0)goto done;
        if(witness){*ready=1;result=0;goto done;}
    }
    if(pool && !(address&3) && !reports_range_drain_at(address,4,__func__,"event-wait") && !event_word_prepare_locked(pool,offset)) {
        // First publish every producer of this word, including ordered event
        // posts. A satisfied wait does not require submitting unrelated work.
        uint32_t current=__atomic_load_n((uint32_t *)(pool->guest+offset),__ATOMIC_ACQUIRE);
        *ready=gequal?(int32_t)(current-value)>=0:current==value;
        if(*ready) {
            perf_metrics.event_wait_ready++;
            perf_metrics.event_wait_kept_pending+=pending_draw_count!=0;
            result=0;
        } else {
            // The CPU signal can depend on preceding GPU work at another
            // address. Preserve progress before releasing the worker to poll.
            perf_metrics.event_wait_progress++;
            result=draw_pending_submit();
        }
    }
done:
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_events_drain(void) {
    pthread_mutex_lock(&lock);int result=0;
    if(dm_vk_completion_pending()) {
        result=draw_pending_submit();
        if(!result) {
            uint64_t serial=0;
            for(counter_report *r=counter_reports;r;r=r->next)if(r->event && r->serial>serial)serial=r->serial;
            result=draw_wait_serial(serial);
        }
    }
    pthread_mutex_unlock(&lock);return result;
}
// FlushMappedRange is after a CPU overwrite, unlike the JIT pre-write hook.
// Preserve only the intersecting report bytes across their ordered retirement.
static int reports_cpu_overwrite(uint64_t address,size_t size) {
    if(!counter_reports || !size)return 0;
    if(size>UINT64_MAX-address)return -1;
    typedef struct saved_report_bytes { struct saved_report_bytes *next;uint64_t address;size_t size;uint8_t bytes[16]; } saved_report_bytes;
    saved_report_bytes *saved=NULL;
    for(counter_report *r=counter_reports;r;r=r->next) {
        uint64_t dst=(uintptr_t)r->pool->guest+r->offset;
        uint64_t lo=address>dst?address:dst,hi=address+size<dst+r->size?address+size:dst+r->size;
        if(lo>=hi)continue;
        saved_report_bytes *s=malloc(sizeof *s);
        if(!s){while(saved){s=saved;saved=s->next;free(s);}return -1;}
        s->address=lo;s->size=(size_t)(hi-lo);memcpy(s->bytes,(void *)(uintptr_t)lo,s->size);
        s->next=saved;saved=s;
    }
    int result=reports_range_drain_at(address,size,__func__,"cpu-overwrite");
    while(saved) {
        saved_report_bytes *s=saved;saved=s->next;
        dm_guest_write_prep((void *)(uintptr_t)s->address,s->size);
        memcpy((void *)(uintptr_t)s->address,s->bytes,s->size);free(s);
    }
    return result;
}
static int draw_range_overlaps(const pending_draw *p,uint64_t address,size_t size) {
    if(p->index_size && address<p->index_address+p->index_size && p->index_address<address+size)return 1;
    for(unsigned j=0;j<16;j++)if(p->vertex_size[j] &&
        address<p->vertex_address[j]+p->vertex_size[j] && p->vertex_address[j]<address+size)return 1;
    // Borrowed descriptor sets may share an original owner, but the read
    // ranges belong to THIS draw's resources/dynamic offsets, not the owner.
    for(unsigned stage=0;stage<2;stage++) {
        const graphics_resources *r=p->resources[stage];if(!r)continue;
        for(unsigned i=0;i<r->count;i++)if((r->resident_storage|r->resident_uniform)&(UINT64_C(1)<<i)) {
            const dm_compute_buffer *b=&r->input_keys[i];
            if(address<b->address+b->size && b->address<address+size)return 1;
        }
        for(unsigned i=0;i<r->count;i++)if(r->resident_texel&(UINT64_C(1)<<i)) {
            uint64_t texel_address=r->image_keys[i].address;
            size_t texel_size=r->payload.buffers[i].range;
            if(address<texel_address+texel_size && texel_address<address+size)return 1;
        }
    }
    return 0;
}
static int draw_pending_write(uint64_t address,size_t size) {
    // Uploads touching no submitted draw readers need no driver roundtrip.
    // Keep the original poll for actual overlaps: it can certify already
    // completed readers without turning every such case into a driver wait.
    int overlap=0;
    for(unsigned i=0;i<draw_submission_count && !overlap;i++) {
        const draw_submission *slot=&draw_submissions[(draw_submission_head+i)%DRAW_SUBMISSION_SLOTS];
        for(unsigned j=0;j<slot->count;j++)if(draw_range_overlaps(&slot->draws[j],address,size)){overlap=1;break;}
    }
    if(overlap) {
        perf_metrics.upload_overlap_polls++;
        if(draw_submissions_poll())return -1;
    } else perf_metrics.upload_no_overlap_polls_avoided++;
    int submitted=0;
    for(unsigned i=0;i<pending_draw_count;i++)if(draw_range_overlaps(&pending_draws[i],address,size)) {
        if(draw_pending_submit())return -1;
        submitted=1;
        break;
    }
    if(!overlap && !submitted)return 0;
    uint64_t wait=0;
    for(unsigned i=0;i<draw_submission_count;i++) {
        draw_submission *slot=&draw_submissions[(draw_submission_head+i)%DRAW_SUBMISSION_SLOTS];
        for(unsigned j=0;j<slot->count;j++)if(draw_range_overlaps(&slot->draws[j],address,size)){wait=slot->serial;break;}
    }
    return draw_wait_serial(wait);
}
static uint8_t *draw_index_capture;
static size_t draw_index_capture_capacity;
static uint8_t *draw_index_capture_get(size_t size) {
    if(size>4u*1024*1024)return NULL;
    if(size>draw_index_capture_capacity) {
        uint8_t *bytes=realloc(draw_index_capture,size);if(!bytes)return NULL;
        draw_index_capture=bytes;draw_index_capture_capacity=size;
    }
    return draw_index_capture;
}
static void draw_submissions_close(void) {
    if(draw_submission_count || pending_draw_count || draw_batch_cmd || draw_batch_idle_count!=draw_batch_allocated)
        die("Vulkan draw command pool closed before retirement");
    if(draw_reset_cmd || draw_reset_idle_count!=draw_reset_allocated)
        die("Vulkan query reset prefix pool closed before retirement");
    if(draw_reset_idle_count)vkFreeCommandBuffers(v.device,v.commands,draw_reset_idle_count,draw_reset_idle);
    memset(draw_reset_idle,0,sizeof draw_reset_idle);draw_reset_idle_count=draw_reset_allocated=0;draw_reset_sealed=0;
    if(draw_batch_idle_count)vkFreeCommandBuffers(v.device,v.commands,draw_batch_idle_count,draw_batch_idle);
    memset(draw_batch_idle,0,sizeof draw_batch_idle);draw_batch_idle_count=draw_batch_allocated=0;
    free(draw_index_capture);draw_index_capture=NULL;draw_index_capture_capacity=0;
    graphics_resources_idle_close();
    draw_query_pages_close();
    draw_query_cache_close();
    // close_locked has drained all draws and waited for device idle first.
    for(unsigned i=0;i<DRAW_SUBMISSION_SLOTS;i++)if(draw_submissions[i].fence)
        vkDestroyFence(v.device,draw_submissions[i].fence,NULL);
    memset(draw_submissions,0,sizeof draw_submissions);
    draw_submission_head=draw_submission_count=0;draw_submitted_serial=draw_completed_serial=0;
}

#include "vk_graphics_pipeline_cache.inc"
static void graphics_pipelines_close(void) {
    while(graphics_pipelines) {
        graphics_cached_pipeline *p=graphics_pipelines;graphics_pipelines=p->next;
        vkDestroyPipeline(v.device,p->pipeline,NULL);vkDestroyPipelineLayout(v.device,p->layout,NULL);
        for(int i=0;i<2;i++)vkDestroyDescriptorSetLayout(v.device,p->sets[i],NULL);free(p);
    }
    graphics_pipeline_cache_clear_index();draw_passes_close();
}
static dm_draw draw_pipeline_key(const dm_draw *d) {
    dm_draw key=*d;
    key.resources[0]=key.resources[1]=NULL;key.samples=NULL;
    memset(key.viewport,0,sizeof key.viewport);memset(key.scissor,0,sizeof key.scissor);
    key.index_address=0;key.index_type=key.count=key.first=key.base_vertex=key.base_instance=key.instances=key.indexed=0;
    for(int i=0;i<16;i++){key.streams[i].address=0;key.streams[i].size=0;}
    // Render-pass compatibility depends on attachment formats, not the image
    // dimensions, mip selection, sampler, storage address or texture serial.
    for(int i=0;i<8;i++){int format=key.color[i].format;memset(&key.color[i],0,sizeof key.color[i]);if(key.color_mask&(1u<<i))key.color[i].format=format;}
    int depth_format=key.depth.format;memset(&key.depth,0,sizeof key.depth);if(key.has_depth)key.depth.format=depth_format;
    return key;
}
static VkBlendFactor draw_blend_factor(int n) {
    switch(n) {
    case 1:return VK_BLEND_FACTOR_ZERO;case 2:return VK_BLEND_FACTOR_ONE;
    case 3:return VK_BLEND_FACTOR_SRC_COLOR;case 4:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5:return VK_BLEND_FACTOR_SRC_ALPHA;case 6:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7:return VK_BLEND_FACTOR_DST_ALPHA;case 8:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9:return VK_BLEND_FACTOR_DST_COLOR;case 10:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 16:return VK_BLEND_FACTOR_SRC1_COLOR;case 17:return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18:return VK_BLEND_FACTOR_SRC1_ALPHA;case 19:return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 97:return VK_BLEND_FACTOR_CONSTANT_COLOR;case 98:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 99:return VK_BLEND_FACTOR_CONSTANT_ALPHA;case 100:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:return VK_BLEND_FACTOR_MAX_ENUM;
    }
}
static VkPrimitiveTopology draw_topology(int n) {
    switch(n){case 0:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;case 1:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;case 4:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;case 6:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default:return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;}
}
static VkShaderModule draw_module(const dm_graphics_shader *s,unsigned set) {
    uint32_t *code=malloc(s->code_size);if(!code)return VK_NULL_HANDLE;memcpy(code,s->code,s->code_size);
    for(size_t at=5;at<s->code_size/4;) {
        unsigned n=code[at]>>16,op=code[at]&65535;
        if(!n || at+n>s->code_size/4){free(code);return VK_NULL_HANDLE;}
        // Every translated shader was emitted with set 0. Preserve binding
        // numbers and move fragment resources into set 1 to avoid collisions.
        if(op==71 && n==4 && code[at+2]==34) {
            if(code[at+3]){free(code);return VK_NULL_HANDLE;}code[at+3]=set;
        }
        at+=n;
    }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=s->code_size,.pCode=code};
    VkShaderModule module=VK_NULL_HANDLE;VkResult r=vkCreateShaderModule(v.device,&ci,NULL,&module);free(code);return r?VK_NULL_HANDLE:module;
}
static int draw_vertex_locations(const dm_graphics_shader *s,uint32_t *mask) {
    unsigned bound=s->code[3];if(bound>1000000)return -1;
    uint8_t *input=calloc(bound,1);int32_t *location=malloc((size_t)bound*4);if(!input || !location){free(input);free(location);return -1;}
    memset(location,255,(size_t)bound*4);int result=-1;*mask=0;
    for(size_t at=5;at<s->code_size/4;) {
        unsigned n=s->code[at]>>16,op=s->code[at]&65535;const uint32_t *p=s->code+at;
        if(!n || at+n>s->code_size/4)goto done;
        if(op==59 && n>=4 && p[2]<bound && p[3]==1)input[p[2]]=1;
        if(op==71 && n==4 && p[1]<bound && p[2]==30)location[p[1]]=p[3];
        at+=n;
    }
    for(unsigned i=0;i<bound;i++)if(input[i] && location[i]>=0){if(location[i]>=16)goto done;*mask|=1u<<location[i];}
    result=0;
done:free(input);free(location);return result;
}
// Pipelines/framebuffers retain the canonical LOAD pass. Vulkan 1.1 render-pass
// compatibility ignores load/store operations; only the begin operation uses
// this cached CLEAR variant, so the following LOAD draw can stay in this pass.
static int draw_clear_pass(const dm_draw *d,gpu_image *const targets[9],unsigned width,unsigned height,
        const VkRenderPassCreateInfo *canonical,VkRenderPass base,VkRenderPass *begin_pass,VkClearValue values[9]) {
    *begin_pass=base;
    if(!attachment_clear_count)return 0;
    for(unsigned i=0;i<9;i++)if(targets[i] && targets[i]->pending_clear) {
        pending_attachment_clear *c=targets[i]->pending_clear;
        const dm_compute_image *view=i<8?&d->color[i]:&d->depth;
        int duplicate=0;for(unsigned j=0;j<i;j++)duplicate|=targets[j]==targets[i];
        if(duplicate || !attachment_clear_view_same(c,view) || c->width!=width || c->height!=height || view->layers!=1)
            if(attachment_clear_flush_image(targets[i]))return -1;
    }
    // Fallback clear recording may fill the FIFO. Resolve capacity before
    // reading the remaining pending values and choosing the CLEAR variant.
    if(pending_draw_count==DRAW_BATCH_CAPACITY && draw_pending_submit())return -1;
    VkAttachmentDescription descriptions[9];memcpy(descriptions,canonical->pAttachments,canonical->attachmentCount*sizeof *descriptions);
    unsigned found=0;
    for(unsigned i=0;i<9;i++)if(targets[i] && targets[i]->pending_clear) {
        pending_attachment_clear *c=targets[i]->pending_clear;
        unsigned at=i<8?canonical->pSubpasses->pColorAttachments[i].attachment:canonical->pSubpasses->pDepthStencilAttachment->attachment;
        if(c->aspects&(VK_IMAGE_ASPECT_COLOR_BIT|VK_IMAGE_ASPECT_DEPTH_BIT))descriptions[at].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
        if(c->aspects&VK_IMAGE_ASPECT_STENCIL_BIT)descriptions[at].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
        values[at]=c->value;found++;
    }
    if(!found)return 0;
    VkRenderPassCreateInfo info=*canonical;info.pAttachments=descriptions;
    VkRenderPass pass=VK_NULL_HANDLE;int cached=0;
    if(draw_pass_get(&info,&pass,&cached))return -1;
    if(!cached) {
        // Do not introduce a second transient render-pass lifetime into draws.
        vkDestroyRenderPass(v.device,pass,NULL);
        for(unsigned i=0;i<9;i++)if(targets[i] && attachment_clear_flush_image(targets[i]))return -1;
        if(pending_draw_count==DRAW_BATCH_CAPACITY && draw_pending_submit())return -1;
        return 0;
    }
    if(draw_active_end())return -1;
    *begin_pass=pass;return 0;
}
#include "vk_draw_fuse.inc"
/* Shared by guest draws and startup seed preparation. No resources or commands
 * are submitted here; callers own modules, layouts and compatible render pass. */
static int draw_pipeline_create(const dm_draw *d,VkRenderPass pass,VkPipelineLayout layout,
        const VkShaderModule modules[2],VkPrimitiveTopology topology,
        unsigned ns,const VkVertexInputBindingDescription *streams,
        unsigned na,const VkVertexInputAttributeDescription *attrs,unsigned ncolor,VkPipeline *pipeline) {
    const VkPhysicalDeviceFeatures features=v.features;
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=ns,.pVertexBindingDescriptions=streams,.vertexAttributeDescriptionCount=na,.pVertexAttributeDescriptions=attrs};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=topology};
    VkPipelineViewportStateCreateInfo vs={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,
        // Keep the upstream Metal/NVN winding. The negative-height viewport
        // below supplies Metal's clip-space Y mapping for every shader/pass.
        .cullMode=d->cull,.frontFace=d->front?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE,.lineWidth=1,
        .depthBiasEnable=d->depth_bias,.depthBiasConstantFactor=d->bias_units,.depthBiasSlopeFactor=d->bias_factor,.depthBiasClamp=d->bias_clamp};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT,.alphaToCoverageEnable=d->alpha_to_coverage};
    if(d->depth_compare<1 || d->depth_compare>8)return -1;
    VkPipelineDepthStencilStateCreateInfo depth={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=d->depth_test,.depthWriteEnable=d->depth_write,.depthCompareOp=d->depth_compare-1,.stencilTestEnable=d->stencil_test};
    for(int i=0;i<2;i++) {
        const dm_stencil *s=&d->stencil[i];
        if(s->fail<1 || s->fail>8 || s->zfail<1 || s->zfail>8 || s->zpass<1 || s->zpass>8 || s->compare<1 || s->compare>8)return -1;
        VkStencilOpState state={s->fail-1,s->zpass-1,s->zfail-1,s->compare-1,s->compare_mask,s->write_mask,s->reference};
        if(i)depth.back=state;else depth.front=state;
    }
    VkPipelineColorBlendAttachmentState blend[8]={0};
    for(unsigned i=0;i<ncolor;i++) {
        const dm_blend *b=&d->blend[i];blend[i].colorWriteMask=b->mask;blend[i].blendEnable=b->enabled;
        if(b->enabled && !features.dualSrcBlend && ((b->src_rgb>=16 && b->src_rgb<=19) || (b->dst_rgb>=16 && b->dst_rgb<=19) || (b->src_alpha>=16 && b->src_alpha<=19) || (b->dst_alpha>=16 && b->dst_alpha<=19)))return -1;
        blend[i].srcColorBlendFactor=draw_blend_factor(b->src_rgb);blend[i].dstColorBlendFactor=draw_blend_factor(b->dst_rgb);
        blend[i].srcAlphaBlendFactor=draw_blend_factor(b->src_alpha);blend[i].dstAlphaBlendFactor=draw_blend_factor(b->dst_alpha);
        if(b->eq_rgb<1 || b->eq_rgb>5 || b->eq_alpha<1 || b->eq_alpha>5 || blend[i].srcColorBlendFactor==VK_BLEND_FACTOR_MAX_ENUM || blend[i].dstColorBlendFactor==VK_BLEND_FACTOR_MAX_ENUM || blend[i].srcAlphaBlendFactor==VK_BLEND_FACTOR_MAX_ENUM || blend[i].dstAlphaBlendFactor==VK_BLEND_FACTOR_MAX_ENUM)return -1;
        blend[i].colorBlendOp=b->eq_rgb-1;blend[i].alphaBlendOp=b->eq_alpha-1;
    }
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=ncolor,.pAttachments=blend};memcpy(cb.blendConstants,d->blend_constant,16);
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamic};
    VkGraphicsPipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,
        .pViewportState=&vs,.pRasterizationState=&rs,.pMultisampleState=&ms,.pDepthStencilState=&depth,.pColorBlendState=&cb,.pDynamicState=&ds,.layout=layout,.renderPass=pass,.subpass=d->fuse_subpass};
    int cached=compute_cache_open();
    VkResult result=vkCreateGraphicsPipelines(v.device,cached?VK_NULL_HANDLE:compute_cache,1,&pc,NULL,pipeline);
    if(result)LOG("Vulkan graphics pipeline result=%d",result);
    return result ? -1 : 0;
}
static int draw_execute_locked(const dm_draw *d,uint64_t fuse_token) {
    // Per-draw phase clocks are diagnostics, not part of ordinary rendering.
    // Even sampling 1/32 adds hundreds of clock syscalls per game frame;
    // HPET-backed emulators make those syscalls a measurable CPU workload.
    // Keep queue-span GPU probes and frame counters independent of this switch.
    draw_timing_sample=DM_VK_DETAIL_TIMING && (draw_timing_sequence++%DRAW_TIMING_STRIDE)==0;
    double t0=draw_time_ms(), t1=t0,t2=t0,t3=t0,t4=t0,t5=t0; size_t vertex_bytes=0; int result=-1;const char *phase="state";
    int pass_cached=0,attachments_cached=0,join_pass=0,cmd_borrowed=0,batch_owned=0;
    int persistent=0,defer=draw_batch,host_visibility=0;dm_draw key=draw_pipeline_key(d);
    int omit_depth=0;gpu_image *depth_source=NULL;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer fb=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkDescriptorSetLayout sets[2]={0};VkShaderModule modules[2]={0};
    VkImageView attachments[9]={0};gpu_image *targets[9]={0};VkCommandBuffer cmd=VK_NULL_HANDLE;VkQueryPool query=VK_NULL_HANDLE;
    unsigned query_index=0;int query_paged=0;
    gpu_image *feedback_sources[9]={0},*feedback_copies[9]={0};unsigned feedback_count=0;
    VkImageSubresourceRange feedback_ranges[9]={0};
    VkBuffer vertex[16]={0};VkDeviceSize vertex_offsets[16]={0};
    compute_scratch index={0};graphics_resources *resources[2]={0};uint64_t dynamic_profiles[2]={0};
    draw_binding_state encoded={0};
    // Only scratch owns its buffer. Resident index buffers remain pool-owned
    // and are protected by the same pending/inflight range hazards as vertices.
    VkBuffer index_buffer=VK_NULL_HANDLE;VkDeviceSize index_offset=0;
    uint8_t *index_snapshot=NULL;dm_vk_pool *index_pool=NULL;
    size_t index_pool_offset=0,index_bytes=0,index_upload_bytes=0;
    int resident_index=0,index_snapshot_owned=0,index_bounded=0;
    if(!v.device || !d->shader[0] || !d->shader[1] || d->count<0 || d->instances<0)goto done;
    if(pending_draw_count==DRAW_BATCH_CAPACITY && draw_pending_submit())goto done;
    // A draw can need 64 uniforms per stage. Poll before borrowing binding
    // storage, and only when free slots plus unallocated banks run low.
    if(graphics_uniform_under_pressure(128) && draw_submissions_poll())goto done;
    for(unsigned stage=0;stage<2;stage++)for(unsigned i=0;i<d->shader[stage]->nbindings;i++)
        if(d->resources[stage][i].writable){defer=0;host_visibility=1;}
    if(!defer && draw_pending_flush())goto done;
    if(!d->count || !d->instances){result=0;goto done;}
    // Do not discard unsupported depth or fixed-function state silently.
    if(d->polygon!=2 || d->logic!=3 || d->alpha!=8 || d->raster_samples>1 || d->base_instance<0 || (!d->indexed && d->first<0))goto done;
    if(d->viewport[2]<=0 || d->viewport[3]<=0){result=0;goto done;}
    VkPrimitiveTopology topology=draw_topology(d->primitive);if(topology==VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)goto done;
    const VkPhysicalDeviceFeatures features=v.features;
    if(d->samples && !features.occlusionQueryPrecise)goto done;
    if(d->bias_clamp && !features.depthBiasClamp)goto done;
    // Upstream TB_DS: bind depth/stencil only when the actual draw needs it.
    // Vulkan 1.1 cannot change a pass's attachments in place: its canonical
    // pass/FB key below starts a new pass whenever demand changes. Keep STORE
    // on the predecessor and never consume a pending DS clear by omission.
    if(!fuse_token && d->has_depth && d->color_mask && !d->depth_test && !d->depth_write && !d->stencil_test) {
        gpu_metrics.ds_omit_candidates++;
        depth_source=image_get(&d->depth);
        if(!depth_source)goto done;
        if(depth_source->pending_clear)gpu_metrics.ds_omit_clear_fallbacks++;
        else {omit_depth=1;key.has_depth=0;memset(&key.depth,0,sizeof key.depth);}
    }
    graphics_cached_pipeline *cached_pipeline=graphics_pipeline_find(&key);
    if(cached_pipeline) {
        graphics_last_pipeline=cached_pipeline;
        pipeline=cached_pipeline->pipeline;layout=cached_pipeline->layout;memcpy(sets,cached_pipeline->sets,sizeof sets);persistent=1;
        memcpy(dynamic_profiles,cached_pipeline->dynamic_profiles,sizeof dynamic_profiles);
    } else graphics_dynamic_profiles(d->shader,dynamic_profiles);
    uint32_t width=0,height=0,nattachment=0,ncolor=0;
    VkImageViewCreateInfo attachment_views[9]={0};
    VkAttachmentDescription descriptions[9]={0};VkAttachmentReference references[8],depth_reference={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    for(unsigned i=0;i<8;i++)references[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    phase="attachments";
    for(unsigned i=0;i<8;i++)if(d->color_mask&(1u<<i)) {
        const dm_compute_image *a=&d->color[i];gpu_image *p=image_get(a);targets[i]=p;
        if(!p || !(p->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || p->desc.target==2 || a->layers!=1 || a->base_level<0 || (unsigned)a->base_level>=p->desc.levels || a->base_layer<0 || (unsigned)a->base_layer>=p->layers)goto done;
        unsigned w=image_mip(p->extent.width,a->base_level),h=image_mip(p->extent.height,a->base_level);
        // Match upstream pass_dims: attachments may select different mip sizes.
        width=width && width<w?width:w;height=height && height<h?height:h;
        if(!fuse_token && image_attachment_prepare(p))goto done;
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=p->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
            .format=image_format(a->format),.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,a->base_level,1,a->base_layer,1}};
        attachment_views[nattachment]=vi;
        descriptions[nattachment]=(VkAttachmentDescription){.format=vi.format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
        references[i].attachment=nattachment++;ncolor=i+1;
    }
    if(d->has_depth) {
        const dm_compute_image *a=&d->depth;gpu_image *p=depth_source?depth_source:image_get(a);
        if(!p || !(p->usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) || p->desc.target==2 || a->layers!=1 || a->base_level<0 || (unsigned)a->base_level>=p->desc.levels || a->base_layer<0 || (unsigned)a->base_layer>=p->layers)goto done;
        unsigned w=image_mip(p->extent.width,a->base_level),h=image_mip(p->extent.height,a->base_level);
        // Match upstream pass_dims: attachments may select different mip sizes.
        width=width && width<w?width:w;height=height && height<h?height:h;
        // The guest's original DS view still constrains render extent, even
        // when no access is made. It is not an attachment/feedback/dirty owner.
        if(!omit_depth) {
        targets[8]=p;
        if(!fuse_token && image_attachment_prepare(p))goto done;
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=p->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
            .format=image_format(a->format),.subresourceRange={p->aspect,a->base_level,1,a->base_layer,1}};
        attachment_views[nattachment]=vi;
        descriptions[nattachment]=(VkAttachmentDescription){.format=vi.format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};depth_reference.attachment=nattachment++;
        }
    }
    if(!nattachment)goto done;
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=ncolor,.pColorAttachments=references,.pDepthStencilAttachment=d->has_depth && !omit_depth?&depth_reference:NULL};
    // ALL_COMMANDS in a subpass dependency means ALL_GRAPHICS. Explicitly
    // include our compute/transfer producers and timestamp execution points
    // so the incoming dependency also replaces the old pre-pass barrier.
    const VkPipelineStageFlags prior_device_stages=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT|
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT|VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubpassDependency dependencies[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=prior_device_stages,.dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT}};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=nattachment,.pAttachments=descriptions,.subpassCount=1,.pSubpasses=&sub,.dependencyCount=2,.pDependencies=dependencies};
    if(fuse_token) {
        if(draw_fuse.token!=fuse_token){result=2;goto done;}
        pass=draw_fuse.pass;fb=draw_fuse.fb;pass_cached=attachments_cached=1;
        memcpy(attachments,draw_fuse.views,sizeof attachments);
        width=draw_fuse.width;height=draw_fuse.height;
    } else {
        if(draw_pass_get(&rp,&pass,&pass_cached))goto done;
        if(draw_attachments_get(pass,pass_cached,attachment_views,nattachment,width,height,1,attachments,&fb,&attachments_cached))goto done;
    }
    t1=draw_time_ms(); phase="shader resources";
    for(unsigned i=0;i<d->shader[0]->nbindings;i++)for(unsigned j=0;j<d->shader[1]->nbindings;j++) {
        const dm_compute_buffer *a=&d->resources[0][i],*b=&d->resources[1][j];
        if(a->address && b->address && (a->writable || b->writable) && a->address<b->address+b->size && b->address<a->address+a->size) {
            phase="cross-stage writable buffer alias";goto done;
        }
    }
    for(unsigned stage=0;stage<2;stage++) {
        const dm_graphics_shader *s=d->shader[stage];if(s->stage!=stage)goto done;
        dm_compute_buffer sampled[64];dm_compute_image snapshots[64];
        memcpy(sampled,d->resources[stage],s->nbindings*sizeof *sampled);
        for(unsigned i=0;i<s->nbindings;i++)if(sampled[i].image && s->bindings[i].kind!=DM_BIND_INPUT_ATTACHMENT) {
            // Non-packaged images have no guest alias/report preparation here.
            // Unique serials can rule out feedback before resolving the image;
            // normal stage preparation still creates/prepares the resource.
            if(!sampled[i].image->packaged) {
                int attached=0;
                for(unsigned j=0;j<9;j++)if(targets[j] && targets[j]->serial==sampled[i].image->serial){attached=1;break;}
                if(!attached)continue;
            }
            gpu_image *source=image_get(sampled[i].image);
            int aliases=0,overlap=0;
            for(unsigned j=0;j<9;j++)if(source && source==targets[j])aliases=1;
            if(!aliases)continue;
            if(sampled[i].writable){phase="writable attachment alias";goto done;}
            VkImageViewType view_type;VkImageSubresourceRange view_range;
            int precise=source->desc.target!=2 &&
                compute_binding_type(&s->bindings[i])!=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                compute_binding_type(&s->bindings[i])!=VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
                !graphics_image_view_range(source,sampled[i].image,&s->bindings[i],&view_type,&view_range) &&
                view_type!=VK_IMAGE_VIEW_TYPE_3D;
            for(unsigned j=0;j<9;j++)if(source && source==targets[j]) {
                const dm_compute_image *a=j<8?&d->color[j]:&d->depth;
                // Actual attachment views contain exactly one mip and layer.
                // Keep aspect handling conservative, including packed DS.
                if(!precise || ((unsigned)a->base_level>=view_range.baseMipLevel &&
                    (unsigned)a->base_level-view_range.baseMipLevel<view_range.levelCount &&
                    (unsigned)a->base_layer>=view_range.baseArrayLayer &&
                    (unsigned)a->base_layer-view_range.baseArrayLayer<view_range.layerCount))overlap=1;
            }
            if(aliases && precise)gpu_metrics.feedback_range_eligible++;
            if(aliases && !overlap){gpu_metrics.feedback_range_disjoint++;continue;}
            if(aliases) {
                unsigned at=0;while(at<feedback_count && feedback_sources[at]!=source)at++;
                if(at==feedback_count) {
                    if(feedback_count>=9){phase="feedback copy count";goto done;}
                    gpu_image *copy=image_feedback_get(source);
                    if(!copy){phase="feedback copy allocation";goto done;}
                    feedback_sources[at]=source;feedback_copies[at]=copy;feedback_count++;
                }
                // Keep the complete snapshot allocation and original view
                // coordinates. Only its copied contents are narrowed; all
                // redirected bindings in both stages contribute to this box.
                VkImageSubresourceRange required=precise?view_range:
                    (VkImageSubresourceRange){source->aspect,0,source->desc.levels,0,source->layers};
                VkImageSubresourceRange *range=&feedback_ranges[at];
                if(!range->levelCount)*range=required;
                else {
                    unsigned mip_end=range->baseMipLevel+range->levelCount;
                    unsigned layer_end=range->baseArrayLayer+range->layerCount;
                    unsigned required_mip_end=required.baseMipLevel+required.levelCount;
                    unsigned required_layer_end=required.baseArrayLayer+required.layerCount;
                    if(required_mip_end>mip_end)mip_end=required_mip_end;
                    if(required_layer_end>layer_end)layer_end=required_layer_end;
                    if(required.baseMipLevel<range->baseMipLevel)range->baseMipLevel=required.baseMipLevel;
                    if(required.baseArrayLayer<range->baseArrayLayer)range->baseArrayLayer=required.baseArrayLayer;
                    range->levelCount=mip_end-range->baseMipLevel;
                    range->layerCount=layer_end-range->baseArrayLayer;
                    range->aspectMask|=required.aspectMask;
                }
                // Both shader stages use the same pre-draw GPU copy. It has
                // no guest backing, so descriptor creation cannot cause the
                // old GPU -> guest -> GPU feedback round trip.
                snapshots[i]=*sampled[i].image;
                snapshots[i].serial=feedback_copies[at]->serial;
                snapshots[i].address=0;snapshots[i].desc=source->desc;
                sampled[i].image=&snapshots[i];
            }
        }
        VkDescriptorSetLayoutBinding bindings[64];
        uint64_t dynamic_profile=dynamic_profiles[stage];
        for(unsigned i=0;i<s->nbindings;i++)bindings[i]=(VkDescriptorSetLayoutBinding){s->bindings[i].binding,graphics_binding_type(&s->bindings[i],dynamic_profile,i),1,stage?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT,NULL};
        VkDescriptorSetLayoutCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=s->nbindings,.pBindings=bindings};
        if(!persistent && vkCreateDescriptorSetLayout(v.device,&ci,NULL,&sets[stage]))goto done;
        int reusable=defer && persistent && pass_cached && attachments_cached && !feedback_count;
        if(reusable && active_draw_pass.cmd && active_draw_pass.pass==pass && active_draw_pass.fb==fb && active_draw_pass.samples==d->samples) {
            uint64_t epoch=active_draw_epoch;
            for(unsigned at=0;at<DRAW_STAGE_CACHE_COUNT;at++) {
                graphics_resources *candidate=active_draw_pass.stages[stage][at];
                if(!candidate || candidate->shader!=s)continue;
                candidate->refs++; // Validation may drain and retire every cache lease.
                double validate_start=gpu_detail_time_ms();
                int same=graphics_stage_matches(candidate,s,sets[stage],sampled,dynamic_profile);
                gpu_metrics.stage_validate_ms+=gpu_detail_time_ms()-validate_start;
                if(same && epoch==active_draw_epoch && active_draw_pass.cmd) {
                    resources[stage]=candidate;gpu_metrics.stage_hits++;break;
                }
                if(!same && epoch==active_draw_epoch && active_draw_pass.cmd) {
                    graphics_resources *shifted=graphics_stage_reoffset(candidate,s,sets[stage],sampled,dynamic_profile);
                    if(shifted && epoch==active_draw_epoch && active_draw_pass.cmd) {
                        resources[stage]=shifted;gpu_metrics.stage_hits++;gpu_metrics.dynamic_stage_reuse++;
                    } else {
                        graphics_resources_free(shifted);gpu_metrics.dynamic_stage_fallback++;
                    }
                }
                graphics_resources_free(candidate);
                if(resources[stage])break;
                // A validation upload/alias flush may have cleared the entire
                // LRU. Do not inspect another entry from the expired pass.
                if(epoch!=active_draw_epoch || !active_draw_pass.cmd)break;
            }
        }
        if(!resources[stage]) {
            gpu_metrics.stage_misses++;
            resources[stage]=graphics_resources_acquire();if(!resources[stage])goto done;
            graphics_resources *image_source=NULL;
            uint64_t image_epoch=active_draw_epoch;
            if(reusable && active_draw_pass.cmd && active_draw_pass.pass==pass &&
               active_draw_pass.fb==fb && active_draw_pass.samples==d->samples) {
                for(unsigned at=0;at<DRAW_STAGE_CACHE_COUNT;at++) {
                    graphics_resources *candidate=active_draw_pass.stages[stage][at];
                    if(candidate && candidate->shader==s && candidate->count==s->nbindings &&
                       candidate->dynamic_profile==dynamic_profile) {
                        image_source=candidate;image_source->refs++;break;
                    }
                }
            }
            // Uniform/image visibility preparation may end the pass and drop
            // its cache leases. Pin the source until every binding is prepared;
            // the new resource object owns its own retained view/sampler refs.
            int prepared=graphics_resources_new(resources[stage],s,sampled,s->nbindings,sets[stage],dynamic_profile,reusable,
                                                image_source,&active_draw_epoch,image_epoch);
            graphics_resources_free(image_source);
            if(prepared)goto done;
            if(graphics_resources_write_set(resources[stage]))goto done;
        }
        if(!persistent){modules[stage]=draw_module(s,stage);if(!modules[stage])goto done;}
    }
    VkPushConstantRange uniform_push={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,.offset=0,.size=128};
    VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=2,.pSetLayouts=sets,
        .pushConstantRangeCount=1,.pPushConstantRanges=&uniform_push};
    if(!persistent && vkCreatePipelineLayout(v.device,&lc,NULL,&layout))goto done;
    t2=draw_time_ms(); phase="vertex input";uint32_t used_attributes=0,used_streams=0;
    if(cached_pipeline)used_attributes=cached_pipeline->vertex_locations;
    else if(draw_vertex_locations(d->shader[0],&used_attributes))goto done;
    VkVertexInputAttributeDescription attrs[16];VkVertexInputBindingDescription streams[16];unsigned na=0,ns=0;
    for(unsigned i=0;i<16;i++)if(used_attributes&(1u<<i)) {
        if(i>=d->nattributes)goto done;const dm_vertex_attribute *a=&d->attributes[i];if(a->stream>=16)goto done;
        VkFormat format=image_format(a->format);VkFormatProperties props;
        if((unsigned)format<256) {
            if(!v.format_properties_valid[format]) {
                vkGetPhysicalDeviceFormatProperties(v.gpu,format,&v.format_properties[format]);
                v.format_properties_valid[format]=1;
            }
            props=v.format_properties[format];
        } else vkGetPhysicalDeviceFormatProperties(v.gpu,format,&props);
        if(!format || !(props.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))goto done;
        attrs[na++]=(VkVertexInputAttributeDescription){i,a->stream,format,a->offset};used_streams|=1u<<a->stream;
    }
    VkIndexType index_type=VK_INDEX_TYPE_UINT16;
    int64_t first_vertex=d->first,last_vertex=(int64_t)d->first+d->count-1;
    if(d->indexed) {
        if(d->index_type<0 || d->index_type>2)goto done;
        if(images_flush_range(d->index_address,(size_t)d->count*(1u<<d->index_type)))goto done;
        index_bytes=(size_t)d->count*(1u<<d->index_type);
        index_pool=pool_address_locked(d->index_address,index_bytes,&index_pool_offset);if(!index_pool)goto done;
        if(reports_range_drain(d->index_address,index_bytes))goto done;
        // Absence of pending GPU writes does not make a CPU_NO_ACCESS pool's
        // guest mirror a valid source for this optional bounds optimization.
        int cpu_owned=!(index_pool->flags&0x1u);
        for(pool_gpu_range *r=index_pool->gpu_dirty;r;r=r->next)
            if(index_pool_offset<r->hi && r->lo<index_pool_offset+index_bytes){cpu_owned=0;break;}
        index_type=d->index_type==2?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16;
        resident_index=d->index_type && !(index_pool_offset&((1u<<d->index_type)-1));
        index_upload_bytes=d->index_type?index_bytes:index_bytes*2;
        if(resident_index && pool_resident_locked(index_pool))goto done;
        int shared_index=resident_index && index_pool->shared_heap;
        // CPU-authoritative indices can prove vertex bounds without any GPU
        // readback. Retain this exact version for the final resident upload.
        // Shared backing needs neither upload bounds nor snapshot writeback;
        // use resident indices directly under the game's NVN fence contract.
        if(cpu_owned && !shared_index)index_snapshot=draw_index_capture_get(index_upload_bytes);
        if(!resident_index && !index_snapshot) {
            // Existing Vulkan 1.1 widening/unaligned fallback remains intact.
            if(!cpu_owned && pool_cpu_read_locked(index_pool,index_pool_offset,index_bytes))goto done;
            index_snapshot=malloc(index_upload_bytes);if(!index_snapshot)goto done;index_snapshot_owned=1;
        }
        if(index_snapshot) {
            if(!d->index_type)for(int i=0;i<d->count;i++)((uint16_t *)index_snapshot)[i]=index_pool->guest[index_pool_offset+i];
            else memcpy(index_snapshot,index_pool->guest+index_pool_offset,index_bytes);
            if(cpu_owned && index_upload_bytes<=4u*1024*1024) {
                uint32_t lo=UINT32_MAX,hi=0;
                for(int i=0;i<d->count;i++) {
                    uint32_t value;
                    if(d->index_type==2)memcpy(&value,index_snapshot+(size_t)i*4,4);
                    else {uint16_t word;memcpy(&word,index_snapshot+(size_t)i*2,2);value=word;}
                    // Primitive restart is disabled; all-ones is an index too.
                    if(value<lo)lo=value;if(value>hi)hi=value;
                }
                first_vertex=(int64_t)lo+d->base_vertex;last_vertex=(int64_t)hi+d->base_vertex;
                index_bounded=first_vertex>=0 && last_vertex<=UINT32_MAX;
            }
        }
        if(index_bounded)gpu_metrics.index_bounded++;
        else if(shared_index)gpu_metrics.index_shared_direct++;
        else if(!cpu_owned)gpu_metrics.index_gpu_fallback++;
        else gpu_metrics.index_bounds_fallback++;
    }
    uint64_t stream_begin[16],stream_end[16]={0};memset(stream_begin,255,sizeof stream_begin);
    unsigned conservative_streams=0;
    for(unsigned i=0;i<16;i++)if(used_attributes&(1u<<i)) {
        const dm_vertex_attribute *a=&d->attributes[i];const dm_vertex_stream *s=&d->streams[a->stream];
        if(d->indexed && !s->divisor && !index_bounded){conservative_streams|=1u<<a->stream;continue;}
        uint64_t last=s->divisor?(uint64_t)d->base_instance+d->instances-1:(uint64_t)last_vertex;
        int width=nvn_fmt_bpb(a->format,NULL,NULL);
        if((!s->divisor && first_vertex<0) || last>UINT32_MAX || width<=0 ||
            last*s->stride>UINT64_MAX-a->offset-(unsigned)width) {
            if(!d->indexed)goto done;conservative_streams|=1u<<a->stream;continue;
        }
        uint64_t first=s->divisor?(uint64_t)d->base_instance:(uint64_t)first_vertex;
        uint64_t begin=first*s->stride+a->offset,end=last*s->stride+a->offset+(unsigned)width;
        if(!d->indexed && end>s->size)goto done;
        if(begin<stream_begin[a->stream])stream_begin[a->stream]=begin;
        if(end>stream_end[a->stream])stream_end[a->stream]=end;
    }
    for(unsigned i=0;i<16;i++)if(used_streams&(1u<<i)) {
        const dm_vertex_stream *s=&d->streams[i];size_t off=0;
        // Like mtl_gpu_addr(address,1), resolve the binding origin first.
        // A declared range may extend past this pool; it cannot make backing
        // storage larger. Bound preparation/hazards by the actual remainder.
        dm_vk_pool *pool=pool_address_locked(s->address,1,&off);
        if(!pool || s->divisor>1)goto done;
        size_t available=pool->size-off;
        size_t declared=s->size && s->size<available?(size_t)s->size:available;
        if(d->indexed) {
            gpu_metrics.vertex_requested_before+=declared;
            if((conservative_streams&(1u<<i)) || stream_end[i]>declared || stream_begin[i]>=stream_end[i]) {
                stream_begin[i]=0;stream_end[i]=declared;
            }
        } else if(stream_end[i]>available)goto done;
        size_t begin=stream_begin[i],bytes=stream_end[i]-begin;
        if(d->indexed)gpu_metrics.vertex_requested_after+=bytes;
        if(images_flush_range(s->address+begin,bytes) || pool_gpu_read_consumer_locked(pool,off+begin,bytes,POOL_PREP_VERTEX))goto done;
        // Keep the original binding origin and draw indices: VertexIndex and
        // InstanceIndex must retain guest semantics even when only a slice is copied.
        vertex[i]=pool->buffer;vertex_offsets[i]=off;vertex_bytes+=bytes;
        streams[ns++]=(VkVertexInputBindingDescription){i,s->stride,s->divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};
    }
    if(d->indexed) {
        if(resident_index) {
            // GPU-owned indices remain on the GPU. Vertex/index aliases read
            // the same resident version and need no independent CPU snapshot.
            // Preparation can still drain an earlier reader on a CPU upload.
            if(index_snapshot) {
                if(pool_resident_locked(index_pool) || pool_upload_consumer_locked(index_pool,index_pool_offset,index_bytes,index_snapshot,POOL_PREP_INDEX))goto done;
            } else if(pool_gpu_read_consumer_locked(index_pool,index_pool_offset,index_bytes,POOL_PREP_INDEX))goto done;
            index_buffer=index_pool->buffer;index_offset=index_pool_offset;
        } else {
            // Vulkan 1.1 needs 8-bit indices widened; unaligned bindings use
            // the same standalone aligned snapshot without scanning min/max.
            if(scratch_new(&index,index_upload_bytes))goto done;
            memcpy(index.mapped,index_snapshot,index_upload_bytes);
            if(scratch_sync(&index,0))goto done;
            index_buffer=index.buffer;
        }
        if(index_snapshot_owned)free(index_snapshot);index_snapshot=NULL;index_snapshot_owned=0;
    }
    t3=draw_time_ms(); phase="graphics pipeline";
    if(!persistent) {
        if(draw_pipeline_create(d,pass,layout,modules,topology,ns,streams,na,attrs,ncolor,&pipeline))goto done;
        compute_cache_checkpoint();
        {
            graphics_cached_pipeline *entry=calloc(1,sizeof *entry);
            if(entry){entry->key=key;entry->pipeline=pipeline;entry->layout=layout;entry->vertex_locations=used_attributes;memcpy(entry->sets,sets,sizeof sets);memcpy(entry->dynamic_profiles,dynamic_profiles,sizeof dynamic_profiles);graphics_pipeline_insert(entry);persistent=1;}
        }
    }
    t4=draw_time_ms(); phase="draw submission";
    // A physical pass may contain several precise counting segments. Each
    // segment belongs to its first draw, before any report that observes it.
    join_pass=defer && persistent;
    // Cache saturation is not a GPU completion dependency. Temporary passes,
    // framebuffers and views belong to this draw until its real batch fence.
    // Different handles below still end the previous physical render pass.
    // Writable/readback fallbacks retain standalone completion. Feedback only
    // needs a render-pass break for its GPU copy, not a submission or CPU wait.
    if(!join_pass && draw_pending_flush())goto done;
    if(pending_draw_count==DRAW_BATCH_CAPACITY && draw_pending_submit())goto done;
    // Submit standalone predecessors before selecting load operations: submit
    // materializes pending clears, so none may occur after selecting CLEAR.
    if(join_pass && pending_draw_count && !draw_batch_cmd && draw_pending_submit())goto done;
    VkRenderPass begin_pass=pass;VkClearValue clear_values[9]={0};
    if(!fuse_token && join_pass && draw_clear_pass(d,targets,width,height,&rp,pass,&begin_pass,clear_values))goto done;
    if(fuse_token) {
        // This is the final fallible/preparation boundary. No variant draw may
        // encode after an upload/submit ended its two-subpass transaction.
        if(draw_fuse.token!=fuse_token || !join_pass || feedback_count ||
           (draw_fuse.started && (active_draw_pass.cmd!=draw_fuse.cmd || active_draw_pass.pass!=pass || active_draw_pass.fb!=fb)) ||
           (d->fuse_subpass && !draw_fuse.started)) {result=2;goto done;}
        int transition=d->fuse_subpass && !draw_fuse.physical;
        int new_query=d->samples && (transition || !active_draw_pass.query || active_draw_pass.samples!=d->samples);
        if(new_query) {if(draw_query_page_acquire(&query,&query_index))goto done;query_paged=1;}
        if(!draw_fuse.started) {
            if(active_draw_pass.cmd){result=2;goto done;}
            if(draw_batch_begin())goto done;
            cmd=draw_batch_cmd;batch_owned=1;
            VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{width,height}}};
            vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
            draw_fuse.cmd=cmd;draw_fuse.started=1;draw_fuse.physical=0;
        } else {
            if(transition || active_draw_pass.samples!=d->samples)draw_active_query_end();
            if(transition) {
                vkCmdNextSubpass(active_draw_pass.cmd,VK_SUBPASS_CONTENTS_INLINE);draw_fuse.physical=1;active_draw_epoch++;gpu_metrics.fuse_transitions++;
                memset(&active_draw_pass.bindings,0,sizeof active_draw_pass.bindings);active_draw_pass.pipeline=VK_NULL_HANDLE;
                for(unsigned i=0;i<2;i++)for(unsigned j=0;j<DRAW_STAGE_CACHE_COUNT;j++) {
                    graphics_resources *r=active_draw_pass.stages[i][j];active_draw_pass.stages[i][j]=NULL;graphics_resources_free(r);
                }
            }
            cmd=active_draw_pass.cmd;cmd_borrowed=1;batch_owned=1;encoded=active_draw_pass.bindings;
        }
    } else {
    if(active_draw_pass.cmd && (feedback_count || !join_pass || active_draw_pass.pass!=pass || active_draw_pass.fb!=fb))
        if(draw_active_end())goto done;
    if(active_draw_pass.cmd && active_draw_pass.samples!=d->samples)draw_active_query_end();
    if(join_pass && active_draw_pass.cmd) {
        cmd=active_draw_pass.cmd;cmd_borrowed=1;batch_owned=1;encoded=active_draw_pass.bindings;
        if(d->samples && !active_draw_pass.query) {
            if(draw_query_page_acquire(&query,&query_index))goto done;
            query_paged=1;
        }
    } else {
        if(d->samples && !join_pass) {
            if(draw_query_acquire(&query))goto done;
        }
        if(join_pass) {
            // All resource preparation and predecessor submission is complete.
            if(draw_batch_begin())goto done;
            cmd=draw_batch_cmd;batch_owned=1;
        } else {
            VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=v.commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
            if(vkAllocateCommandBuffers(v.device,&ca,&cmd))goto done;
            VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            if(vkBeginCommandBuffer(cmd,&bi))goto done;
        }
        for(unsigned i=0;i<feedback_count;i++)if(image_feedback_encode(cmd,feedback_sources[i],feedback_copies[i],&feedback_ranges[i])) {
            phase="feedback copy recording";goto done;
        }
        // The incoming RP dependency covers prior device commands. Submission
        // supplies host-write visibility after the existing flush/hazard path;
        // query resets establish their own same-query execution dependencies.
        if(d->samples && join_pass) {
            if(draw_query_page_acquire(&query,&query_index))goto done;
            query_paged=1;
        } else if(query) {
            vkCmdResetQueryPool(cmd,query,0,1);perf_metrics.query_standalone_resets++;
        }
        VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=begin_pass,.framebuffer=fb,.renderArea={{0,0},{width,height}},
            .clearValueCount=begin_pass!=pass?nattachment:0,.pClearValues=clear_values};
        vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
        gpu_metrics.pass_barrier_merged++;
        if(begin_pass!=pass)for(unsigned i=0;i<9;i++)if(targets[i])attachment_clear_consume(targets[i]);
    }
    } // ordinary single-subpass encoder
    unsigned pass_viewport_valid=encoded.fixed.viewport_valid,pass_scissor_valid=encoded.fixed.scissor_valid;
    unsigned pass_vertex_valid=encoded.fixed.vertex_valid,pass_index_valid=encoded.fixed.index_valid;
    if(batch_owned) {
        if(cmd!=draw_batch_cmd || draw_batch_sealed)die("Vulkan binding shadow has no recording batch");
        encoded.fixed=draw_batch_bindings;
    }
    if(!cmd_borrowed || active_draw_pass.pipeline!=pipeline)vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    if(d->shader[0]->guarded_uniform_abi==1 || d->shader[1]->guarded_uniform_abi==1) {
        uint32_t counts[32];
        for(unsigned stage=0;stage<2;stage++)memcpy(counts+stage*16,resources[stage]->uniform_vec4_counts,16*sizeof(uint32_t));
        if(!encoded.uniform_counts_valid || memcmp(encoded.uniform_counts,counts,sizeof counts)) {
            vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof counts,counts);
            memcpy(encoded.uniform_counts,counts,sizeof counts);encoded.uniform_counts_valid=1;gpu_metrics.uniform_push_calls++;
        } else gpu_metrics.uniform_push_reuses++;
    }
    VkDescriptorSet bound[2]={resources[0]->set,resources[1]->set};
    uint32_t dynamic_offsets[64];unsigned dynamic_count=0;
    for(unsigned stage=0;stage<2;stage++)for(unsigned i=0;i<resources[stage]->dynamic_count;i++)
        dynamic_offsets[dynamic_count++]=resources[stage]->dynamic_offsets[i];
    // Exact layout-handle equality deliberately avoids broader Vulkan layout
    // compatibility assumptions. Offsets belong to this draw, not set owners.
    if(!encoded.descriptor_valid || encoded.descriptor_layout!=layout ||
       encoded.sets[0]!=bound[0] || encoded.sets[1]!=bound[1] ||
       encoded.dynamic_count!=dynamic_count ||
       memcmp(encoded.dynamic_offsets,dynamic_offsets,dynamic_count*sizeof *dynamic_offsets)) {
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,2,bound,dynamic_count,dynamic_count?dynamic_offsets:NULL);
        encoded.descriptor_valid=1;encoded.descriptor_layout=layout;
        memcpy(encoded.sets,bound,sizeof bound);encoded.dynamic_count=dynamic_count;
        memcpy(encoded.dynamic_offsets,dynamic_offsets,dynamic_count*sizeof *dynamic_offsets);
    }
    // Metal maps positive clip Y toward the top of the render target. Vulkan
    // 1.1 negative viewport height gives the same mapping without modifying
    // translated shaders; scissor and texture coordinates remain unchanged.
    VkViewport viewport={d->viewport[0],d->viewport[1]+d->viewport[3],d->viewport[2],-d->viewport[3],d->viewport[4],d->viewport[5]};
    int64_t x=d->scissor[0],y=d->scissor[1],right=x+d->scissor[2],bottom=y+d->scissor[3];if(x<0)x=0;if(y<0)y=0;if(x>width)x=width;if(y>height)y=height;if(right>width)right=width;if(bottom>height)bottom=height;
    VkRect2D scissor={{x,y},{right>x?right-x:0,bottom>y?bottom-y:0}};
    if(!encoded.fixed.viewport_valid || memcmp(&encoded.fixed.viewport,&viewport,sizeof viewport)) {
        vkCmdSetViewport(cmd,0,1,&viewport);encoded.fixed.viewport=viewport;encoded.fixed.viewport_valid=1;
    } else if(batch_owned && !pass_viewport_valid)gpu_metrics.cb_binding_reuse[0]++;
    if(!encoded.fixed.scissor_valid || memcmp(&encoded.fixed.scissor,&scissor,sizeof scissor)) {
        vkCmdSetScissor(cmd,0,1,&scissor);encoded.fixed.scissor=scissor;encoded.fixed.scissor_valid=1;
    } else if(batch_owned && !pass_scissor_valid)gpu_metrics.cb_binding_reuse[1]++;
    for(unsigned first=0;first<16;) {
        if(!(used_streams&(1u<<first))){first++;continue;}
        unsigned end=first,changed=0;
        while(end<16 && (used_streams&(1u<<end))) {
            changed+=!(encoded.fixed.vertex_valid&(1u<<end)) || encoded.fixed.vertex[end]!=vertex[end] ||
                encoded.fixed.vertex_offsets[end]!=vertex_offsets[end];
            end++;
        }
        if(changed) {
            // All slots in this contiguous used run were prepared above.
            // Rebinding unchanged slots is legal and avoids per-stream calls.
            vkCmdBindVertexBuffers(cmd,first,end-first,&vertex[first],&vertex_offsets[first]);
            gpu_metrics.vertex_bind_calls++;gpu_metrics.vertex_bind_changed+=changed;
            gpu_metrics.vertex_bind_slots+=end-first;
            for(unsigned i=first;i<end;i++) {
                encoded.fixed.vertex[i]=vertex[i];encoded.fixed.vertex_offsets[i]=vertex_offsets[i];encoded.fixed.vertex_valid|=1u<<i;
            }
        } else if(batch_owned && ((pass_vertex_valid>>first)&((1u<<(end-first))-1))!=((1u<<(end-first))-1))
            gpu_metrics.cb_binding_reuse[2]++;
        first=end;
    }
    if(query) {
        vkCmdBeginQuery(cmd,query,query_index,VK_QUERY_CONTROL_PRECISE_BIT);
        perf_metrics.query_segments++;
    }
    if(d->indexed){
        if(!encoded.fixed.index_valid || encoded.fixed.index!=index_buffer || encoded.fixed.index_offset!=index_offset || encoded.fixed.index_type!=index_type) {
            vkCmdBindIndexBuffer(cmd,index_buffer,index_offset,index_type);
            encoded.fixed.index=index_buffer;encoded.fixed.index_offset=index_offset;encoded.fixed.index_type=index_type;encoded.fixed.index_valid=1;
        } else if(batch_owned && !pass_index_valid)gpu_metrics.cb_binding_reuse[3]++;
        vkCmdDrawIndexed(cmd,d->count,d->instances,0,d->base_vertex,d->base_instance);
    }
    else vkCmdDraw(cmd,d->count,d->instances,d->first,d->base_instance);
    if(batch_owned)draw_batch_bindings=encoded.fixed;
    if(fuse_token && d->fuse_subpass)gpu_metrics.fuse_q_draws++;
    if(!join_pass) {
        if(query)vkCmdEndQuery(cmd,query,query_index);
        vkCmdEndRenderPass(cmd);
        // Writable descriptor fallback reads scratch buffers directly on the
        // CPU after completion. Retain host visibility for that path only.
        if(host_visibility) {
            VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);
        }
        if(vkEndCommandBuffer(cmd))goto done;
    }
    if(!persistent)defer=0; // An allocation failure must not destroy an in-use pipeline.
    uint64_t samples=0;
    if(!defer) {
        if(draw_pending_flush())goto done;
        VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
        if(vkQueueSubmit(v.queue,1,&si,VK_NULL_HANDLE) || vkQueueWaitIdle(v.queue))goto done;
        for(unsigned i=0;i<feedback_count;i++)feedback_copies[i]->layout=VK_IMAGE_LAYOUT_GENERAL;
        if(query) {
            gpu_metrics.report_samples_queried++;
            if(vkGetQueryPoolResults(v.device,query,query_index,1,sizeof samples,&samples,sizeof samples,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT))goto done;
            sample_owner *o=sample_owner_get(d->samples);if(!o)goto done;
            o->interval->total+=samples;
        }
    }
    static unsigned draw_report;
    if(!defer && draw_report++<24)LOG("Vulkan game draw complete: vs=%s ps=%s samples=%llu target=%ux%u viewport=%g,%g,%g,%g cull=%d front=%d",d->shader[0]->key,d->shader[1]->key,(unsigned long long)samples,width,height,d->viewport[0],d->viewport[1],d->viewport[2],d->viewport[3],d->cull,d->front);
    t5=draw_time_ms();
    if(omit_depth){gpu_metrics.ds_omit_draws++;gpu_metrics.ds_omit_passes+=!cmd_borrowed;}
    for(unsigned i=0;i<8;i++)if(targets[i] && d->blend[i].mask) {
        if(draw_batch || !targets[i]->packaged)image_set_dirty(targets[i],1);
        else if(image_download_rect(targets[i],&d->color[i],scissor))goto done;
    }
    /* Merely binding depth does not make it writable. UI draws commonly keep
     * the full-size scene depth attached with both tests disabled; downloading
     * that unchanged surface per glyph was dominating the loading frame. */
    int depth_changed=d->depth_test && d->depth_write;
    int stencil_changed=d->stencil_test &&
        ((d->stencil[0].write_mask && (d->stencil[0].fail!=1 || d->stencil[0].zfail!=1 || d->stencil[0].zpass!=1)) ||
         (d->stencil[1].write_mask && (d->stencil[1].fail!=1 || d->stencil[1].zfail!=1 || d->stencil[1].zpass!=1)));
    if(targets[8] && (depth_changed || stencil_changed)) {
        if(draw_batch || !targets[8]->packaged)image_set_dirty(targets[8],1);else if(image_transfer(targets[8],1))goto done;
    }
    if(!join_pass)for(unsigned i=0;i<2;i++)if(graphics_resources_readback(resources[i],d->resources[i],d->shader[i]->nbindings))goto done;
    { static unsigned timing_count; double end=draw_time_ms();
      static double report_at,totals[7];static unsigned draws,samples;static size_t copied;
      // Actual draw/byte counts include every draw. Time sums include only
      // measured samples and are never scaled up to pretend full coverage.
      draws++;copied+=vertex_bytes;
      if(draw_timing_sample) {
        double times[]={end-t0,t1-t0,t2-t1,t3-t2,t4-t3,t5-t4,end-t5};
        for(unsigned i=0;i<7;i++)totals[i]+=times[i];samples++;
        if(!report_at)report_at=end;
        if(end-report_at>=5000) {
            LOG("Draw aggregate: timing=sampled phaseMs=sampledSum stride=%u samples=%u draws=%u totalMs=%.1f attachments=%.1f descriptors=%.1f vertex=%.1f pipeline=%.1f submit=%.1f readback=%.1f vertexMiB=%.2f pipelines=%u",
                (unsigned)DRAW_TIMING_STRIDE,samples,draws,totals[0],totals[1],totals[2],totals[3],totals[4],totals[5],totals[6],copied/1048576.0,graphics_pipeline_count);
            memset(totals,0,sizeof totals);draws=0;samples=0;copied=0;report_at=end;
        }
        if(timing_count++<32 || (end-t0>250 && timing_count%120==0))
            LOG("Draw timing ms: sampled=1 stride=%u total=%.1f attachments=%.1f descriptors=%.1f vertex=%.1f pipeline=%.1f submit=%.1f readback=%.1f vertexMiB=%.2f",
                (unsigned)DRAW_TIMING_STRIDE,end-t0,t1-t0,t2-t1,t3-t2,t4-t3,t5-t4,end-t5,vertex_bytes/1048576.0);
      }
    }
    result=0;
    if(defer) {
        if(join_pass && feedback_count) {
            gpu_metrics.feedback_shared_draws++;gpu_metrics.feedback_shared_copies+=feedback_count;
        }
        if(join_pass && (!pass_cached || !attachments_cached))perf_metrics.cache_only_batched++;
        pending_draw *p=&pending_draws[pending_draw_count++];
        p->pass_cached=pass_cached;p->attachments_cached=attachments_cached;
        if(join_pass) {
            if(!cmd_borrowed)active_draw_epoch++;
            if(!cmd_borrowed || query){active_draw_pass.query=query;active_draw_pass.query_index=query_index;}
            active_draw_pass.samples=d->samples;
            active_draw_pass.cmd=cmd;active_draw_pass.pass=pass;active_draw_pass.fb=fb;active_draw_pass.pipeline=pipeline;
            active_draw_pass.bindings=encoded;
            // Publish only after both stages and the final join decision. A
            // stage's preparation may have flushed the previous active pass.
            for(unsigned i=0;i<2;i++) {
                graphics_resources *next=resources[i];
                if(!graphics_stage_cacheable(next))continue;
                graphics_resources **entries=active_draw_pass.stages[i];
                unsigned at=0;while(at<DRAW_STAGE_CACHE_COUNT && entries[at]!=next)at++;
                if(at==DRAW_STAGE_CACHE_COUNT) {
                    // A new lease owns the bundle, not its individual draws.
                    // Eviction drops only this lease; fences retain old users.
                    next->refs++;
                    at=DRAW_STAGE_CACHE_COUNT-1;
                    graphics_resources_free(entries[at]);
                }
                // Existing hits retain their one lease and move to MRU only
                // after both stages have finished preparation successfully.
                for(unsigned j=at;j;j--)entries[j]=entries[j-1];
                entries[0]=next;
            }
        }
        p->cmd=batch_owned?VK_NULL_HANDLE:cmd;p->query=query;p->query_index=query_index;p->query_paged=query_paged;p->pass=pass;p->fb=fb;
        p->pass_begin=!cmd_borrowed;
        p->resources[0]=resources[0];p->resources[1]=resources[1];p->index=index;
        p->samples=query?sample_capture(d->samples):NULL;
        if(query && !p->samples)die("Vulkan sample interval allocation failed");
        p->index_address=resident_index?d->index_address:0;p->index_size=resident_index?index_bytes:0;
        memcpy(p->attachments,attachments,sizeof attachments);
        for(unsigned i=0;i<16;i++)if(used_streams&(1u<<i)) {
            p->vertex_address[i]=d->streams[i].address+stream_begin[i];p->vertex_size[i]=stream_end[i]-stream_begin[i];
        }
        cmd=VK_NULL_HANDLE;query=VK_NULL_HANDLE;pass=VK_NULL_HANDLE;fb=VK_NULL_HANDLE;resources[0]=resources[1]=NULL;
        memset(attachments,0,sizeof attachments);memset(&index,0,sizeof index);
        for(unsigned i=0;i<feedback_count;i++)feedback_copies[i]->layout=VK_IMAGE_LAYOUT_GENERAL;
    }
done:
    if(fuse_token && result && !batch_owned) {
        // Preparation never encoded this draw. Preserve already-issued P/Q,
        // finish an empty remaining subpass, then retry only this draw plain.
        if(draw_fuse.token==fuse_token && draw_active_end())result=-1;
        else result=2;
    }
    // Shared recording has no recoverable per-draw rollback: commands may
    // already reference this draw's resources. Do not free them and continue.
    if(result && batch_owned)die("Vulkan shared draw recording failed phase=%s; terminating",phase);
    if(index_snapshot_owned)free(index_snapshot);
    if(cmd && !batch_owned) {
        if(vkQueueWaitIdle(v.queue)!=VK_SUCCESS)die("Vulkan standalone draw retirement failed");
        vkFreeCommandBuffers(v.device,v.commands,1,&cmd);
    }
    if(query && !query_paged)draw_query_release(query);
    if(!persistent && pipeline)vkDestroyPipeline(v.device,pipeline,NULL);if(!persistent && layout)vkDestroyPipelineLayout(v.device,layout,NULL);
    for(unsigned i=0;i<2;i++){graphics_resources_free(resources[i]);if(!persistent && sets[i])vkDestroyDescriptorSetLayout(v.device,sets[i],NULL);if(modules[i])vkDestroyShaderModule(v.device,modules[i],NULL);}
    scratch_free(&index);
    if(!attachments_cached) {
        if(fb)vkDestroyFramebuffer(v.device,fb,NULL);
        for(unsigned i=0;i<9;i++)if(attachments[i])vkDestroyImageView(v.device,attachments[i],NULL);
    }
    if(pass && !pass_cached)vkDestroyRenderPass(v.device,pass,NULL);
    if(result<0 && !strcmp(phase,"attachments")) {
        for(unsigned i=0;i<9;i++)if(i<8?(d->color_mask&(1u<<i)):d->has_depth) {
            const dm_compute_image *a=i<8?&d->color[i]:&d->depth;
            LOG("Draw attachment %u serial=%llu fmt=%d target=%d mip=%d layer=%d count=%d size=%ux%u",i,(unsigned long long)a->serial,a->format,a->desc.target,a->base_level,a->base_layer,a->layers,a->desc.width,a->desc.height);
        }
    }
    if(result<0)LOG("Vulkan draw failed: phase=%s vs=%s ps=%s",phase,d->shader[0]?d->shader[0]->key:"none",d->shader[1]?d->shader[1]->key:"none");
    return result;
}

int dm_vk_draw(const dm_draw *original) {
    if(!original)return -1;
    pthread_mutex_lock(&lock);
    dm_draw draw=*original;draw.fuse_pass=0;draw.fuse_subpass=0;
    uint64_t token=0;dm_compute_buffer inputs[64];
    if(draw_fuse.token) {
        if(!original->shader[0] || !original->shader[1] || !fuse_draw_allowed(original))draw_active_end();
        else {
            token=draw_fuse.token;draw.fuse_pass=(uint64_t)(uintptr_t)draw_fuse.pass;draw.fuse_subpass=draw_fuse.requested;
            if(draw_fuse.requested) {
                draw.shader[1]=dm_vk_graphics_fuse_variant(original->shader[1],draw_fuse.plan.q_mask);
                if(!draw.shader[1]){draw_active_end();token=0;draw=*original;draw.fuse_pass=0;draw.fuse_subpass=0;}
                else {
                    memcpy(inputs,original->resources[1],original->shader[1]->nbindings*sizeof *inputs);
                    const dm_fuse_shader_info *facts=original->shader[1]->fuse;
                    for(unsigned i=0;i<facts->ninputs;i++)if(draw_fuse.plan.q_mask&(1u<<i))
                        inputs[facts->inputs[i].binding_index].image=&draw_fuse.plan.inputs[facts->inputs[i].input_index];
                    draw.resources[1]=inputs;
                }
            }
        }
    }
    int result=draw_execute_locked(&draw,token);
    if(result==2) {
        gpu_metrics.fuse_plain_retries++;
        draw=*original;draw.fuse_pass=0;draw.fuse_subpass=0;
        result=draw_execute_locked(&draw,0);
    }
    pthread_mutex_unlock(&lock);return result;
}
