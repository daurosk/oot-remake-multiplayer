// SPDX-License-Identifier: GPL-3.0-or-later
/* Freestanding stand-in for <string.h> (see stdlib.h). sdk exports all four. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#ifdef __cplusplus
}
#endif
