/*
 * sha256.c — SHA-256, HMAC-SHA256, HKDF, all from scratch.
 *
 * SHA-256 chews the message in 64-byte blocks. For each block it runs
 * 64 rounds that stir the 512 bits of message into a 256-bit state
 * (eight 32-bit words h0..h7). Each round mixes in one "message
 * schedule" word and one cube-root-of-a-prime constant, using only
 * additions, XORs, and bit rotations — no multiplication, no tables of
 * secrets. The irreversibility comes from doing this 64 times: every
 * output bit depends on every input bit in a way nobody can untangle.
 *
 * The constants below aren't arbitrary — they're the fractional parts
 * of the square roots (h[]) and cube roots (K[]) of the first primes,
 * chosen precisely *because* they look random but anyone can rederive
 * them, proving there's no hidden backdoor ("nothing-up-my-sleeve").
 */

#include "sha256.h"
#include "lib.h"

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

/* The 64 round constants: frac(cbrt(first 64 primes)) * 2^32. */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/* Compress one 64-byte block into the state. */
static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];

    /* First 16 schedule words are the block, read big-endian. */
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    /* The other 48 are stirred from earlier ones. */
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
    h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void sha256_init(struct sha256 *s)
{
    /* frac(sqrt(first 8 primes)) * 2^32 */
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(s->h, iv, sizeof iv);
    s->total = 0;
    s->n = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->total += len;
    while (len) {
        size_t take = 64 - s->n;
        if (take > len) take = len;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 64) {
            sha256_block(s->h, s->buf);
            s->n = 0;
        }
    }
}

void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST])
{
    /* Padding: a single 1 bit, then zeros, then the 64-bit bit-length —
     * so the padded message is an exact multiple of 64 bytes and its
     * length is unforgeably baked in. */
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->n != 56)
        sha256_update(s, &zero, 1);
    uint8_t lenbe[8];
    for (int i = 0; i < 8; i++)
        lenbe[i] = (uint8_t)(bits >> (56 - i*8));
    sha256_update(s, lenbe, 8);

    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s->h[i] >> 24);
        out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);
        out[i*4+3] = (uint8_t)(s->h[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST])
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

/*
 * HMAC: hash the message twice, wrapped in the key. The two different
 * pads (ipad/opad) and the double hashing are what stop an attacker who
 * sees H(key||msg) from extending the message — the classic
 * length-extension attack SHA-256 alone is vulnerable to.
 */
void hmac_sha256(const uint8_t *key, size_t keylen,
                 const uint8_t *msg, size_t msglen,
                 uint8_t out[SHA256_DIGEST])
{
    uint8_t k[64];
    memset(k, 0, sizeof k);
    if (keylen > 64)
        sha256(key, keylen, k);         /* long keys are hashed down */
    else
        memcpy(k, key, keylen);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    uint8_t inner[SHA256_DIGEST];
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, ipad, 64);
    sha256_update(&s, msg, msglen);
    sha256_final(&s, inner);

    sha256_init(&s);
    sha256_update(&s, opad, 64);
    sha256_update(&s, inner, SHA256_DIGEST);
    sha256_final(&s, out);
}

void hkdf_extract(const uint8_t *salt, size_t saltlen,
                  const uint8_t *ikm, size_t ikmlen,
                  uint8_t prk[SHA256_DIGEST])
{
    /* "Extract" is just HMAC(salt, ikm): it concentrates the entropy of
     * a raw shared secret into one full-strength pseudorandom key. */
    uint8_t zero[SHA256_DIGEST];
    if (!salt) {
        memset(zero, 0, sizeof zero);
        salt = zero; saltlen = sizeof zero;
    }
    hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
}

void hkdf_expand(const uint8_t *prk, size_t prklen,
                 const uint8_t *info, size_t infolen,
                 uint8_t *out, size_t outlen)
{
    /* "Expand" chains HMACs (T(1)=HMAC(prk, info||0x01), T(2)=
     * HMAC(prk, T(1)||info||0x02), ...) to produce arbitrarily many
     * bytes — each block depends on the last, so output looks random. */
    uint8_t t[SHA256_DIGEST];
    size_t tlen = 0;
    uint8_t counter = 1;
    size_t done = 0;

    while (done < outlen) {
        struct sha256 s;
        uint8_t k[64];
        /* HMAC with prk as key, message = T(prev) || info || counter */
        memset(k, 0, sizeof k);
        if (prklen > 64) sha256(prk, prklen, k); else memcpy(k, prk, prklen);
        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; i++) { ipad[i]=k[i]^0x36; opad[i]=k[i]^0x5c; }

        uint8_t inner[SHA256_DIGEST];
        sha256_init(&s);
        sha256_update(&s, ipad, 64);
        sha256_update(&s, t, tlen);
        sha256_update(&s, info, infolen);
        sha256_update(&s, &counter, 1);
        sha256_final(&s, inner);
        sha256_init(&s);
        sha256_update(&s, opad, 64);
        sha256_update(&s, inner, SHA256_DIGEST);
        sha256_final(&s, t);
        tlen = SHA256_DIGEST;

        size_t take = outlen - done;
        if (take > SHA256_DIGEST) take = SHA256_DIGEST;
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }
}
