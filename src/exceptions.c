/*
 * exceptions.c — the C side of interrupt and fault handling.
 *
 * vectors.S saves all registers and lands here. Three entry points:
 *
 *   handle_irq()           a device wants attention (timer, ...)
 *   handle_sync()          our own code faulted (or ran `brk`)
 *   handle_bad_exception() an exception from a state we never expect
 */

#include <stdint.h>
#include "gic.h"
#include "timer.h"
#include "uart.h"
#include "task.h"
#include "virtio.h"
#include "board.h"
#include "kprintf.h"

void handle_irq(void)
{
    /* Several devices can share the IRQ line — ask the GIC who it was. */
    uint32_t intid = gic_acknowledge();

    switch (intid) {
    case 27:                            /* virtual timer PPI */
        timer_handle_irq();
        /* EOI BEFORE the scheduler may switch us away: if we held the
         * interrupt "active", the GIC would deliver no more timer
         * ticks while another task runs — preemption would die after
         * one switch. (Another classic scheduler bug, free of charge.) */
        gic_end_of_interrupt(intid);
        scheduler_tick();               /* may switch tasks right here */
        return;
    case BOARD_UART_IRQ:                /* UART0 SPI: a keystroke!
                                         * (33 on virt, 153 on the Pi) */
        uart_handle_irq();
        break;
    case GIC_SPURIOUS:                  /* false alarm: must NOT do EOI */
        return;
    default:
        /* disk, network, tablet, sound — anything virtio */
        if (!virtio_dispatch_irq(intid))
            kprintf("[irq] unexpected interrupt %u\n", intid);
        break;
    }

    gic_end_of_interrupt(intid);
}

static void halt_forever(void)
{
    kprintf("System halted. Press Ctrl-A then X to quit QEMU.\n");
    for (;;)
        asm volatile("wfe");
}

/* Crash reports must print even if the fault hit mid-kprintf with the
 * print lock held — drop the lock discipline; we're done being polite. */
static void enter_panic(void)
{
    kprintf_panic_mode();
}

/*
 * Synchronous exception: the instruction at `elr` caused it. The top 6
 * bits of ESR (the "exception class") say what kind of problem it was.
 * Decoding a few common ones turns a mystery hang into a readable
 * crash report — most of debugging an OS is reading these two numbers.
 */
void handle_sync(uint64_t esr, uint64_t elr)
{
    uint32_t ec = (esr >> 26) & 0x3F;
    enter_panic();

    /* FAR_EL1 holds the ADDRESS the faulting instruction touched
     * (ELR holds where the instruction itself lives). The pair is
     * what makes MMU faults debuggable: "code at X touched Y". */
    uint64_t far;
    asm volatile("mrs %0, far_el1" : "=r"(far));

    kprintf("\n*** SYNCHRONOUS EXCEPTION ***\n");
    kprintf("  ESR_EL1 = 0x%lx  (exception class 0x%x)\n", esr, ec);
    kprintf("  ELR_EL1 = 0x%lx  (faulting instruction address)\n", elr);

    switch (ec) {
    case 0x15:
        kprintf("  Cause: SVC system call (no handler yet!)\n");
        break;
    case 0x20:
    case 0x21:
        kprintf("  Cause: instruction abort — tried to EXECUTE 0x%lx\n", far);
        kprintf("         (that memory is not executable: W^X at work)\n");
        break;
    case 0x24:
    case 0x25:
        kprintf("  Cause: data abort — tried to %s address 0x%lx\n",
                (esr & (1u << 6)) ? "WRITE" : "READ", far);
        if (far < 0x1000)
            kprintf("         ...a null pointer dereference!\n");
        break;
    case 0x3C:
        kprintf("  Cause: BRK instruction (deliberate trap)\n");
        break;
    default:
        kprintf("  Cause: see ARMv8 manual, table D17-1\n");
        break;
    }

    halt_forever();
}

void handle_bad_exception(uint64_t index, uint64_t esr, uint64_t elr)
{
    enter_panic();
    kprintf("\n*** UNEXPECTED EXCEPTION (vector slot %lu) ***\n", index);
    kprintf("  ESR_EL1 = 0x%lx, ELR_EL1 = 0x%lx\n", esr, elr);
    halt_forever();
}
