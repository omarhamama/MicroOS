#pragma once

/*
 * sha512.h — SHA-512 and SHA-384.
 *
 * Same shape as SHA-256 but with 64-bit words, 80 rounds, and 128-byte
 * blocks — twice the everything. SHA-384 is literally SHA-512 with a
 * different starting state and the output truncated to 48 bytes. We
 * need these because certificate authorities sign with SHA-384 (and
 * occasionally SHA-512) as often as SHA-256.
 */

#include <stdint.h>
#include <stddef.h>

#define SHA512_DIGEST 64
#define SHA384_DIGEST 48

struct sha512 {
    uint64_t h[8];
    uint64_t total_lo, total_hi;
    uint8_t  buf[128];
    size_t   n;
    int      is384;
};

void sha512_init(struct sha512 *s);
void sha384_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, size_t len);
void sha512_final(struct sha512 *s, uint8_t *out);   /* 64 or 48 bytes */

void sha512(const void *data, size_t len, uint8_t out[64]);
void sha384(const void *data, size_t len, uint8_t out[48]);
