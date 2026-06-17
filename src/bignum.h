#pragma once

/*
 * bignum.h — just enough big-integer arithmetic for RSA verification.
 *
 * RSA verifies a signature by computing  m = s^e mod n,  where s, n are
 * ~2048-4096 bit numbers — far bigger than any CPU register. So we hold
 * them as arrays of 32-bit "limbs", little-endian (limb[0] is least
 * significant), and do grade-school arithmetic on the array.
 *
 * IMPORTANT scope note: these routines work only with PUBLIC values
 * (the signature and the public key), so they need not be constant-time
 * — and they aren't. Never reuse this module for secret-key operations.
 */

#include <stdint.h>

#define BN_MAX_LIMBS 128        /* up to 4096-bit moduli */

/* Load a big-endian byte string (as found in certificates/keys) into a
 * little-endian limb array of exactly `limbs` words (zero-padded). */
void bn_from_be(uint32_t *out, int limbs, const uint8_t *be, int nbytes);

/* Serialize `limbs` words back to `nbytes` big-endian bytes. */
void bn_to_be(uint8_t *be, int nbytes, const uint32_t *in, int limbs);

/* out = base^e mod n.  base and n are `k` limbs; e is big-endian bytes.
 * out is `k` limbs. Requires n odd and base < n (true for RSA). */
void bn_modexp(uint32_t *out, const uint32_t *base, int k,
               const uint8_t *e, int elen, const uint32_t *n);

/* Modular arithmetic over `k` limbs, used by the P-256 curve code.
 * Inputs are assumed already reduced (< m) for add/sub. */
int  bn_cmp_pub(const uint32_t *a, const uint32_t *b, int k);
int  bn_is_zero(const uint32_t *a, int k);
void bn_addmod(uint32_t *out, const uint32_t *a, const uint32_t *b,
               const uint32_t *m, int k);
void bn_submod(uint32_t *out, const uint32_t *a, const uint32_t *b,
               const uint32_t *m, int k);
void bn_mulmod(uint32_t *out, const uint32_t *a, const uint32_t *b,
               const uint32_t *m, int k);
