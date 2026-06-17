/*
 * echo.c — the simplest useful program: print the arguments.
 *
 * Proof that argv survived the trip from the shell, through the exec
 * syscall, onto this process's freshly built user stack.
 */

#include "usys.h"
#include "ulib.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        puts(argv[i]);
        if (i < argc - 1)
            puts(" ");
    }
    puts("\n");
    return 0;
}
