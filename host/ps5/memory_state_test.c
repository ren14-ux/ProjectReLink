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
    assert(ps5_mem_view_reserved(&state, 0x100000) == PS5_MEM_OK);
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
    assert(ps5_mem_view_reserved(&state, 0x200000) == PS5_MEM_OK);
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
    /* A second guest extent cannot alias the first extent's SDK handle. */
    assert(ps5_mem_extent_add(&state, 0x400000, 1) == PS5_MEM_STATE);
    assert(state.count == 1);
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
    assert(ps5_mem_view_reserved(&state, first + 3 * PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, first + 3 * PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_view_gpu_granted(&state, first + 3 * PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_view_reserved(&state, second) == PS5_MEM_OK);
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

static void test_unmap_requires_explicit_reservation_restore(void) {
    Ps5MemState state;
    Ps5MemExtent storage[1];
    const uint64_t view = 0x600000;
    assert(ps5_mem_state_init(&state, storage, 1) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, view, 21) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, view) == PS5_MEM_STATE);
    assert(ps5_mem_view_reserved(&state, view) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, view) == PS5_MEM_OK);
    assert(ps5_mem_view_unmapped(&state, view) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, view) == PS5_MEM_STATE);
    assert(ps5_mem_view_reserved(&state, view) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, view) == PS5_MEM_OK);
}

static void test_reserved_view_blocks_extent_release(void) {
    Ps5MemState state;
    Ps5MemExtent storage[1];
    uint64_t handle = 0;
    const uint64_t base = 0x700000;

    assert(ps5_mem_state_init(&state, storage, 1) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, base, 31) == PS5_MEM_OK);
    assert(ps5_mem_view_reserved(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_extent_retire(&state, base) == PS5_MEM_OK);
    assert(!ps5_mem_extent_release_ready(&state, base));
    assert(ps5_mem_extent_forget(&state, base, &handle) == PS5_MEM_BUSY);
    /* Once this extent relinquishes its slice of the parent reservation,
       bookkeeping can allow backing release; the parent window stays reserved. */
    assert(ps5_mem_view_reservation_released(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_extent_release_ready(&state, base));
    assert(ps5_mem_extent_forget(&state, base, &handle) == PS5_MEM_OK);
    assert(handle == 31);
}


static void test_guest_page_ownership_and_import_lifetime(void) {
    Ps5MemState state;
    Ps5MemExtent storage[1];
    const uint64_t base = 0x800000;
    const uint64_t page = base + 0x1000;
    uint8_t views = 0;
    uint16_t pending = 0;
    uint8_t reserved = 0, mapped = 0, gpu = 0;
    uint32_t imports[PS5_MEM_VIEWS_PER_EXTENT] = {0};

    assert(ps5_mem_state_init(&state, storage, 1) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, base, 41) == PS5_MEM_OK);

    /* A 4 KiB page is tracked separately from its 16 KiB mapping view. */
    assert(ps5_mem_pages_claim(&state, page, 0x1000) == PS5_MEM_OK);
    assert(ps5_mem_pages_claim_missing(&state, page, 0x1000) == PS5_MEM_OK);
    assert(ps5_mem_pages_allocated_views(&state, base, &views) == PS5_MEM_OK);
    assert(views == 1);
    assert(ps5_mem_pages_zero_pending(&state, base, &pending) == PS5_MEM_OK);
    assert(pending == 0x2);

    /* The page cannot be declared initialized until its view is mapped. */
    assert(ps5_mem_pages_mark_zeroed(&state, page, 0x1000) == PS5_MEM_STATE);
    assert(ps5_mem_view_reserved(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_view_mapped(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_extent_view_state(&state, base, &reserved, &mapped, &gpu, imports) == PS5_MEM_OK);
    assert(reserved == 0 && mapped == 1 && gpu == 0 && imports[0] == 0);
    assert(ps5_mem_pages_mark_zeroed(&state, page, 0x1000) == PS5_MEM_OK);
    assert(ps5_mem_pages_zero_pending(&state, base, &pending) == PS5_MEM_OK);
    assert(pending == 0);

    assert(ps5_mem_view_gpu_granted(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_import_acquire(&state, base, PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_extent_view_state(&state, base, &reserved, &mapped, &gpu, imports) == PS5_MEM_OK);
    assert(mapped == 1 && gpu == 1 && imports[0] == 1);
    /* A live import prevents ownership changes to any 4 KiB subpage in its view. */
    assert(ps5_mem_pages_claim(&state, base, 0x1000) == PS5_MEM_BUSY);
    assert(ps5_mem_pages_claim_missing(&state, base, 0x1000) == PS5_MEM_BUSY);
    assert(ps5_mem_pages_free(&state, page, 0x1000) == PS5_MEM_BUSY);
    assert(ps5_mem_import_release(&state, base, PS5_MEM_VIEW_SIZE) == PS5_MEM_OK);
    assert(ps5_mem_pages_claim_missing(&state, base, 0x1000) == PS5_MEM_OK);
    assert(ps5_mem_pages_mark_zeroed(&state, base, 0x1000) == PS5_MEM_OK);
    assert(ps5_mem_pages_free(&state, base, 0x1000) == PS5_MEM_OK);
    assert(ps5_mem_pages_free(&state, page, 0x1000) == PS5_MEM_OK);
    assert(ps5_mem_extent_retire(&state, base) == PS5_MEM_OK);
    assert(!ps5_mem_extent_release_ready(&state, base));

    assert(ps5_mem_view_gpu_revoked(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_view_unmapped(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_view_reserved(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_view_reservation_released(&state, base) == PS5_MEM_OK);
    assert(ps5_mem_extent_release_ready(&state, base));
}

static void test_page_claim_range_is_atomic(void) {
    Ps5MemState state;
    Ps5MemExtent storage[1];
    uint8_t views = 0;
    assert(ps5_mem_state_init(&state, storage, 1) == PS5_MEM_OK);
    assert(ps5_mem_extent_add(&state, 0x900000, 51) == PS5_MEM_OK);

    /* The range reaches an unregistered extent: the valid first page stays free. */
    assert(ps5_mem_pages_claim(&state, 0x90f000, 0x2000) == PS5_MEM_NOT_FOUND);
    assert(ps5_mem_pages_allocated_views(&state, 0x900000, &views) == PS5_MEM_OK);
    assert(views == 0);
    assert(ps5_mem_pages_claim(&state, 0x900001, 0x1000) == PS5_MEM_INVALID);
}

int main(void) {
    test_extent_lifecycle();
    test_atomic_range_validation();
    test_extent_capacity_and_boundaries();
    test_cross_extent_import_lifecycle();
    test_unmap_requires_explicit_reservation_restore();
    test_reserved_view_blocks_extent_release();
    test_guest_page_ownership_and_import_lifetime();
    test_page_claim_range_is_atomic();
    return 0;
}
