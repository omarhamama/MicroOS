/* Host test for chain validation. argv: host now_yyyymmddhhmmss certfile...
 * Each certfile is a DER cert; first is the leaf. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/x509.h"

int ct_memcmp(const void*a,const void*b,size_t n){const unsigned char*p=a,*q=b;unsigned char d=0;for(size_t i=0;i<n;i++)d|=p[i]^q[i];return d;}

static uint8_t bufs[10][70000];
static size_t lens[10];

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: test_chain host now der...\n"); return 2; }
    const char *host = argv[1];
    long long now = atoll(argv[2]);
    int n = argc - 3;
    const uint8_t *certs[10];
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(argv[3+i], "rb");
        if (!f) { printf("cannot open %s\n", argv[3+i]); return 2; }
        lens[i] = fread(bufs[i], 1, sizeof bufs[i], f);
        fclose(f);
        certs[i] = bufs[i];
    }
    char err[128] = "", anchor[64] = "";
    int rc = x509_verify_chain(certs, lens, n, host, now, err, sizeof err, anchor, sizeof anchor);
    if (rc == 0) printf("ok   CHAIN VALID for %s  [anchor: %s]\n", host, anchor);
    else         printf("FAIL chain rejected: %s\n", err);
    return rc == 0 ? 0 : 1;
}
