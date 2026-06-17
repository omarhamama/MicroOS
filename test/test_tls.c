/* Host harness for tls.c: provides the kernel's transport + RNG +
 * allocator with POSIX equivalents, then does an HTTPS GET.
 * argv: host ip port  (e.g. micro.test 127.0.0.1 4443) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "../src/tls.h"

/* ---- shims the kernel normally provides ---- */
int ct_memcmp(const void*a,const void*b,size_t n){const unsigned char*p=a,*q=b;unsigned char d=0;for(size_t i=0;i<n;i++)d|=p[i]^q[i];return d;}
void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
void  tls_random_bytes(unsigned char *o, size_t n){ int f=open("/dev/urandom",O_RDONLY); read(f,o,n); close(f); }

static int g_sock = -1;
int tcp_connect(const unsigned char ip[4], unsigned short port){
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons(port);
    a.sin_addr.s_addr = htonl((ip[0]<<24)|(ip[1]<<16)|(ip[2]<<8)|ip[3]);
    struct timeval tv={10,0}; setsockopt(g_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    return connect(g_sock,(struct sockaddr*)&a,sizeof a)==0?0:-1;
}
int tcp_send(const void*b,unsigned int n){ return (int)write(g_sock,b,n); }
int tcp_recv(void*b,unsigned int max,unsigned int to){ (void)to; return (int)read(g_sock,b,max); }
void tcp_close(void){ if(g_sock>=0) close(g_sock); g_sock=-1; }

int main(int argc,char**argv){
    if(argc<4){ printf("usage: test_tls host ip port\n"); return 2; }
    const char *host=argv[1];
    unsigned int a,b,c,d; sscanf(argv[2],"%u.%u.%u.%u",&a,&b,&c,&d);
    unsigned char ip[4]={a,b,c,d};
    unsigned short port=atoi(argv[3]);

    struct tls_info info;
    struct tls_conn *t = tls_connect(host, ip, port, 0 /*skip date*/, &info);
    printf("cipher=%s group=%s\n", info.cipher?info.cipher:"?", info.group?info.group:"?");
    printf("sigscheme=%s\n", info.sigscheme?info.sigscheme:"?");
    if(!t){ printf("HANDSHAKE FAILED: %s\n", info.err); return 1; }
    printf("HANDSHAKE OK  subject=%s issuer=%s anchor=%s\n", info.subject, info.issuer, info.anchor);

    char req[256];
    snprintf(req,sizeof req,"GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",host);
    tls_write(t, req, strlen(req));

    char buf[8192]; int total=0, n;
    while((n=tls_read(t, buf, sizeof buf-1))>0){
        buf[n]=0;
        if(total==0){ char*nl=strstr(buf,"\r\n"); printf("first line: %.*s\n", nl?(int)(nl-buf):n, buf); }
        total+=n;
    }
    printf("decrypted %d bytes of application data\n", total);
    tls_close(t);
    return 0;
}
