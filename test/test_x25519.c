/* Host test for x25519.c — RFC 7748 vectors. */
#include <stdio.h>
#include <string.h>
#include "../src/x25519.h"

static int fails=0;
static void hex(const uint8_t*b,int n,char*o){static const char*h="0123456789abcdef";for(int i=0;i<n;i++){o[i*2]=h[b[i]>>4];o[i*2+1]=h[b[i]&15];}o[n*2]=0;}
static void check(const char*nm,const uint8_t*g,int n,const char*w){char s[80];hex(g,n,s);if(strcmp(s,w)){printf("FAIL %s\n  got  %s\n  want %s\n",nm,s,w);fails++;}else printf("ok   %s\n",nm);}
static void unhex(const char*h,uint8_t*o,int n){for(int i=0;i<n;i++){int hi=h[i*2],lo=h[i*2+1];hi=hi<='9'?hi-'0':(hi|32)-'a'+10;lo=lo<='9'?lo-'0':(lo|32)-'a'+10;o[i]=hi*16+lo;}}

int main(void){
    uint8_t scalar[32], u[32], out[32];

    /* RFC 7748 section 5.2, first test vector */
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
    x25519(out, scalar, u);
    check("x25519 vector 1", out, 32,
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

    /* RFC 7748 section 6.1: Alice's public key from her private key */
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", scalar, 32);
    x25519_base(out, scalar);
    check("x25519 base (Alice pub)", out, 32,
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");

    /* Full ECDH: Alice priv * Bob pub == Bob priv * Alice pub == shared */
    uint8_t apriv[32], bpriv[32], apub[32], bpub[32], s1[32], s2[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", apriv, 32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bpriv, 32);
    x25519_base(apub, apriv);
    x25519_base(bpub, bpriv);
    x25519(s1, apriv, bpub);
    x25519(s2, bpriv, apub);
    if (memcmp(s1, s2, 32)==0) {
        check("ecdh shared", s1, 32,
            "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    } else { printf("FAIL ecdh: s1 != s2\n"); fails++; }

    printf(fails?"\n%d FAILED\n":"\nALL PASS\n", fails);
    return fails?1:0;
}
