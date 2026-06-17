/* Host test for jpeg.c: decode a JPEG from stdin, write raw RGB to
 * stdout, print "w h" to stderr. Compared against PIL by run.sh/python. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/jpeg.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }

int main(void) {
    static uint8_t in[1<<21];
    int n = (int)fread(in, 1, sizeof in, stdin);
    static uint32_t px[1<<20];
    int w, h;
    if (jpeg_decode(in, n, px, 1<<20, &w, &h)) { fprintf(stderr, "FAIL\n"); return 1; }
    fprintf(stderr, "%d %d\n", w, h);
    for (int i = 0; i < w*h; i++) {
        uint8_t rgb[3] = { (px[i]>>16)&255, (px[i]>>8)&255, px[i]&255 };
        fwrite(rgb, 1, 3, stdout);
    }
    return 0;
}
