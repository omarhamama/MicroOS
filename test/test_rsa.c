/* Host test for rsa.c — argv: n_hex e_hex sig_hex message
 * Hashes the message (SHA-256) and verifies the PKCS#1 v1.5 signature. */
#include <stdio.h>
#include <string.h>
#include "../src/rsa.h"
#include "../src/sha256.h"

int ct_memcmp(const void*a,const void*b,size_t n){const unsigned char*p=a,*q=b;unsigned char d=0;for(size_t i=0;i<n;i++)d|=p[i]^q[i];return d;}
static int unhex(const char *h, uint8_t *o) {
    int n = strlen(h) / 2;
    for (int i = 0; i < n; i++) {
        int hi=h[i*2], lo=h[i*2+1];
        hi = hi<='9'?hi-'0':(hi|32)-'a'+10;
        lo = lo<='9'?lo-'0':(lo|32)-'a'+10;
        o[i] = hi*16+lo;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 5) { printf("usage: test_rsa n e sig msg\n"); return 2; }
    uint8_t n[600], e[8], sig[600];
    int nlen = unhex(argv[1], n);
    int elen = unhex(argv[2], e);
    int siglen = unhex(argv[3], sig);

    uint8_t hash[32];
    sha256(argv[4], strlen(argv[4]), hash);

    int rc = rsa_verify(n, nlen, e, elen, sig, siglen, hash, 32,
                        RSA_SHA256_PREFIX, 19);
    printf("valid signature: %s\n", rc==0 ? "ok   YES" : "FAIL NO");

    /* negative: flip a hash bit -> must reject */
    hash[0] ^= 1;
    int rc2 = rsa_verify(n, nlen, e, elen, sig, siglen, hash, 32,
                         RSA_SHA256_PREFIX, 19);
    printf("rejects wrong hash: %s\n", rc2!=0 ? "ok   YES" : "FAIL NO");

    return (rc==0 && rc2!=0) ? 0 : 1;
}
