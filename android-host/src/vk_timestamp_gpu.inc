/* Query -> exact 19.2 MHz record, on the existing GPU queue. Scratch and its
 * descriptor belong to the timestamp lease until its draw report retires. */
#include "timestamp_spv.inc"
#include <float.h>
typedef struct {uint32_t bits,multiplier,shift,left;} timestamp_parameters;
_Static_assert(sizeof(timestamp_parameters)==16,"timestamp shader push ABI");
static int timestamp_parameters_get(timestamp_parameters *p) {
    // float32 is an exact integer significand times a power of two. Preserve
    // that rational instead of converting a 64-bit counter through float.
    if(LDBL_MANT_DIG!=113 || !v.timestamp_bits || v.timestamp_bits>64)return -1;
    uint32_t raw;memcpy(&raw,&v.properties.limits.timestampPeriod,4);
    unsigned exponent=(raw>>23)&255,significand=raw&0x7fffff;
    if((raw>>31) || exponent==255 || (!exponent && !significand))return -1;
    int power=exponent?(int)exponent-150:-149;
    if(exponent)significand|=0x800000;
    p->bits=v.timestamp_bits;p->multiplier=significand*12u;
    p->left=power>0;p->shift=(uint32_t)(power>0?power:-power);
    // __uint128_t is host-only; Vulkan shader arithmetic remains uint32.
    __uint128_t maximum=(__uint128_t)(UINT64_MAX>>(64-v.timestamp_bits))*p->multiplier;
    const __uint128_t limit=(__uint128_t)625<<64;
    if(p->left) {
        if(p->shift>=128 || maximum>((limit-1)>>p->shift))return -1;
    } else if(p->shift<128 && ((maximum/625)>>p->shift)>UINT64_MAX)return -1;
    return 0;
}
static int timestamp_gpu_open(void) {
    if(timestamp_gpu.pipeline)return 0;
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo sc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    if(!timestamp_gpu.set && vkCreateDescriptorSetLayout(v.device,&sc,NULL,&timestamp_gpu.set))return -1;
    VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(timestamp_parameters)};
    VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&timestamp_gpu.set,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
    if(!timestamp_gpu.layout && vkCreatePipelineLayout(v.device,&lc,NULL,&timestamp_gpu.layout))return -1;
    VkShaderModule module=VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mc={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof timestamp_spv,.pCode=timestamp_spv};
    if(vkCreateShaderModule(v.device,&mc,NULL,&module))return -1;
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
            .module=module,.pName="main"},.layout=timestamp_gpu.layout};
    VkResult result=vkCreateComputePipelines(v.device,VK_NULL_HANDLE,1,&pc,NULL,&timestamp_gpu.pipeline);
    vkDestroyShaderModule(v.device,module,NULL);return result? -1:0;
}
static void timestamp_gpu_lease_clear(timestamp_lease *q) {
    if(q->descriptors)vkDestroyDescriptorPool(v.device,q->descriptors,NULL);
    if(q->buffer)vkDestroyBuffer(v.device,q->buffer,NULL);
    if(q->memory)device_memory_free(q->memory);
    q->descriptors=VK_NULL_HANDLE;q->set=VK_NULL_HANDLE;
    q->buffer=VK_NULL_HANDLE;q->memory=VK_NULL_HANDLE;
}
static int timestamp_gpu_lease_prepare(timestamp_lease *q) {
    if(q->set)return 0;
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=32,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    if(vkCreateBuffer(v.device,&bc,NULL,&q->buffer))goto fail;
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(v.device,q->buffer,&req);
    VkPhysicalDeviceMemoryProperties mp=device_memory.properties;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++)if(req.memoryTypeBits&(1u<<i)) {
        type=i;if(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)break;
    }
    if(type==UINT32_MAX)goto fail;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    if(device_memory_allocate(&ai,&q->memory,DEVICE_MEMORY_TIMESTAMP) || vkBindBufferMemory(v.device,q->buffer,q->memory,0))goto fail;
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    if(vkCreateDescriptorPool(v.device,&dp,NULL,&q->descriptors))goto fail;
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=q->descriptors,.descriptorSetCount=1,.pSetLayouts=&timestamp_gpu.set};
    if(vkAllocateDescriptorSets(v.device,&da,&q->set))goto fail;
    VkDescriptorBufferInfo info={q->buffer,0,32};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=q->set,.dstBinding=0,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&info};
    vkUpdateDescriptorSets(v.device,1,&write,0,NULL);return 0;
fail:
    timestamp_gpu_lease_clear(q);return -1;
}
// 1 means unsupported: preserve the CPU implementation. Errors before command
// encoding leave no producer; after encoding, ownership publication is fatal on
// failure, as for every shared compute/transfer producer.
static int timestamp_gpu_record_locked(dm_vk_pool *dst,size_t offset,int top) {
    timestamp_parameters params;
    if(timestamp_parameters_get(&params) || (offset&3))return 1;
    uint64_t address=(uintptr_t)dst->guest+offset;
    pool_cpu_hazard_add();
    counter_report *r=NULL;timestamp_lease *q=NULL;int result=-1;
    // An older CPU callback must publish before a newer GPU report, otherwise
    // it could overwrite that newer report when its draw slot retires.
    if(reports_cpu_only_drain(address,16) || pool_resident_locked(dst) || timestamp_gpu_open())goto done;
    q=timestamp_acquire();if(!q)goto done;
    if(timestamp_gpu_lease_prepare(q))goto done;
    r=calloc(1,sizeof *r);if(!r)goto done;
    r->pool=dst;r->offset=offset;r->size=16;r->timestamp=q;r->gpu_published=1;
    VkCommandBuffer cmd=VK_NULL_HANDLE;uint64_t draw_serial=0;
    // All waits/callbacks precede this final begin. The lease is not in the
    // report list yet, so recursive retirement cannot release it.
    if(draw_compute_begin_locked(&cmd,&draw_serial,0) || draw_batch_query_reset(q->pool))goto done;
    if(compute_submission_count==COMPUTE_SUBMISSION_SLOTS || compute_submitted_serial==UINT64_MAX)
        die("Vulkan GPU report FIFO changed after preparation");
    vkCmdWriteTimestamp(cmd,top?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,q->pool,0);
    vkCmdCopyQueryPoolResults(cmd,q->pool,0,1,q->buffer,0,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,NULL,0,NULL);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,timestamp_gpu.pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,timestamp_gpu.layout,0,1,&q->set,0,NULL);
    vkCmdPushConstants(cmd,timestamp_gpu.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdDispatch(cmd,1,1,1);
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
    mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
    VkBufferCopy copy={16,offset,16};vkCmdCopyBuffer(cmd,q->buffer,dst->buffer,1,&copy);
    mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);
    compute_submission *slot=&compute_submissions[(compute_submission_head+compute_submission_count)%COMPUTE_SUBMISSION_SLOTS];
    uint64_t serial=++compute_submitted_serial;
    slot->serial=serial;slot->pending_shared=1;slot->completion_draw_serial=draw_serial;
    slot->pool_ranges_complete=1;slot->pool_range_count=0;
    compute_pool_range_add(slot,dst,offset,16,COMPUTE_POOL_WRITE);
    draw_compute_report_record_locked(cmd,serial,draw_serial,r);
    compute_submission_count++;pool_cpu_hazard_add();dst->last_compute_use=serial;
    report_track(r);
    gpu_metrics.report_gpu_published++;
    if(top)gpu_metrics.report_top++;else gpu_metrics.report_timestamp++;
    // May recurse/retire on dirty-range pressure. Both owners are complete,
    // and neither r nor q may be dereferenced after this publication.
    if(pool_gpu_written_locked(dst,offset,16,serial))die("Vulkan GPU report output ownership failed");
    pool_cpu_hazard_remove();return 0;
done:
    if(q)q->leased=0;
    free(r);pool_cpu_hazard_remove();return result;
}
