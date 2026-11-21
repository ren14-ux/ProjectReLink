#ifndef REPLINK_PS5_MEMORY_H
#define REPLINK_PS5_MEMORY_H

#include <stdint.h>

#include "guest_window.h"
#include "memory_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize after the complete guest window has been reserved. */
int ps5_memory_init(const Ps5GuestWindow *window);

/* Guest addresses passed here are offsets from A64_MEM, as used by HOS. */
int ps5_memory_commit_pages(uint64_t guest_offset, uint64_t bytes);
int ps5_memory_free_pages(uint64_t guest_offset, uint64_t bytes);

/* Vulkan imports hold mapped guest pages until the renderer's timeline retires them. */
int ps5_memory_import_acquire(uint64_t guest_offset, uint64_t bytes);
int ps5_memory_import_release(uint64_t guest_offset, uint64_t bytes);

/* Call only after renderer imports have been retired and released. */
int ps5_memory_shutdown(void);

/* PS5 implementation of the HOS commit hook. */
void hos_commit(uint64_t guest_offset, uint64_t bytes);

#ifdef __cplusplus
}
#endif

#endif
