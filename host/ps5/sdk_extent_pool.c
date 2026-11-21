#include "sdk_extent_pool.h"

#include "ps5platform/kernel.h"

#include <string.h>

static int decode_token(const Ps5SdkExtentPool *pool, uint64_t token,
                        size_t *index_out, uint32_t *generation_out) {
    uint32_t low = (uint32_t)token;
    uint32_t generation = (uint32_t)(token >> 32);
    size_t index;
    if (!pool || !pool->slots || !low || !generation) return 0;
    index = (size_t)(low - 1u);
    if (index >= pool->capacity) return 0;
    if (!pool->slots[index].occupied ||
        pool->slots[index].generation != generation) return 0;
    *index_out = index;
    *generation_out = generation;
    return 1;
}

Ps5SdkExtentResult ps5_sdk_extent_pool_init(Ps5SdkExtentPool *pool,
                                             Ps5SdkExtentSlot *slots,
                                             size_t capacity) {
    if (!pool || !slots || !capacity || capacity > UINT32_MAX)
        return PS5_SDK_EXTENT_INVALID;
    memset(slots, 0, capacity * sizeof(*slots));
    pool->slots = slots;
    pool->capacity = capacity;
    pool->count = 0;
    return PS5_SDK_EXTENT_OK;
}

Ps5SdkExtentResult ps5_sdk_extent_allocate(Ps5SdkExtentPool *pool, uint64_t *token_out) {
    size_t i;
    int64_t direct_start = -1;
    int32_t result;
    if (token_out) *token_out = PS5_SDK_EXTENT_NO_TOKEN;
    if (!pool || !pool->slots || !pool->capacity || !token_out)
        return PS5_SDK_EXTENT_INVALID;

    for (i = 0; i < pool->capacity; ++i) {
        Ps5SdkExtentSlot *slot = &pool->slots[i];
        if (!slot->occupied && slot->generation != UINT32_MAX) {
            result = sceKernelAllocateDirectMemory(
                0, sceKernelGetDirectMemorySize(), PS5_SDK_EXTENT_BYTES,
                PS5_SDK_EXTENT_BYTES, PS5_KERNEL_DIRECT_TYPE_CPU, &direct_start);
            if (result != 0) return PS5_SDK_EXTENT_KERNEL;
            if (direct_start < 0) {
                /* Do not pass an invalid offset back to the kernel as a release. */
                return PS5_SDK_EXTENT_KERNEL;
            }
            ++slot->generation;
            slot->shm.direct_start = direct_start;
            slot->shm.bytes = PS5_SDK_EXTENT_BYTES;
            slot->occupied = 1;
            ++pool->count;
            *token_out = ((uint64_t)slot->generation << 32) | (uint64_t)(i + 1u);
            return PS5_SDK_EXTENT_OK;
        }
    }
    return PS5_SDK_EXTENT_FULL;
}

Ps5SdkExtentResult ps5_sdk_extent_get(Ps5SdkExtentPool *pool, uint64_t token,
                                      const struct ps5_shm **shm_out) {
    size_t index;
    uint32_t generation;
    if (shm_out) *shm_out = NULL;
    if (!shm_out) return PS5_SDK_EXTENT_INVALID;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    (void)generation;
    *shm_out = &pool->slots[index].shm;
    return PS5_SDK_EXTENT_OK;
}

Ps5SdkExtentResult ps5_sdk_extent_release(Ps5SdkExtentPool *pool, uint64_t token) {
    size_t index;
    uint32_t generation;
    Ps5SdkExtentSlot *slot;
    int32_t result;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    slot = &pool->slots[index];
    (void)generation;

    /* Do not use ps5_shm_destroy(): the pinned SDK implementation discards
       the object even when sceKernelReleaseDirectMemory reports failure. */
    result = sceKernelReleaseDirectMemory(slot->shm.direct_start, slot->shm.bytes);
    if (result != 0) return PS5_SDK_EXTENT_KERNEL;

    memset(&slot->shm, 0, sizeof(slot->shm));
    slot->occupied = 0;
    --pool->count;
    return PS5_SDK_EXTENT_OK;
}
