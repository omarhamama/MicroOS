/*
 * wc.c — count lines, words, and bytes.
 *
 * The classic end-of-pipeline program: it reads stdin and reports. In
 * `cat file | wc -l`, wc never opens the file — it just reads fd 0,
 * which the shell wired to the pipe from cat. wc has no idea cat
 * exists; that decoupling is the whole point of pipes.
 */

#include "usys.h"
#include "ulib.h"

int main(int argc, char **argv)
{
    int lines = 0, words = 0, bytes = 0, inword = 0;
    char buf[512];
    long n;

    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            bytes++;
            char c = buf[i];
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                inword = 0;
            } else if (!inword) {
                inword = 1;
                words++;
            }
        }
    }

    /* -l prints just the line count (what the preview shows). */
    if (argc > 1 && ustrcmp(argv[1], "-l") == 0)
        printf("%d\n", lines);
    else
        printf("%d %d %d\n", lines, words, bytes);
    return 0;
}
