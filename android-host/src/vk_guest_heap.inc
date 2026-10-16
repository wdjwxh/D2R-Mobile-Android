/* Vulkan-owned guest backing, established before guest addresses are exposed.
 * No import extension or fixed overwrite of an existing mapping is required.
 * Shared CPU/GPU access follows the game's NVN fence contract, as with Metal
 * shared buffers; explicit host reads/writes keep their completion boundaries.
 */
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <errno.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
void *dm_vk_guest_heap_reserve(size_t bytes,size_t prefix_bytes) {
    char option[PROP_VALUE_MAX]={0};
    __system_property_get("debug.d2rm.shared_heap",option);
    if(!strcmp(option,"0")){LOG("Vulkan guest heap disabled by diagnostic property");return NULL;}
    if(!bytes || !prefix_bytes || dm_vk_open())return NULL;
    pthread_mutex_lock(&lock);
    if(guest_heap.mapped) {
        void *result=guest_heap.size==bytes && guest_heap.prefix_size==prefix_bytes?guest_heap.mapped:NULL;
        pthread_mutex_unlock(&lock);return result;
    }
    VkBuffer probe=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    void *mapped=NULL,*prefix=MAP_FAILED;VkResult result=VK_ERROR_INITIALIZATION_FAILED;
    const char *phase="buffer requirements";
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if((result=vkCreateBuffer(v.device,&bc,NULL,&probe)))goto fail;
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(v.device,probe,&req);
    VkPhysicalDeviceMaintenance3Properties limits={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&limits};
    vkGetPhysicalDeviceProperties2(v.gpu,&properties);
    if(req.size<bytes || req.size>limits.maxMemoryAllocationSize)goto fail;
    VkPhysicalDeviceMemoryProperties mp=device_memory.properties;
    VkMemoryPropertyFlags desired=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t type=0;
    while(type<mp.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) ||
          (mp.memoryTypes[type].propertyFlags&desired)!=desired || mp.memoryHeaps[mp.memoryTypes[type].heapIndex].size<req.size))type++;
    phase="coherent cached memory type";if(type==mp.memoryTypeCount)goto fail;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    phase="allocation";if((result=device_memory_allocate(&ai,&memory,DEVICE_MEMORY_GUEST)))goto fail;
    phase="mapping";if((result=vkMapMemory(v.device,memory,0,bytes,0,&mapped)))goto fail;
    size_t page=(size_t)getpagesize();uintptr_t address=(uintptr_t)mapped;
    phase="mapped heap alignment";
    if(address%page || bytes%page || prefix_bytes%page || address<prefix_bytes || bytes>UINTPTR_MAX-address)goto fail;
    void *wanted=(void *)(address-prefix_bytes);
    phase="exact non-replacing address prefix";
    prefix=mmap(wanted,prefix_bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    if(prefix!=wanted)goto fail;
    guest_heap.memory=memory;guest_heap.mapped=mapped;guest_heap.prefix=prefix;
    guest_heap.size=bytes;guest_heap.prefix_size=prefix_bytes;guest_heap.memory_type=type;
    __atomic_store_n(&guest_heap_read_base,(uintptr_t)mapped,__ATOMIC_RELEASE);
    vkDestroyBuffer(v.device,probe,NULL);
    LOG("Vulkan guest heap ready: base=%p bytes=%zu prefix=%p prefixBytes=%zu type=%u; backing shared, zero on first exposure",
        mapped,bytes,prefix,prefix_bytes,type);
    pthread_mutex_unlock(&lock);return mapped;
fail:
    LOG("Vulkan guest heap fallback: phase=%s result=%d errno=%d",phase,result,errno);
    if(prefix!=MAP_FAILED)munmap(prefix,prefix_bytes);
    if(mapped)vkUnmapMemory(v.device,memory);
    if(probe)vkDestroyBuffer(v.device,probe,NULL);
    if(memory)device_memory_free(memory);
    pthread_mutex_unlock(&lock);return NULL;
}
int dm_vk_guest_heap_resize(size_t visible) {
    pthread_mutex_lock(&lock);int result=-1;
    if(!guest_heap.mapped || visible>guest_heap.size)goto done;
    // Preserve contents on shrink/regrow as anonymous mprotect did. A shrink
    // must first drain users; it changes the logical accessible range only.
    if(visible<guest_heap.visible && (draw_pending_flush() || compute_pending_flush()))goto done;
    if(visible>guest_heap.zeroed) {
        size_t bytes=visible-guest_heap.zeroed;
        memset(guest_heap.mapped+guest_heap.zeroed,0,bytes);
        guest_heap.zeroed=visible;
        LOG("Vulkan guest heap first exposure: visible=%zu zeroed=%zu",visible,bytes);
    }
    guest_heap.visible=visible;
    __atomic_store_n(&guest_heap_read_visible,visible,__ATOMIC_RELEASE);result=0;
done:
    pthread_mutex_unlock(&lock);return result;
}
int dm_vk_guest_heap_read(uint64_t address,void *out,size_t bytes) {
    uintptr_t base=__atomic_load_n(&guest_heap_read_base,__ATOMIC_ACQUIRE);
    size_t visible=__atomic_load_n(&guest_heap_read_visible,__ATOMIC_ACQUIRE);
    if(!out || !bytes || !base || address<base || bytes>visible || address-base>visible-bytes)return 0;
    // Backing stays mapped until process exit, including across heap shrink.
    // CPU page tracking preserves read permission. No backend lock is taken:
    // HUD callbacks may hold their own input lock while sampling guest state.
    memcpy(out,(const void *)(uintptr_t)address,bytes);return 1;
}
