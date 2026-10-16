#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct dm_vk_pool dm_vk_pool;
/* Guest addresses are virtual CPU addresses, never Vulkan device addresses. */
dm_vk_pool *dm_vk_pool_create(void *guest, size_t size,uint32_t flags);
int dm_vk_pool_destroy(dm_vk_pool *pool);
int dm_vk_texture_retire(uint64_t serial);
int dm_vk_pool_sync(dm_vk_pool *pool, size_t offset, size_t size, int download);
int dm_vk_pool_cpu_read(dm_vk_pool *pool,size_t offset,size_t size);
int dm_vk_cpu_read_address(uint64_t address,size_t size);
int dm_vk_cpu_write_address(uint64_t address,size_t size);
int dm_vk_copy(uint64_t source, uint64_t destination, size_t size);
int dm_vk_guest_read(uint64_t address, void *out, size_t size);
// Read the current length-prefixed record under one ownership lock. Returns
// 0 for a full snapshot, 1 for header-only (record exceeds capacity), -1 on error.
// The first two uint32_t fields are opaque kind and total aligned byte length.
int dm_vk_guest_record_read(uint64_t address,void *out,size_t capacity);
// Planner-only snapshots: 0 = complete CPU-ready bytes, 1 = fall back.
// Never flush, submit, wait, poll, publish reports, or change ownership. Only
// registered pool ranges qualify; failure output must not be used. CPU writers
// still obey the NVN command-memory lifetime contract; this does not freeze them.
int dm_vk_guest_range_peek(uint64_t address,void *out,size_t bytes);
int dm_vk_guest_record_peek(uint64_t address,void *out,size_t capacity);
// OP_UPDATE_UB payload must be an owned host copy, outside guest pools. The
// aligned small-update path records a GPU write and consumes data before return.
int dm_vk_guest_write(uint64_t address, const void *data, size_t size);
int dm_vk_guest_event(uint64_t address, uint32_t value, int add);
int dm_vk_guest_event_get(uint64_t address,uint32_t *value);
int dm_vk_event_record(uint64_t address,uint32_t value,int add,const void *queue,uint64_t job);
int dm_vk_event_wait_prepare(uint64_t address,uint32_t value,int gequal,const void *queue,uint64_t job,int *ready);
int dm_vk_completion_pending(void);
int dm_vk_completion_poll(void);
int dm_vk_events_drain(void);
/* Real GPU timestamp converted to NVN's 19.2 MHz tick domain. */
int dm_vk_report_counter(uint64_t address,uint64_t *samples,int top);
int dm_vk_samples_forget(uint64_t *samples);
/* Start a new samples interval; old query objects remain fence-owned. */
int dm_vk_reset_samples(uint64_t *samples);
int dm_vk_open(void);
void dm_vk_close(void);
/* Reserve a stable mapped heap plus an exact anonymous prefix before any
 * guest address is exposed. NULL preserves the ordinary anonymous fallback. */
void *dm_vk_guest_heap_reserve(size_t heap_bytes,size_t prefix_bytes);
int dm_vk_guest_heap_resize(size_t visible_bytes);
/* Device/PFN mappings may reject process_vm_readv. Read only an owned,
 * exposed heap interval; return zero so callers can try their normal guard. */
int dm_vk_guest_heap_read(uint64_t address,void *out,size_t bytes);
int dm_vk_finish(void);
int dm_vk_barrier(void);
/* Attribute real CPU copies at existing completion boundaries, without clocks. */
enum {
    DM_VK_MATERIALIZE_OTHER, DM_VK_MATERIALIZE_COPY, DM_VK_MATERIALIZE_QUERY,
    DM_VK_MATERIALIZE_FENCE, DM_VK_MATERIALIZE_FINISH, DM_VK_MATERIALIZE_EVENT,
    DM_VK_MATERIALIZE_COUNT
};
int dm_vk_finish_reason(unsigned reason);
int dm_vk_barrier_reason(unsigned reason);
/* Prior GPU work complete; no implicit guest-buffer CPU materialization. */
int dm_vk_gpu_complete(void);
/* A token outlives its reusable hardware fence through explicit references.
 * Wait returns 0=complete, 1=timeout, -1=error; timeout is nanoseconds. */
typedef struct dm_vk_sync_ticket dm_vk_sync_ticket;
dm_vk_sync_ticket *dm_vk_sync_submit(int cpu_visible);
void dm_vk_sync_retain(dm_vk_sync_ticket *ticket);
void dm_vk_sync_release(dm_vk_sync_ticket *ticket);
int dm_vk_sync_wait(dm_vk_sync_ticket *ticket,uint64_t timeout,int *waited);
int dm_vk_sync_ordered(dm_vk_sync_ticket *ticket);
/* GPU memory ordering only; no CPU completion or texture readback. */
int dm_vk_gpu_barrier(void);
int dm_vk_nvn_barrier(uint32_t bits);
/* Record pending attachment clears before changing the render-target set. */
int dm_vk_attachment_boundary(void);
/* Diagnostic presentation, never substituted for a guest draw. */

int dm_vk_prepare_window(void);

int dm_vk_draw_batch(int enabled);
