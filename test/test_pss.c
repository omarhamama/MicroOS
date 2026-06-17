/* Host test for RSA-PSS — argv: n_hex e_hex sig_hex message */
#include <stdio.h>
#include <string.h>
#include "../src/rsa.h"
#include "../src/sha256.h"

int ct_memcmp(const void*a,const void*b,size_t n){const unsigned char*p=a,*q=b;unsigned char d=0;for(size_t i=0;i<n;i++)d|=p[i]^q[i];return d;}
static int unhex(const char*h,uint8_t*o){int n=strlen(h)/2;for(int i=0;i<n;i++){int hi=h[i*2],lo=h[i*2+1];hi=hi<='9'?hi-'0':(hi|32)-'a'+10;lo=lo<='9'?lo-'0':(lo|32)-'a'+10;o[i]=hi*16+lo;}return n;}

int main(int argc,char**argv){
    if(argc<5){printf("usage: test_pss n e sig msg\n");return 2;}
    uint8_t n[600],e[8],sig[600];
    int nlen=unhex(argv[1],n),elen=unhex(argv[2],e),siglen=unhex(argv[3],sig);
    uint8_t hash[32]; sha256(argv[4],strlen(argv[4]),hash);
    int rc=rsa_pss_verify(n,nlen,e,elen,sig,siglen,hash,32);
    printf("pss valid: %s\n", rc==0?"ok   YES":"FAIL NO");
    hash[5]^=1;
    int rc2=rsa_pss_verify(n,nlen,e,elen,sig,siglen,hash,32);
    printf("pss rejects tamper: %s\n", rc2!=0?"ok   YES":"FAIL NO");
    return (rc==0&&rc2!=0)?0:1;
}
