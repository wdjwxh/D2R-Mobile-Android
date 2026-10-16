// nv::（NVN 下面的显卡驱动胶水层：建传输内存、连 nvdrv 服务）。NVN 接入 Android Vulkan 设备，这层无事可做。
// 分配器回调记在宿主全局 g_nv_alloc，图形层以后要按游戏的分配器分内存时从这里取。
// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). Android Vulkan owns the host device; Horizon nvdrv IPC is not used.
// The allocator callbacks are recorded in the host global g_nv_alloc; the graphics layer takes them from here later when it needs to allocate memory with the game's allocator.
#include "dm_host.h"
#include "vk_backend.h"

typedef void *(*nv_alloc_fn)(unsigned long size, unsigned long align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, unsigned long size, void *user);
struct nv_allocator { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; };
struct nv_allocator g_nv_alloc, g_nv_devtools_alloc;

static void h_nv_SetAlloc(nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_alloc = (struct nv_allocator){a, f, r, u}; }
static void h_nv_SetDevAlloc(nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (struct nv_allocator){a, f, r, u}; }
static void h_nv_Init(void *mem, unsigned long size) { (void)mem; (void)size; if(dm_vk_open()) die("NV Vulkan initialization failed"); }
void dm_nvn_translate_drain(void);
static void h_nv_Fin(void) { dm_nvn_translate_drain();dm_vk_close(); }

typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *nv_force_lookup(const char *name) {
    static const hle_ent entries[]={
        {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",(void *)h_nv_SetAlloc},
        {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",(void *)h_nv_SetDevAlloc},
        {"_ZN2nv18InitializeGraphicsEPvm",(void *)h_nv_Init},
        {"_ZN2nv16FinalizeGraphicsEv",(void *)h_nv_Fin},
    };
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++)
        if(!strcmp(name,entries[i].name)) return &entries[i];
    return NULL;
}
