/* Android ABI adapter for the unchanged upstream Rift gameplay implementation.
 * No fallback implementations: save/tick/command integration is mandatory.
 */
#pragma once
#include "dm_host.h"
#include <stddef.h>
#include <time.h>
#define guest_ns dm_guest_ns
enum { OBS_LEVEL_WARP = 16, OBS_QUEST_KILL = 17, OBS_PRESET_SPAWN = 22 };
void gate_obs_set(int, void (*)(const uint64_t *));
int gate_obs_present(int);
void dm_quiet_difficulty_msg(int);
/* Call only once the complete gameplay and save transaction chain is ready. */
void dm_android_rift_install_save_hook(void);
static inline uint64_t clock_gettime_nsec_np(clockid_t clock_id) {
    struct timespec ts;
    if (clock_gettime(clock_id, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
