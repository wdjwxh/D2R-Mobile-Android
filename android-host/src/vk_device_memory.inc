/* Application-owned VkDeviceMemory allocation metadata, NOT physical RSS or
 * driver-private memory. The backend lock owns this registry and its gauges.
 * Shared pool bindings borrow the guest allocation; they never enter here. */
enum { DEVICE_MEMORY_GUEST,DEVICE_MEMORY_IMAGE,DEVICE_MEMORY_POOL,
       DEVICE_MEMORY_SCRATCH,DEVICE_MEMORY_PRESENT,DEVICE_MEMORY_TIMESTAMP,DEVICE_MEMORY_CATEGORIES };
typedef struct {
    uint64_t live_bytes,peak_bytes,live_count,peak_count,allocated_bytes,allocations,failures;
} device_memory_stats;
typedef struct device_memory_record {
    struct device_memory_record *next;
    VkDeviceMemory memory;VkDeviceSize bytes;unsigned category,type,heap;
} device_memory_record;
static struct {
    VkPhysicalDeviceMemoryProperties properties;
    int initialized;
    device_memory_record *buckets[1024];
    device_memory_stats total,categories[DEVICE_MEMORY_CATEGORIES];
    device_memory_stats types[VK_MAX_MEMORY_TYPES],heaps[VK_MAX_MEMORY_HEAPS];
} device_memory;
static void device_memory_open(void) {
    if(device_memory.initialized)die("Vulkan memory accounting already initialized");
    memset(&device_memory,0,sizeof device_memory);
    vkGetPhysicalDeviceMemoryProperties(v.gpu,&device_memory.properties);
    device_memory.initialized=1;
}
static unsigned device_memory_bucket(VkDeviceMemory memory) {
    uint64_t key=(uint64_t)(uintptr_t)memory;key^=key>>33;key*=UINT64_C(0xff51afd7ed558ccd);
    return (unsigned)(key^(key>>33))&1023;
}
static void device_memory_add(device_memory_stats *s,VkDeviceSize bytes) {
    s->live_bytes+=bytes;s->allocated_bytes+=bytes;s->live_count++;s->allocations++;
    if(s->live_bytes>s->peak_bytes)s->peak_bytes=s->live_bytes;
    if(s->live_count>s->peak_count)s->peak_count=s->live_count;
}
static void device_memory_sub(device_memory_stats *s,VkDeviceSize bytes) {
    if(!s->live_count || s->live_bytes<bytes)die("Vulkan memory accounting underflow");
    s->live_bytes-=bytes;s->live_count--;
}
static VkResult device_memory_allocate(const VkMemoryAllocateInfo *info,VkDeviceMemory *out,unsigned category) {
    *out=VK_NULL_HANDLE;
    if(!device_memory.initialized || category>=DEVICE_MEMORY_CATEGORIES ||
       info->memoryTypeIndex>=device_memory.properties.memoryTypeCount)die("Vulkan memory accounting invalid allocation");
    unsigned type=info->memoryTypeIndex,heap=device_memory.properties.memoryTypes[type].heapIndex;
    // Reserve metadata first: no successful Vulkan allocation can be untracked.
    device_memory_record *record=malloc(sizeof *record);
    VkResult result=record?vkAllocateMemory(v.device,info,NULL,out):VK_ERROR_OUT_OF_HOST_MEMORY;
    if(result!=VK_SUCCESS) {
        free(record);*out=VK_NULL_HANDLE;
        device_memory.total.failures++;device_memory.categories[category].failures++;
        device_memory.types[type].failures++;device_memory.heaps[heap].failures++;
        return result;
    }
    unsigned bucket=device_memory_bucket(*out);
    *record=(device_memory_record){.next=device_memory.buckets[bucket],.memory=*out,
        .bytes=info->allocationSize,.category=category,.type=type,.heap=heap};
    device_memory.buckets[bucket]=record;
    device_memory_add(&device_memory.total,record->bytes);
    device_memory_add(&device_memory.categories[category],record->bytes);
    device_memory_add(&device_memory.types[type],record->bytes);
    device_memory_add(&device_memory.heaps[heap],record->bytes);
    return result;
}
static void device_memory_free(VkDeviceMemory memory) {
    if(!memory)return;
    device_memory_record **link=&device_memory.buckets[device_memory_bucket(memory)];
    while(*link && (*link)->memory!=memory)link=&(*link)->next;
    if(!*link)die("Vulkan freeing untracked device memory");
    device_memory_record *record=*link;
    vkFreeMemory(v.device,memory,NULL);
    *link=record->next;
    device_memory_sub(&device_memory.total,record->bytes);
    device_memory_sub(&device_memory.categories[record->category],record->bytes);
    device_memory_sub(&device_memory.types[record->type],record->bytes);
    device_memory_sub(&device_memory.heaps[record->heap],record->bytes);
    free(record);
}
static void device_memory_report_stats(const char *scope,unsigned index,const device_memory_stats *s) {
    LOG("VkDeviceMemory owned: scope=%s index=%u liveBytes=%llu peakBytes=%llu liveCount=%llu peakCount=%llu allocatedBytes=%llu allocations=%llu failures=%llu",
        scope,index,(unsigned long long)s->live_bytes,(unsigned long long)s->peak_bytes,
        (unsigned long long)s->live_count,(unsigned long long)s->peak_count,
        (unsigned long long)s->allocated_bytes,(unsigned long long)s->allocations,(unsigned long long)s->failures);
}
static void device_memory_report(void) {
    if(!device_memory.initialized)return;
    static const char *names[]={"guest-shared","image","pool","scratch","present","timestamp"};
    LOG("VkDeviceMemory accounting: allocation metadata only; not RSS or driver-private memory; cumulative per device, no budget admission");
    device_memory_report_stats("total",0,&device_memory.total);
    for(unsigned i=0;i<DEVICE_MEMORY_CATEGORIES;i++)device_memory_report_stats(names[i],i,&device_memory.categories[i]);
    for(unsigned i=0;i<device_memory.properties.memoryTypeCount;i++)if(device_memory.types[i].allocations || device_memory.types[i].failures) {
        LOG("VkDeviceMemory type: index=%u heap=%u flags=%#x",i,device_memory.properties.memoryTypes[i].heapIndex,device_memory.properties.memoryTypes[i].propertyFlags);
        device_memory_report_stats("type",i,&device_memory.types[i]);
    }
    for(unsigned i=0;i<device_memory.properties.memoryHeapCount;i++)if(device_memory.heaps[i].allocations || device_memory.heaps[i].failures) {
        LOG("VkDeviceMemory heap: index=%u sizeBytes=%llu flags=%#x",i,(unsigned long long)device_memory.properties.memoryHeaps[i].size,device_memory.properties.memoryHeaps[i].flags);
        device_memory_report_stats("heap",i,&device_memory.heaps[i]);
    }
}
static void device_memory_close(void) {
    if(device_memory.total.live_count || device_memory.total.live_bytes)die("Vulkan device memory remains owned at destruction");
    for(unsigned i=0;i<1024;i++)if(device_memory.buckets[i])die("Vulkan memory registry remains at destruction");
    memset(&device_memory,0,sizeof device_memory);
}
