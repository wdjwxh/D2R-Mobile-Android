#pragma once
#include <android/native_window.h>

/* Callers own the returned reference; release after use. Surface replacement
 * never invalidates an outstanding reference. The Android surface itself may
 * still be lost; Vulkan callers must handle VK_ERROR_SURFACE_LOST_KHR. */
ANativeWindow *dm_window_acquire(void);
void dm_window_set(ANativeWindow *window);
