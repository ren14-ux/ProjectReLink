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
