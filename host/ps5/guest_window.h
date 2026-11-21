#ifndef RELINK_PS5_GUEST_WINDOW_H
#define RELINK_PS5_GUEST_WINDOW_H

/*
 * Exact ownership of the guest virtual-address window.
 *
 * The caller supplies a console-validated base and size. This module never
 * selects a guessed address and never treats a relocated reservation as
 * success. Calls must be externally serialized.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ps5GuestWindow {
    void *address;
    size_t bytes;
    int reserved;
} Ps5GuestWindow;

typedef enum Ps5GuestWindowResult {
    PS5_GUEST_WINDOW_OK = 0,
    PS5_GUEST_WINDOW_INVALID = -1,
    PS5_GUEST_WINDOW_KERNEL = -2,
    PS5_GUEST_WINDOW_ADDRESS_MISMATCH = -3,
    PS5_GUEST_WINDOW_CLEANUP_REQUIRED = -4,
    PS5_GUEST_WINDOW_NOT_RESERVED = -5
} Ps5GuestWindowResult;

/* Reserve exactly [base, base + bytes). base/bytes must satisfy SDK alignment. */
Ps5GuestWindowResult ps5_guest_window_reserve(Ps5GuestWindow *window,
                                               uintptr_t base, size_t bytes);
/* Release the exact reservation. On kernel failure ownership remains tracked. */
Ps5GuestWindowResult ps5_guest_window_release(Ps5GuestWindow *window);

#ifdef __cplusplus
}
#endif
#endif
