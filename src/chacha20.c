/*
 * chacha20.c — ChaCha20 stream cipher + Poly1305 MAC + the AEAD glue.
 * Follows RFC 8439 exactly.
 *
 * CHACHA20: start from a 16-word (512-bit) state — 4 constant words
 * ("expand 32-byte k"), the 8-word key, a 32-bit block counter, and a
 * 96-bit nonce. Scramble it with 20 rounds of the "quarter-round" (a
 * fixed dance of add/xor/rotate on 4 words), add the original state
 * back, and you have 64 pseudorandom "keystream" bytes. XOR those into
 * the plaintext to encrypt; XOR again to decrypt. The counter lets you
 * make as many independent 64-byte blocks as the message needs.
 *
 * POLY1305: a one-time authenticator. Treat the message as a series of
 * big numbers, evaluate a polynomial in a secret point r modulo the
 * prime 2^130-5, add a secret pad s. The result is a 16-byte tag that
 * is astronomically hard to forge without knowing (r,s) — and (r,s) is
 * derived fresh for every record from ChaCha20 itself.
 */

#include "chacha20.h"
#include "lib.h"

/* ----------------------------- ChaCha20 ----------------------------- */

static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t rd32(const uint8_t *p)
{ return p[0] | (p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

#define QR(a,b,c,d) \
    a += b; d ^= a; d = rotl(d,16); \
    c += d; b ^= c; b = rotl(b,12); \
    a += b; d ^= a; d = rotl(d, 8); \
    c += d; b ^= c; b = rotl(b, 7)

void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0]=0x61707865; s[1]=0x3320646e; s[2]=0x79622d32; s[3]=0x6b206574;
    for (int i = 0; i < 8; i++) s[4+i] = rd32(key + i*4);
    s[12] = counter;
    s[13] = rd32(nonce); s[14] = rd32(nonce+4); s[15] = rd32(nonce+8);

    memcpy(x, s, sizeof s);
    for (int i = 0; i < 10; i++) {          /* 10 double-rounds = 20 */
        QR(x[0], x[4], x[ 8], x[12]);       /* columns */
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);       /* diagonals */
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];           /* add the original back */
        out[i*4]   = (uint8_t)v;
        out[i*4+1] = (uint8_t)(v >> 8);
        out[i*4+2] = (uint8_t)(v >> 16);
        out[i*4+3] = (uint8_t)(v >> 24);
    }
}

void chacha20_xor(const uint8_t key[32], uint32_t counter,
                  const uint8_t nonce[12], const uint8_t *in, uint8_t *out,
                  size_t len)
{
    uint8_t ks[64];
    size_t off = 0;
    while (off < len) {
        chacha20_block(key, counter++, nonce, ks);
        size_t n = len - off;
        if (n > 64) n = 64;
        for (size_t i = 0; i < n; i++)
            out[off+i] = in[off+i] ^ ks[i];
        off += n;
    }
}

/* ----------------------------- Poly1305 ----------------------------- */
/*
 * Poly1305 arithmetic mod 2^130-5. We carry the 130-bit accumulator in
 * five 26-bit limbs so each multiply stays inside 64 bits — the same
 * trick every portable Poly1305 uses (Bernstein's). r is "clamped"
 * (some bits forced to 0) so the partial products can't overflow.
 */
void poly1305_mac(const uint8_t key[32], const uint8_t *msg, size_t len,
                  uint8_t tag[16])
{
    uint32_t r0,r1,r2,r3,r4, s1,s2,s3,s4, h0=0,h1=0,h2=0,h3=0,h4=0;
    uint64_t d0,d1,d2,d3,d4;
    uint32_t t0,t1,t2,t3;

    t0 = rd32(key);   t1 = rd32(key+4); t2 = rd32(key+8); t3 = rd32(key+12);
    /* clamp r */
    r0 =  t0                      & 0x3ffffff;
    r1 = ((t0>>26)|(t1<<6))       & 0x3ffff03;
    r2 = ((t1>>20)|(t2<<12))      & 0x3ffc0ff;
    r3 = ((t2>>14)|(t3<<18))      & 0x3f03fff;
    r4 =  (t3>>8)                 & 0x00fffff;
    s1 = r1*5; s2 = r2*5; s3 = r3*5; s4 = r4*5;

    while (len > 0) {
        uint8_t block[16];
        size_t n = len < 16 ? len : 16;
        memset(block, 0, 16);
        memcpy(block, msg, n);
        /* the high bit: a full block adds 2^128; a final short block
         * sets the bit just past its last byte instead. */
        uint32_t hibit;
        if (n == 16) { hibit = 1u << 24; }
        else { block[n] = 1; hibit = 0; }

        uint32_t m0=rd32(block), m1=rd32(block+4), m2=rd32(block+8), m3=rd32(block+12);
        h0 += m0 & 0x3ffffff;
        h1 += ((m0>>26)|(m1<<6)) & 0x3ffffff;
        h2 += ((m1>>20)|(m2<<12)) & 0x3ffffff;
        h3 += ((m2>>14)|(m3<<18)) & 0x3ffffff;
        h4 += (m3>>8) | hibit;

        /* h *= r  (mod 2^130-5), schoolbook with the *5 fold-back */
        d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        uint32_t c;
        h0 = (uint32_t)d0 & 0x3ffffff; c = (uint32_t)(d0 >> 26);
        d1 += c; h1 = (uint32_t)d1 & 0x3ffffff; c = (uint32_t)(d1 >> 26);
        d2 += c; h2 = (uint32_t)d2 & 0x3ffffff; c = (uint32_t)(d2 >> 26);
        d3 += c; h3 = (uint32_t)d3 & 0x3ffffff; c = (uint32_t)(d3 >> 26);
        d4 += c; h4 = (uint32_t)d4 & 0x3ffffff; c = (uint32_t)(d4 >> 26);
        h0 += c * 5; h1 += h0 >> 26; h0 &= 0x3ffffff;

        msg += n; len -= n;
    }

    /* final carry propagation */
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    /* compute h + -p (i.e. h - (2^130-5)); pick it if h >= p */
    uint32_t g0,g1,g2,g3,g4;
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;     /* all-ones if g4 did NOT borrow */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    /* serialize h to 128 bits, then add s = key[16..31] */
    uint64_t f;
    uint32_t hh0 = h0 | (h1 << 26);
    uint32_t hh1 = (h1 >> 6) | (h2 << 20);
    uint32_t hh2 = (h2 >> 12) | (h3 << 14);
    uint32_t hh3 = (h3 >> 18) | (h4 << 8);

    f = (uint64_t)hh0 + rd32(key+16);             tag[0]=(uint8_t)f; tag[1]=(uint8_t)(f>>8); tag[2]=(uint8_t)(f>>16); tag[3]=(uint8_t)(f>>24);
    f = (uint64_t)hh1 + rd32(key+20) + (f >> 32); tag[4]=(uint8_t)f; tag[5]=(uint8_t)(f>>8); tag[6]=(uint8_t)(f>>16); tag[7]=(uint8_t)(f>>24);
    f = (uint64_t)hh2 + rd32(key+24) + (f >> 32); tag[8]=(uint8_t)f; tag[9]=(uint8_t)(f>>8); tag[10]=(uint8_t)(f>>16); tag[11]=(uint8_t)(f>>24);
    f = (uint64_t)hh3 + rd32(key+28) + (f >> 32); tag[12]=(uint8_t)f; tag[13]=(uint8_t)(f>>8); tag[14]=(uint8_t)(f>>16); tag[15]=(uint8_t)(f>>24);
}

/* ------------------------------- AEAD ------------------------------- */

/* The Poly1305 key is the first 32 bytes of ChaCha20 keystream block 0
 * (with counter 0); encryption starts at counter 1. A fresh MAC key per
 * record is what makes the one-time MAC safe to reuse the cipher key. */
static void poly_key(const uint8_t key[32], const uint8_t nonce[12],
                     uint8_t out[32])
{
    uint8_t blk[64];
    chacha20_block(key, 0, nonce, blk);
    memcpy(out, blk, 32);
}

/* The MAC is taken over (aad || pad16 || ciphertext || pad16 ||
 * len(aad) || len(ct)), each length a 64-bit little-endian word. We
 * assemble that into one bounded buffer. TLS plaintext records are
 * capped at 2^14 + 256, so this static scratch (not reentrant — TLS
 * runs in a single task here) is big enough. */
#define POLY_MAXMSG (16384 + 512)

static size_t build_mac_msg(uint8_t *buf, const uint8_t *aad, size_t aadlen,
                            const uint8_t *ct, size_t ctlen)
{
    size_t n = 0;
    memcpy(buf + n, aad, aadlen); n += aadlen;
    while (n % 16) buf[n++] = 0;
    memcpy(buf + n, ct, ctlen); n += ctlen;
    while (n % 16) buf[n++] = 0;
    /* aadlen and ctlen as 64-bit little-endian */
    for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)(aadlen >> (i*8));
    for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)(ctlen  >> (i*8));
    return n;
}

void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *aad, size_t aadlen,
                            const uint8_t *pt, size_t ptlen,
                            uint8_t *ct, uint8_t tag[16])
{
    chacha20_xor(key, 1, nonce, pt, ct, ptlen);     /* encrypt @ counter 1 */

    uint8_t pkey[32];
    poly_key(key, nonce, pkey);

    static uint8_t mac[POLY_MAXMSG];                /* bounded by record size */
    size_t mlen = build_mac_msg(mac, aad, aadlen, ct, ptlen);
    poly1305_mac(pkey, mac, mlen, tag);
}

int chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aadlen,
                           const uint8_t *ct, size_t ctlen,
                           const uint8_t tag[16], uint8_t *pt)
{
    uint8_t pkey[32];
    poly_key(key, nonce, pkey);

    static uint8_t mac[POLY_MAXMSG];
    size_t mlen = build_mac_msg(mac, aad, aadlen, ct, ctlen);
    uint8_t want[16];
    poly1305_mac(pkey, mac, mlen, want);

    if (ct_memcmp(want, tag, 16) != 0)
        return -1;                                  /* forged — reject */

    chacha20_xor(key, 1, nonce, ct, pt, ctlen);     /* only now decrypt */
    return 0;
}
