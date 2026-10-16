// Vulkan 1.1 foundation. All queue access is serialized; no Metal/Mach code.
#define VK_USE_PLATFORM_ANDROID_KHR
#include "dm_host.h"
#include "display.h"
#include "vk_backend.h"
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <time.h>
static double gpu_time_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
// Fine-grained diagnostics are opt-in: under ARM native translation, calling
// the clock around every binding can itself dominate small operations. Keep
// frame counters, completion statistics and sparse GPU probes in ordinary builds.
#ifndef DM_VK_DETAIL_TIMING
#define DM_VK_DETAIL_TIMING 0
#endif
enum { CPU_OPERATION_TIMING_STRIDE=DM_VK_DETAIL_TIMING?1:32 };
static inline double gpu_detail_time_ms(void) {
#if DM_VK_DETAIL_TIMING
    return gpu_time_ms();
#else
    return 0;
#endif
}
static struct {
    double image_ms,batch_ms,texture_prepare_ms,view_sampler_ms,uniform_prepare_ms,descriptor_api_ms;
    uint64_t uploads,downloads,up_bytes,down_bytes,batches,draws,commands;
    uint64_t batch_timing_samples,compute_timing_samples;
    uint64_t texture_prepares,view_sampler_prepares,uniform_prepares,descriptor_sets;
    double tex_lookup_ms,tex_transfer_ms,tex_page_ms,tex_compare_ms;
    double uniform_flush_ms,uniform_lookup_ms,uniform_cache_ms,uniform_scratch_ms,uniform_copy_ms,uniform_sync_ms;
    double uniform_compare_ms,buffer_compare_ms;
    uint64_t buffer_hits,buffer_misses,buffer_upload_bytes;
    uint64_t stage_hits,stage_misses;
    uint64_t dynamic_stage_reuse,dynamic_stage_fallback;
    uint64_t resource_objects_allocated,resource_objects_reused;
    uint64_t descriptor_set_reuse,descriptor_set_alloc;
    uint64_t descriptor_template_updates,descriptor_write_updates;
    uint64_t resolved_set_hits,resolved_set_misses,dynamic_uniform_bindings;
    uint64_t descriptor_bundle_evictions,descriptor_bundle_fallbacks;
    uint64_t query_cache_hits,query_cache_creates,query_cache_fallbacks;
    uint64_t report_samples,report_timestamp,report_top,report_resets;
    uint64_t report_published,report_range_waits,report_query_creates;
    uint64_t report_gpu_published,report_gpu_reads;
    uint64_t report_page_checks,report_page_skips;
    uint64_t report_samples_queried,report_samples_discarded;
    uint64_t fuse_accepted,fuse_transitions,fuse_q_draws,fuse_abandoned,fuse_plain_retries;
    uint64_t attachment_clears,attachment_clear_joins;
    uint64_t attachment_clear_deferred,attachment_clear_loads,attachment_clear_fallbacks;
    uint64_t ds_omit_candidates,ds_omit_draws,ds_omit_passes,ds_omit_clear_fallbacks;
    uint64_t sync_fence_submits,sync_fence_creates,sync_fence_backpressure;
    uint64_t sync_cpu_gpu_waits,sync_cpu_polls,sync_cpu_materializations;
    uint64_t dynamic_storage_bindings,resident_storage_prepares,resident_texel_prepares;
    double stage_validate_ms;
    double compute_ms;
    uint64_t compute_calls,compute_readback_bytes,compute_deferred;
    uint64_t compute_descriptor_pools,compute_descriptor_allocs,compute_descriptor_reuses,compute_command_allocs,compute_uniform_arena;
    uint64_t buffer_materialize_bytes,buffer_materialize_ranges;
    uint64_t materialize_reason_bytes[DM_VK_MATERIALIZE_COUNT];
    uint64_t uniform_compare_bytes;
    uint64_t snapshot_copy_bytes,snapshot_allocs,snapshot_borrowed_static;
    uint64_t snapshot_borrowed_guest,snapshot_borrowed_buffer,snapshot_reused_arena,snapshot_shadow_grows;
    uint64_t uniform_content_hits,uniform_content_misses,uniform_content_probes,uniform_content_limits;
    uint64_t uniform_content_active_hits,uniform_content_idle_hits,uniform_content_avoided_bytes;
    uint64_t uniform_dynamic_upload_bytes,uniform_dynamic_zero_bytes;
    uint64_t compute_range_wait_calls,compute_range_already_retired,compute_range_no_overlap;
    uint64_t compute_range_older,compute_range_latest,compute_range_fallback,compute_range_skipped_distance;
    uint64_t uniform_arena_spills;
    uint64_t range_compare_requested_bytes,page_compare_requested_bytes,range_prefix_skipped_bytes;
    uint64_t vertex_bind_calls,vertex_bind_changed,vertex_bind_slots;
    uint64_t cb_binding_reuse[4];
    uint64_t compute_view_bindings,compute_sampler_bindings,compute_texel_bindings;
    uint64_t image_view_cache_hits,image_view_cache_creates,sampler_cache_hits,sampler_cache_creates;
    uint64_t texel_view_cache_hits,texel_view_cache_creates;
    uint64_t pass_barrier_merged;
    uint64_t feedback_shared_draws,feedback_shared_copies;
    uint64_t feedback_range_eligible,feedback_range_disjoint;
    uint64_t feedback_full_texels,feedback_copy_texels,feedback_partial_copies;
    uint64_t range_snapshot_bytes,pool_upload_bytes,pool_certificate_hits,pool_certificate_scans;
    uint64_t page_certificate_hits,page_certificate_scans,page_snapshot_bytes;
    uint64_t unknown_shadow_upload_bytes;
    uint64_t vertex_requested_before,vertex_requested_after,index_bounded,index_shared_direct,index_gpu_fallback,index_bounds_fallback;
    uint64_t texture_async_uploads,texture_async_initializations,texture_staging_waits;
    uint64_t texture_shared_records,texture_shared_bytes,texture_shared_clears;
    size_t texture_staging_peak_bytes;
    uint64_t tex_tracked,tex_untracked,tex_generation_hits,tex_page_scans,tex_compare_bytes;
    uint64_t uniform_static_hits,uniform_guest_hits,uniform_arena_hits,uniform_fallbacks,uniform_copy_bytes,uniform_zero_bytes;
    uint64_t uniform_direct_candidates,uniform_direct_prepares,uniform_direct_fallbacks,uniform_direct_bytes;
    uint64_t uniform_push_calls,uniform_push_reuses;
} gpu_metrics;

// Called under the backend lock at non-nested public CPU completion boundaries.
// Count actual copies, including copies made by their locked helper calls.
static void gpu_materialize_account(unsigned reason,uint64_t before) {
    if(reason>=DM_VK_MATERIALIZE_COUNT)reason=DM_VK_MATERIALIZE_OTHER;
    gpu_metrics.materialize_reason_bytes[reason]+=gpu_metrics.buffer_materialize_bytes-before;
}

static struct {
    ANativeWindow *window;
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    VkFormatProperties format_properties[256];
    unsigned char format_properties_valid[256];
    VkDevice device;
    VkSurfaceKHR surface;
    VkQueue queue;
    uint32_t family;
    uint32_t timestamp_bits;
    VkCommandPool commands;
    VkSwapchainKHR swapchain;
    VkImage *images;
    uint32_t image_count;
    int mirror_clamp;
    VkExtent2D extent;
    VkFormat surface_format;
} v;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
// The heap outlives NVN device sessions: guest pointers must never move.
static struct {
    VkDeviceMemory memory;
    uint8_t *mapped;
    void *prefix;
    size_t size,prefix_size,visible,zeroed;
    uint32_t memory_type;
} guest_heap;
static uintptr_t guest_heap_read_base;
static size_t guest_heap_read_visible;
enum { VK_COMPUTE_FIFO_CAPACITY=32,VK_DRAW_FIFO_CAPACITY=8 };
#include "vk_performance.inc"
#include "vk_completion.inc"
static int images_flush_range_at(uint64_t address,size_t size,const char *site);
#define images_flush_range(address,size) images_flush_range_at(address,size,__func__)
static int images_flush_alias_range(uint64_t address,size_t size);
static int images_flush_all(void);
static int images_retire_range_locked(uint64_t address,size_t size);
static int draw_batch;
static int draw_pending_flush_at(const char *function,unsigned line);
static int draw_pending_submit_at(const char *function,unsigned line);
#define draw_pending_flush() draw_pending_flush_at(__func__,__LINE__)
#define draw_pending_submit() draw_pending_submit_at(__func__,__LINE__)
static int attachment_clears_flush(void);
static int draw_active_end(void);
static int draw_pending_write(uint64_t address,size_t size);
static int reports_range_drain_at(uint64_t address,size_t size,const char *site,const char *via);
static int reports_gpu_range_prepare(uint64_t address,size_t size,const char *site);
static int reports_cpu_only_drain(uint64_t address,size_t size);
#define reports_range_drain(address,size) reports_range_drain_at(address,size,__func__,"direct/mixed")
static int reports_cpu_overwrite(uint64_t address,size_t size);
static int event_word_prepare_locked(dm_vk_pool *p,size_t offset);
static int draw_submissions_poll(void);
typedef struct draw_submission draw_submission;
static int draw_submit_prepare(draw_submission **slot);
static uint64_t draw_submit_commit_at(draw_submission *slot,const char *function,unsigned line);
static int draw_compute_begin_locked(VkCommandBuffer *cmd,uint64_t *draw_serial,size_t staging_bytes);
static void draw_compute_record_locked(VkCommandBuffer cmd,uint64_t serial,uint64_t draw_serial);
static int draw_compute_pending_valid(uint64_t serial,uint64_t draw_serial);
static int draw_serial_ready(uint64_t serial);
static int draw_submissions_poll_cached(void);
static int draw_wait_serial_at(uint64_t serial,const char *site);
static int compute_wait_serial_at(uint64_t serial,const char *site);
#define draw_wait_serial(serial) draw_wait_serial_at(serial,__func__)
#define compute_wait_serial(serial) compute_wait_serial_at(serial,__func__)
static int compute_wait_pool_range_locked(dm_vk_pool *p,size_t offset,size_t size);
static int compute_cpu_write_range_locked(dm_vk_pool *p,size_t offset,size_t size);
static uint64_t compute_pool_range_target_locked(dm_vk_pool *p,size_t offset,size_t size,int *fallback);
static int compute_pending_flush(void);
static void compute_submissions_close(void);
static void draw_attachments_retire_image(VkImage image);
static void draw_attachments_close(void);
static int present_wait_serial_locked(uint64_t serial);
extern unsigned char dm_no_fp_state;
#include "vk_device_memory.inc"
#include "vk_bytes_equal.inc"
#include "vk_memory.inc"
#include "vk_queries.inc"
static int draw_batch_query_reset(VkQueryPool pool);
static void draw_compute_report_record_locked(VkCommandBuffer cmd,uint64_t serial,uint64_t draw_serial,counter_report *report);
#include "vk_compute.inc"
#include "vk_graphics_catalog.inc"
#include "vk_draw.inc"
#include "vk_shader_prepare.inc"
#include "vk_sync_tickets.inc"
#include "vk_present_pixels.inc"
#include "vk_present_ring.inc"
#include "vk_guest_heap.inc"

static void close_locked(void) {
    if (v.device) {
        if(draw_pending_flush() || vkDeviceWaitIdle(v.device))die("Vulkan device retirement failed; retaining resources");
        compute_submissions_close();
        draw_submissions_close();
        sync_tickets_close();
        present_ring_close();
        gpu_performance_close();
        graphics_pipelines_close();
        graphics_buffer_cache_close();
        compute_close_locked();
        graphics_catalog_close();
        pools_close_locked();
        if (v.swapchain) vkDestroySwapchainKHR(v.device, v.swapchain, NULL);
        v.swapchain=VK_NULL_HANDLE;
        timestamp_close();
        free(v.images);v.images=NULL;v.image_count=0;
        v.extent=(VkExtent2D){0};v.surface_format=VK_FORMAT_UNDEFINED;
        // A live guest heap pins its backing and the core device until process
        // exit. Session caches are gone, but command/completion infrastructure
        // must remain valid for a later NVN Initialize on the same device.
        if(guest_heap.mapped)return;
        if (v.commands) vkDestroyCommandPool(v.device, v.commands, NULL);
        completion_close();
        device_memory_close();
        vkDestroyDevice(v.device, NULL);
    }
    free(v.images);
    if (v.surface) vkDestroySurfaceKHR(v.instance, v.surface, NULL);
    if (v.instance) vkDestroyInstance(v.instance, NULL);
    if (v.window) ANativeWindow_release(v.window);
    memset(&v, 0, sizeof v);
}

void dm_vk_close(void) {
    pthread_mutex_lock(&lock);
    close_locked();
    pthread_mutex_unlock(&lock);
}

int dm_vk_open(void) {
    pthread_mutex_lock(&lock);
    if (v.device) { pthread_mutex_unlock(&lock); return 0; }
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDevice *gpus = NULL;
    v.window = dm_window_acquire();
    if (!v.window) goto fail;
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    uint32_t loader_version=VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerate_version=(PFN_vkEnumerateInstanceVersion)
        vkGetInstanceProcAddr(VK_NULL_HANDLE,"vkEnumerateInstanceVersion");
    if(enumerate_version && enumerate_version(&loader_version)!=VK_SUCCESS)goto fail;
    if(loader_version<VK_API_VERSION_1_1)goto fail;
    uint32_t instance_version=loader_version>=VK_API_VERSION_1_2?VK_API_VERSION_1_2:VK_API_VERSION_1_1;
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="D2R Android", .apiVersion=instance_version};
    VkInstanceCreateInfo ic = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&app, .enabledExtensionCount=2, .ppEnabledExtensionNames=extensions};
    result = vkCreateInstance(&ic, NULL, &v.instance);
    if (result) goto fail;
    VkAndroidSurfaceCreateInfoKHR sc = {.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window=v.window};
    result = vkCreateAndroidSurfaceKHR(v.instance, &sc, NULL, &v.surface);
    if (result) goto fail;
    uint32_t n=0;
    result = vkEnumeratePhysicalDevices(v.instance, &n, NULL);
    if (result || !n) goto fail;
    gpus = calloc(n, sizeof *gpus);
    if (!gpus) goto fail;
    result = vkEnumeratePhysicalDevices(v.instance, &n, gpus);
    if (result) goto fail;
    for (uint32_t i=0; i<n && !v.gpu; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpus[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t nq=0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nq, NULL);
        VkQueueFamilyProperties *qp=calloc(nq, sizeof *qp);
        if (!qp) goto fail;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nq, qp);
        for (uint32_t q=0; q<nq; q++) {
            VkBool32 present=VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i],q,v.surface,&present) == VK_SUCCESS &&
                present && (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qp[q].queueCount) {
                v.gpu=gpus[i]; v.family=q; v.timestamp_bits=qp[q].timestampValidBits; break;
            }
        }
        free(qp);
    }
    free(gpus); gpus=NULL;
    if (!v.gpu) goto fail;
    vkGetPhysicalDeviceProperties(v.gpu,&v.properties);
    vkGetPhysicalDeviceFeatures(v.gpu,&v.features);
    float priority=1;
    VkDeviceQueueCreateInfo qc={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=v.family, .queueCount=1, .pQueuePriorities=&priority};
    const char *de[3]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    int timeline_extension=0;
    uint32_t nde=1, ne=0;
    if(vkEnumerateDeviceExtensionProperties(v.gpu,NULL,&ne,NULL)==VK_SUCCESS && ne) {
        VkExtensionProperties *ep=calloc(ne,sizeof *ep);
        if(!ep)goto fail;
        if(vkEnumerateDeviceExtensionProperties(v.gpu,NULL,&ne,ep)==VK_SUCCESS)
            for(uint32_t i=0;i<ne;i++) {
                if(!strcmp(ep[i].extensionName,VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) {
                    de[nde++]=VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME;v.mirror_clamp=1;
                }
                if(!strcmp(ep[i].extensionName,VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))timeline_extension=1;
            }
        free(ep);
    }
    VkPhysicalDeviceFeatures available=v.features,enabled={0};
    enabled.shaderImageGatherExtended=available.shaderImageGatherExtended;
    enabled.shaderStorageImageReadWithoutFormat=available.shaderStorageImageReadWithoutFormat;
    enabled.shaderStorageImageWriteWithoutFormat=available.shaderStorageImageWriteWithoutFormat;
    enabled.shaderStorageImageExtendedFormats=available.shaderStorageImageExtendedFormats;
    enabled.samplerAnisotropy=available.samplerAnisotropy;
    enabled.textureCompressionBC=available.textureCompressionBC;
    enabled.textureCompressionASTC_LDR=available.textureCompressionASTC_LDR;
    enabled.independentBlend=available.independentBlend;
    enabled.shaderClipDistance=available.shaderClipDistance;
    enabled.shaderCullDistance=available.shaderCullDistance;
    enabled.vertexPipelineStoresAndAtomics=available.vertexPipelineStoresAndAtomics;
    enabled.fragmentStoresAndAtomics=available.fragmentStoresAndAtomics;
    enabled.occlusionQueryPrecise=available.occlusionQueryPrecise;
    enabled.dualSrcBlend=available.dualSrcBlend;
    enabled.robustBufferAccess=available.robustBufferAccess;
    enabled.depthBiasClamp=available.depthBiasClamp;
    int timeline_core=instance_version>=VK_API_VERSION_1_2 && v.properties.apiVersion>=VK_API_VERSION_1_2;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    if(timeline_core || timeline_extension) {
        VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&timeline};
        vkGetPhysicalDeviceFeatures2(v.gpu,&features);
    }
    uint32_t base_extensions=nde;
    if(timeline.timelineSemaphore && !timeline_core)de[nde++]=VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext=timeline.timelineSemaphore?&timeline:NULL,
        .queueCreateInfoCount=1, .pQueueCreateInfos=&qc,
        .enabledExtensionCount=nde, .ppEnabledExtensionNames=de,.pEnabledFeatures=&enabled};
    result=vkCreateDevice(v.gpu,&dc,NULL,&v.device);
    if(timeline.timelineSemaphore && (result!=VK_SUCCESS || completion_open(timeline_core))) {
        // No queue work or device resources exist yet. Retry the same base
        // features without timeline; never change completion mode at runtime.
        LOG("Vulkan timeline initialization unavailable; selecting fence completion");
        completion_close();
        if(v.device)vkDestroyDevice(v.device,NULL);
        v.device=VK_NULL_HANDLE;dc.pNext=NULL;dc.enabledExtensionCount=base_extensions;
        result=vkCreateDevice(v.gpu,&dc,NULL,&v.device);
    }
    if(result) goto fail;
    device_memory_open();
    LOG("Vulkan completion mode: %s (instance API %u.%u)",completion.semaphore?(timeline_core?"timeline core":"timeline KHR"):"fence",
        VK_VERSION_MAJOR(instance_version),VK_VERSION_MINOR(instance_version));
    vkGetDeviceQueue(v.device,v.family,0,&v.queue);
    VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=v.family, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    result=vkCreateCommandPool(v.device,&pc,NULL,&v.commands);
    if(result) goto fail;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(v.gpu,&props);
    LOG("Vulkan device created: %s vendor=0x%x device=0x%x queue=%u (API baseline 1.1)",
        props.deviceName,props.vendorID,props.deviceID,v.family);
    pthread_mutex_unlock(&lock);
    return 0;
fail:
    LOG("Vulkan device initialization failed: %d", result);
    free(gpus);
    close_locked();
    pthread_mutex_unlock(&lock);
    return -1;
}

// Pure NVN ordering stays in the GPU command stream. CPU completion users
// retain dm_vk_barrier/finish and their existing wait/materialization semantics.
int dm_vk_gpu_barrier(void) {
    pthread_mutex_lock(&lock);
    int result=draw_gpu_barrier_locked();
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_nvn_barrier(uint32_t bits) {
    pthread_mutex_lock(&lock);
    unsigned low=bits&255u;perf_metrics.nvn_barriers[low]++;
    if(active_draw_pass.cmd)perf_metrics.nvn_active_barriers[low]++;
    if(bits&~255u){perf_metrics.nvn_unknown_barriers++;perf_metrics.nvn_unknown_bits|=bits&~255u;}
    // Observe the guest's actual requirements before narrowing this mapping.
    // Vulkan has no implicit Metal resource hazard tracking: keep the full
    // existing GPU dependency, including zero/unknown flags, for this pass.
    int result=draw_gpu_barrier_locked();
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_barrier_reason(unsigned reason) {
    pthread_mutex_lock(&lock);
    uint64_t before=gpu_metrics.buffer_materialize_bytes;
    int result=draw_pending_flush();
    if(!result && (!v.device || vkQueueWaitIdle(v.queue)))result=-1;
    if(!result && compute_pending_flush())result=-1;
    if(!result && pools_cpu_read_all_locked())result=-1;
    gpu_materialize_account(reason,before);
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_attachment_boundary(void) {
    pthread_mutex_lock(&lock);
    int result=attachment_clears_flush();
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_barrier(void) { return dm_vk_barrier_reason(DM_VK_MATERIALIZE_OTHER); }
// Swapchain availability requires prior GPU work to complete, not a CPU
// snapshot of every registered buffer. Keep the conservative queue drain;
// CPU visibility remains an explicit finish/fence/event/invalidate boundary.
int dm_vk_gpu_complete(void) {
    pthread_mutex_lock(&lock);
    int result=draw_pending_flush();
    if(!result && (!v.device || vkQueueWaitIdle(v.queue)))result=-1;
    if(!result && compute_pending_flush())result=-1;
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_finish_reason(unsigned reason) {
    pthread_mutex_lock(&lock);
    uint64_t before=gpu_metrics.buffer_materialize_bytes;
    int pending=draw_pending_flush();
    VkResult r=v.device ? vkQueueWaitIdle(v.queue) : VK_ERROR_INITIALIZATION_FAILED;
    if(pending)r=VK_ERROR_UNKNOWN;
    if(!r && compute_pending_flush())r=VK_ERROR_UNKNOWN;
    if(!r && images_flush_all())r=VK_ERROR_UNKNOWN;
    if(!r && pools_cpu_read_all_locked())r=VK_ERROR_UNKNOWN;
    gpu_materialize_account(reason,before);
    pthread_mutex_unlock(&lock);
    return r == VK_SUCCESS ? 0 : -1;
}
int dm_vk_finish(void) { return dm_vk_finish_reason(DM_VK_MATERIALIZE_FINISH); }

int dm_vk_draw_batch(int enabled) {
    pthread_mutex_lock(&lock);int result=draw_pending_submit();draw_batch=enabled;
    pthread_mutex_unlock(&lock);return result;
}

static VkResult swapchain_locked(void) {
    if (v.swapchain) return VK_SUCCESS;
    VkSurfaceCapabilitiesKHR caps;
    VkResult r=vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.gpu,v.surface,&caps);
    if(r) return r;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint32_t n=0;
    r=vkGetPhysicalDeviceSurfaceFormatsKHR(v.gpu,v.surface,&n,NULL);
    if(r || !n) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkSurfaceFormatKHR *formats=calloc(n,sizeof *formats);
    if(!formats) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r=vkGetPhysicalDeviceSurfaceFormatsKHR(v.gpu,v.surface,&n,formats);
    if(r) { free(formats); return r; }
    VkSurfaceFormatKHR format=formats[0];
    for(uint32_t i=0;i<n;i++)
        if(formats[i].format==VK_FORMAT_R8G8B8A8_UNORM || formats[i].format==VK_FORMAT_B8G8R8A8_UNORM) {
            format=formats[i]; break;
        }
    free(formats);
    if(format.format==VK_FORMAT_UNDEFINED) format.format=VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D extent=caps.currentExtent;
    if(extent.width==UINT32_MAX) {
        int w=ANativeWindow_getWidth(v.window), h=ANativeWindow_getHeight(v.window);
        if(w<=0 || h<=0) return VK_ERROR_SURFACE_LOST_KHR;
        extent.width=(uint32_t)w; extent.height=(uint32_t)h;
        if(extent.width<caps.minImageExtent.width) extent.width=caps.minImageExtent.width;
        if(extent.width>caps.maxImageExtent.width) extent.width=caps.maxImageExtent.width;
        if(extent.height<caps.minImageExtent.height) extent.height=caps.minImageExtent.height;
        if(extent.height>caps.maxImageExtent.height) extent.height=caps.maxImageExtent.height;
    }
    if(!extent.width || !extent.height) return VK_ERROR_SURFACE_LOST_KHR;
    uint32_t count=caps.minImageCount+1;
    if(caps.maxImageCount && count>caps.maxImageCount) count=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(caps.supportedCompositeAlpha & alpha))
        alpha=(VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha+1));
    VkSwapchainCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface=v.surface,.minImageCount=count,.imageFormat=format.format,.imageColorSpace=format.colorSpace,
        .imageExtent=extent,.imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        /* The blit is already in Activity coordinates. Let Android's
         * compositor rotate to the physical panel instead of claiming the
         * image has been pre-rotated by the renderer. */
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    r=vkCreateSwapchainKHR(v.device,&ci,NULL,&v.swapchain);
    if(r) return r;
    r=vkGetSwapchainImagesKHR(v.device,v.swapchain,&v.image_count,NULL);
    if(r || !v.image_count) goto fail;
    v.images=calloc(v.image_count,sizeof *v.images);
    if(!v.images) { r=VK_ERROR_OUT_OF_HOST_MEMORY; goto fail; }
    r=vkGetSwapchainImagesKHR(v.device,v.swapchain,&v.image_count,v.images);
    if(r) goto fail;
    v.extent=extent;v.surface_format=format.format;
    LOG("Vulkan swapchain created: %ux%u format=%u images=%u",extent.width,extent.height,format.format,v.image_count);
    return VK_SUCCESS;
fail:
    free(v.images); v.images=NULL;
    vkDestroySwapchainKHR(v.device,v.swapchain,NULL); v.swapchain=VK_NULL_HANDLE;
    return r ? r : VK_ERROR_INITIALIZATION_FAILED;
}

static int present_image(const dm_compute_image *source,const int *crop,uint64_t *ticket) {
    *ticket=0;
    if(dm_vk_open()) return -1;
    pthread_mutex_lock(&lock);
    VkResult r=present_faulted?VK_ERROR_DEVICE_LOST:
        (draw_pending_submit()?VK_ERROR_UNKNOWN:swapchain_locked());
    if(r) goto done;
    if((r=present_ready_create()))goto done;
    present_slot *slot=&present_slots[present_next_slot];
    if((r=present_slot_prepare(slot)))goto done;
    VkCommandBuffer cmd=slot->cmd;
    gpu_image *src=NULL;int x=0,y=0,w=0,h=0;
    if(source) {
        src=image_get(source);
        if(!src || src->desc.target!=1 || src->layers!=1){r=VK_ERROR_FORMAT_NOT_SUPPORTED;goto done;}
        w=src->extent.width;h=src->extent.height;
        if(crop && crop[2]>0 && crop[3]>0){x=crop[0];y=crop[1];w=crop[2];h=crop[3];}
        if(x<0 || y<0 || w<=0 || h<=0 || (uint64_t)x+w>src->extent.width || (uint64_t)y+h>src->extent.height) {r=VK_ERROR_INITIALIZATION_FAILED;goto done;}
        if((r=present_pixels_prepare(&slot->scratch,src->format,w,h)))goto done;
        if(image_transfer(src,0)){r=VK_ERROR_DEVICE_LOST;goto done;}
    }
    uint32_t index;
    // WSI can block the translation worker without entering a fence wait.
    // Measure only these three once-per-present calls, never individual draws.
    double wsi_started=gpu_time_ms();
    r=vkAcquireNextImageKHR(v.device,v.swapchain,5000000000ull,slot->acquired,VK_NULL_HANDLE,&index);
    double wsi_elapsed=gpu_time_ms()-wsi_started;
    perf_metrics.wsi_acquires++;perf_metrics.wsi_acquire_ms+=wsi_elapsed;
    if(wsi_elapsed>perf_metrics.wsi_acquire_max_ms)perf_metrics.wsi_acquire_max_ms=wsi_elapsed;
    if(r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR) goto done;
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if((r=vkBeginCommandBuffer(cmd,&bi))) goto done;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=v.images[index],.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(src) {
        VkImageMemoryBarrier ready_src={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=src->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ready_src);
        present_pixels_encode(&slot->scratch,cmd,src,v.images[index],x,y,w,h);
        // Later draws/dispatches may reuse the NVN image. Order their writes
        // after this transfer read without forcing the CPU to idle the queue.
        ready_src.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        ready_src.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&ready_src);
    }
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
    if((r=vkEndCommandBuffer(cmd))) goto done;
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&slot->acquired,
        .pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&cmd,.signalSemaphoreCount=1,.pSignalSemaphores=&present_ready[index]};
    // Reset only when a submit will follow. A failed submit faults the whole
    // presenter; the unsignaled fence is never subsequently waited or reused.
    if((r=vkResetFences(v.device,1,&slot->fence)))goto done;
    slot->completion_frontier=completion.submitted;
    wsi_started=gpu_time_ms();
    r=vkQueueSubmit(v.queue,1,&submit,slot->fence);
    wsi_elapsed=gpu_time_ms()-wsi_started;
    perf_metrics.wsi_submits++;perf_metrics.wsi_submit_ms+=wsi_elapsed;
    if(wsi_elapsed>perf_metrics.wsi_submit_max_ms)perf_metrics.wsi_submit_max_ms=wsi_elapsed;
    if(r)goto done;
    if(present_submitted_serial==UINT64_MAX)die("Vulkan present serial overflow");
    slot->serial=++present_submitted_serial;slot->pending=1;
    if(src)src->last_present_use=slot->serial;
    *ticket=slot->serial;
    present_next_slot=(present_next_slot+1)%PRESENT_SLOT_COUNT;
    VkPresentInfoKHR present={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&present_ready[index],
        .swapchainCount=1,.pSwapchains=&v.swapchain,.pImageIndices=&index};
    wsi_started=gpu_time_ms();
    r=vkQueuePresentKHR(v.queue,&present);
    wsi_elapsed=gpu_time_ms()-wsi_started;
    perf_metrics.wsi_presents++;perf_metrics.wsi_present_ms+=wsi_elapsed;
    if(wsi_elapsed>perf_metrics.wsi_present_max_ms)perf_metrics.wsi_present_max_ms=wsi_elapsed;
    if(r==VK_SUBOPTIMAL_KHR) r=VK_SUCCESS;
done:
    // Preserve every resource on abnormal WSI/submission paths. The caller
    // terminates rather than pretending a failed acquire/present was retired.
    if(r){present_faulted=1;LOG("Vulkan guest texture present fault: result=%d; resources retained",r);}
    if(source && !r) {
        static double last;static unsigned frames;
        double now=gpu_time_ms();if(!last)last=now;frames++;
        if(now-last>=5000) {
            LOG("Frame aggregate: frames=%u wallMs=%.1f fps=%.2f imageMs=%.1f uploads=%llu upMiB=%.1f downloads=%llu downMiB=%.1f batches=%llu batchedDraws=%llu batchSampleMs=%.1f drawPasses=%llu texPrepMs=%.1f texPrepN=%llu viewSamplerMs=%.1f viewSamplerN=%llu uniformPrepMs=%.1f uniformPrepN=%llu descriptorApiMs=%.1f descriptorSets=%llu computeCalls=%llu computeDeferred=%llu computeSampleMs=%.1f computeBufferReadMiB=%.2f bufferMaterializeMiB=%.2f bufferMaterializeRanges=%llu",frames,now-last,frames*1000.0/(now-last),gpu_metrics.image_ms,(unsigned long long)gpu_metrics.uploads,gpu_metrics.up_bytes/1048576.0,(unsigned long long)gpu_metrics.downloads,gpu_metrics.down_bytes/1048576.0,(unsigned long long)gpu_metrics.batches,(unsigned long long)gpu_metrics.draws,gpu_metrics.batch_ms,(unsigned long long)gpu_metrics.commands,
                gpu_metrics.texture_prepare_ms,(unsigned long long)gpu_metrics.texture_prepares,
                gpu_metrics.view_sampler_ms,(unsigned long long)gpu_metrics.view_sampler_prepares,
                gpu_metrics.uniform_prepare_ms,(unsigned long long)gpu_metrics.uniform_prepares,
                gpu_metrics.descriptor_api_ms,(unsigned long long)gpu_metrics.descriptor_sets,
                (unsigned long long)gpu_metrics.compute_calls,(unsigned long long)gpu_metrics.compute_deferred,gpu_metrics.compute_ms,gpu_metrics.compute_readback_bytes/1048576.0,gpu_metrics.buffer_materialize_bytes/1048576.0,(unsigned long long)gpu_metrics.buffer_materialize_ranges);
            LOG("CPU operation timing: stride=%u batchSamples=%llu computeSamples=%llu sums=unscaled",
                (unsigned)CPU_OPERATION_TIMING_STRIDE,(unsigned long long)gpu_metrics.batch_timing_samples,
                (unsigned long long)gpu_metrics.compute_timing_samples);
            LOG("Resource detail: texLookupMs=%.1f texTransferMs=%.1f texTracked=%llu texUntracked=%llu texGenHits=%llu texPageScans=%llu texPageMs=%.1f texCompareMiB=%.2f texCompareMs=%.1f uniformRangeFlushMs=%.1f uniformPoolLookupMs=%.1f uniformCacheMs=%.1f uniformScratchMs=%.1f uniformCopyMs=%.1f uniformSyncMs=%.1f uniformStatic=%llu uniformGuest=%llu uniformArena=%llu uniformFallback=%llu fallbackCopyMiB=%.2f fallbackZeroMiB=%.2f",
                gpu_metrics.tex_lookup_ms,gpu_metrics.tex_transfer_ms,(unsigned long long)gpu_metrics.tex_tracked,(unsigned long long)gpu_metrics.tex_untracked,
                (unsigned long long)gpu_metrics.tex_generation_hits,(unsigned long long)gpu_metrics.tex_page_scans,gpu_metrics.tex_page_ms,gpu_metrics.tex_compare_bytes/1048576.0,gpu_metrics.tex_compare_ms,
                gpu_metrics.uniform_flush_ms,gpu_metrics.uniform_lookup_ms,gpu_metrics.uniform_cache_ms,gpu_metrics.uniform_scratch_ms,gpu_metrics.uniform_copy_ms,gpu_metrics.uniform_sync_ms,
                (unsigned long long)gpu_metrics.uniform_static_hits,(unsigned long long)gpu_metrics.uniform_guest_hits,(unsigned long long)gpu_metrics.uniform_arena_hits,(unsigned long long)gpu_metrics.uniform_fallbacks,
                gpu_metrics.uniform_copy_bytes/1048576.0,gpu_metrics.uniform_zero_bytes/1048576.0);
            LOG("Resource cache detail: detailedTiming=%d packagedDirty=%u uniformCompareMs=%.1f uniformCompareMiB=%.2f stageHits=%llu stageMisses=%llu stageValidateMs=%.1f stageValidateTiming=detailOnly descriptorSetReuse=%llu descriptorSetAlloc=%llu templateUpdates=%llu writeUpdates=%llu resolvedHits=%llu resolvedMisses=%llu dynamicUniforms=%llu dynamicStorage=%llu residentStoragePreps=%llu",
                DM_VK_DETAIL_TIMING,packaged_dirty_count,gpu_metrics.uniform_compare_ms,gpu_metrics.uniform_compare_bytes/1048576.0,
                (unsigned long long)gpu_metrics.stage_hits,(unsigned long long)gpu_metrics.stage_misses,gpu_metrics.stage_validate_ms,
                (unsigned long long)gpu_metrics.descriptor_set_reuse,(unsigned long long)gpu_metrics.descriptor_set_alloc,
                (unsigned long long)gpu_metrics.descriptor_template_updates,(unsigned long long)gpu_metrics.descriptor_write_updates,
                (unsigned long long)gpu_metrics.resolved_set_hits,(unsigned long long)gpu_metrics.resolved_set_misses,(unsigned long long)gpu_metrics.dynamic_uniform_bindings,
                (unsigned long long)gpu_metrics.dynamic_storage_bindings,(unsigned long long)gpu_metrics.resident_storage_prepares);
            LOG("Dynamic stage offset reuse: dynamicStageReuse=%llu fallback=%llu",
                (unsigned long long)gpu_metrics.dynamic_stage_reuse,(unsigned long long)gpu_metrics.dynamic_stage_fallback);
            LOG("Guarded uniform direct: candidates=%llu prepares=%llu fallbacks=%llu directMiB=%.2f pushCalls=%llu pushReuse=%llu",
                (unsigned long long)gpu_metrics.uniform_direct_candidates,(unsigned long long)gpu_metrics.uniform_direct_prepares,
                (unsigned long long)gpu_metrics.uniform_direct_fallbacks,gpu_metrics.uniform_direct_bytes/1048576.0,
                (unsigned long long)gpu_metrics.uniform_push_calls,(unsigned long long)gpu_metrics.uniform_push_reuses);
            LOG("Graphics resource reuse: objectsAlloc=%llu objectsReuse=%llu objectsRetained=%u objectsLive=%u",
                (unsigned long long)gpu_metrics.resource_objects_allocated,(unsigned long long)gpu_metrics.resource_objects_reused,
                graphics_resources_idle_count,graphics_resources_live);
            LOG("Graphics resource idle budget: objectBytes=%zu limitBytes=%u peakBytes=%zu overflowFrees=%llu",
                sizeof(graphics_resources),GRAPHICS_RESOURCE_IDLE_BYTES,
                (size_t)graphics_resources_idle_peak*sizeof(graphics_resources),
                (unsigned long long)graphics_resources_idle_overflow);
            graphics_resources_idle_peak=graphics_resources_idle_count;graphics_resources_idle_overflow=0;
            device_memory_report();
            uint64_t classified=0;
            for(unsigned i=1;i<DM_VK_MATERIALIZE_COUNT;i++)classified+=gpu_metrics.materialize_reason_bytes[i];
            LOG("Buffer materialize reasons MiB: copy=%.2f query=%.2f fence=%.2f finish=%.2f event=%.2f other=%.2f residentTexelPreps=%llu",
                gpu_metrics.materialize_reason_bytes[DM_VK_MATERIALIZE_COPY]/1048576.0,
                gpu_metrics.materialize_reason_bytes[DM_VK_MATERIALIZE_QUERY]/1048576.0,
                gpu_metrics.materialize_reason_bytes[DM_VK_MATERIALIZE_FENCE]/1048576.0,
                gpu_metrics.materialize_reason_bytes[DM_VK_MATERIALIZE_FINISH]/1048576.0,
                gpu_metrics.materialize_reason_bytes[DM_VK_MATERIALIZE_EVENT]/1048576.0,
                (gpu_metrics.buffer_materialize_bytes-classified)/1048576.0,
                (unsigned long long)gpu_metrics.resident_texel_prepares);
            LOG("Readonly buffer cache: hits=%llu misses=%llu uploadMiB=%.2f compareMs=%.1f retainedMiB=%.2f",
                (unsigned long long)gpu_metrics.buffer_hits,(unsigned long long)gpu_metrics.buffer_misses,
                gpu_metrics.buffer_upload_bytes/1048576.0,gpu_metrics.buffer_compare_ms,graphics_buffer_bytes/1048576.0);
            pool_materialize_consumers_report();
            pool_prepare_report();
            LOG("Indexed vertex bounds: requestedBefore=%llu requestedAfter=%llu bounded=%llu sharedDirect=%llu gpuFallback=%llu boundsFallback=%llu",
                (unsigned long long)gpu_metrics.vertex_requested_before,(unsigned long long)gpu_metrics.vertex_requested_after,
                (unsigned long long)gpu_metrics.index_bounded,(unsigned long long)gpu_metrics.index_shared_direct,
                (unsigned long long)gpu_metrics.index_gpu_fallback,(unsigned long long)gpu_metrics.index_bounds_fallback);
            LOG("Compute reuse: descriptorPools=%llu setAllocs=%llu setReuses=%llu commandAllocs=%llu uniformArena=%llu",
                (unsigned long long)gpu_metrics.compute_descriptor_pools,(unsigned long long)gpu_metrics.compute_descriptor_allocs,
                (unsigned long long)gpu_metrics.compute_descriptor_reuses,(unsigned long long)gpu_metrics.compute_command_allocs,
                (unsigned long long)gpu_metrics.compute_uniform_arena);
            LOG("Pool range cache: rangeCompareRequestedBytes=%llu pageCompareRequestedBytes=%llu rangeSnapshotBytes=%llu poolUploadBytes=%llu certificateHits=%llu certificateScans=%llu",
                (unsigned long long)gpu_metrics.range_compare_requested_bytes,(unsigned long long)gpu_metrics.page_compare_requested_bytes,
                (unsigned long long)gpu_metrics.range_snapshot_bytes,(unsigned long long)gpu_metrics.pool_upload_bytes,
                (unsigned long long)gpu_metrics.pool_certificate_hits,(unsigned long long)gpu_metrics.pool_certificate_scans);
            LOG("Pool page cache: pageCertHits=%llu pageCertScans=%llu pageSnapshotBytes=%llu unknownShadowUploadBytes=%llu",
                (unsigned long long)gpu_metrics.page_certificate_hits,(unsigned long long)gpu_metrics.page_certificate_scans,
                (unsigned long long)gpu_metrics.page_snapshot_bytes,(unsigned long long)gpu_metrics.unknown_shadow_upload_bytes);
            LOG("Pool equal prefix: repeatedPageCompareAvoidedBytes=%llu",
                (unsigned long long)gpu_metrics.range_prefix_skipped_bytes);
            LOG("CB binding reuse across pass: viewport=%llu scissor=%llu vertexCalls=%llu index=%llu",
                (unsigned long long)gpu_metrics.cb_binding_reuse[0],(unsigned long long)gpu_metrics.cb_binding_reuse[1],
                (unsigned long long)gpu_metrics.cb_binding_reuse[2],(unsigned long long)gpu_metrics.cb_binding_reuse[3]);
            LOG("Vertex stream binding: calls=%llu changedSlots=%llu encodedSlots=%llu avoidedCalls=%llu",
                (unsigned long long)gpu_metrics.vertex_bind_calls,(unsigned long long)gpu_metrics.vertex_bind_changed,
                (unsigned long long)gpu_metrics.vertex_bind_slots,
                (unsigned long long)(gpu_metrics.vertex_bind_changed-gpu_metrics.vertex_bind_calls));
            LOG("Shared image handles: viewHits=%llu viewCreates=%llu samplerHits=%llu samplerCreates=%llu computeViews=%llu computeSamplers=%llu",
                (unsigned long long)gpu_metrics.image_view_cache_hits,(unsigned long long)gpu_metrics.image_view_cache_creates,
                (unsigned long long)gpu_metrics.sampler_cache_hits,(unsigned long long)gpu_metrics.sampler_cache_creates,
                (unsigned long long)gpu_metrics.compute_view_bindings,(unsigned long long)gpu_metrics.compute_sampler_bindings);
            LOG("Shared texel handles: hits=%llu creates=%llu computeBindings=%llu",
                (unsigned long long)gpu_metrics.texel_view_cache_hits,(unsigned long long)gpu_metrics.texel_view_cache_creates,
                (unsigned long long)gpu_metrics.compute_texel_bindings);
            LOG("Render pass dependencies: mergedPrePassBarriers=%llu",
                (unsigned long long)gpu_metrics.pass_barrier_merged);
            LOG("Feedback GPU batching: sharedDraws=%llu snapshotCopies=%llu rangeEligibleBindings=%llu disjointBindings=%llu",
                (unsigned long long)gpu_metrics.feedback_shared_draws,(unsigned long long)gpu_metrics.feedback_shared_copies,
                (unsigned long long)gpu_metrics.feedback_range_eligible,(unsigned long long)gpu_metrics.feedback_range_disjoint);
            LOG("Feedback copy extent: fullTexels=%llu copiedTexels=%llu partialCopies=%llu texels=per-aspect",
                (unsigned long long)gpu_metrics.feedback_full_texels,(unsigned long long)gpu_metrics.feedback_copy_texels,
                (unsigned long long)gpu_metrics.feedback_partial_copies);
            LOG("Descriptor bundle cache: hits=%llu misses=%llu evictions=%llu fallbacks=%llu",
                (unsigned long long)gpu_metrics.resolved_set_hits,(unsigned long long)gpu_metrics.resolved_set_misses,
                (unsigned long long)gpu_metrics.descriptor_bundle_evictions,(unsigned long long)gpu_metrics.descriptor_bundle_fallbacks);
            LOG("Stage snapshots: snapshotCopyBytes=%llu allocs=%llu borrowedStatic=%llu borrowedGuest=%llu borrowedBuffer=%llu reusedArena=%llu shadowGrows=%llu arenaSpills=%llu",
                (unsigned long long)gpu_metrics.snapshot_copy_bytes,(unsigned long long)gpu_metrics.snapshot_allocs,
                (unsigned long long)gpu_metrics.snapshot_borrowed_static,(unsigned long long)gpu_metrics.snapshot_borrowed_guest,
                (unsigned long long)gpu_metrics.snapshot_borrowed_buffer,(unsigned long long)gpu_metrics.snapshot_reused_arena,
                (unsigned long long)gpu_metrics.snapshot_shadow_grows,(unsigned long long)gpu_metrics.uniform_arena_spills);
            LOG("Compute CPU overwrite range: calls=%llu alreadyRetired=%llu noOverlap=%llu olderTarget=%llu latestTarget=%llu fallback=%llu serialDistance=%llu",
                (unsigned long long)gpu_metrics.compute_range_wait_calls,(unsigned long long)gpu_metrics.compute_range_already_retired,
                (unsigned long long)gpu_metrics.compute_range_no_overlap,(unsigned long long)gpu_metrics.compute_range_older,
                (unsigned long long)gpu_metrics.compute_range_latest,(unsigned long long)gpu_metrics.compute_range_fallback,
                (unsigned long long)gpu_metrics.compute_range_skipped_distance);
            LOG("Uniform content reuse: hits=%llu activeHits=%llu idleHits=%llu misses=%llu probes=%llu limits=%llu avoidedUploadBytes=%llu uploadBytes=%llu zeroTailBytes=%llu",
                (unsigned long long)gpu_metrics.uniform_content_hits,(unsigned long long)gpu_metrics.uniform_content_active_hits,
                (unsigned long long)gpu_metrics.uniform_content_idle_hits,(unsigned long long)gpu_metrics.uniform_content_misses,
                (unsigned long long)gpu_metrics.uniform_content_probes,(unsigned long long)gpu_metrics.uniform_content_limits,
                (unsigned long long)gpu_metrics.uniform_content_avoided_bytes,(unsigned long long)gpu_metrics.uniform_dynamic_upload_bytes,
                (unsigned long long)gpu_metrics.uniform_dynamic_zero_bytes);
            LOG("Async textures: uploads=%llu initializations=%llu stagingWaits=%llu stagingPeakMiB=%.2f",
                (unsigned long long)gpu_metrics.texture_async_uploads,(unsigned long long)gpu_metrics.texture_async_initializations,
                (unsigned long long)gpu_metrics.texture_staging_waits,gpu_metrics.texture_staging_peak_bytes/1048576.0);
            LOG("Shared texture commands: records=%llu stagingMiB=%.2f clears=%llu",
                (unsigned long long)gpu_metrics.texture_shared_records,gpu_metrics.texture_shared_bytes/1048576.0,
                (unsigned long long)gpu_metrics.texture_shared_clears);
            images_upload_report();
            LOG("Vulkan FUSE: accepted=%llu nextSubpass=%llu qDraws=%llu abandoned=%llu plainRetries=%llu",
                (unsigned long long)gpu_metrics.fuse_accepted,(unsigned long long)gpu_metrics.fuse_transitions,
                (unsigned long long)gpu_metrics.fuse_q_draws,(unsigned long long)gpu_metrics.fuse_abandoned,
                (unsigned long long)gpu_metrics.fuse_plain_retries);
            LOG("Occlusion query cache: hits=%llu creates=%llu fallbacks=%llu",
                (unsigned long long)gpu_metrics.query_cache_hits,(unsigned long long)gpu_metrics.query_cache_creates,
                (unsigned long long)gpu_metrics.query_cache_fallbacks);
            LOG("Attachment clears: encoded=%llu joined=%llu deferred=%llu loadOp=%llu fallback=%llu",
                (unsigned long long)gpu_metrics.attachment_clears,(unsigned long long)gpu_metrics.attachment_clear_joins,
                (unsigned long long)gpu_metrics.attachment_clear_deferred,(unsigned long long)gpu_metrics.attachment_clear_loads,
                (unsigned long long)gpu_metrics.attachment_clear_fallbacks);
            LOG("CPU report prefilter: activeChecks=%llu pageRejects=%llu",(unsigned long long)gpu_metrics.report_page_checks,(unsigned long long)gpu_metrics.report_page_skips);
            LOG("GPU attachment demand: dsCandidates=%llu dsOmittedDraws=%llu dsOmittedPasses=%llu pendingClearFallbacks=%llu",
                (unsigned long long)gpu_metrics.ds_omit_candidates,(unsigned long long)gpu_metrics.ds_omit_draws,
                (unsigned long long)gpu_metrics.ds_omit_passes,(unsigned long long)gpu_metrics.ds_omit_clear_fallbacks);
            LOG("GPU timestamp reports: encoded=%llu gpuReadReuse=%llu",(unsigned long long)gpu_metrics.report_gpu_published,(unsigned long long)gpu_metrics.report_gpu_reads);
            LOG("Async reports: samples=%llu timestamp=%llu top=%llu resets=%llu published=%llu forcedRangeWaits=%llu queryCreates=%llu queried=%llu discarded=%llu",
                (unsigned long long)gpu_metrics.report_samples,(unsigned long long)gpu_metrics.report_timestamp,
                (unsigned long long)gpu_metrics.report_top,(unsigned long long)gpu_metrics.report_resets,
                (unsigned long long)gpu_metrics.report_published,(unsigned long long)gpu_metrics.report_range_waits,
                (unsigned long long)gpu_metrics.report_query_creates,
                (unsigned long long)gpu_metrics.report_samples_queried,(unsigned long long)gpu_metrics.report_samples_discarded);
            LOG("Async sync: submits=%llu creates=%llu backpressure=%llu cpuGpuWaits=%llu cpuPolls=%llu materializations=%llu",
                (unsigned long long)gpu_metrics.sync_fence_submits,(unsigned long long)gpu_metrics.sync_fence_creates,
                (unsigned long long)gpu_metrics.sync_fence_backpressure,(unsigned long long)gpu_metrics.sync_cpu_gpu_waits,
                (unsigned long long)gpu_metrics.sync_cpu_polls,(unsigned long long)gpu_metrics.sync_cpu_materializations);
            graphics_pipeline_cache_report();graphics_pipeline_cache_reset_metrics();graphics_image_binding_reuse_report();
            svc_thread_perf_report();
            gpu_performance_report();
            memset(&gpu_metrics,0,sizeof gpu_metrics);last=now;frames=0;
        }
    }
    // Pool handles remain owned by NVN. Do not destroy the device on a surface
    // error and leave live guest objects pointing at freed allocations.
    pthread_mutex_unlock(&lock);
    return r==VK_SUCCESS ? 0 : -1;
}
int dm_vk_present_texture(const dm_compute_image *source,const int crop[4],uint64_t *ticket) {
    if(!source || !ticket)return -1;
    return present_image(source,crop,ticket);
}
int dm_vk_present_wait(uint64_t ticket) {
    pthread_mutex_lock(&lock);
    int result=present_faulted?-1:present_wait_serial_locked(ticket);
    pthread_mutex_unlock(&lock);return result;
}

int dm_vk_prepare_window(void) {
    pthread_mutex_lock(&lock);
    VkResult result=v.device?swapchain_locked():VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_unlock(&lock);return result==VK_SUCCESS?0:-1;
}
