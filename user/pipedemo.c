/*
 * pipedemo.c — one process feeding another through a pipe.
 *
 * This is exactly the dance a shell does for `echo ... | cat`:
 *   1. pipe() — make the tube, get [read end, write end]
 *   2. fork() — split in two
 *   3. the child points its stdout (fd 1) at the write end (dup2) and
 *      execs echo, which writes normally — into the pipe
 *   4. the parent reads the read end until EOF and prints what it got
 * Closing the unused ends is essential: the reader only sees EOF once
 * EVERY write end is closed.
 */

#include "usys.h"
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int p[2];
    if (pipe(p) < 0) {
        printf("(pipedemo) pipe failed\n");
        return 1;
    }

    if (fork() == 0) {                  /* child: the writer */
        dup2(p[1], 1);                  /* stdout now goes into the pipe */
        close(p[0]);
        close(p[1]);
        char *a[] = { "/bin/echo", "data", "through", "a", "pipe", 0 };
        exec("/bin/echo", a);
        return 1;
    }

    /* parent: the reader */
    close(p[1]);                        /* MUST close our write end for EOF */
    printf("(pipedemo) parent received: ");
    char buf[128];
    long n;
    while ((n = read(p[0], buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(p[0]);
    wait(0);
    return 0;
}
