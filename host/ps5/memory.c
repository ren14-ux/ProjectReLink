#include "memory.h"

#include "memory_backend.h"
#include "sdk_extent_pool.h"
#include "../hos.h"

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PS5_MEMORY_INITIAL_EXTENTS 512u
#define PS5_MEMORY_MAX_EXTENTS 262144u

typedef struct Ps5MemoryService {
    Ps5MemState state;
    Ps5SdkExtentPool pool;
    Ps5MemExtent *extents;
    Ps5SdkExtentSlot *slots;
    size_t capacity;
    uintptr_t guest_base;
    size_t guest_bytes;
    int initialized;
} Ps5MemoryService;

static Ps5MemoryService service;
static pthread_mutex_t memory_mutex = PTHREAD_MUTEX_INITIALIZER;

static int normalize_range(uint64_t offset, uint64_t bytes, uint64_t *begin_out, uint64_t *end_out) {
    uint64_t end;
    if (!service.initialized || !begin_out || !end_out || !bytes ||
        offset > service.guest_bytes || bytes > service.guest_bytes - offset)
        return 0;
    end = offset + bytes;
    if (end > UINT64_MAX - (PS5_MEM_PAGE_SIZE - 1)) return 0;
    *begin_out = offset & ~(PS5_MEM_PAGE_SIZE - 1);
    *end_out = (end + PS5_MEM_PAGE_SIZE - 1) & ~(PS5_MEM_PAGE_SIZE - 1);
    return *end_out <= service.guest_bytes && *end_out > *begin_out;
}

static uint64_t absolute_address(uint64_t offset) {
    return (uint64_t)service.guest_base + offset;
}

static int grow_storage(void) {
    size_t next;
    Ps5MemExtent *extents;
    Ps5SdkExtentSlot *slots;
    if (service.capacity >= PS5_MEMORY_MAX_EXTENTS) return 0;
    next = service.capacity > PS5_MEMORY_MAX_EXTENTS / 2
               ? PS5_MEMORY_MAX_EXTENTS : service.capacity * 2;
    if (next <= service.capacity || next > SIZE_MAX / sizeof(*extents) ||
        next > SIZE_MAX / sizeof(*slots)) return 0;
    extents = (Ps5MemExtent *)calloc(next, sizeof(*extents));
    slots = (Ps5SdkExtentSlot *)calloc(next, sizeof(*slots));
    if (!extents || !slots) {
        free(extents);
        free(slots);
        return 0;
    }
    if (service.state.count)
        memcpy(extents, service.extents, service.state.count * sizeof(*extents));
    if (service.pool.count)
        memcpy(slots, service.slots, service.pool.capacity * sizeof(*slots));
    free(service.extents);
    free(service.slots);
    service.extents = extents;
    service.slots = slots;
    service.capacity = next;
    service.state.extents = extents;
    service.state.capacity = next;
    service.pool.slots = slots;
    service.pool.capacity = next;
    return 1;
}

static Ps5MemResult ensure_extent(uint64_t guest_base) {
    uint64_t handle;
    Ps5MemResult r = ps5_mem_extent_get_handle(&service.state, guest_base, &handle);
    if (r == PS5_MEM_OK) return PS5_MEM_OK;
    if (r != PS5_MEM_NOT_FOUND) return r;
    if (service.state.count == service.capacity || service.pool.count == service.capacity) {
        if (!grow_storage()) return PS5_MEM_FULL;
    }
    return ps5_mem_backend_allocate_extent(&service.state, &service.pool, guest_base, &handle);
}

static Ps5MemResult map_allocated_views(uint64_t guest_extent) {
    uint8_t allocated_views = 0, reserved = 0, mapped = 0, gpu = 0;
    Ps5MemResult r = ps5_mem_pages_allocated_views(&service.state, guest_extent, &allocated_views);
    unsigned view;
    if (r != PS5_MEM_OK) return r;
    r = ps5_mem_extent_view_state(&service.state, guest_extent, &reserved, &mapped, &gpu, imports);
    if (r != PS5_MEM_OK) return r;

    for (view = 0; view < PS5_MEM_VIEWS_PER_EXTENT; ++view) {
        const uint8_t bit = (uint8_t)(1u << view);
        const uint64_t address = guest_extent + (uint64_t)view * PS5_MEM_VIEW_SIZE;
        void *mapped_view = NULL;
        if (!(allocated_views & bit)) continue;
        if (!(mapped & bit)) {
            r = ps5_mem_backend_ensure_view_reserved(&service.state, &service.pool,
                                                      guest_extent, view);
            if (r != PS5_MEM_OK) return r;
            r = ps5_mem_backend_map_view(&service.state, &service.pool, guest_extent, view,
                                         (void *)(uintptr_t)address,
                                         PS5_SHM_READ | PS5_SHM_WRITE, &mapped_view);
            if (r != PS5_MEM_OK) return r;
            mapped |= bit;
        }
        if (!(gpu & bit)) {
            r = ps5_mem_backend_grant_gpu_view(&service.state, &service.pool, guest_extent, view,
                                               PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE);
            if (r != PS5_MEM_OK) return r;
            gpu |= bit;
        }
    }

    {
        uint16_t pending = 0;
        unsigned page;
        r = ps5_mem_pages_zero_pending(&service.state, guest_extent, &pending);
        if (r != PS5_MEM_OK) return r;
        for (page = 0; page < PS5_MEM_PAGES_PER_EXTENT; ++page) {
            const uint16_t bit = (uint16_t)(UINT16_C(1) << page);
            const uint64_t address = guest_extent + (uint64_t)page * PS5_MEM_PAGE_SIZE;
            if (!(pending & bit)) continue;
            memset((void *)(uintptr_t)address, 0, PS5_MEM_PAGE_SIZE);
            r = ps5_mem_backend_mark_pages_zeroed(&service.state, address, PS5_MEM_PAGE_SIZE);
            if (r != PS5_MEM_OK) return r;
        }
    }
    return PS5_MEM_OK;
}

static Ps5MemResult commit_locked(uint64_t offset, uint64_t bytes) {
    uint64_t begin, end, extent_offset;
    Ps5MemResult r;
    if (!normalize_range(offset, bytes, &begin, &end)) return PS5_MEM_INVALID;

    for (extent_offset = begin & ~(PS5_MEM_EXTENT_SIZE - 1);
         extent_offset < end; extent_offset += PS5_MEM_EXTENT_SIZE) {
        r = ensure_extent(absolute_address(extent_offset));
        if (r != PS5_MEM_OK) return r;
    }

    r = ps5_mem_pages_claim_missing(&service.state, absolute_address(begin), end - begin);
    if (r != PS5_MEM_OK) return r;

    for (extent_offset = begin & ~(PS5_MEM_EXTENT_SIZE - 1);
         extent_offset < end; extent_offset += PS5_MEM_EXTENT_SIZE) {
        r = map_allocated_views(absolute_address(extent_offset));
        if (r != PS5_MEM_OK) return r;
    }
    return PS5_MEM_OK;
}

static Ps5MemResult reap_extent(uint64_t guest_extent) {
    uint16_t pending_free = 0, allocated = 0;
    uint8_t allocated_views = 0, reserved = 0, mapped = 0, gpu = 0;
    uint32_t imports[PS5_MEM_VIEWS_PER_EXTENT] = {0};
    Ps5MemResult r;
    unsigned page, view;

    r = ps5_mem_pages_pending_free(&service.state, guest_extent, &pending_free);
    if (r != PS5_MEM_OK) return r;
    r = ps5_mem_extent_view_state(&service.state, guest_extent, &reserved, &mapped, &gpu, imports);
    if (r != PS5_MEM_OK) return r;

    for (page = 0; page < PS5_MEM_PAGES_PER_EXTENT; ++page) {
        const uint16_t bit = (uint16_t)(UINT16_C(1) << page);
        const unsigned view_index = page / PS5_MEM_PAGES_PER_VIEW;
        const uint64_t address = guest_extent + (uint64_t)page * PS5_MEM_PAGE_SIZE;
        if (!(pending_free & bit) || imports[view_index]) continue;
        if (mapped & (1u << view_index))
            memset((void *)(uintptr_t)address, 0, PS5_MEM_PAGE_SIZE);
        r = ps5_mem_backend_free_pages(&service.state, address, PS5_MEM_PAGE_SIZE);
        if (r != PS5_MEM_OK) return r;
    }

    r = ps5_mem_pages_allocated_mask(&service.state, guest_extent, &allocated);
    if (r != PS5_MEM_OK) return r;
    r = ps5_mem_pages_allocated_views(&service.state, guest_extent, &allocated_views);
    if (r != PS5_MEM_OK) return r;
    r = ps5_mem_extent_view_state(&service.state, guest_extent, &reserved, &mapped, &gpu, imports);
    if (r != PS5_MEM_OK) return r;

    for (view = 0; view < PS5_MEM_VIEWS_PER_EXTENT; ++view) {
        const uint8_t bit = (uint8_t)(1u << view);
        if ((allocated_views & bit) || imports[view]) continue;
        if (gpu & bit) {
            r = ps5_mem_backend_revoke_gpu_view(&service.state, &service.pool, guest_extent, view);
            if (r != PS5_MEM_OK) return r;
            gpu &= (uint8_t)~bit;
        }
        if (mapped & bit) {
            r = ps5_mem_backend_unmap_view(&service.state, &service.pool, guest_extent, view);
            if (r == PS5_MEM_BUSY) {
                r = ps5_mem_backend_restore_view_reservation(&service.state, &service.pool,
                                                               guest_extent, view);
            }
            if (r != PS5_MEM_OK) return r;
            mapped &= (uint8_t)~bit;
            reserved |= bit;
        }
        if (!(reserved & bit)) {
            r = ps5_mem_backend_ensure_view_reserved(&service.state, &service.pool, guest_extent, view);
            if (r != PS5_MEM_OK) return r;
            reserved |= bit;
        }
        r = ps5_mem_backend_release_view_reservation(&service.state, &service.pool, guest_extent, view);
        if (r != PS5_MEM_OK) return r;
        reserved &= (uint8_t)~bit;
    }

    r = ps5_mem_pages_allocated_mask(&service.state, guest_extent, &allocated);
    if (r != PS5_MEM_OK) return r;
    r = ps5_mem_extent_view_state(&service.state, guest_extent, &reserved, &mapped, &gpu, imports);
    if (r != PS5_MEM_OK) return r;
    if (!allocated && !reserved && !mapped && !gpu) {
        for (view = 0; view < PS5_MEM_VIEWS_PER_EXTENT; ++view)
            if (imports[view]) return PS5_MEM_BUSY;
        r = ps5_mem_backend_retire_extent(&service.state, guest_extent);
        if (r != PS5_MEM_OK) return r;
        return ps5_mem_backend_release_extent(&service.state, &service.pool, guest_extent);
    }
    return PS5_MEM_OK;
}

static Ps5MemResult free_locked(uint64_t offset, uint64_t bytes) {
    uint64_t begin, end, page_offset, extent_offset;
    Ps5MemResult r;
    if (!normalize_range(offset, bytes, &begin, &end)) return PS5_MEM_INVALID;

    for (page_offset = begin; page_offset < end; page_offset += PS5_MEM_PAGE_SIZE) {
        const uint64_t guest_address = absolute_address(page_offset);
        const uint64_t guest_extent = guest_address & ~(PS5_MEM_EXTENT_SIZE - 1);
        const unsigned page = (unsigned)((guest_address - guest_extent) / PS5_MEM_PAGE_SIZE);
        uint16_t allocated = 0;
        r = ps5_mem_pages_allocated_mask(&service.state, guest_extent, &allocated);
        if (r == PS5_MEM_NOT_FOUND) continue;
        if (r != PS5_MEM_OK) return r;
        if (!(allocated & (UINT16_C(1) << page))) continue;
        r = ps5_mem_backend_mark_pending_free(&service.state, guest_address, PS5_MEM_PAGE_SIZE);
        if (r != PS5_MEM_OK) return r;
    }

    for (extent_offset = begin & ~(PS5_MEM_EXTENT_SIZE - 1);
         extent_offset < end; extent_offset += PS5_MEM_EXTENT_SIZE) {
        const uint64_t guest_extent = absolute_address(extent_offset);
        r = reap_extent(guest_extent);
        if (r == PS5_MEM_NOT_FOUND) continue;
        if (r != PS5_MEM_OK) return r;
    }
    return PS5_MEM_OK;
}

int ps5_memory_init(const Ps5GuestWindow *window) {
    Ps5MemResult mr;
    Ps5SdkExtentResult sr;
    if (!window || !window->reserved || !window->address || !window->bytes ||
        ((uintptr_t)window->address & (PS5_MEM_EXTENT_SIZE - 1)) ||
        (window->bytes & (PS5_MEM_EXTENT_SIZE - 1)))
        return -1;

    pthread_mutex_lock(&memory_mutex);
    if (service.initialized) {
        pthread_mutex_unlock(&memory_mutex);
        return -1;
    }
    service.capacity = PS5_MEMORY_INITIAL_EXTENTS;
    service.extents = (Ps5MemExtent *)calloc(service.capacity, sizeof(*service.extents));
    service.slots = (Ps5SdkExtentSlot *)calloc(service.capacity, sizeof(*service.slots));
    if (!service.extents || !service.slots) goto fail;

    mr = ps5_mem_state_init(&service.state, service.extents, service.capacity);
    if (mr != PS5_MEM_OK) goto fail;
    sr = ps5_sdk_extent_pool_init(&service.pool, service.slots, service.capacity);
    if (sr != PS5_SDK_EXTENT_OK) goto fail;
    sr = ps5_sdk_extent_pool_bind_window(&service.pool, (uintptr_t)window->address, window->bytes);
    if (sr != PS5_SDK_EXTENT_OK) goto fail;

    service.guest_base = (uintptr_t)window->address;
    service.guest_bytes = window->bytes;
    service.initialized = 1;
    pthread_mutex_unlock(&memory_mutex);
    return 0;

fail:
    free(service.extents);
    free(service.slots);
    memset(&service, 0, sizeof(service));
    pthread_mutex_unlock(&memory_mutex);
    return -1;
}

int ps5_memory_commit_pages(uint64_t guest_offset, uint64_t bytes) {
    Ps5MemResult r;
    pthread_mutex_lock(&memory_mutex);
    r = commit_locked(guest_offset, bytes);
    pthread_mutex_unlock(&memory_mutex);
    return r == PS5_MEM_OK ? 0 : -1;
}

int ps5_memory_free_pages(uint64_t guest_offset, uint64_t bytes) {
    Ps5MemResult r;
    pthread_mutex_lock(&memory_mutex);
    r = free_locked(guest_offset, bytes);
    pthread_mutex_unlock(&memory_mutex);
    return r == PS5_MEM_OK ? 0 : -1;
}

int ps5_memory_import_acquire(uint64_t guest_offset, uint64_t bytes) {
    uint64_t begin, end;
    Ps5MemResult r;
    pthread_mutex_lock(&memory_mutex);
    if (!normalize_range(guest_offset, bytes, &begin, &end) ||
        (begin & (PS5_MEM_VIEW_SIZE - 1)) || ((end - begin) & (PS5_MEM_VIEW_SIZE - 1))) {
        pthread_mutex_unlock(&memory_mutex);
        return -1;
    }
    r = commit_locked(begin, end - begin);
    if (r == PS5_MEM_OK)
        r = ps5_mem_backend_import_acquire(&service.state, absolute_address(begin), end - begin);
    pthread_mutex_unlock(&memory_mutex);
    return r == PS5_MEM_OK ? 0 : -1;
}

int ps5_memory_import_release(uint64_t guest_offset, uint64_t bytes) {
    uint64_t begin, end, extent_offset;
    Ps5MemResult r;
    pthread_mutex_lock(&memory_mutex);
    if (!normalize_range(guest_offset, bytes, &begin, &end) ||
        (begin & (PS5_MEM_VIEW_SIZE - 1)) || ((end - begin) & (PS5_MEM_VIEW_SIZE - 1))) {
        pthread_mutex_unlock(&memory_mutex);
        return -1;
    }
    r = ps5_mem_backend_import_release(&service.state, absolute_address(begin), end - begin);
    if (r == PS5_MEM_OK) {
        for (extent_offset = begin & ~(PS5_MEM_EXTENT_SIZE - 1);
             extent_offset < end; extent_offset += PS5_MEM_EXTENT_SIZE) {
            r = reap_extent(absolute_address(extent_offset));
            if (r != PS5_MEM_OK && r != PS5_MEM_NOT_FOUND) break;
        }
    }
    pthread_mutex_unlock(&memory_mutex);
    return r == PS5_MEM_OK || r == PS5_MEM_NOT_FOUND ? 0 : -1;
}

int ps5_memory_shutdown(void) {
    Ps5MemResult r = PS5_MEM_OK;
    pthread_mutex_lock(&memory_mutex);
    if (!service.initialized) {
        pthread_mutex_unlock(&memory_mutex);
        return 0;
    }
    while (service.state.count) {
        const size_t count_before = service.state.count;
        const uint64_t guest_extent = service.state.extents[0].guest_base;
        uint16_t allocated = 0;
        unsigned page;
        r = ps5_mem_pages_allocated_mask(&service.state, guest_extent, &allocated);
        if (r != PS5_MEM_OK) break;
        for (page = 0; page < PS5_MEM_PAGES_PER_EXTENT; ++page) {
            if (allocated & (UINT16_C(1) << page)) {
                const uint64_t address = guest_extent + (uint64_t)page * PS5_MEM_PAGE_SIZE;
                r = ps5_mem_backend_mark_pending_free(&service.state, address, PS5_MEM_PAGE_SIZE);
                if (r != PS5_MEM_OK) break;
            }
        }
        if (r != PS5_MEM_OK) break;
        r = reap_extent(guest_extent);
        if (r != PS5_MEM_OK) break;
        if (service.state.count == count_before) {
            r = PS5_MEM_BUSY; /* outstanding import or failed kernel transition: leave state intact */
            break;
        }
    }
    if (r != PS5_MEM_OK || service.state.count) {
        pthread_mutex_unlock(&memory_mutex);
        return -1;
    }
    free(service.extents);
    free(service.slots);
    memset(&service, 0, sizeof(service));
    pthread_mutex_unlock(&memory_mutex);
    return 0;
}

void hos_commit(uint64_t guest_offset, uint64_t bytes) {
    if (!bytes) return;
    if (ps5_memory_commit_pages(guest_offset, bytes) != 0)
        hos_fatal(NULL, "PS5 guest memory commit failed at %#llx (%#llx bytes)",
                  (unsigned long long)guest_offset, (unsigned long long)bytes);
}
