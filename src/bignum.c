/*
 * bignum.c — multi-precision integers for RSA verification.
 *
 * Numbers are little-endian limb arrays (uint32_t[]). The two workhorses
 * are bn_mul (schoolbook O(n^2) multiply, accumulating partial products
 * in a 64-bit register so nothing overflows) and bn_mod (reduction by
 * binary long division — exactly the "shift the divisor down, subtract
 * if it fits" you learned in school, one bit at a time). Modular
 * exponentiation is then ordinary square-and-multiply.
 *
 * None of this is fast or constant-time; it doesn't need to be. RSA
 * verify does ~17 modular squarings of public numbers and finishes in
 * well under a millisecond even on this emulated machine.
 */

#include "bignum.h"
#include "lib.h"

void bn_from_be(uint32_t *out, int limbs, const uint8_t *be, int nbytes)
{
    for (int i = 0; i < limbs; i++) out[i] = 0;
    /* big-endian: last byte is least significant */
    for (int i = 0; i < nbytes; i++) {
        int bytepos = nbytes - 1 - i;       /* 0 = LSB */
        out[bytepos >> 2] |= (uint32_t)be[i] << ((bytepos & 3) * 8);
    }
}

void bn_to_be(uint8_t *be, int nbytes, const uint32_t *in, int limbs)
{
    for (int i = 0; i < nbytes; i++) {
        int bytepos = nbytes - 1 - i;       /* 0 = LSB */
        uint32_t limb = (bytepos >> 2) < limbs ? in[bytepos >> 2] : 0;
        be[i] = (uint8_t)(limb >> ((bytepos & 3) * 8));
    }
}

/* a <=> b over `n` limbs: -1, 0, or 1. */
static int bn_cmp(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return  1;
    }
    return 0;
}

/* a -= b over `n` limbs (assumes a >= b). */
static void bn_sub(uint32_t *a, const uint32_t *b, int n)
{
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;             /* set if it underflowed */
    }
}

/* a <<= 1 over `n` limbs; returns the bit shifted out of the top. */
static uint32_t bn_shl1(uint32_t *a, int n)
{
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint32_t nc = a[i] >> 31;
        a[i] = (a[i] << 1) | carry;
        carry = nc;
    }
    return carry;
}

/* out[2k] = a[k] * b[k] */
static void bn_mul(uint32_t *out, const uint32_t *a, const uint32_t *b, int k)
{
    for (int i = 0; i < 2 * k; i++) out[i] = 0;
    for (int i = 0; i < k; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < k; j++) {
            uint64_t cur = (uint64_t)out[i + j] +
                           (uint64_t)a[i] * b[j] + carry;
            out[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        out[i + k] += (uint32_t)carry;
    }
}

/* rem[k] = x[xl] mod n[k], by binary long division. */
static void bn_mod(uint32_t *rem, const uint32_t *x, int xl,
                   const uint32_t *n, int k)
{
    uint32_t r[BN_MAX_LIMBS + 1];
    uint32_t ne[BN_MAX_LIMBS + 1];
    for (int i = 0; i <= k; i++) { r[i] = 0; ne[i] = (i < k) ? n[i] : 0; }

    for (int bit = xl * 32 - 1; bit >= 0; bit--) {
        bn_shl1(r, k + 1);                          /* r <<= 1 */
        r[0] |= (x[bit >> 5] >> (bit & 31)) & 1;    /* bring down next bit */
        if (bn_cmp(r, ne, k + 1) >= 0)              /* r >= n ? */
            bn_sub(r, ne, k + 1);                   /* then r -= n */
    }
    for (int i = 0; i < k; i++) rem[i] = r[i];
}

void bn_modexp(uint32_t *out, const uint32_t *base, int k,
               const uint8_t *e, int elen, const uint32_t *n)
{
    uint32_t result[BN_MAX_LIMBS];
    uint32_t b[BN_MAX_LIMBS];
    uint32_t prod[2 * BN_MAX_LIMBS];

    bn_mod(b, base, k, n, k);                       /* ensure base < n */
    for (int i = 0; i < k; i++) result[i] = 0;
    result[0] = 1;

    int started = 0;
    for (int i = 0; i < elen; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            if (!started) {
                if (!((e[i] >> bit) & 1)) continue; /* skip leading zeros */
                started = 1;
            }
            bn_mul(prod, result, result, k);        /* result = result^2 */
            bn_mod(result, prod, 2 * k, n, k);
            if ((e[i] >> bit) & 1) {                /* and *base if bit set */
                bn_mul(prod, result, b, k);
                bn_mod(result, prod, 2 * k, n, k);
            }
        }
    }
    if (!started) { for (int i = 0; i < k; i++) out[i] = 0; out[0] = 1; return; }
    for (int i = 0; i < k; i++) out[i] = result[i];
}

/* ---- public modular helpers (for the P-256 curve arithmetic) ------- */

int bn_cmp_pub(const uint32_t *a, const uint32_t *b, int k) { return bn_cmp(a, b, k); }

int bn_is_zero(const uint32_t *a, int k)
{
    uint32_t acc = 0;
    for (int i = 0; i < k; i++) acc |= a[i];
    return acc == 0;
}

void bn_addmod(uint32_t *out, const uint32_t *a, const uint32_t *b,
               const uint32_t *m, int k)
{
    uint32_t t[BN_MAX_LIMBS + 1], me[BN_MAX_LIMBS + 1];
    uint64_t carry = 0;
    for (int i = 0; i < k; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        t[i] = (uint32_t)s; carry = s >> 32;
        me[i] = m[i];
    }
    t[k] = (uint32_t)carry; me[k] = 0;
    if (bn_cmp(t, me, k + 1) >= 0) bn_sub(t, me, k + 1);
    for (int i = 0; i < k; i++) out[i] = t[i];
}

void bn_submod(uint32_t *out, const uint32_t *a, const uint32_t *b,
               const uint32_t *m, int k)
{
    if (bn_cmp(a, b, k) >= 0) {
        for (int i = 0; i < k; i++) out[i] = a[i];
        bn_sub(out, b, k);
    } else {
        /* out = a + m - b, which lands back in [0, m) */
        uint32_t t[BN_MAX_LIMBS + 1], be[BN_MAX_LIMBS + 1];
        uint64_t carry = 0;
        for (int i = 0; i < k; i++) {
            uint64_t s = (uint64_t)a[i] + m[i] + carry;
            t[i] = (uint32_t)s; carry = s >> 32;
            be[i] = b[i];
        }
        t[k] = (uint32_t)carry; be[k] = 0;
        bn_sub(t, be, k + 1);
        for (int i = 0; i < k; i++) out[i] = t[i];
    }
}

void bn_mulmod(uint32_t *out, const uint32_t *a, const uint32_t *b,
               const uint32_t *m, int k)
{
    uint32_t prod[2 * BN_MAX_LIMBS];
    bn_mul(prod, a, b, k);
    bn_mod(out, prod, 2 * k, m, k);
}
