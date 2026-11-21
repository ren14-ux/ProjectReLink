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
    if (capacity > SIZE_MAX / sizeof(*slots))
        return PS5_SDK_EXTENT_OVERFLOW;
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

Ps5SdkExtentResult ps5_sdk_extent_reserve_view(Ps5SdkExtentPool *pool, uint64_t token,
                                               unsigned view_index, void *address) {
    size_t index;
    uint32_t generation;
    Ps5SdkExtentSlot *slot;
    void *reserved;
    uint8_t bit;

    if (!address || view_index >= PS5_SDK_EXTENT_VIEW_COUNT ||
        (uintptr_t)address % PS5_SDK_EXTENT_VIEW_BYTES != 0)
        return PS5_SDK_EXTENT_INVALID;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    slot = &pool->slots[index];
    (void)generation;
    bit = (uint8_t)(1u << view_index);
    if ((slot->reserved_mask | slot->mapped_mask) & bit)
        return PS5_SDK_EXTENT_INVALID;

    /*
     * The complete 512 GiB guest window is already reserved by guest_window.c.
     * Reserving this subrange again would collide with that parent reservation.
     * Record the caller's ownership of the existing reservation only.
     */
    reserved = address;
    slot->reservations[view_index] = reserved;
    slot->reserved_mask |= bit;
    return PS5_SDK_EXTENT_OK;
}

Ps5SdkExtentResult ps5_sdk_extent_release_view_reservation(Ps5SdkExtentPool *pool,
                                                            uint64_t token,
                                                            unsigned view_index) {
    size_t index;
    uint32_t generation;
    Ps5SdkExtentSlot *slot;
    uint8_t bit;
    int32_t result;

    if (view_index >= PS5_SDK_EXTENT_VIEW_COUNT) return PS5_SDK_EXTENT_INVALID;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    slot = &pool->slots[index];
    (void)generation;
    bit = (uint8_t)(1u << view_index);
    if (!(slot->reserved_mask & bit) || (slot->mapped_mask & bit) ||
        !slot->reservations[view_index])
        return PS5_SDK_EXTENT_INVALID;

    /* This is bookkeeping only: the parent guest-window reservation remains owned. */
    slot->reservations[view_index] = NULL;
    slot->reserved_mask &= (uint8_t)~bit;
    return PS5_SDK_EXTENT_OK;
}

Ps5SdkExtentResult ps5_sdk_extent_map_view(Ps5SdkExtentPool *pool, uint64_t token,
                                           unsigned view_index, void *address,
                                           int protection, void **view_out) {
    size_t index;
    uint32_t generation;
    Ps5SdkExtentSlot *slot;
    void *mapped = NULL;
    int result;
    if (view_out) *view_out = NULL;
    if (!view_out || !address || view_index >= PS5_SDK_EXTENT_VIEW_COUNT ||
        (uintptr_t)address % PS5_SDK_EXTENT_VIEW_BYTES != 0)
        return PS5_SDK_EXTENT_INVALID;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    slot = &pool->slots[index];
    (void)generation;
    if ((slot->mapped_mask & (uint8_t)(1u << view_index)) ||
        !(slot->reserved_mask & (uint8_t)(1u << view_index)) ||
        slot->reservations[view_index] != address)
        return PS5_SDK_EXTENT_INVALID;

    result = ps5_shm_map(&slot->shm,
                         (size_t)view_index * PS5_SDK_EXTENT_VIEW_BYTES,
                         PS5_SDK_EXTENT_VIEW_BYTES, address, protection,
                         PS5_SHM_FIXED, &mapped);
    if (result != 0) return PS5_SDK_EXTENT_KERNEL;
    if (mapped != address) {
        /* Fixed placement must be exact; undo any unexpected mapping. */
        (void)ps5_shm_unmap(mapped, PS5_SDK_EXTENT_VIEW_BYTES, 0);
        return PS5_SDK_EXTENT_KERNEL;
    }
    slot->views[view_index] = mapped;
    /* Keep the address so unmap can restore the parent-window reservation. */
    slot->reserved_mask &= (uint8_t)~(1u << view_index);
    slot->mapped_mask |= (uint8_t)(1u << view_index);
    *view_out = mapped;
    return PS5_SDK_EXTENT_OK;
}

Ps5SdkExtentResult ps5_sdk_extent_restore_view_reservation(Ps5SdkExtentPool *pool,
                                                            uint64_t token,
                                                            unsigned view_index) {
    size_t index;
    uint32_t generation;
    Ps5SdkExtentSlot *slot;
    void *at;
    int32_t result;
    uint8_t bit;

    if (view_index >= PS5_SDK_EXTENT_VIEW_COUNT) return PS5_SDK_EXTENT_INVALID;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    slot = &pool->slots[index];
    (void)generation;
    bit = (uint8_t)(1u << view_index);
    if (!(slot->restore_mask & bit) || (slot->mapped_mask & bit) ||
        !slot->reservations[view_index]) return PS5_SDK_EXTENT_INVALID;

    at = slot->reservations[view_index];
    result = sceKernelReserveVirtualRange(&at, PS5_SDK_EXTENT_VIEW_BYTES,
                                          PS5_KERNEL_MAP_FIXED, PS5_SDK_EXTENT_VIEW_BYTES);
    if (result != 0) return PS5_SDK_EXTENT_KERNEL;
    if (at != slot->reservations[view_index]) {
        (void)sceKernelMunmap(at, PS5_SDK_EXTENT_VIEW_BYTES);
        return PS5_SDK_EXTENT_KERNEL;
    }
    slot->restore_mask &= (uint8_t)~bit;
    slot->reserved_mask |= bit;
    return PS5_SDK_EXTENT_OK;
}

Ps5SdkExtentResult ps5_sdk_extent_unmap_view(Ps5SdkExtentPool *pool, uint64_t token,
                                             unsigned view_index) {
    size_t index;
    uint32_t generation;
    Ps5SdkExtentSlot *slot;
    int result;
    uint8_t bit;
    if (view_index >= PS5_SDK_EXTENT_VIEW_COUNT) return PS5_SDK_EXTENT_INVALID;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    slot = &pool->slots[index];
    (void)generation;
    bit = (uint8_t)(1u << view_index);
    if (!(slot->mapped_mask & bit) || (slot->gpu_mask & bit) || !slot->views[view_index])
        return PS5_SDK_EXTENT_INVALID;

    result = ps5_shm_unmap(slot->views[view_index], PS5_SDK_EXTENT_VIEW_BYTES, 0);
    if (result != 0) return PS5_SDK_EXTENT_KERNEL;
    slot->views[view_index] = NULL;
    slot->mapped_mask &= (uint8_t)~bit;

    /*
     * sceKernelMunmap leaves a hole. Restore the exact slice of the parent
     * guest-window reservation before allowing it to be mapped again.
     */
    {
        void *at = slot->reservations[view_index];
        result = sceKernelReserveVirtualRange(&at, PS5_SDK_EXTENT_VIEW_BYTES,
                                              PS5_KERNEL_MAP_FIXED,
                                              PS5_SDK_EXTENT_VIEW_BYTES);
        if (result != 0 || at != slot->reservations[view_index]) {
            if (result == 0 && at != slot->reservations[view_index])
                (void)sceKernelMunmap(at, PS5_SDK_EXTENT_VIEW_BYTES);
            slot->restore_mask |= bit;
            return PS5_SDK_EXTENT_RESTORE_REQUIRED;
        }
    }
    slot->reserved_mask |= bit;
    return PS5_SDK_EXTENT_OK;
}

Ps5SdkExtentResult ps5_sdk_extent_set_gpu_access(Ps5SdkExtentPool *pool, uint64_t token,
                                                  unsigned view_index, int protection) {
    size_t index;
    uint32_t generation;
    Ps5SdkExtentSlot *slot;
    uint8_t bit;
    int32_t result;
    const int gpu_bits = PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE;

    if (view_index >= PS5_SDK_EXTENT_VIEW_COUNT ||
        (protection & ~gpu_bits) || protection == 0)
        return PS5_SDK_EXTENT_INVALID;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    slot = &pool->slots[index];
    (void)generation;
    bit = (uint8_t)(1u << view_index);
    if (!(slot->mapped_mask & bit) || !slot->views[view_index])
        return PS5_SDK_EXTENT_INVALID;
    if (slot->gpu_mask & bit) return PS5_SDK_EXTENT_INVALID;

    /* mprotect replaces the complete protection mask; preserve CPU R/W. */
    result = sceKernelMprotect(slot->views[view_index], PS5_SDK_EXTENT_VIEW_BYTES,
                               PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE |
                               protection);
    if (result != 0) return PS5_SDK_EXTENT_KERNEL;
    slot->gpu_mask |= bit;
    return PS5_SDK_EXTENT_OK;
}

Ps5SdkExtentResult ps5_sdk_extent_revoke_gpu_access(Ps5SdkExtentPool *pool, uint64_t token,
                                                      unsigned view_index) {
    size_t index;
    uint32_t generation;
    Ps5SdkExtentSlot *slot;
    uint8_t bit;
    int32_t result;
    const int cpu_protection = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;

    if (view_index >= PS5_SDK_EXTENT_VIEW_COUNT) return PS5_SDK_EXTENT_INVALID;
    if (!decode_token(pool, token, &index, &generation)) return PS5_SDK_EXTENT_STALE;
    slot = &pool->slots[index];
    (void)generation;
    bit = (uint8_t)(1u << view_index);
    if (!(slot->mapped_mask & bit) || !(slot->gpu_mask & bit) || !slot->views[view_index])
        return PS5_SDK_EXTENT_INVALID;

    result = sceKernelMprotect(slot->views[view_index], PS5_SDK_EXTENT_VIEW_BYTES,
                               cpu_protection);
    if (result != 0) return PS5_SDK_EXTENT_KERNEL;
    slot->gpu_mask &= (uint8_t)~bit;
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

    if (slot->mapped_mask || slot->reserved_mask || slot->gpu_mask || slot->restore_mask)
        return PS5_SDK_EXTENT_INVALID;

    /* Do not use ps5_shm_destroy(): the pinned SDK implementation discards
       the object even when sceKernelReleaseDirectMemory reports failure. */
    result = sceKernelReleaseDirectMemory(slot->shm.direct_start, slot->shm.bytes);
    if (result != 0) return PS5_SDK_EXTENT_KERNEL;

    memset(&slot->shm, 0, sizeof(slot->shm));
    slot->occupied = 0;
    --pool->count;
    return PS5_SDK_EXTENT_OK;
}
