#pragma once

/*
 * x25519.h — Diffie-Hellman on Curve25519 (RFC 7748).
 *
 * This is how TLS 1.3 agrees on a shared secret without ever sending
 * it. Each side picks a random 32-byte private scalar; x25519_base()
 * turns it into a public value to send. Each side then runs x25519()
 * on its own private scalar and the OTHER side's public value — and,
 * by the magic of the curve, both arrive at the SAME 32-byte secret,
 * while an eavesdropper who saw both public values cannot compute it.
 *
 * Under the hood it's a "Montgomery ladder": a fixed sequence of
 * additions and doublings of a point on the curve y^2 = x^3 +
 * 486662 x^2 + x, with all arithmetic done modulo the prime 2^255-19.
 * The ladder touches every bit of the scalar in the same pattern
 * regardless of its value, so it leaks nothing through timing.
 */

#include <stdint.h>

/* shared = scalar * point   (both 32 bytes, little-endian). */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

/* public = scalar * basepoint(9). */
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);
