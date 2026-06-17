/* Host test for p256.c — argv: pub_hex(128) r_hex(64) s_hex(64) message */
#include <stdio.h>
#include <string.h>
#include "../src/p256.h"
#include "../src/sha256.h"

static int unhex(const char *h, uint8_t *o) {
    int n = strlen(h)/2;
    for (int i=0;i<n;i++){int hi=h[i*2],lo=h[i*2+1];hi=hi<='9'?hi-'0':(hi|32)-'a'+10;lo=lo<='9'?lo-'0':(lo|32)-'a'+10;o[i]=hi*16+lo;}
    return n;
}

int main(int argc, char **argv) {
    if (argc < 5) { printf("usage: test_p256 pub r s msg\n"); return 2; }
    uint8_t pub[64], r[32], s[32];
    unhex(argv[1], pub);
    unhex(argv[2], r);
    unhex(argv[3], s);
    uint8_t hash[32];
    sha256(argv[4], strlen(argv[4]), hash);

    int rc = p256_ecdsa_verify(pub, hash, 32, r, s);
    printf("valid signature: %s\n", rc==0 ? "ok   YES" : "FAIL NO");

    r[31] ^= 1;     /* corrupt r -> must reject */
    int rc2 = p256_ecdsa_verify(pub, hash, 32, r, s);
    printf("rejects bad sig: %s\n", rc2!=0 ? "ok   YES" : "FAIL NO");

    return (rc==0 && rc2!=0) ? 0 : 1;
}
