/* Host test for inflate.c: read a gzip stream on stdin, inflate, write
 * to stdout. Compare against the original with the shell. */
#include <stdio.h>
#include <unistd.h>
#include "../src/inflate.h"

int main(int argc, char **argv) {
    static uint8_t in[1<<20], out[1<<22];
    int n = (int)fread(in, 1, sizeof in, stdin);
    int r;
    if (argc > 1 && argv[1][0] == 'z') r = zlib_inflate(in, n, out, sizeof out);
    else                               r = gzip_inflate(in, n, out, sizeof out);
    if (r < 0) { fprintf(stderr, "inflate failed\n"); return 1; }
    fwrite(out, 1, r, stdout);
    return 0;
}
