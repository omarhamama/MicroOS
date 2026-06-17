/*
 * power.c — shut down / restart the machine via PSCI.
 *
 * PSCI calls are an `hvc #0` with the function id in x0 (see smp.c, which
 * uses the same conduit to bring core 1 online). The two we want:
 *
 *   SYSTEM_OFF   (0x84000008)  — power the machine down (QEMU exits)
 *   SYSTEM_RESET (0x84000009)  — reboot it
 *
 * MicroFS is write-through and journaled, so files are already on disk by
 * the time either of these is called — no flush/sync step is needed.
 */

#include <stdint.h>
#include "power.h"
#include "kprintf.h"

#define PSCI_SYSTEM_OFF    0x84000008u
#define PSCI_SYSTEM_RESET  0x84000009u

static void psci(uint32_t fn)
{
    register unsigned long x0 asm("x0") = fn;
    asm volatile("hvc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
}

void power_off(void)
{
    kprintf("\n[power] shutting down — PSCI SYSTEM_OFF\n");
    psci(PSCI_SYSTEM_OFF);
    for (;;) asm volatile("wfi");        /* never reached; halt if it is */
}

void power_reset(void)
{
    kprintf("\n[power] restarting — PSCI SYSTEM_RESET\n");
    psci(PSCI_SYSTEM_RESET);
    for (;;) asm volatile("wfi");
}
