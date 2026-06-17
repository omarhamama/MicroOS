/*
 * rogue.c — a program with bad intentions, for science.
 *
 * It tries to read KERNEL memory. With per-process page tables, the
 * kernel isn't even mapped into this process's address space — so the
 * access faults, the kernel kills rogue, and every other process
 * carries on. Run `exec /bin/rogue` and watch isolation work.
 */

#include "usys.h"
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("(rogue) trying to read kernel memory at 0x40080000...\n");

    volatile unsigned long *kernel = (unsigned long *)0x40080000UL;
    unsigned long loot = *kernel;           /* faults: not in MY tables */
    (void)loot;

    puts("(rogue) ...if you see this, isolation is broken\n");
    return 0;
}
