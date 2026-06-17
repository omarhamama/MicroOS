#pragma once

/*
 * p256.h — ECDSA signature verification on the NIST P-256 curve.
 *
 * The modern alternative to RSA: instead of a 2048-bit modulus, the key
 * is a single point on the elliptic curve y^2 = x^3 - 3x + b over a
 * 256-bit prime field. Smaller keys, same security. Many certificates
 * (Let's Encrypt's ECDSA chain, most CDN leaf certs) use it.
 *
 * Verification recovers a point R = u1*G + u2*Q from the signature and
 * the public key Q, and checks that R's x-coordinate equals the
 * signature's r. Only public values are involved, so — like RSA verify
 * — this need not be constant-time.
 */

#include <stdint.h>

/* Verify an ECDSA-P256 signature.
 *   pub  : 64 bytes, the uncompressed public point (x||y, 32 bytes each)
 *   hash : the message digest (e.g. SHA-256 output), hashlen bytes
 *   r,s  : 32-byte big-endian signature components
 * Returns 0 if valid, -1 otherwise. */
int p256_ecdsa_verify(const uint8_t pub[64],
                      const uint8_t *hash, int hashlen,
                      const uint8_t r[32], const uint8_t s[32]);
