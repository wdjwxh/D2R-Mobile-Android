// Bootstrap subset of upstream hle_oe.c: docked profile, matching hle_hid.c.
// Android controls CPU/GPU frequencies; Horizon performance IDs are not usable.
#include "dm_host.h"
#include <pthread.h>
static void device_id(uint8_t *out) { memcpy(out, "xbl\0SWNATIVE1001", 16); }
static int operation_mode(void) { return 1; }
static void performance_config(int mode, int config) { (void)mode; (void)config; }
// Small FIFO mirrors upstream notification 15 (focus changed).
static pthread_mutex_t focus_lock = PTHREAD_MUTEX_INITIALIZER;
static int focus = 1, head, count;
static uint32_t messages[32];
void dm_focus_set(int value) {
    pthread_mutex_lock(&focus_lock);
    if (focus != value) {
        focus = value;
        if (count == 32) { head = (head + 1) % 32; count--; }
        messages[(head + count++) % 32] = 15;
    }
    pthread_mutex_unlock(&focus_lock);
}
static int get_focus(void) {
    pthread_mutex_lock(&focus_lock); int value = focus; pthread_mutex_unlock(&focus_lock);
    return value;
}
static unsigned pop_message(uint32_t *out) {
    pthread_mutex_lock(&focus_lock);
    unsigned found = count != 0;
    if (found) { *out = messages[head]; head = (head + 1) % 32; count--; }
    pthread_mutex_unlock(&focus_lock);
    return found;
}
static void focus_mode(int mode) { (void)mode; }
static void notification_enable(unsigned on) { (void)on; } // mode is fixed; no mode-change events
static void capture_policy(void) {} // no Horizon recording/screenshot service

typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *oe_force_lookup(const char *name) {
    static const hle_ent entries[] = {
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", (void *)device_id},
        {"_ZN2nn2oe20GetCurrentFocusStateEv", (void *)get_focus},
        {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", (void *)focus_mode},
        {"_ZN2nn2oe25TryPopNotificationMessageEPj", (void *)pop_message},
        {"_ZN2nn2oe28SetResumeNotificationEnabledEb", (void *)notification_enable},
        {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", (void *)notification_enable},
        {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", (void *)notification_enable},
        {"_ZN2nn2oe15EnableRecordingEv", (void *)capture_policy},
        {"_ZN2nn2oe16DisableRecordingEv", (void *)capture_policy},
        {"_ZN2nn2oe16EnableScreenShotEv", (void *)capture_policy},
        {"_ZN2nn2oe17DisableScreenShotEv", (void *)capture_policy},

        {"_ZN2nn2oe16GetOperationModeEv", (void *)operation_mode},
        {"_ZN2nn2oe18GetPerformanceModeEv", (void *)operation_mode},
        {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", (void *)performance_config},
    };
    for (unsigned i=0; i<sizeof entries/sizeof entries[0]; i++)
        if (!strcmp(name, entries[i].name)) return &entries[i];
    return NULL;
}
