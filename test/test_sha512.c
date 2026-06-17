/* Host test for sha512.c — FIPS 180-4 examples. */
#include <stdio.h>
#include <string.h>
#include "../src/sha512.h"

static int fails=0;
static void hex(const uint8_t*b,int n,char*o){static const char*h="0123456789abcdef";for(int i=0;i<n;i++){o[i*2]=h[b[i]>>4];o[i*2+1]=h[b[i]&15];}o[n*2]=0;}
static void check(const char*nm,const uint8_t*g,int n,const char*w){char s[200];hex(g,n,s);if(strcmp(s,w)){printf("FAIL %s\n  got  %s\n  want %s\n",nm,s,w);fails++;}else printf("ok   %s\n",nm);}

int main(void){
    uint8_t d[64];
    sha512("abc",3,d);
    check("sha512(abc)",d,64,
      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
      "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    sha384("abc",3,d);
    check("sha384(abc)",d,48,
      "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
      "8086072ba1e7cc2358baeca134c825a7");
    printf(fails?"\n%d FAILED\n":"\nALL PASS\n",fails);
    return fails?1:0;
}
