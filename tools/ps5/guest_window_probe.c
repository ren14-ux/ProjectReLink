/*
 * Project ReLink - PS5 guest-window reservation probe.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Call ps5_probe_guest_window() from a PS5 title entry point after the SDK
 * platform layer is initialized. It reserves virtual address space only:
 * it does not allocate direct memory or map/modify guest pages.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "../../host/ps5/guest_window.h"

#define GIB UINT64_C(0x40000000)
#define BASE UINT64_C(0x1000000000) /* 64 GiB candidate, not a production default */
#define SEGMENT_BYTES (UINT64_C(16) * GIB)
#define SEGMENT_COUNT_512 (32u)
#define SEGMENT_COUNT_448 (28u)

static int
reserve_exact(uintptr_t hint, size_t bytes, Ps5GuestWindow *window)
{
   const Ps5GuestWindowResult result =
      ps5_guest_window_reserve(window, hint, bytes);
   if (result == PS5_GUEST_WINDOW_OK)
      return 0;
   if (result == PS5_GUEST_WINDOW_CLEANUP_REQUIRED) {
      printf("guest-window: relocated reservation cleanup pending address=%p bytes=0x%zx\n",
             window->address, window->bytes);
      return -2;
   }
   printf("guest-window: reserve hint=0x%" PRIxPTR " bytes=0x%zx result=%d\n",
          hint, bytes, (int)result);
   return -1;
}

static int
probe_one_range(size_t bytes)
{
   Ps5GuestWindow window = {0};
   const int result = reserve_exact((uintptr_t)BASE, bytes, &window);
   if (result != 0)
      return result;
   printf("guest-window: contiguous reservation PASS base=%p bytes=0x%zx end=0x%" PRIxPTR "\n",
          window.address, bytes, (uintptr_t)window.address + bytes);
   const Ps5GuestWindowResult release = ps5_guest_window_release(&window);
   printf("guest-window: contiguous release result=%d\n", (int)release);
   return release == PS5_GUEST_WINDOW_OK ? 0 : -2;
}

static int
probe_segments(unsigned count)
{
   Ps5GuestWindow ranges[SEGMENT_COUNT_512] = {{0}};
   unsigned reserved = 0;
   int result = 0;

   for (unsigned i = 0; i < count; ++i) {
      const uintptr_t hint = (uintptr_t)(BASE + (uint64_t)i * SEGMENT_BYTES);
      result = reserve_exact(hint, (size_t)SEGMENT_BYTES, &ranges[i]);
      if (result != 0)
         break;
      ++reserved;
   }

   if (result == 0) {
      const uintptr_t expected_end = (uintptr_t)(BASE + (uint64_t)count * SEGMENT_BYTES);
      printf("guest-window: segmented reservation PASS segments=%u base=0x%" PRIxPTR
             " end=0x%" PRIxPTR " bytes=0x%" PRIx64 "\n",
             count, (uintptr_t)ranges[0].address, expected_end, (uint64_t)count * SEGMENT_BYTES);
   } else {
      printf("guest-window: segmented reservation FAIL requested=%u reserved=%u\n",
             count, reserved);
   }

   int cleanup_failed = 0;
   while (reserved) {
      --reserved;
      const Ps5GuestWindowResult release = ps5_guest_window_release(&ranges[reserved]);
      if (release != PS5_GUEST_WINDOW_OK) {
         printf("guest-window: cleanup segment=%u FAILED result=%d\n",
                reserved, (int)release);
         cleanup_failed = 1;
      }
   }
   return cleanup_failed ? -2 : result;
}

/*
 * Returns 0 if either a single contiguous reservation or a set of adjacent
 * 16 GiB reservations succeeded for the 512 GiB or 448 GiB guest window.
 * Returns -1 if all layouts fail, or -2 if cleanup fails (stop immediately).
 * A successful result always releases the test reservation before returning.
 */
int
ps5_probe_guest_window(void)
{
   printf("guest-window: begin base=0x%" PRIx64 " segment=0x%" PRIx64 "\n",
          BASE, SEGMENT_BYTES);

   int result = probe_one_range((size_t)(SEGMENT_COUNT_512 * SEGMENT_BYTES));
   if (result == 0)
      return 0;
   if (result == -2)
      return -2;
   result = probe_segments(SEGMENT_COUNT_512);
   if (result == 0)
      return 0;
   if (result == -2)
      return -2;

   printf("guest-window: retrying reduced 448 GiB window\n");
   result = probe_one_range((size_t)(SEGMENT_COUNT_448 * SEGMENT_BYTES));
   if (result == 0)
      return 0;
   if (result == -2)
      return -2;
   result = probe_segments(SEGMENT_COUNT_448);
   if (result == 0)
      return 0;
   if (result == -2)
      return -2;

   printf("guest-window: no tested contiguous guest window could be reserved\n");
   return -1;
}
