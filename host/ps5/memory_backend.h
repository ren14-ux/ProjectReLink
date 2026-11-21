#ifndef RELINK_PS5_MEMORY_BACKEND_H
#define RELINK_PS5_MEMORY_BACKEND_H

/*
 * Coordination helpers for PS5 guest extent bookkeeping and SDK backing.
 * All calls must run under the same external memory lock as both modules.
 */
#include <stdint.h>
#include "memory_state.h"
#include "sdk_extent_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocate backing and register it in memory state as one serialized operation.
 * On registration failure, the helper attempts rollback. If rollback itself
 * fails, token_out retains the live SDK token so the caller can retry release.
 */
Ps5MemResult ps5_mem_backend_allocate_extent(Ps5MemState *state,
                                             Ps5SdkExtentPool *pool,
                                             uint64_t guest_base,
                                             uint64_t *token_out);

/* Adopt a view of the already-reserved guest window, then publish its state transition. */
Ps5MemResult ps5_mem_backend_reserve_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                          uint64_t guest_base, unsigned view_index,
                                          void *address);
Ps5MemResult ps5_mem_backend_release_view_reservation(Ps5MemState *state,
                                                       Ps5SdkExtentPool *pool,
                                                       uint64_t guest_base,
                                                       unsigned view_index);
Ps5MemResult ps5_mem_backend_restore_view_reservation(Ps5MemState *state,
                                                       Ps5SdkExtentPool *pool,
                                                       uint64_t guest_base,
                                                       unsigned view_index);
Ps5MemResult ps5_mem_backend_map_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                      uint64_t guest_base, unsigned view_index,
                                      void *address, int protection, void **view_out);
Ps5MemResult ps5_mem_backend_unmap_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                        uint64_t guest_base, unsigned view_index);
Ps5MemResult ps5_mem_backend_grant_gpu_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                            uint64_t guest_base, unsigned view_index,
                                            int protection);
Ps5MemResult ps5_mem_backend_revoke_gpu_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                             uint64_t guest_base, unsigned view_index);

/* 4 KiB guest-page ownership, coordinated under the backend memory lock. */
Ps5MemResult ps5_mem_backend_claim_pages(Ps5MemState *state,
                                         uint64_t guest_address, uint64_t bytes);
Ps5MemResult ps5_mem_backend_free_pages(Ps5MemState *state,
                                        uint64_t guest_address, uint64_t bytes);
Ps5MemResult ps5_mem_backend_mark_pages_zeroed(Ps5MemState *state,
                                                uint64_t guest_address, uint64_t bytes);
Ps5MemResult ps5_mem_backend_allocated_views(const Ps5MemState *state,
                                             uint64_t guest_extent, uint8_t *view_mask_out);
Ps5MemResult ps5_mem_backend_zero_pending(const Ps5MemState *state,
                                          uint64_t guest_extent, uint16_t *page_mask_out);

/* Vulkan imports hold every covered 16 KiB guest view until the import is retired. */
Ps5MemResult ps5_mem_backend_import_acquire(Ps5MemState *state,
                                            uint64_t guest_address, uint64_t bytes);
Ps5MemResult ps5_mem_backend_import_release(Ps5MemState *state,
                                            uint64_t guest_address, uint64_t bytes);
/* Stop new imports before waiting for existing GPU/import references to drain. */
Ps5MemResult ps5_mem_backend_retire_extent(Ps5MemState *state, uint64_t guest_base);

/*
 * Release a retired extent only after reservations, CPU mappings, GPU grants,
 * and Vulkan import holds have all drained. The state entry is forgotten only
 * after the kernel confirms that direct memory was released.
 */
Ps5MemResult ps5_mem_backend_release_extent(Ps5MemState *state,
                                            Ps5SdkExtentPool *pool,
                                            uint64_t guest_base);

#ifdef __cplusplus
}
#endif
#endif
