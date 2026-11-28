// SPDX-License-Identifier: GPL-3.0-or-later
/* Freestanding stand-in for <stdlib.h>: only what ENet (fetched, pinned in
   tools/switch_mod/deps.py) uses. On the console/emulator these resolve to the
   game's own malloc/free (main) at load time; in the PC test, to the C runtime's. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t size);
void free(void *p);
void abort(void);
#ifdef __cplusplus
}
#endif
