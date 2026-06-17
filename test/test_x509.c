/* Host test for x509.c — parses a DER cert read from stdin. */
#include <stdio.h>
#include <string.h>
#include "../src/x509.h"
#include "../src/asn1.h"

int main(int argc, char **argv) {
    static uint8_t der[65536];
    size_t n = fread(der, 1, sizeof der, stdin);
    struct x509 c;
    if (x509_parse(&c, der, n) < 0) { printf("FAIL parse\n"); return 1; }

    const char *sa[] = {"unknown","rsa-sha256","rsa-sha384","rsa-sha512","ecdsa-sha256","ecdsa-sha384"};
    const char *ka[] = {"unknown","RSA","EC-P256"};
    printf("ok   parsed\n");
    printf("  sig_alg   = %s\n", sa[c.sig_alg]);
    printf("  key_alg   = %s\n", ka[c.key_alg]);
    if (c.key_alg==KEY_RSA) printf("  rsa modulus bytes = %zu, exp bytes = %zu\n", c.rsa_n_len, c.rsa_e_len);
    if (c.key_alg==KEY_EC_P256) printf("  ec point bytes = %zu (0x%02x)\n", c.ec_point_len, c.ec_point[0]);
    printf("  not_before= %lld\n", (long long)c.not_before);
    printf("  not_after = %lld\n", (long long)c.not_after);
    printf("  is_ca     = %d\n", c.is_ca);
    printf("  tbs_len   = %zu, sig_len = %zu, issuer_len=%zu subject_len=%zu\n",
           c.tbs_len, c.sig_len, c.issuer_len, c.subject_len);

    /* list SAN dNSNames */
    if (c.san) {
        struct der d = der_span(c.san, c.san_len);
        int tag; const uint8_t *v; size_t vl;
        printf("  SAN dNSNames:");
        while (der_next(&d, &tag, &v, &vl) == 0)
            if (tag == 0x82) printf(" %.*s", (int)vl, v);
        printf("\n");
    }
    /* hostname match test if a host given */
    if (argc > 1)
        printf("  matches %s : %d\n", argv[1], x509_matches_host(&c, argv[1]));
    return 0;
}
