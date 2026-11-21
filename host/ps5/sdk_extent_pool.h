#ifndef RELINK_PS5_SDK_EXTENT_POOL_H
#define RELINK_PS5_SDK_EXTENT_POOL_H

/*
 * PS5 direct-memory ownership adapter.
 *
 * All calls must be externally serialized. A token is an opaque slot +
 * generation value, not a kernel physical address. The caller must retire all
 * mappings and GPU/import references before releasing a token.
 */
#include <stddef.h>
#include <stdint.h>
#include "ps5platform/shm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS5_SDK_EXTENT_BYTES ((size_t)0x10000)
#define PS5_SDK_EXTENT_VIEW_BYTES ((size_t)0x4000)
#define PS5_SDK_EXTENT_VIEW_COUNT 4u
#define PS5_SDK_EXTENT_NO_TOKEN UINT64_C(0)

typedef struct Ps5SdkExtentSlot {
    struct ps5_shm shm;
    uint32_t generation;
    void *views[PS5_SDK_EXTENT_VIEW_COUNT];
    void *reservations[PS5_SDK_EXTENT_VIEW_COUNT];
    uint8_t reserved_mask;
    uint8_t mapped_mask;
    uint8_t occupied;
} Ps5SdkExtentSlot;

typedef struct Ps5SdkExtentPool {
    Ps5SdkExtentSlot *slots;
    size_t capacity;
    size_t count;
} Ps5SdkExtentPool;

typedef enum Ps5SdkExtentResult {
    PS5_SDK_EXTENT_OK = 0,
    PS5_SDK_EXTENT_INVALID = -1,
    PS5_SDK_EXTENT_FULL = -2,
    PS5_SDK_EXTENT_STALE = -3,
    PS5_SDK_EXTENT_KERNEL = -4,
    PS5_SDK_EXTENT_OVERFLOW = -5
} Ps5SdkExtentResult;

/* Caller owns slot storage for the pool's lifetime; capacity must fit a token. */
Ps5SdkExtentResult ps5_sdk_extent_pool_init(Ps5SdkExtentPool *pool,
                                             Ps5SdkExtentSlot *slots,
                                             size_t capacity);

/* Allocates one 64 KiB direct-memory extent and returns a generation token. */
Ps5SdkExtentResult ps5_sdk_extent_allocate(Ps5SdkExtentPool *pool, uint64_t *token_out);

/* Reserve/release one exact 16 KiB guest view before/after fixed mapping. */
Ps5SdkExtentResult ps5_sdk_extent_reserve_view(Ps5SdkExtentPool *pool, uint64_t token,
                                               unsigned view_index, void *address);
Ps5SdkExtentResult ps5_sdk_extent_release_view_reservation(Ps5SdkExtentPool *pool,
                                                            uint64_t token,
                                                            unsigned view_index);

/* Map/unmap one fixed 16 KiB view. Mapping consumes its tracked reservation. */
Ps5SdkExtentResult ps5_sdk_extent_map_view(Ps5SdkExtentPool *pool, uint64_t token,
                                           unsigned view_index, void *address,
                                           int protection, void **view_out);
Ps5SdkExtentResult ps5_sdk_extent_unmap_view(Ps5SdkExtentPool *pool, uint64_t token,
                                             unsigned view_index);

/*
 * Releases the direct-memory backing. Tracked CPU views must already be unmapped;
 * caller must also revoke GPU access and retire every Vulkan import. A kernel failure leaves
 * the slot and token intact so release can be retried.
 */
Ps5SdkExtentResult ps5_sdk_extent_release(Ps5SdkExtentPool *pool, uint64_t token);

#ifdef __cplusplus
}
#endif
#endif
