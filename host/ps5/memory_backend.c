#include "memory_backend.h"

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
