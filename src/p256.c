/*
 * p256.c — NIST P-256 point arithmetic + ECDSA verification.
 *
 * Field elements and scalars are eight 32-bit limbs (256 bits). All the
 * modular grunt-work (multiply, add, subtract, inverse-via-Fermat) is
 * borrowed from bignum.c; this file adds the geometry: how to "add" two
 * points on the curve, and how to multiply a point by an integer
 * (repeated add/double). Points are kept in Jacobian coordinates
 * (X, Y, Z) representing affine (X/Z^2, Y/Z^3) — that lets point
 * addition avoid a modular inverse on every step, doing just one at the
 * very end.
 *
 * Reduction here goes through the generic bit-at-a-time bn_mod, so a
 * verify takes a few hundred thousand field ops — a fraction of a
 * second. Plenty fast for checking a certificate; a production library
 * would use the special shape of the P-256 prime to go ~50x quicker.
 */

#include "p256.h"
#include "bignum.h"
#include "lib.h"

#define K 8                                 /* 8 limbs = 256 bits */

/* Curve constants, big-endian, from FIPS 186-4 / SEC2. */
static const uint8_t P_be[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const uint8_t N_be[32] = {        /* group order */
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
static const uint8_t B_be[32] = {        /* curve coefficient b */
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
    0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b };
static const uint8_t GX_be[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96 };
static const uint8_t GY_be[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5 };

/* p-2 and n-2 as big-endian exponents for the Fermat inverse a^(m-2). */
static const uint8_t Pm2_be[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfd };
static const uint8_t Nm2_be[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x4f };

static uint32_t P[K], N[K], B[K], three[K];

static void load_consts(void)
{
    bn_from_be(P, K, P_be, 32);
    bn_from_be(N, K, N_be, 32);
    bn_from_be(B, K, B_be, 32);
    for (int i = 0; i < K; i++) three[i] = 0;
    three[0] = 3;
}

/* Field ops modulo the curve prime P. */
static void fadd(uint32_t *o, const uint32_t *a, const uint32_t *b){ bn_addmod(o,a,b,P,K); }
static void fsub(uint32_t *o, const uint32_t *a, const uint32_t *b){ bn_submod(o,a,b,P,K); }
static void fmul(uint32_t *o, const uint32_t *a, const uint32_t *b){ bn_mulmod(o,a,b,P,K); }
static void finv(uint32_t *o, const uint32_t *a){ bn_modexp(o,a,K,Pm2_be,32,P); }

/* A point in Jacobian coordinates; Z==0 means the point at infinity. */
struct jac { uint32_t X[K], Y[K], Z[K]; };

static int is_inf(const struct jac *p){ return bn_is_zero(p->Z, K); }

static void set_inf(struct jac *p)
{
    for (int i = 0; i < K; i++) { p->X[i]=0; p->Y[i]=0; p->Z[i]=0; }
    p->X[0] = 1; p->Y[0] = 1;       /* (1,1,0) — conventional infinity */
}

/* Point doubling, a = -3 form (FIPS-friendly). */
static void jdouble(struct jac *r, const struct jac *p)
{
    if (is_inf(p)) { set_inf(r); return; }
    uint32_t delta[K], gamma[K], beta[K], alpha[K], t[K], u[K], X3[K], Y3[K], Z3[K];

    fmul(delta, p->Z, p->Z);            /* delta = Z^2 */
    fmul(gamma, p->Y, p->Y);            /* gamma = Y^2 */
    fmul(beta, p->X, gamma);            /* beta  = X*gamma */

    fsub(t, p->X, delta);               /* X - delta */
    fadd(u, p->X, delta);               /* X + delta */
    fmul(t, t, u);                      /* (X-delta)(X+delta) */
    fmul(alpha, three, t);              /* alpha = 3*(...) */

    fmul(X3, alpha, alpha);             /* alpha^2 */
    fadd(t, beta, beta); fadd(t, t, t); /* 4*beta */
    fadd(u, t, t);                      /* 8*beta */
    fsub(X3, X3, u);                    /* X3 = alpha^2 - 8*beta */

    fadd(Z3, p->Y, p->Z); fmul(Z3, Z3, Z3);
    fsub(Z3, Z3, gamma); fsub(Z3, Z3, delta);   /* Z3 = (Y+Z)^2-gamma-delta */

    fsub(Y3, t, X3);                    /* 4*beta - X3 */
    fmul(Y3, alpha, Y3);                /* alpha*(4*beta-X3) */
    fmul(t, gamma, gamma);             /* gamma^2 */
    fadd(t, t, t); fadd(t, t, t); fadd(t, t, t); /* 8*gamma^2 */
    fsub(Y3, Y3, t);

    memcpy(r->X, X3, sizeof X3); memcpy(r->Y, Y3, sizeof Y3); memcpy(r->Z, Z3, sizeof Z3);
}

/* Jacobian point addition (general). */
static void jadd(struct jac *r, const struct jac *p, const struct jac *q)
{
    if (is_inf(p)) { *r = *q; return; }
    if (is_inf(q)) { *r = *p; return; }

    uint32_t Z1Z1[K], Z2Z2[K], U1[K], U2[K], S1[K], S2[K], H[K], Rr[K];
    uint32_t t[K], I[K], J[K], V[K], X3[K], Y3[K], Z3[K];

    fmul(Z1Z1, p->Z, p->Z);
    fmul(Z2Z2, q->Z, q->Z);
    fmul(U1, p->X, Z2Z2);
    fmul(U2, q->X, Z1Z1);
    fmul(S1, p->Y, q->Z); fmul(S1, S1, Z2Z2);
    fmul(S2, q->Y, p->Z); fmul(S2, S2, Z1Z1);

    if (bn_cmp_pub(U1, U2, K) == 0) {
        if (bn_cmp_pub(S1, S2, K) != 0) { set_inf(r); return; }  /* P + (-P) */
        jdouble(r, p);                                          /* P == Q */
        return;
    }

    fsub(H, U2, U1);
    fadd(t, H, H); fmul(I, t, t);       /* I = (2H)^2 */
    fmul(J, H, I);                      /* J = H*I */
    fsub(Rr, S2, S1); fadd(Rr, Rr, Rr); /* r = 2(S2-S1) */
    fmul(V, U1, I);

    fmul(X3, Rr, Rr); fsub(X3, X3, J);
    fadd(t, V, V); fsub(X3, X3, t);     /* X3 = r^2 - J - 2V */

    fsub(Y3, V, X3); fmul(Y3, Rr, Y3);
    fmul(t, S1, J); fadd(t, t, t);      /* 2*S1*J */
    fsub(Y3, Y3, t);

    fadd(Z3, p->Z, q->Z); fmul(Z3, Z3, Z3);
    fsub(Z3, Z3, Z1Z1); fsub(Z3, Z3, Z2Z2); fmul(Z3, Z3, H);

    memcpy(r->X, X3, sizeof X3); memcpy(r->Y, Y3, sizeof Y3); memcpy(r->Z, Z3, sizeof Z3);
}

/* r = k*p, scalar k as 8 limbs. Double-and-add (not constant-time;
 * public-only inputs). */
static void jmul(struct jac *r, const uint32_t *k, const struct jac *p)
{
    set_inf(r);
    for (int bit = 255; bit >= 0; bit--) {
        struct jac t;
        jdouble(&t, r); *r = t;
        if ((k[bit >> 5] >> (bit & 31)) & 1) {
            jadd(&t, r, p); *r = t;
        }
    }
}

/* Convert Jacobian -> affine x only (we just need R.x). */
static void affine_x(uint32_t *x, const struct jac *p)
{
    uint32_t zinv[K], zinv2[K];
    finv(zinv, p->Z);
    fmul(zinv2, zinv, zinv);
    fmul(x, p->X, zinv2);
}

int p256_ecdsa_verify(const uint8_t pub[64],
                      const uint8_t *hash, int hashlen,
                      const uint8_t r[32], const uint8_t s[32])
{
    load_consts();

    uint32_t rr[K], ss[K], z[K];
    bn_from_be(rr, K, r, 32);
    bn_from_be(ss, K, s, 32);

    /* r, s must be in [1, n-1] */
    if (bn_is_zero(rr, K) || bn_is_zero(ss, K)) return -1;
    if (bn_cmp_pub(rr, N, K) >= 0 || bn_cmp_pub(ss, N, K) >= 0) return -1;

    /* z = leftmost min(hashlen,32) bytes of the hash, as an integer */
    uint8_t zb[32];
    int take = hashlen < 32 ? hashlen : 32;
    memset(zb, 0, 32);
    memcpy(zb, hash, take);             /* big-endian, left-aligned */
    bn_from_be(z, K, zb, 32);
    if (bn_cmp_pub(z, N, K) >= 0) bn_submod(z, z, N, N, K);  /* z mod n (coarse) */

    /* w = s^-1 mod n; u1 = z*w mod n; u2 = r*w mod n */
    uint32_t w[K], u1[K], u2[K];
    bn_modexp(w, ss, K, Nm2_be, 32, N);
    bn_mulmod(u1, z, w, N, K);
    bn_mulmod(u2, rr, w, N, K);

    /* Load G and Q as Jacobian (Z=1). Q taken on trust as on-curve here;
     * the chain's signatures are what actually anchor trust. */
    struct jac G, Q, A, Bp, Rp;
    bn_from_be(G.X, K, GX_be, 32); bn_from_be(G.Y, K, GY_be, 32);
    for (int i=0;i<K;i++) G.Z[i]=0;
    G.Z[0]=1;
    bn_from_be(Q.X, K, pub, 32); bn_from_be(Q.Y, K, pub+32, 32);
    for (int i=0;i<K;i++) Q.Z[i]=0;
    Q.Z[0]=1;

    jmul(&A, u1, &G);                   /* u1*G */
    jmul(&Bp, u2, &Q);                  /* u2*Q */
    jadd(&Rp, &A, &Bp);                 /* R = u1*G + u2*Q */
    if (is_inf(&Rp)) return -1;

    uint32_t x[K];
    affine_x(x, &Rp);
    if (bn_cmp_pub(x, N, K) >= 0) bn_submod(x, x, N, N, K);  /* x mod n */

    return bn_cmp_pub(x, rr, K) == 0 ? 0 : -1;
}
