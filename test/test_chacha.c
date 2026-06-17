/* Host test for chacha20.c — RFC 8439 test vectors. */
#include <stdio.h>
#include <string.h>
#include "../src/chacha20.h"

/* lib.h declares ct_memcmp; provide it for the host link. */
int ct_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p=a,*q=b; unsigned char d=0;
    for (size_t i=0;i<n;i++) d|=p[i]^q[i]; return d;
}

static int fails = 0;
static void hex(const uint8_t *b, int n, char *o){static const char*h="0123456789abcdef";for(int i=0;i<n;i++){o[i*2]=h[b[i]>>4];o[i*2+1]=h[b[i]&15];}o[n*2]=0;}
static void check(const char*name,const uint8_t*got,int n,const char*want){char g[2200];hex(got,n,g);if(strcmp(g,want)){printf("FAIL %s\n  got  %s\n  want %s\n",name,g,want);fails++;}else printf("ok   %s\n",name);}

int main(void) {
    /* RFC 8439 2.4.2: ChaCha20 encryption */
    uint8_t key[32]; for (int i=0;i<32;i++) key[i]=i;
    uint8_t nonce[12]={0,0,0,0, 0,0,0,0x4a, 0,0,0,0};
    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you "
                     "only one tip for the future, sunscreen would be it.";
    size_t ptlen = strlen(pt);
    uint8_t ct[200];
    chacha20_xor(key, 1, nonce, (const uint8_t*)pt, ct, ptlen);
    check("chacha20 encrypt", ct, (int)ptlen,
        "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
        "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
        "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
        "5af90bbf74a35be6b40b8eedf2785e42874d");

    /* RFC 8439 2.5.2: Poly1305 */
    uint8_t pk[32];
    static const uint8_t pkv[32]={0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b};
    memcpy(pk,pkv,32);
    const char *m = "Cryptographic Forum Research Group";
    uint8_t tag[16];
    poly1305_mac(pk, (const uint8_t*)m, strlen(m), tag);
    check("poly1305", tag, 16, "a8061dc1305136c6c22b8baf0c0127a9");

    /* RFC 8439 2.8.2: full AEAD seal */
    static const uint8_t ak[32]={0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f};
    static const uint8_t an[12]={0x07,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
    static const uint8_t aad[12]={0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
    const char *apt = "Ladies and Gentlemen of the class of '99: If I could offer you "
                      "only one tip for the future, sunscreen would be it.";
    size_t aptlen = strlen(apt);
    uint8_t act[200], atag[16];
    chacha20_poly1305_seal(ak, an, aad, 12, (const uint8_t*)apt, aptlen, act, atag);
    check("aead ciphertext", act, (int)aptlen,
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116");
    check("aead tag", atag, 16, "1ae10b594f09e26a7e902ecbd0600691");

    /* open() round-trip: must verify and decrypt back */
    uint8_t back[200];
    int ok = chacha20_poly1305_open(ak, an, aad, 12, act, aptlen, atag, back);
    if (ok==0 && memcmp(back, apt, aptlen)==0) printf("ok   aead open round-trip\n");
    else { printf("FAIL aead open round-trip (rc=%d)\n", ok); fails++; }
    /* tamper a byte -> must reject */
    act[0]^=1;
    if (chacha20_poly1305_open(ak, an, aad, 12, act, aptlen, atag, back)!=0)
        printf("ok   aead rejects tamper\n");
    else { printf("FAIL aead accepted tampered ciphertext\n"); fails++; }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails?1:0;
}
