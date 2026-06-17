/* Host test for sha256.c — compile natively, check against published
 * vectors (FIPS 180-4, RFC 4231, RFC 5869). Run: see test/run.sh */
#include <stdio.h>
#include <string.h>
#include "../src/sha256.h"

static int fails = 0;

static void hex(const uint8_t *b, int n, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i*2]=h[b[i]>>4]; out[i*2+1]=h[b[i]&15]; }
    out[n*2] = 0;
}
static void check(const char *name, const uint8_t *got, int n, const char *want) {
    char g[256]; hex(got, n, g);
    if (strcmp(g, want)) { printf("FAIL %s\n  got  %s\n  want %s\n", name, g, want); fails++; }
    else printf("ok   %s\n", name);
}

int main(void) {
    uint8_t d[32];

    sha256("abc", 3, d);
    check("sha256(abc)", d, 32,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    sha256("", 0, d);
    check("sha256(empty)", d, 32,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const char *msg2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(msg2, strlen(msg2), d);
    check("sha256(56-byte)", d, 32,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* HMAC-SHA256, RFC 4231 Test Case 2 */
    hmac_sha256((const uint8_t*)"Jefe", 4,
        (const uint8_t*)"what do ya want for nothing?", 28, d);
    check("hmac(Jefe)", d, 32,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    /* HKDF-SHA256, RFC 5869 Test Case 1 */
    uint8_t ikm[22]; memset(ikm, 0x0b, 22);
    uint8_t salt[13]; for (int i=0;i<13;i++) salt[i]=i;
    uint8_t info[10] = {0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9};
    uint8_t prk[32];
    hkdf_extract(salt, 13, ikm, 22, prk);
    check("hkdf_extract", prk, 32,
        "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    uint8_t okm[42];
    hkdf_expand(prk, 32, info, 10, okm, 42);
    check("hkdf_expand", okm, 42,
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ec"
        "c4c5bf34007208d5b887185865");

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
