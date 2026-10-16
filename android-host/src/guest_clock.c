#include "dm_host.h"
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <sched.h>
// Freeze writers are rare. Guest tick readers must not serialize every game
// thread on a mutex around the same monotonic clock syscall.
static pthread_mutex_t clock_lock=PTHREAD_MUTEX_INITIALIZER;
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2 && ATOMIC_INT_LOCK_FREE==2,"guest clock snapshot requires lock-free atomics");
static _Atomic uint64_t clock_sequence;
static _Atomic uint32_t frozen_by;
static _Atomic uint64_t freeze_start,paused_ns;
static uint64_t monotonic_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
uint64_t dm_guest_ns(void) {
    for(;;) {
        uint64_t sequence=atomic_load_explicit(&clock_sequence,memory_order_acquire);
        if(sequence&1){sched_yield();continue;}
        uint32_t frozen=atomic_load_explicit(&frozen_by,memory_order_relaxed);
        uint64_t start=atomic_load_explicit(&freeze_start,memory_order_relaxed);
        uint64_t paused=atomic_load_explicit(&paused_ns,memory_order_relaxed);
        uint64_t now=frozen?start:monotonic_ns();
        atomic_thread_fence(memory_order_acquire);
        if(sequence==atomic_load_explicit(&clock_sequence,memory_order_relaxed))return now-paused;
    }
}
void guest_clock_freeze_for(unsigned reason,int on) {
    if(reason>=32) die("Invalid guest clock freeze reason");
    pthread_mutex_lock(&clock_lock);
    atomic_fetch_add_explicit(&clock_sequence,1,memory_order_acq_rel);
    // If a reader observes any new state, its acquire fence must also see
    // this odd version and reject a snapshot begun before the transition.
    atomic_thread_fence(memory_order_release);
    uint32_t old=atomic_load_explicit(&frozen_by,memory_order_relaxed);
    uint32_t next=on?old|(1u<<reason):old&~(1u<<reason);
    uint64_t now=monotonic_ns();
    if(!old && next)atomic_store_explicit(&freeze_start,now,memory_order_relaxed);
    if(old && !next) {
        uint64_t start=atomic_load_explicit(&freeze_start,memory_order_relaxed);
        atomic_fetch_add_explicit(&paused_ns,now-start,memory_order_relaxed);
    }
    atomic_store_explicit(&frozen_by,next,memory_order_relaxed);
    atomic_fetch_add_explicit(&clock_sequence,1,memory_order_release);
    pthread_mutex_unlock(&clock_lock);
}
