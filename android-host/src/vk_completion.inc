/* Optional completion certificates for asynchronous resource FIFOs only.
 * Presentation and sync tickets keep their original binary/fence contracts. */
static struct {
    VkSemaphore semaphore;
    PFN_vkGetSemaphoreCounterValue counter;
    PFN_vkWaitSemaphores wait;
    uint64_t submitted,completed;
} completion;
static int completion_open(int core) {
    completion.counter=(PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(v.device,
        core?"vkGetSemaphoreCounterValue":"vkGetSemaphoreCounterValueKHR");
    completion.wait=(PFN_vkWaitSemaphores)vkGetDeviceProcAddr(v.device,
        core?"vkWaitSemaphores":"vkWaitSemaphoresKHR");
    if(!completion.counter || !completion.wait)return -1;
    VkSemaphoreTypeCreateInfo type={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&type};
    return vkCreateSemaphore(v.device,&ci,NULL,&completion.semaphore)==VK_SUCCESS?0:-1;
}
static void completion_close(void) {
    if(completion.semaphore)vkDestroySemaphore(v.device,completion.semaphore,NULL);
    memset(&completion,0,sizeof completion);
}
static int completion_prepare(VkFence *fence) {
    if(completion.semaphore)return 0;
    if(*fence)return vkResetFences(v.device,1,fence)==VK_SUCCESS?0:-1;
    VkFenceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return vkCreateFence(v.device,&ci,NULL,fence)==VK_SUCCESS?0:-1;
}
static VkResult completion_submit(const VkSubmitInfo *original,VkFence fence,uint64_t *value) {
    *value=0;
    if(!completion.semaphore)return vkQueueSubmit(v.queue,1,original,fence);
    // These private FIFO submissions have no semaphore waits/signals. Never
    // use this wrapper for WSI or silently replace an existing semaphore list.
    if(original->waitSemaphoreCount || original->signalSemaphoreCount)
        die("Vulkan completion submit received semaphore dependencies");
    if(completion.submitted==UINT64_MAX)die("Vulkan completion value exhausted");
    uint64_t next=completion.submitted+1;
    VkTimelineSemaphoreSubmitInfo timeline={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext=original->pNext,.signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&next};
    VkSubmitInfo submit=*original;submit.pNext=&timeline;
    submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&completion.semaphore;
    VkResult result=vkQueueSubmit(v.queue,1,&submit,VK_NULL_HANDLE);
    if(result==VK_SUCCESS){completion.submitted=next;*value=next;perf_metrics.timeline_submits++;}
    return result;
}
static int completion_poll(void) {
    if(!completion.semaphore || completion.completed==completion.submitted)return 0;
    uint64_t value=0;perf_metrics.timeline_polls++;
    VkResult result=completion.counter(v.device,completion.semaphore,&value);
    if(result!=VK_SUCCESS)return -1;
    if(value<completion.completed || value>completion.submitted)die("Vulkan completion counter out of range");
    completion.completed=value;return 0;
}
// A successful fence on this same queue also proves all timeline signals
// submitted before that fence. The caller must capture the frontier before
// submitting the fence, never infer it from the current queue tail later.
static void completion_certify_fence(uint64_t frontier) {
    if(!completion.semaphore)return;
    if(frontier>completion.submitted)die("Vulkan fence completion frontier out of range");
    if(frontier>completion.completed) {
        completion.completed=frontier;
        perf_metrics.timeline_fence_certificates++;
    }
}
static int completion_ready(uint64_t value,VkFence fence) {
    if(completion.semaphore)return value && value<=completion.completed;
    VkResult result=vkGetFenceStatus(v.device,fence);
    return result==VK_SUCCESS?1:result==VK_NOT_READY?0:-1;
}
static int completion_wait_at(uint64_t value,VkFence fence,const char *site) {
    if(!completion.semaphore)return vkWaitForFences(v.device,1,&fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS?0:-1;
    if(!value || value>completion.submitted)return -1;
    if(value<=completion.completed){perf_metrics.timeline_wait_cached++;return 0;}
    // Cached progress can lag a GPU which finished during CPU translation.
    // Query once; never spin, change the requested value, or infer completion.
    completion_wait_site *stats=completion_wait_site_get(site);stats->attempts++;
    double query_started=gpu_time_ms();int polled=completion_poll();
    stats->query_ms+=gpu_time_ms()-query_started;
    if(polled)return -1;
    if(value<=completion.completed){stats->ready++;return 0;}
    VkSemaphoreWaitInfo info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount=1,.pSemaphores=&completion.semaphore,.pValues=&value};
    double started=gpu_time_ms();perf_metrics.timeline_waits++;stats->waits++;
    VkResult result=completion.wait(v.device,&info,UINT64_MAX);
    double elapsed=gpu_time_ms()-started;
    perf_metrics.timeline_wait_ms+=elapsed;stats->wait_ms+=elapsed;report_wait_elapsed(1,elapsed);
    if(result!=VK_SUCCESS)return -1;
    completion.completed=value;return 0;
}
