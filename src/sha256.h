#pragma once

/*
 * sha256.h — SHA-256 and the two things TLS builds on top of it.
 *
 * SHA-256 (FIPS 180-4) is the hash at the heart of TLS 1.3 with our
 * cipher suite: it secures the handshake transcript, feeds HMAC, and
 * HMAC in turn feeds HKDF — the "key derivation function" that turns
 * one shared secret into the dozen keys a TLS connection actually uses.
 *
 * A hash takes any amount of data and returns a fixed 32-byte
 * fingerprint, with two magic properties: you can't run it backwards,
 * and you can't find two inputs with the same output. Everything below
 * is built from that one primitive.
 */

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST 32
#define SHA256_BLOCK  64

/* Streaming interface: init, update with chunks, final to get 32 bytes. */
struct sha256 {
    uint32_t h[8];          /* the running 256-bit state */
    uint64_t total;         /* total bytes seen (for the length padding) */
    uint8_t  buf[64];       /* partial block not yet compressed */
    size_t   n;             /* bytes currently in buf */
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST]);

/* One-shot convenience: hash a single buffer. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST]);

/* HMAC-SHA256 (RFC 2104): a keyed hash. "Does this message come from
 * someone who knows the key?" — the basis of TLS's integrity. */
void hmac_sha256(const uint8_t *key, size_t keylen,
                 const uint8_t *msg, size_t msglen,
                 uint8_t out[SHA256_DIGEST]);

/* HKDF (RFC 5869): extract-then-expand key derivation. Extract distils
 * a uniformly-random key from a (possibly lumpy) shared secret; Expand
 * stretches it into as many labelled subkeys as you need. TLS 1.3's
 * whole key schedule is HKDF calls. */
void hkdf_extract(const uint8_t *salt, size_t saltlen,
                  const uint8_t *ikm, size_t ikmlen,
                  uint8_t prk[SHA256_DIGEST]);
void hkdf_expand(const uint8_t *prk, size_t prklen,
                 const uint8_t *info, size_t infolen,
                 uint8_t *out, size_t outlen);
