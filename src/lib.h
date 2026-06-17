#pragma once

#include <stdint.h>
#include <stddef.h>

/* The few libc routines a kernel can't live without. */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
int    ct_memcmp(const void *a, const void *b, size_t n);   /* constant-time */

/* Parse a hexadecimal string ("40080000" or "0x40080000").
 * Returns 0 on success, -1 if the string isn't valid hex. */
int parse_hex(const char *s, uint64_t *out);
