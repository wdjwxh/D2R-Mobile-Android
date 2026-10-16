/* HUD sampling remains on upstream OBS_TICK, never on the Android UI thread. */
#include "dm_host.h"
#include "../../d2r-mobile/dm/host/dm_hud.c"
#include "../../d2r-mobile/dm/ios/Sources/dm_touch_game_identity.h"
static int tk_rd(uint64_t address,void *out,size_t bytes) { return dm_tg_vm_read(address,out,bytes); }
#include "../generated/hud_sample.inc"
#include "../generated/rift/rift_diag.c"
void gate_obs_set(int,void (*)(const uint64_t *));
int gate_obs_present(int);
static void android_hud_tick(const uint64_t *regs) {dm_android_rift_observe_game(regs[0]);players_apply();hud_tick(regs[0]);}
int dm_android_hud_attach(void) {
    if(!gate_obs_present(2)){LOG("Android HUD: missing upstream OBS_TICK");return 0;}
    gate_obs_set(2,android_hud_tick);return 1;
}
