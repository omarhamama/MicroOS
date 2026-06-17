/*
 * hello.c — a real user program, now with command-line arguments.
 *
 * Compiled to its own ELF binary (/bin/hello), loaded into its own
 * address space by the kernel's ELF loader, and entered at EL0 with
 * argc/argv on its stack — exactly like a program on any Unix.
 */

#include "usys.h"
#include "ulib.h"

int main(int argc, char **argv)
{
    printf("(hello) I am a real ELF program loaded from disk.\n");
    printf("(hello) I got %d argument%s:\n", argc, argc == 1 ? "" : "s");
    for (int i = 0; i < argc; i++)
        printf("(hello)   argv[%d] = \"%s\"\n", i, argv[i]);

    printf("(hello) my code is read-only+executable, my data is "
           "writable+no-execute (real W^X).\n");
    return 0;               /* crt0 turns this into exit(0) */
}
