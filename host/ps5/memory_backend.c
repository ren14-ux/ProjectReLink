#include "memory_backend.h"

Ps5MemResult ps5_mem_backend_allocate_extent(Ps5MemState *state,
                                             Ps5SdkExtentPool *pool,
                                             uint64_t guest_base,
                                             uint64_t *token_out) {
    uint64_t token = PS5_SDK_EXTENT_NO_TOKEN;
    Ps5SdkExtentResult sdk_result;
    Ps5MemResult state_result;

    if (token_out) *token_out = PS5_SDK_EXTENT_NO_TOKEN;
    if (!state || !pool || !token_out ||
        (guest_base & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;

    sdk_result = ps5_sdk_extent_allocate(pool, &token);
    if (sdk_result == PS5_SDK_EXTENT_FULL) return PS5_MEM_FULL;
    if (sdk_result != PS5_SDK_EXTENT_OK) return PS5_MEM_STATE;

    state_result = ps5_mem_extent_add(state, guest_base, token);
    if (state_result == PS5_MEM_OK) {
        *token_out = token;
        return PS5_MEM_OK;
    }

    /* Registration failed: do not leak backing if rollback succeeds. */
    sdk_result = ps5_sdk_extent_release(pool, token);
    if (sdk_result != PS5_SDK_EXTENT_OK) {
        /* Caller retains the token and can retry release under the same lock. */
        *token_out = token;
        return PS5_MEM_BUSY;
    }
    return state_result;
}

static Ps5MemResult backend_get_token(Ps5MemState *state, uint64_t guest_base,
                                        uint64_t *token) {
    return ps5_mem_extent_get_handle(state, guest_base, token);
}

Ps5MemResult ps5_mem_backend_reserve_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                          uint64_t guest_base, unsigned view_index,
                                          void *address) {
    uint64_t token;
    Ps5MemResult sr;
    Ps5SdkExtentResult kr;
    if (!state || !pool || view_index >= PS5_MEM_VIEWS_PER_EXTENT ||
        (guest_base & (PS5_MEM_EXTENT_SIZE - 1)) ||
        (uintptr_t)address != guest_base + (uint64_t)view_index * PS5_MEM_VIEW_SIZE)
        return PS5_MEM_INVALID;
    sr = backend_get_token(state, guest_base, &token);
    if (sr != PS5_MEM_OK) return sr;
    kr = ps5_sdk_extent_reserve_view(pool, token, view_index, address);
    if (kr != PS5_SDK_EXTENT_OK) return kr == PS5_SDK_EXTENT_STALE ? PS5_MEM_NOT_FOUND : PS5_MEM_STATE;
    sr = ps5_mem_view_reserved(state, (uint64_t)(uintptr_t)address);
    if (sr != PS5_MEM_OK) {
        (void)ps5_sdk_extent_release_view_reservation(pool, token, view_index);
        return sr;
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_backend_release_view_reservation(Ps5MemState *state,
                                                       Ps5SdkExtentPool *pool,
                                                       uint64_t guest_base,
                                                       unsigned view_index) {
    uint64_t token, address;
    Ps5MemResult sr;
    Ps5SdkExtentResult kr;
    if (!state || !pool || view_index >= PS5_MEM_VIEWS_PER_EXTENT ||
        (guest_base & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    address = guest_base + (uint64_t)view_index * PS5_MEM_VIEW_SIZE;
    sr = backend_get_token(state, guest_base, &token);
    if (sr != PS5_MEM_OK) return sr;
    sr = ps5_mem_view_reservation_released(state, address);
    if (sr != PS5_MEM_OK) return sr;
    kr = ps5_sdk_extent_release_view_reservation(pool, token, view_index);
    if (kr != PS5_SDK_EXTENT_OK) {
        /* Restore bookkeeping: the kernel reservation remains live on failure. */
        (void)ps5_mem_view_reserved(state, address);
        return PS5_MEM_BUSY;
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_backend_map_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                      uint64_t guest_base, unsigned view_index,
                                      void *address, int protection, void **view_out) {
    uint64_t token;
    Ps5MemResult sr;
    Ps5SdkExtentResult kr;
    if (view_out) *view_out = NULL;
    if (!state || !pool || !view_out || view_index >= PS5_MEM_VIEWS_PER_EXTENT ||
        (guest_base & (PS5_MEM_EXTENT_SIZE - 1)) ||
        (uintptr_t)address != guest_base + (uint64_t)view_index * PS5_MEM_VIEW_SIZE)
        return PS5_MEM_INVALID;
    sr = backend_get_token(state, guest_base, &token);
    if (sr != PS5_MEM_OK) return sr;
    kr = ps5_sdk_extent_map_view(pool, token, view_index, address, protection, view_out);
    if (kr != PS5_SDK_EXTENT_OK) return kr == PS5_SDK_EXTENT_STALE ? PS5_MEM_NOT_FOUND : PS5_MEM_STATE;
    sr = ps5_mem_view_mapped(state, (uint64_t)(uintptr_t)address);
    if (sr != PS5_MEM_OK) {
        /* Mapping succeeded but state rejected it: unmap, then clear stale reservation state. */
        (void)ps5_sdk_extent_unmap_view(pool, token, view_index);
        (void)ps5_mem_view_reservation_released(state, (uint64_t)(uintptr_t)address);
        *view_out = NULL;
        return sr;
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_backend_unmap_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                        uint64_t guest_base, unsigned view_index) {
    uint64_t token, address;
    Ps5MemResult sr;
    Ps5SdkExtentResult kr;
    if (!state || !pool || view_index >= PS5_MEM_VIEWS_PER_EXTENT ||
        (guest_base & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    address = guest_base + (uint64_t)view_index * PS5_MEM_VIEW_SIZE;
    sr = backend_get_token(state, guest_base, &token);
    if (sr != PS5_MEM_OK) return sr;
    sr = ps5_mem_view_unmapped(state, address);
    if (sr != PS5_MEM_OK) return sr;
    kr = ps5_sdk_extent_unmap_view(pool, token, view_index);
    if (kr != PS5_SDK_EXTENT_OK) {
        /* Kernel mapping remains live on unmap failure; restore state masks. */
        if (ps5_mem_view_reserved(state, address) == PS5_MEM_OK)
            (void)ps5_mem_view_mapped(state, address);
        return PS5_MEM_BUSY;
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_backend_release_extent(Ps5MemState *state,
                                            Ps5SdkExtentPool *pool,
                                            uint64_t guest_base) {
    uint64_t handle;
    Ps5MemResult state_result;
    Ps5SdkExtentResult sdk_result;

    if (!state || !pool || (guest_base & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    if (!ps5_mem_extent_release_ready(state, guest_base))
        return PS5_MEM_BUSY;

    state_result = ps5_mem_extent_get_handle(state, guest_base, &handle);
    if (state_result != PS5_MEM_OK) return state_result;

    /*
     * The SDK pool independently refuses release while tracked CPU views
     * remain mapped. The memory-state gate additionally checks reservations,
     * GPU permissions, and all Vulkan import references.
     */
    sdk_result = ps5_sdk_extent_release(pool, handle);
    if (sdk_result == PS5_SDK_EXTENT_STALE) return PS5_MEM_NOT_FOUND;
    if (sdk_result != PS5_SDK_EXTENT_OK) return PS5_MEM_BUSY;

    /*
     * With the caller holding the shared lock, release_ready cannot change
     * between the preflight above and this metadata removal.
     */
    return ps5_mem_extent_forget(state, guest_base, &handle);
}
