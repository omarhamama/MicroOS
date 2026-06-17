/*
 * cat.c — copy files (or stdin) to stdout.
 *
 * The purest demonstration of file descriptors: cat doesn't know if
 * fd 1 is the screen, a file, or a pipe to `wc`. It just read()s a
 * source and write()s fd 1. With no arguments it copies stdin — which
 * is how `cat | something` or `cat` reading the keyboard works.
 */

#include "usys.h"
#include "ulib.h"

static void copy(int fd)
{
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        copy(0);                        /* no files: copy stdin */
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("cat: cannot open %s\n", argv[i]);
            continue;
        }
        copy(fd);
        close(fd);
    }
    return 0;
}
