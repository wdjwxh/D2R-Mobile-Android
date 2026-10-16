// VI handles are opaque guest-facing identities. The future NVN backend must
// acquire the current ANativeWindow via dm_window_acquire(), not cast a handle.
#include "dm_host.h"
#include "display.h"
static const uint32_t display = 0x50534944, layer = 0x5259414c;
static const uint32_t native = 0x4e49574e;
static void vi_init(void) { LOG("VI initialized (Android surface adapter)"); }
static uint32_t vi_open(const void **out) {
    if (!out) return 0x3e2;
    *out = &display;
    return 0;
}
static uint32_t vi_create(const void **out, const void *disp) {
    if (!out || disp != &display) return 0x3e2;
    *out = &layer;
    return 0;
}
static uint32_t vi_native(const void **out, const void *l) {
    if (!out || l != &layer) return 0x3e2;
    *out = NULL;
    ANativeWindow *window = dm_window_acquire();
    if (!window) return 0x3e2;
    ANativeWindow_release(window);
    *out = &native;
    LOG("VI native window ready");
    return 0;
}
typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *vi_force_lookup(const char *name) {
    static const hle_ent entries[] = {
        {"_ZN2nn2vi10InitializeEv", (void *)vi_init},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", (void *)vi_open},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", (void *)vi_create},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", (void *)vi_native},
    };
    for (unsigned i=0; i<sizeof entries/sizeof entries[0]; i++)
        if (!strcmp(name, entries[i].name)) return &entries[i];
    return NULL;
}
