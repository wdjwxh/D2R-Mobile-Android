/* The iOS pure-C binding parser/cohort and atlas logic are shared unchanged.
 * Android replaces Mach safe reads and the host clock/module ABI only. */
#include "dm_host.h"
#include "vk_backend.h"
#include <pthread.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include "../../d2r-mobile/dm/ios/Sources/dm_touch_game_identity.h"
#include "../../d2r-mobile/dm/ios/Sources/dm_touch_game_state.c"
#define dm_tg_vm_read dm_tg_vm_read_unused_platform
#include "../../d2r-mobile/dm/ios/Sources/dm_touch_game_identity.c"
#undef dm_tg_vm_read
int dm_tg_vm_read(uint64_t address,void *out,size_t bytes) {
    if(!out || !bytes || !address || address>UINT64_MAX-bytes)return 0;
    if(dm_vk_guest_heap_read(address,out,bytes))return 1;
    struct iovec local={out,bytes},remote={(void *)(uintptr_t)address,bytes};
    return syscall(__NR_process_vm_readv,getpid(),&local,1,&remote,1,0)==(ssize_t)bytes;
}
static uint64_t now_ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
void gate_obs_set(int,void (*)(const uint64_t *));
int gate_obs_present(int);
#include "../generated/touch_game_bridge.inc"
#include "../generated/touch_native_art.inc"

#include "../generated/dm_cursor_item_android.c"
