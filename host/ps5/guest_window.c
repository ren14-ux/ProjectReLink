#include "guest_window.h"

#include "ps5platform/kernel.h"

#include <string.h>

Ps5GuestWindowResult ps5_guest_window_reserve(Ps5GuestWindow *window,
                                               uintptr_t base, size_t bytes) {
    void *actual;
    int32_t result;

    if (!window || !base || !bytes || window->reserved ||
        (base % PS5_KERNEL_DIRECT_ALIGNMENT) != 0 ||
        (bytes % PS5_KERNEL_DIRECT_ALIGNMENT) != 0 ||
        base > UINTPTR_MAX - (uintptr_t)bytes)
        return PS5_GUEST_WINDOW_INVALID;
    actual = (void *)base;
    result = sceKernelReserveVirtualRange(&actual, bytes, 0,
                                          PS5_KERNEL_DIRECT_ALIGNMENT);
    if (result != 0)
        return PS5_GUEST_WINDOW_KERNEL;

    if ((uintptr_t)actual != base) {
        /*
         * Preserve ownership if cleanup fails. The caller can retry release;
         * silently dropping this reservation would leak an unknown VA range.
         */
        window->address = actual;
        window->bytes = bytes;
        window->reserved = 1;
        if (sceKernelMunmap(actual, bytes) != 0)
            return PS5_GUEST_WINDOW_CLEANUP_REQUIRED;
        memset(window, 0, sizeof(*window));
        return PS5_GUEST_WINDOW_ADDRESS_MISMATCH;
    }

    window->address = actual;
    window->bytes = bytes;
    window->reserved = 1;
    return PS5_GUEST_WINDOW_OK;
}

Ps5GuestWindowResult ps5_guest_window_release(Ps5GuestWindow *window) {
    if (!window || !window->reserved || !window->address || !window->bytes)
        return PS5_GUEST_WINDOW_NOT_RESERVED;

    if (sceKernelMunmap(window->address, window->bytes) != 0)
        return PS5_GUEST_WINDOW_KERNEL;

    memset(window, 0, sizeof(*window));
    return PS5_GUEST_WINDOW_OK;
}
