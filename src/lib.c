/*
 * lib.c — a miniature C library.
 *
 * We compile with -ffreestanding, which means "no standard library
 * exists". But even then, GCC reserves the right to emit calls to
 * memset/memcpy for things like struct assignment — so a kernel must
 * always provide these four by hand.
 */

#include "lib.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

/* Both routines move 8 bytes at a time once aligned. This started as
 * a byte loop — fine for filenames, ruinous for the GUI, which copies
 * a 3 MB framebuffer every frame. An 8x speedup here is an 8x faster
 * screen flip. (Real libcs go further with SIMD; our kernel compiles
 * with the FPU off, so 64-bit registers are the ceiling.) */

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    uint64_t pattern = (uint8_t)c;
    pattern |= pattern << 8;
    pattern |= pattern << 16;
    pattern |= pattern << 32;

    while (n && ((uint64_t)d & 7)) {        /* lead-in to alignment */
        *d++ = (unsigned char)c;
        n--;
    }
    uint64_t *dw = (uint64_t *)d;
    while (n >= 8) {
        *dw++ = pattern;
        n -= 8;
    }
    d = (unsigned char *)dw;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    /* The word loop only works if both pointers can reach alignment
     * together (same offset within an 8-byte word). */
    if (((uint64_t)d & 7) == ((uint64_t)s & 7)) {
        while (n && ((uint64_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        uint64_t *dw = (uint64_t *)d;
        const uint64_t *sw = (const uint64_t *)s;
        while (n >= 8) {
            *dw++ = *sw++;
            n -= 8;
        }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

/* memmove — like memcpy but safe when the regions overlap (TLS shifts
 * leftover bytes to the front of its receive buffer). Copy backwards
 * when dst is above src so we don't clobber bytes we haven't read. */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s)
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    else
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i])
            return (int)p[i] - (int)q[i];
    return 0;
}

/* CONSTANT-TIME compare — for secrets (MAC tags, Finished hashes). A
 * normal memcmp returns at the first differing byte, so an attacker can
 * time how far the comparison got and recover the value byte by byte.
 * This one always walks all n bytes, OR-ing differences into an
 * accumulator, so the timing reveals nothing. Returns 0 iff equal. */
int ct_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= p[i] ^ q[i];
    return diff;     /* 0 = identical; nonzero = differ (no leak of where) */
}

int parse_hex(const char *s, uint64_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (!*s)
        return -1;

    uint64_t v = 0;
    for (; *s; s++) {
        int digit;
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F')
            digit = *s - 'A' + 10;
        else
            return -1;
        v = (v << 4) | (uint64_t)digit;
    }
    *out = v;
    return 0;
}
