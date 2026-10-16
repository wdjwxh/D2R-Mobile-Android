/* Vulkan 1.1: acquire semaphores follow submit slots; render-finished
 * semaphores follow swapchain image indices, not graphics submit fences. */
#define PRESENT_SLOT_COUNT 3
typedef struct {
    VkCommandBuffer cmd;
    VkSemaphore acquired;
    VkFence fence;
    present_pixels_scratch scratch;
    uint64_t serial,completion_frontier;
    int pending;
} present_slot;
static present_slot present_slots[PRESENT_SLOT_COUNT];
static VkSemaphore *present_ready;
static uint64_t present_submitted_serial,present_completed_serial;
static unsigned present_next_slot;
static int present_faulted;

static int present_wait_serial_locked(uint64_t serial) {
    if(!serial || serial<=present_completed_serial)return 0;
    if(serial>present_submitted_serial)return -1;
    present_slot *slot=NULL;
    for(unsigned i=0;i<PRESENT_SLOT_COUNT;i++)
        if(present_slots[i].pending && present_slots[i].serial==serial)slot=&present_slots[i];
    if(!slot)return -1;
    if(vkWaitForFences(v.device,1,&slot->fence,VK_TRUE,UINT64_MAX))return -1;
    completion_certify_fence(slot->completion_frontier);
    // All submissions use the same queue. This fence also covers every older
    // present copy, but does not claim that WSI consumed its ready semaphore.
    present_completed_serial=serial;
    for(unsigned i=0;i<PRESENT_SLOT_COUNT;i++)
        if(present_slots[i].serial<=serial)present_slots[i].pending=0;
    return 0;
}
static VkResult present_slot_prepare(present_slot *slot) {
    if(slot->pending && present_wait_serial_locked(slot->serial))return VK_ERROR_DEVICE_LOST;
    if(!slot->acquired) {
        VkSemaphoreCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore=VK_NULL_HANDLE;
        VkResult r=vkCreateSemaphore(v.device,&ci,NULL,&semaphore);if(r)return r;
        slot->acquired=semaphore;
    }
    if(!slot->fence) {
        VkFenceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence=VK_NULL_HANDLE;
        VkResult r=vkCreateFence(v.device,&ci,NULL,&fence);if(r)return r;
        slot->fence=fence;
    }
    if(!slot->cmd) {
        VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=v.commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        VkCommandBuffer cmd=VK_NULL_HANDLE;
        VkResult r=vkAllocateCommandBuffers(v.device,&ai,&cmd);if(r)return r;
        slot->cmd=cmd;
    }
    // The slot's fence proved completion above; Begin resets it implicitly
    // through v.commands' RESET_COMMAND_BUFFER_BIT. Present faults latch, so
    // failed recordings can never re-enter this reuse path.
    return VK_SUCCESS;
}
static VkResult present_ready_create(void) {
    if(present_ready)return VK_SUCCESS;
    VkSemaphore *ready=calloc(v.image_count,sizeof(*ready));
    if(!ready)return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkSemaphoreCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(unsigned i=0;i<v.image_count;i++) {
        VkResult r=vkCreateSemaphore(v.device,&ci,NULL,&ready[i]);
        if(r) {
            for(unsigned j=0;j<i;j++)vkDestroySemaphore(v.device,ready[j],NULL);
            free(ready);return r;
        }
    }
    present_ready=ready;return VK_SUCCESS;
}
static void present_ring_close(void) {
    // Caller first drains translation and successfully idles the device.
    if(present_faulted || present_wait_serial_locked(present_submitted_serial))die("Vulkan present retirement failed during close; retaining resources");
    for(unsigned i=0;i<PRESENT_SLOT_COUNT;i++) {
        present_slot *slot=&present_slots[i];
        present_pixels_close(&slot->scratch);
        if(slot->cmd)vkFreeCommandBuffers(v.device,v.commands,1,&slot->cmd);
        if(slot->fence)vkDestroyFence(v.device,slot->fence,NULL);
        if(slot->acquired)vkDestroySemaphore(v.device,slot->acquired,NULL);
    }
    if(present_ready) {
        for(unsigned i=0;i<v.image_count;i++)vkDestroySemaphore(v.device,present_ready[i],NULL);
        free(present_ready);present_ready=NULL;
    }
    memset(present_slots,0,sizeof present_slots);
    // Keep serials monotonic across device lifetimes. Any image teardown still
    // carrying an old serial can recognize that its copy already completed.
    present_next_slot=0;
}
