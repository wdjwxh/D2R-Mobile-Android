#include "display.h"
#include <pthread.h>

static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *current_window;

void dm_window_set(ANativeWindow *window) {
    if (window) ANativeWindow_acquire(window);
    pthread_mutex_lock(&window_lock);
    ANativeWindow *old = current_window;
    current_window = window;
    pthread_mutex_unlock(&window_lock);
    if (old) ANativeWindow_release(old);
}

ANativeWindow *dm_window_acquire(void) {
    pthread_mutex_lock(&window_lock);
    ANativeWindow *window = current_window;
    if (window) ANativeWindow_acquire(window);
    pthread_mutex_unlock(&window_lock);
    return window;
}
