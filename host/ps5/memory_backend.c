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
        (guest_base & (PS5_MEM_EXTENT_SIZE - 1)) || !pool->guest_window_bound ||
        guest_base < pool->guest_window_base)
        return PS5_MEM_INVALID;
    {
        const uintptr_t offset = (uintptr_t)guest_base - pool->guest_window_base;
        if (offset > pool->guest_window_bytes ||
            PS5_MEM_EXTENT_SIZE > pool->guest_window_bytes - offset)
            return PS5_MEM_INVALID;
    }

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

Ps5MemResult ps5_mem_backend_restore_view_reservation(Ps5MemState *state,
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
    kr = ps5_sdk_extent_restore_view_reservation(pool, token, view_index);
    if (kr != PS5_SDK_EXTENT_OK)
        return kr == PS5_SDK_EXTENT_STALE ? PS5_MEM_NOT_FOUND : PS5_MEM_BUSY;
    return ps5_mem_view_reserved(state, address);
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
        if (ps5_sdk_extent_unmap_view(pool, token, view_index) == PS5_SDK_EXTENT_OK) {
            if (ps5_mem_view_reservation_released(state, (uint64_t)(uintptr_t)address) == PS5_MEM_OK)
                (void)ps5_sdk_extent_release_view_reservation(pool, token, view_index);
        }
        /* If rollback fails, keep the state reservation bit set as a fail-closed
           blocker; the pool's mapped bit also prevents backing release. */
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
    if (kr == PS5_SDK_EXTENT_RESTORE_REQUIRED)
        return PS5_MEM_BUSY; /* Mapping is gone; retry reservation restoration separately. */
    if (kr != PS5_SDK_EXTENT_OK) {
        /* Kernel unmap failed, so the original mapping remains live. */
        if (ps5_mem_view_reserved(state, address) == PS5_MEM_OK)
            (void)ps5_mem_view_mapped(state, address);
        return PS5_MEM_BUSY;
    }
    sr = ps5_mem_view_reserved(state, address);
    return sr;
}

Ps5MemResult ps5_mem_backend_grant_gpu_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
                                            uint64_t guest_base, unsigned view_index,
                                            int protection) {
    uint64_t token, address;
    Ps5MemResult sr;
    Ps5SdkExtentResult kr;
    if (!state || !pool || view_index >= PS5_MEM_VIEWS_PER_EXTENT ||
        (guest_base & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    address = guest_base + (uint64_t)view_index * PS5_MEM_VIEW_SIZE;
    sr = backend_get_token(state, guest_base, &token);
    if (sr != PS5_MEM_OK) return sr;
    kr = ps5_sdk_extent_set_gpu_access(pool, token, view_index, protection);
    if (kr != PS5_SDK_EXTENT_OK)
        return kr == PS5_SDK_EXTENT_STALE ? PS5_MEM_NOT_FOUND : PS5_MEM_STATE;
    sr = ps5_mem_view_gpu_granted(state, address);
    if (sr != PS5_MEM_OK) {
        /* Do not expose a GPU grant that the state machine did not accept. */
        (void)ps5_sdk_extent_revoke_gpu_access(pool, token, view_index);
        return sr;
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_backend_revoke_gpu_view(Ps5MemState *state, Ps5SdkExtentPool *pool,
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
    /* The state gate refuses revocation while any Vulkan import still holds the view. */
    sr = ps5_mem_view_gpu_revoked(state, address);
    if (sr != PS5_MEM_OK) return sr;
    kr = ps5_sdk_extent_revoke_gpu_access(pool, token, view_index);
    if (kr != PS5_SDK_EXTENT_OK) {
        (void)ps5_mem_view_gpu_granted(state, address);
        return PS5_MEM_BUSY;
    }
    return PS5_MEM_OK;
}


Ps5MemResult ps5_mem_backend_claim_pages(Ps5MemState *state,
                                         uint64_t guest_address, uint64_t bytes) {
    return ps5_mem_pages_claim(state, guest_address, bytes);
}

Ps5MemResult ps5_mem_backend_free_pages(Ps5MemState *state,
                                        uint64_t guest_address, uint64_t bytes) {
    return ps5_mem_pages_free(state, guest_address, bytes);
}

Ps5MemResult ps5_mem_backend_mark_pages_zeroed(Ps5MemState *state,
                                                uint64_t guest_address, uint64_t bytes) {
    return ps5_mem_pages_mark_zeroed(state, guest_address, bytes);
}

Ps5MemResult ps5_mem_backend_allocated_views(const Ps5MemState *state,
                                             uint64_t guest_extent, uint8_t *view_mask_out) {
    return ps5_mem_pages_allocated_views(state, guest_extent, view_mask_out);
}

Ps5MemResult ps5_mem_backend_zero_pending(const Ps5MemState *state,
                                          uint64_t guest_extent, uint16_t *page_mask_out) {
    return ps5_mem_pages_zero_pending(state, guest_extent, page_mask_out);
}

Ps5MemResult ps5_mem_backend_import_acquire(Ps5MemState *state,
                                            uint64_t guest_address, uint64_t bytes) {
    if (!state) return PS5_MEM_INVALID;
    return ps5_mem_import_acquire(state, guest_address, bytes);
}

Ps5MemResult ps5_mem_backend_import_release(Ps5MemState *state,
                                            uint64_t guest_address, uint64_t bytes) {
    if (!state) return PS5_MEM_INVALID;
    return ps5_mem_import_release(state, guest_address, bytes);
}

Ps5MemResult ps5_mem_backend_retire_extent(Ps5MemState *state, uint64_t guest_base) {
    if (!state || (guest_base & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    return ps5_mem_extent_retire(state, guest_base);
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
