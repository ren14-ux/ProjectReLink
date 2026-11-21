/*
 * Project ReLink - PS5 guest-window reservation diagnostic.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>

int ps5_probe_guest_window(void);

int
main(void)
{
   const int result = ps5_probe_guest_window();
   printf("guest-window: diagnostic %s (result=%d)\n",
          result == 0 ? "PASS" : "FAIL", result);
   return result == 0 ? 0 : 1;
}
