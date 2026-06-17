/*
 * x25519.c — Curve25519 scalar multiplication.
 *
 * Field elements ("gf") are 255-bit numbers mod 2^255-19, carried as
 * sixteen 16-bit limbs in int64_t slots (radix 2^16). The slack in the
 * 64-bit limbs lets us add and multiply lazily and only normalise
 * ("carry") afterwards. This is the TweetNaCl representation — chosen
 * here because it is tiny, branch-free, and has been audited to death,
 * which is exactly what you want for security code you wrote yourself.
 *
 * The one routine that matters is crypto_scalarmult: a constant-time
 * Montgomery ladder. sel25519() conditionally swaps two field elements
 * using a bit-mask (never an `if`), so the same instructions run for
 * every scalar — no secret-dependent branches or memory addresses.
 */

#include "x25519.h"
#include "lib.h"

typedef int64_t gf[16];

static const gf _121665 = { 0xDB41, 1 };    /* (486662-2)/4, the curve const */

static void car25519(gf o)
{
    /* Propagate carries between limbs; the top limb folds back times 38
     * because 2^256 ≡ 38 (mod 2^255-19). */
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    /* Constant-time conditional swap: c is all-ones if b==1 else 0. */
    int64_t t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    gf t, m;
    int b;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    /* Two conditional subtractions of p bring t fully into [0,p). */
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2*i]   = (uint8_t)(t[i] & 0xff);
        o[2*i+1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++)
        o[i] = n[2*i] + ((int64_t)n[2*i+1] << 8);
    o[15] &= 0x7fff;                            /* clear the top bit */
}

static void A(gf o, const gf a, const gf b) { for (int i=0;i<16;i++) o[i]=a[i]+b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i=0;i<16;i++) o[i]=a[i]-b[i]; }

static void M(gf o, const gf a, const gf b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];   /* fold 2^256≡38 */
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i)
{
    /* Inverse via Fermat: a^(p-2) = a^-1 (mod p). p-2 has a fixed bit
     * pattern, so this is a fixed square-and-multiply chain. */
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static void scalarmult(uint8_t *q, const uint8_t *n, const uint8_t *p)
{
    uint8_t z[32];
    int64_t x[80];
    int64_t r;
    gf a, b, c, d, e, f;

    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;                 /* clamp the scalar... */
    z[0] &= 248;                                /* ...per RFC 7748 */

    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, (int)r); sel25519(c, d, (int)r);
        A(e, a, c); Z(a, a, c);
        A(c, b, d); Z(b, b, d);
        S(d, e); S(f, a);
        M(a, c, a); M(c, b, e);
        A(e, a, c); Z(a, a, c);
        S(b, a); Z(c, d, f);
        M(a, c, _121665); A(a, a, d);
        M(c, c, a); M(a, d, f);
        M(d, b, x); S(b, e);
        sel25519(a, b, (int)r); sel25519(c, d, (int)r);
    }
    for (int i = 0; i < 16; i++) {
        x[i+16] = a[i]; x[i+32] = c[i];
        x[i+48] = b[i]; x[i+64] = d[i];
    }
    inv25519(x + 32, x + 32);
    M(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    scalarmult(out, scalar, point);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    uint8_t base[32];
    memset(base, 0, 32);
    base[0] = 9;                                /* the curve's base point */
    scalarmult(out, scalar, base);
}
