#include "memory_state.h"

#include <assert.h>
#include <stdint.h>

static void test_extent_lifecycle(void) {
    Ps5MemState state;
    Ps5MemExtent storage[2];
    uint64_t handle = 0;

    assert(ps5_mem_state_init(&state, storage, 2) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, 0x100000, 7) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, 0x100000, 8) == PS5_MEM_STATE);

    /* A view cannot be granted to the GPU before it is mapped. */
    assert(ps5_mem_view_gpu_granted(&state, 0x100000) == PS5_MEM_STATE);
    assert(ps5_mem_view_mapped(&state, 0x100000) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, 0x100000) == PS5_MEM_STATE);
    assert(ps5_mem_view_gpu_granted(&state, 0x100000) == PS5_MEM_OK);

    assert(ps5_mem_import_acquire(&state, 0x100000, PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_extent_retire(&state, 0x100000) == PS5_MEM_OK);
    assert(ps5_mem_import_acquire(&state, 0x100000, PS5_MEM_VIEW_SIZE) == PS5_MEM_STATE);
    assert(!ps5_mem_extent_release_ready(&state, 0x100000));

    /* Neither unmapping nor revoking GPU access may race an outstanding import. */
    assert(ps5_mem_view_unmapped(&state, 0x100000) == PS5_MEM_BUSY);
    assert(ps5_mem_view_gpu_revoked(&state, 0x100000) == PS5_MEM_BUSY);
    assert(ps5_mem_import_release(&state, 0x100000, PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_view_gpu_revoked(&state, 0x100000) == PS5_MEM_OK);
    assert(ps5_mem_view_unmapped(&state, 0x100000) == PS5_MEM_OK);
    assert(ps5_mem_extent_release_ready(&state, 0x100000));
    assert(ps5_mem_extent_forget(&state, 0x100000, &handle) == PS5_MEM_OK);
    assert(handle == 7 && state.count == 0);
}

static void test_atomic_range_validation(void) {
    Ps5MemState state;
    Ps5MemExtent storage[1];

    assert(ps5_mem_state_init(&state, storage, 1) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, 0x200000, 9) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, 0x200000) == PS5_MEM_OK);
    assert(ps5_mem_view_gpu_granted(&state, 0x200000) == PS5_MEM_OK);

    /* The first view is valid but the second extent is absent: no partial hold. */
    assert(ps5_mem_import_acquire(&state, 0x200000, 2 * PS5_MEM_VIEW_SIZE) == PS5_MEM_NOT_FOUND);
    assert(ps5_mem_view_gpu_revoked(&state, 0x200000) == PS5_MEM_OK);

    assert(ps5_mem_import_acquire(&state, UINT64_MAX - 0x3fff, PS5_MEM_VIEW_SIZE) == PS5_MEM_OVERFLOW);
    assert(ps5_mem_import_acquire(&state, 0x200001, PS5_MEM_VIEW_SIZE) == PS5_MEM_INVALID);
    assert(ps5_mem_import_release(&state, 0x200000, PS5_MEM_VIEW_SIZE) == PS5_MEM_STATE);
    assert(ps5_mem_view_unmapped(&state, 0x200000) == PS5_MEM_OK);
}

static void test_extent_capacity_and_boundaries(void) {
    Ps5MemState state;
    Ps5MemExtent storage[1];

    assert(ps5_mem_state_init(&state, storage, 1) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, 0x300001, 1) == PS5_MEM_INVALID);
    assert(ps5_mem_extent_add(&state, UINT64_MAX - 0xffff, 3) == PS5_MEM_INVALID);
    assert(ps5_mem_extent_add(&state, 0x300000, PS5_MEM_NO_HANDLE) == PS5_MEM_INVALID);
    assert(ps5_mem_extent_add(&state, 0x300000, 1) == PS5_MEM_OK);
    uint64_t handle = 0;
    assert(ps5_mem_extent_get_handle(&state, 0x300000, &handle) == PS5_MEM_OK);
    assert(handle == 1);
    assert(ps5_mem_extent_add(&state, 0x400000, 2) == PS5_MEM_FULL);
    assert(ps5_mem_extent_retire(&state, 0x300001) == PS5_MEM_INVALID);
    assert(ps5_mem_extent_retire(&state, 0x300000) == PS5_MEM_OK);
    assert(ps5_mem_extent_release_ready(&state, 0x300000));
}


static void test_cross_extent_import_lifecycle(void) {
    Ps5MemState state;
    Ps5MemExtent storage[2];
    const uint64_t first = 0x500000;
    const uint64_t second = first + PS5_MEM_EXTENT_SIZE;
    unsigned i;

    assert(ps5_mem_state_init(&state, storage, 2) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, first, 11) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, second, 12) == PS5_MEM_OK);

    /* Map and grant the last view of extent one and first view of extent two. */
    assert(ps5_mem_view_mapped(&state, first + 3 * PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_view_gpu_granted(&state, first + 3 * PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, second) == PS5_MEM_OK);
    assert(ps5_mem_view_gpu_granted(&state, second) == PS5_MEM_OK);

    /* One import spanning the boundary must hold both extents atomically. */
    assert(ps5_mem_import_acquire(&state, first + 3 * PS5_MEM_VIEW_SIZE,
                                  2 * PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_extent_retire(&state, first) == PS5_MEM_OK);
    assert(ps5_mem_extent_retire(&state, second) == PS5_MEM_OK);
    assert(!ps5_mem_extent_release_ready(&state, first));
    assert(!ps5_mem_extent_release_ready(&state, second));

    assert(ps5_mem_import_release(&state, first + 3 * PS5_MEM_VIEW_SIZE,
                                  2 * PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_import_release(&state, first + 3 * PS5_MEM_VIEW_SIZE,
                                  2 * PS5_MEM_VIEW_SIZE) == PS5_MEM_STATE);

    for (i = 0; i < 2; ++i) {
        uint64_t view = i == 0 ? first + 3 * PS5_MEM_VIEW_SIZE : second;
        uint64_t base = i == 0 ? first : second;
        uint64_t handle = 0;
        assert(ps5_mem_view_gpu_revoked(&state, view) == PS5_MEM_OK);
        assert(ps5_mem_view_unmapped(&state, view) == PS5_MEM_OK);
        assert(ps5_mem_extent_release_ready(&state, base));
        assert(ps5_mem_extent_forget(&state, base, &handle) == PS5_MEM_OK);
        assert(handle == (i == 0 ? 11u : 12u));
    }
    assert(state.count == 0);
}

int main(void) {
    test_extent_lifecycle();
    test_atomic_range_validation();
    test_extent_capacity_and_boundaries();
    test_cross_extent_import_lifecycle();
    return 0;
}
