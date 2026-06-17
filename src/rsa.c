/*
 * rsa.c — PKCS#1 v1.5 signature verification.
 *
 * verify = recover the padded block, then check it has the exact shape
 * a real signature must have. We rebuild the expected block ourselves
 * (all-public data) and compare byte for byte — equivalent to, and
 * simpler than, parsing the recovered block, and it sidesteps the
 * classic "Bleichenbacher / BERserk" forgeries that come from sloppy
 * parsing of the padding.
 */

#include "rsa.h"
#include "bignum.h"
#include "sha256.h"
#include "sha512.h"
#include "lib.h"

const uint8_t RSA_SHA256_PREFIX[19] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
const uint8_t RSA_SHA384_PREFIX[19] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
const uint8_t RSA_SHA512_PREFIX[19] = {
    0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };

int rsa_verify(const uint8_t *n, int nlen,
               const uint8_t *e, int elen,
               const uint8_t *sig, int siglen,
               const uint8_t *hash, int hashlen,
               const uint8_t *prefix, int prefixlen)
{
    int k = nlen;                                   /* modulus size in bytes */
    if (k > BN_MAX_LIMBS * 4 || siglen > k || k < 11 + prefixlen + hashlen)
        return -1;
    int limbs = (k + 3) / 4;

    uint32_t bn_n[BN_MAX_LIMBS], bn_s[BN_MAX_LIMBS], bn_m[BN_MAX_LIMBS];
    bn_from_be(bn_n, limbs, n, nlen);
    bn_from_be(bn_s, limbs, sig, siglen);

    bn_modexp(bn_m, bn_s, limbs, e, elen, bn_n);    /* m = s^e mod n */

    uint8_t em[BN_MAX_LIMBS * 4];
    bn_to_be(em, k, bn_m, limbs);

    /* Build the block a valid signature MUST decrypt to, then compare. */
    uint8_t expect[BN_MAX_LIMBS * 4];
    int pslen = k - 3 - prefixlen - hashlen;        /* 0xFF padding length */
    if (pslen < 8)
        return -1;
    int p = 0;
    expect[p++] = 0x00;
    expect[p++] = 0x01;
    for (int i = 0; i < pslen; i++) expect[p++] = 0xff;
    expect[p++] = 0x00;
    memcpy(expect + p, prefix, prefixlen); p += prefixlen;
    memcpy(expect + p, hash, hashlen);     p += hashlen;

    return memcmp(em, expect, k) == 0 ? 0 : -1;
}

/* ---- RSASSA-PSS verification (RFC 8017 §9.1.2), salt len == hash len. */

static void hash_any(const uint8_t *d, size_t n, uint8_t *out, int hlen)
{
    if (hlen == 32) sha256(d, n, out);
    else if (hlen == 48) sha384(d, n, out);
    else sha512(d, n, out);
}

/* MGF1: stretch `seed` into `masklen` bytes by hashing seed||counter. */
static void mgf1(const uint8_t *seed, int seedlen, uint8_t *mask, int masklen, int hlen)
{
    uint8_t cnt[4], block[64];
    int done = 0;
    uint32_t c = 0;
    while (done < masklen) {
        cnt[0]=(uint8_t)(c>>24); cnt[1]=(uint8_t)(c>>16);
        cnt[2]=(uint8_t)(c>>8);  cnt[3]=(uint8_t)c;
        /* Hash(seed || counter) using a streaming hash of the chosen size */
        if (hlen == 32) { struct sha256 s; sha256_init(&s); sha256_update(&s,seed,seedlen); sha256_update(&s,cnt,4); sha256_final(&s,block); }
        else { struct sha512 s; if (hlen==48) sha384_init(&s); else sha512_init(&s); sha512_update(&s,seed,seedlen); sha512_update(&s,cnt,4); sha512_final(&s,block); }
        int take = masklen - done; if (take > hlen) take = hlen;
        memcpy(mask + done, block, take);
        done += take; c++;
    }
}

int rsa_pss_verify(const uint8_t *n, int nlen,
                   const uint8_t *e, int elen,
                   const uint8_t *sig, int siglen,
                   const uint8_t *hash, int hashlen)
{
    int k = nlen, hLen = hashlen, sLen = hashlen;
    if (k > BN_MAX_LIMBS * 4 || siglen > k || k < hLen + sLen + 2)
        return -1;
    int limbs = (k + 3) / 4;

    uint32_t bn_n[BN_MAX_LIMBS], bn_s[BN_MAX_LIMBS], bn_m[BN_MAX_LIMBS];
    bn_from_be(bn_n, limbs, n, nlen);
    bn_from_be(bn_s, limbs, sig, siglen);
    bn_modexp(bn_m, bn_s, limbs, e, elen, bn_n);

    uint8_t em[BN_MAX_LIMBS * 4];
    bn_to_be(em, k, bn_m, limbs);               /* emLen = k */

    if (em[k - 1] != 0xbc) return -1;           /* trailer */
    if (em[0] & 0x80) return -1;                /* top (only leftover) bit */

    int dbLen = k - hLen - 1;
    const uint8_t *H = em + dbLen;

    uint8_t db[BN_MAX_LIMBS * 4];
    mgf1(H, hLen, db, dbLen, hLen);
    for (int i = 0; i < dbLen; i++) db[i] ^= em[i];
    db[0] &= 0x7f;                              /* clear the leftover bit */

    int zeros = dbLen - sLen - 1;
    for (int i = 0; i < zeros; i++) if (db[i] != 0) return -1;
    if (db[zeros] != 0x01) return -1;
    const uint8_t *salt = db + dbLen - sLen;

    /* H' = Hash( 0x00*8 || mHash || salt ) */
    uint8_t mp[8 + 64 + 64];
    memset(mp, 0, 8);
    memcpy(mp + 8, hash, hLen);
    memcpy(mp + 8 + hLen, salt, sLen);
    uint8_t hp[64];
    hash_any(mp, 8 + hLen + sLen, hp, hLen);

    return ct_memcmp(hp, H, hLen) == 0 ? 0 : -1;
}
