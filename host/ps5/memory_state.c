#include "memory_state.h"

#include <string.h>

static Ps5MemExtent *find_extent(Ps5MemState *state, uint64_t base) {
    size_t i;
    if (!state || !state->extents) return NULL;
    for (i = 0; i < state->capacity; ++i) {
        Ps5MemExtent *e = &state->extents[i];
        if (e->occupied && e->guest_base == base) return e;
    }
    return NULL;
}

static const Ps5MemExtent *find_extent_const(const Ps5MemState *state, uint64_t base) {
    size_t i;
    if (!state || !state->extents) return NULL;
    for (i = 0; i < state->capacity; ++i) {
        const Ps5MemExtent *e = &state->extents[i];
        if (e->occupied && e->guest_base == base) return e;
    }
    return NULL;
}

static Ps5MemResult view_location(uint64_t address, uint64_t *base, unsigned *view) {
    if ((address & (PS5_MEM_VIEW_SIZE - 1)) != 0) return PS5_MEM_INVALID;
    *base = address & ~(PS5_MEM_EXTENT_SIZE - 1);
    *view = (unsigned)((address - *base) / PS5_MEM_VIEW_SIZE);
    return *view < PS5_MEM_VIEWS_PER_EXTENT ? PS5_MEM_OK : PS5_MEM_INVALID;
}

static Ps5MemResult range_validate(uint64_t address, uint64_t bytes, uint64_t *end) {
    if (!bytes || (address & (PS5_MEM_VIEW_SIZE - 1)) ||
        (bytes & (PS5_MEM_VIEW_SIZE - 1))) return PS5_MEM_INVALID;
    if (bytes > UINT64_MAX - address) return PS5_MEM_OVERFLOW;
    *end = address + bytes;
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_state_init(Ps5MemState *state, Ps5MemExtent *storage, size_t capacity) {
    if (!state || !storage || !capacity || capacity > SIZE_MAX / sizeof(*storage))
        return PS5_MEM_INVALID;
    memset(storage, 0, capacity * sizeof(*storage));
    state->extents = storage;
    state->capacity = capacity;
    state->count = 0;
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_extent_add(Ps5MemState *state, uint64_t guest_base, uint64_t backing_handle) {
    size_t i;
    if (!state || !state->extents || !state->capacity ||
        (guest_base & (PS5_MEM_EXTENT_SIZE - 1)) ||
        guest_base > UINT64_MAX - PS5_MEM_EXTENT_SIZE || backing_handle == PS5_MEM_NO_HANDLE)
        return PS5_MEM_INVALID;
    if (find_extent(state, guest_base)) return PS5_MEM_STATE;
    for (i = 0; i < state->capacity; ++i) {
        Ps5MemExtent *e = &state->extents[i];
        /* One SDK backing allocation must have exactly one guest owner. */
        if (e->occupied && e->backing_handle == backing_handle) return PS5_MEM_STATE;
    }
    for (i = 0; i < state->capacity; ++i) {
        Ps5MemExtent *e = &state->extents[i];
        if (!e->occupied) {
            memset(e, 0, sizeof(*e));
            e->guest_base = guest_base;
            e->backing_handle = backing_handle;
            e->occupied = 1;
            ++state->count;
            return PS5_MEM_OK;
        }
    }
    return PS5_MEM_FULL;
}

Ps5MemResult ps5_mem_view_reserved(Ps5MemState *state, uint64_t guest_view) {
    uint64_t base;
    unsigned view;
    Ps5MemExtent *e;
    Ps5MemResult r = view_location(guest_view, &base, &view);
    if (r != PS5_MEM_OK) return r;
    e = find_extent(state, base);
    if (!e) return PS5_MEM_NOT_FOUND;
    if (e->pending_release || (e->mapped_mask & (1u << view)) ||
        (e->reserved_mask & (1u << view))) return PS5_MEM_STATE;
    e->reserved_mask |= (uint8_t)(1u << view);
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_view_reservation_released(Ps5MemState *state, uint64_t guest_view) {
    uint64_t base;
    unsigned view;
    Ps5MemExtent *e;
    Ps5MemResult r = view_location(guest_view, &base, &view);
    if (r != PS5_MEM_OK) return r;
    e = find_extent(state, base);
    if (!e) return PS5_MEM_NOT_FOUND;
    if (!(e->reserved_mask & (1u << view)) || (e->mapped_mask & (1u << view)))
        return PS5_MEM_STATE;
    e->reserved_mask &= (uint8_t)~(1u << view);
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_view_mapped(Ps5MemState *state, uint64_t guest_view) {
    uint64_t base;
    unsigned view;
    Ps5MemExtent *e;
    Ps5MemResult r = view_location(guest_view, &base, &view);
    if (r != PS5_MEM_OK) return r;
    e = find_extent(state, base);
    if (!e) return PS5_MEM_NOT_FOUND;
    if (e->pending_release || (e->mapped_mask & (1u << view)) ||
        !(e->reserved_mask & (1u << view))) return PS5_MEM_STATE;
    e->reserved_mask &= (uint8_t)~(1u << view);
    e->mapped_mask |= (uint8_t)(1u << view);
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_view_unmapped(Ps5MemState *state, uint64_t guest_view) {
    uint64_t base;
    unsigned view;
    Ps5MemExtent *e;
    Ps5MemResult r = view_location(guest_view, &base, &view);
    if (r != PS5_MEM_OK) return r;
    e = find_extent(state, base);
    if (!e) return PS5_MEM_NOT_FOUND;
    if (!(e->mapped_mask & (1u << view)) || (e->gpu_mask & (1u << view)) || e->import_refs[view])
        return PS5_MEM_BUSY;
    e->mapped_mask &= (uint8_t)~(1u << view);
    e->reserved_mask &= (uint8_t)~(1u << view);
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_view_gpu_granted(Ps5MemState *state, uint64_t guest_view) {
    uint64_t base;
    unsigned view;
    Ps5MemExtent *e;
    Ps5MemResult r = view_location(guest_view, &base, &view);
    if (r != PS5_MEM_OK) return r;
    e = find_extent(state, base);
    if (!e) return PS5_MEM_NOT_FOUND;
    if (e->pending_release || !(e->mapped_mask & (1u << view)) || (e->gpu_mask & (1u << view)))
        return PS5_MEM_STATE;
    e->gpu_mask |= (uint8_t)(1u << view);
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_view_gpu_revoked(Ps5MemState *state, uint64_t guest_view) {
    uint64_t base;
    unsigned view;
    Ps5MemExtent *e;
    Ps5MemResult r = view_location(guest_view, &base, &view);
    if (r != PS5_MEM_OK) return r;
    e = find_extent(state, base);
    if (!e) return PS5_MEM_NOT_FOUND;
    if (!(e->gpu_mask & (1u << view)) || e->import_refs[view]) return PS5_MEM_BUSY;
    e->gpu_mask &= (uint8_t)~(1u << view);
    return PS5_MEM_OK;
}


#define PS5_MEM_PAGE_SIZE UINT64_C(0x1000)
#define PS5_MEM_PAGES_PER_EXTENT 16u
#define PS5_MEM_PAGES_PER_VIEW 4u

static Ps5MemResult page_range_validate(uint64_t address, uint64_t bytes, uint64_t *end) {
    if (!bytes || (address & (PS5_MEM_PAGE_SIZE - 1)) ||
        (bytes & (PS5_MEM_PAGE_SIZE - 1))) return PS5_MEM_INVALID;
    if (bytes > UINT64_MAX - address) return PS5_MEM_OVERFLOW;
    *end = address + bytes;
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_pages_claim(Ps5MemState *state, uint64_t guest_address, uint64_t bytes) {
    uint64_t end, at;
    Ps5MemResult r = page_range_validate(guest_address, bytes, &end);
    if (r != PS5_MEM_OK) return r;
    if (!state || !state->extents) return PS5_MEM_INVALID;
    /* Validate every page before mutating any extent. */
    for (at = guest_address; at < end; at += PS5_MEM_PAGE_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned page = (unsigned)((at - base) / PS5_MEM_PAGE_SIZE);
        Ps5MemExtent *e = find_extent(state, base);
        if (!e) return PS5_MEM_NOT_FOUND;
        if (e->pending_release || (e->allocated_pages & (UINT16_C(1) << page)))
            return PS5_MEM_STATE;
        /* Do not change guest ownership beneath a live import of the containing view. */
        if (e->import_refs[page / PS5_MEM_PAGES_PER_VIEW]) return PS5_MEM_BUSY;
    }
    for (at = guest_address; at < end; at += PS5_MEM_PAGE_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned page = (unsigned)((at - base) / PS5_MEM_PAGE_SIZE);
        Ps5MemExtent *e = find_extent(state, base);
        const uint16_t bit = (uint16_t)(UINT16_C(1) << page);
        e->allocated_pages |= bit;
        e->zero_pending |= bit;
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_pages_free(Ps5MemState *state, uint64_t guest_address, uint64_t bytes) {
    uint64_t end, at;
    Ps5MemResult r = page_range_validate(guest_address, bytes, &end);
    if (r != PS5_MEM_OK) return r;
    if (!state || !state->extents) return PS5_MEM_INVALID;
    for (at = guest_address; at < end; at += PS5_MEM_PAGE_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned page = (unsigned)((at - base) / PS5_MEM_PAGE_SIZE);
        Ps5MemExtent *e = find_extent(state, base);
        unsigned view = page / PS5_MEM_PAGES_PER_VIEW;
        if (!e) return PS5_MEM_NOT_FOUND;
        if (!(e->allocated_pages & (UINT16_C(1) << page))) return PS5_MEM_STATE;
        /* A page cannot be freed while its containing 16 KiB view is imported. */
        if (e->import_refs[view]) return PS5_MEM_BUSY;
    }
    for (at = guest_address; at < end; at += PS5_MEM_PAGE_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned page = (unsigned)((at - base) / PS5_MEM_PAGE_SIZE);
        Ps5MemExtent *e = find_extent(state, base);
        const uint16_t bit = (uint16_t)(UINT16_C(1) << page);
        e->allocated_pages &= (uint16_t)~bit;
        e->zero_pending &= (uint16_t)~bit;
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_pages_mark_zeroed(Ps5MemState *state, uint64_t guest_address, uint64_t bytes) {
    uint64_t end, at;
    Ps5MemResult r = page_range_validate(guest_address, bytes, &end);
    if (r != PS5_MEM_OK) return r;
    if (!state || !state->extents) return PS5_MEM_INVALID;
    for (at = guest_address; at < end; at += PS5_MEM_PAGE_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned page = (unsigned)((at - base) / PS5_MEM_PAGE_SIZE);
        Ps5MemExtent *e = find_extent(state, base);
        if (!e) return PS5_MEM_NOT_FOUND;
        if (!(e->allocated_pages & (UINT16_C(1) << page)) ||
            !(e->zero_pending & (UINT16_C(1) << page))) return PS5_MEM_STATE;
        if (!(e->mapped_mask & (1u << (page / PS5_MEM_PAGES_PER_VIEW))))
            return PS5_MEM_STATE;
    }
    for (at = guest_address; at < end; at += PS5_MEM_PAGE_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned page = (unsigned)((at - base) / PS5_MEM_PAGE_SIZE);
        find_extent(state, base)->zero_pending &= (uint16_t)~(UINT16_C(1) << page);
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_pages_allocated_views(const Ps5MemState *state, uint64_t guest_extent,
                                           uint8_t *view_mask_out) {
    const Ps5MemExtent *e;
    unsigned view;
    uint8_t mask = 0;
    if (!view_mask_out || (guest_extent & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    e = find_extent_const(state, guest_extent);
    if (!e) return PS5_MEM_NOT_FOUND;
    for (view = 0; view < PS5_MEM_VIEWS_PER_EXTENT; ++view) {
        const uint16_t pages = (uint16_t)(UINT16_C(0xF) << (view * PS5_MEM_PAGES_PER_VIEW));
        if (e->allocated_pages & pages) mask |= (uint8_t)(1u << view);
    }
    *view_mask_out = mask;
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_pages_zero_pending(const Ps5MemState *state, uint64_t guest_extent,
                                        uint16_t *page_mask_out) {
    const Ps5MemExtent *e;
    if (!page_mask_out || (guest_extent & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    e = find_extent_const(state, guest_extent);
    if (!e) return PS5_MEM_NOT_FOUND;
    *page_mask_out = e->zero_pending;
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_import_acquire(Ps5MemState *state, uint64_t guest_address, uint64_t bytes) {
    uint64_t end, at;
    Ps5MemResult r = range_validate(guest_address, bytes, &end);
    if (r != PS5_MEM_OK) return r;

    /* Validate the entire range before changing any reference count. */
    for (at = guest_address; at < end; at += PS5_MEM_VIEW_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned view = (unsigned)((at - base) / PS5_MEM_VIEW_SIZE);
        Ps5MemExtent *e = find_extent(state, base);
        if (!e) return PS5_MEM_NOT_FOUND;
        if (e->pending_release || !(e->mapped_mask & (1u << view)) ||
            !(e->gpu_mask & (1u << view)) || e->import_refs[view] == UINT32_MAX)
            return PS5_MEM_STATE;
        /* The GPU must not import a view while any claimed subpage still needs zero-fill. */
        if (e->zero_pending & (uint16_t)(UINT16_C(0xF) << (view * PS5_MEM_PAGES_PER_VIEW)))
            return PS5_MEM_BUSY;
    }
    for (at = guest_address; at < end; at += PS5_MEM_VIEW_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned view = (unsigned)((at - base) / PS5_MEM_VIEW_SIZE);
        ++find_extent(state, base)->import_refs[view];
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_import_release(Ps5MemState *state, uint64_t guest_address, uint64_t bytes) {
    uint64_t end, at;
    Ps5MemResult r = range_validate(guest_address, bytes, &end);
    if (r != PS5_MEM_OK) return r;

    /* Validate first, so a bad/duplicate release cannot partially drop the hold. */
    for (at = guest_address; at < end; at += PS5_MEM_VIEW_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned view = (unsigned)((at - base) / PS5_MEM_VIEW_SIZE);
        Ps5MemExtent *e = find_extent(state, base);
        if (!e) return PS5_MEM_NOT_FOUND;
        if (!e->import_refs[view]) return PS5_MEM_STATE;
    }
    for (at = guest_address; at < end; at += PS5_MEM_VIEW_SIZE) {
        uint64_t base = at & ~(PS5_MEM_EXTENT_SIZE - 1);
        unsigned view = (unsigned)((at - base) / PS5_MEM_VIEW_SIZE);
        --find_extent(state, base)->import_refs[view];
    }
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_extent_retire(Ps5MemState *state, uint64_t guest_base) {
    Ps5MemExtent *e;
    if (guest_base & (PS5_MEM_EXTENT_SIZE - 1)) return PS5_MEM_INVALID;
    e = find_extent(state, guest_base);
    if (!e) return PS5_MEM_NOT_FOUND;
    e->pending_release = 1;
    return PS5_MEM_OK;
}

int ps5_mem_extent_release_ready(const Ps5MemState *state, uint64_t guest_base) {
    const Ps5MemExtent *e = find_extent_const(state, guest_base);
    unsigned i;
    if (!e || !e->pending_release || e->reserved_mask || e->mapped_mask || e->gpu_mask ||
        e->allocated_pages || e->zero_pending) return 0;
    for (i = 0; i < PS5_MEM_VIEWS_PER_EXTENT; ++i)
        if (e->import_refs[i]) return 0;
    return 1;
}

Ps5MemResult ps5_mem_extent_get_handle(const Ps5MemState *state, uint64_t guest_base,
                                       uint64_t *backing_handle_out) {
    const Ps5MemExtent *e;
    if (!backing_handle_out || (guest_base & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    e = find_extent_const(state, guest_base);
    if (!e) return PS5_MEM_NOT_FOUND;
    *backing_handle_out = e->backing_handle;
    return PS5_MEM_OK;
}

Ps5MemResult ps5_mem_extent_forget(Ps5MemState *state, uint64_t guest_base, uint64_t *backing_handle_out) {
    Ps5MemExtent *e;
    if (!state || !backing_handle_out || (guest_base & (PS5_MEM_EXTENT_SIZE - 1)))
        return PS5_MEM_INVALID;
    e = find_extent(state, guest_base);
    if (!e) return PS5_MEM_NOT_FOUND;
    if (!ps5_mem_extent_release_ready(state, guest_base)) return PS5_MEM_BUSY;
    *backing_handle_out = e->backing_handle;
    memset(e, 0, sizeof(*e));
    --state->count;
    return PS5_MEM_OK;
}
