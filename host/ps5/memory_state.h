#ifndef RELINK_PS5_MEMORY_STATE_H
#define RELINK_PS5_MEMORY_STATE_H

/*
 * Platform-independent bookkeeping for the PS5 guest-memory backend.
 *
 * This module deliberately performs no kernel calls and owns no allocation.
 * The caller supplies a fixed metadata array and performs direct-memory,
 * mapping, protection, and GPU-timeline operations around these transitions.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5_MEM_EXTENT_SIZE UINT64_C(0x10000)
#define PS5_MEM_VIEW_SIZE   UINT64_C(0x4000)
#define PS5_MEM_VIEWS_PER_EXTENT 4u
#define PS5_MEM_NO_HANDLE UINT64_MAX

typedef struct Ps5MemExtent {
    uint64_t guest_base;       /* 64 KiB aligned absolute guest virtual address */
    uint64_t backing_handle;   /* opaque SDK direct-memory handle */
    uint32_t import_refs[PS5_MEM_VIEWS_PER_EXTENT];
    uint8_t reserved_mask;     /* address-space views reserved and available to map */
    uint8_t mapped_mask;       /* CPU virtual views currently mapped */
    uint8_t gpu_mask;          /* views currently granted GPU access */
    uint8_t pending_release;   /* reject new imports; release when quiescent */
    uint16_t allocated_pages;  /* sixteen 4 KiB logical guest pages in this extent */
    uint16_t zero_pending;     /* newly allocated/recycled pages requiring zero-fill */
    uint8_t occupied;
} Ps5MemExtent;

typedef struct Ps5MemState {
    Ps5MemExtent *extents;
    size_t capacity;
    size_t count;
} Ps5MemState;

typedef enum Ps5MemResult {
    PS5_MEM_OK = 0,
    PS5_MEM_INVALID = -1,
    PS5_MEM_FULL = -2,
    PS5_MEM_NOT_FOUND = -3,
    PS5_MEM_STATE = -4,
    PS5_MEM_BUSY = -5,
    PS5_MEM_OVERFLOW = -6
} Ps5MemResult;

/*
 * Storage must remain alive for the lifetime of the state object. All calls
 * for a state object must be externally serialized by the backend's memory
 * lock; this module deliberately contains no platform lock.
 */
Ps5MemResult ps5_mem_state_init(Ps5MemState *state, Ps5MemExtent *storage, size_t capacity);

/* Register a successfully allocated 64 KiB physical extent. */
Ps5MemResult ps5_mem_extent_add(Ps5MemState *state, uint64_t guest_base, uint64_t backing_handle);

/* Reservation is recorded after reserve succeeds; a fixed map consumes it. */
Ps5MemResult ps5_mem_view_reserved(Ps5MemState *state, uint64_t guest_view);
Ps5MemResult ps5_mem_view_reservation_released(Ps5MemState *state, uint64_t guest_view);
/* Mapping/protection transitions are recorded only after the corresponding SDK call succeeds. */
Ps5MemResult ps5_mem_view_mapped(Ps5MemState *state, uint64_t guest_view);
/* Successful unmap clears mapped state; reserve again separately if the SDK restored a hole. */
Ps5MemResult ps5_mem_view_unmapped(Ps5MemState *state, uint64_t guest_view);
Ps5MemResult ps5_mem_view_gpu_granted(Ps5MemState *state, uint64_t guest_view);
Ps5MemResult ps5_mem_view_gpu_revoked(Ps5MemState *state, uint64_t guest_view);

/*
 * Logical game allocations are tracked at 4 KiB granularity, independently of
 * the 16 KiB virtual mapping views and 64 KiB physical backing extents.
 * All ranges are 4 KiB aligned. Claim/free validate the whole range before
 * changing any bit. A newly claimed page is returned as zero-pending until
 * the caller has explicitly zero-filled it and calls mark_zeroed.
 */
Ps5MemResult ps5_mem_pages_claim(Ps5MemState *state, uint64_t guest_address, uint64_t bytes);
/* Idempotently claim only unowned pages; existing ownership is preserved. */
Ps5MemResult ps5_mem_pages_claim_missing(Ps5MemState *state, uint64_t guest_address, uint64_t bytes);
Ps5MemResult ps5_mem_pages_free(Ps5MemState *state, uint64_t guest_address, uint64_t bytes);
Ps5MemResult ps5_mem_pages_mark_zeroed(Ps5MemState *state, uint64_t guest_address, uint64_t bytes);
/* Return the four-bit 16 KiB view mask containing at least one allocated page. */
Ps5MemResult ps5_mem_pages_allocated_views(const Ps5MemState *state, uint64_t guest_extent,
                                           uint8_t *view_mask_out);
/* Return the sixteen-bit mask of pages that still require zero-fill. */
Ps5MemResult ps5_mem_pages_zero_pending(const Ps5MemState *state, uint64_t guest_extent,
                                        uint16_t *page_mask_out);
Ps5MemResult ps5_mem_pages_allocated_mask(const Ps5MemState *state, uint64_t guest_extent,
                                          uint16_t *page_mask_out);

/* Import ranges are 16 KiB aligned and must be fully mapped and GPU-granted. */
Ps5MemResult ps5_mem_import_acquire(Ps5MemState *state, uint64_t guest_address, uint64_t bytes);
Ps5MemResult ps5_mem_import_release(Ps5MemState *state, uint64_t guest_address, uint64_t bytes);

/* Mark an extent for release. New imports are refused after this succeeds. */
Ps5MemResult ps5_mem_extent_retire(Ps5MemState *state, uint64_t guest_base);

/* True only when the SDK backing may be released: no mappings, grants, or imports remain. */
int ps5_mem_extent_release_ready(const Ps5MemState *state, uint64_t guest_base);
/* Snapshot per-view ownership; any output may be NULL. */
Ps5MemResult ps5_mem_extent_view_state(const Ps5MemState *state, uint64_t guest_base,
                                       uint8_t *reserved_mask, uint8_t *mapped_mask,
                                       uint8_t *gpu_mask, uint32_t import_refs[PS5_MEM_VIEWS_PER_EXTENT]);

/* Read the opaque handle while the extent is still registered. */
Ps5MemResult ps5_mem_extent_get_handle(const Ps5MemState *state, uint64_t guest_base,
                                       uint64_t *backing_handle_out);

/* Remove metadata only after the caller successfully releases the SDK backing handle. */
Ps5MemResult ps5_mem_extent_forget(Ps5MemState *state, uint64_t guest_base, uint64_t *backing_handle_out);

#ifdef __cplusplus
}
#endif
#endif
