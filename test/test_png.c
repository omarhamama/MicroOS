/* Host test for png.c: decode a PNG from stdin, print size + corners. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/png.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }

int main(void) {
    static uint8_t in[1<<21];
    int n = (int)fread(in, 1, sizeof in, stdin);
    static uint32_t px[1<<20];
    int w, h;
    if (png_decode(in, n, px, 1<<20, &w, &h)) { printf("decode FAIL\n"); return 1; }
    printf("%dx%d\n", w, h);
    printf("TL=%06x TR=%06x BL=%06x BR=%06x C=%06x\n",
           px[0], px[w-1], px[(h-1)*w], px[(h-1)*w + w-1], px[(h/2)*w + w/2]);
    return 0;
}
