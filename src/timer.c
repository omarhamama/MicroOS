/*
 * timer.c — the ARM Generic Timer: the heartbeat of the OS.
 *
 * Every ARM64 CPU has a built-in timer that counts up at a fixed
 * frequency and can raise an interrupt when a deadline passes. This is
 * the most important interrupt in any OS: it's what guarantees the
 * kernel regularly regains control no matter what the CPU was doing.
 * Preemptive multitasking is exactly this — a timer tick where the
 * handler decides to resume a *different* task.
 *
 * Unlike the UART and GIC, the timer is not memory-mapped: it lives in
 * "system registers" accessed with the dedicated mrs/msr instructions:
 *
 *   CNTFRQ_EL0    counter frequency in Hz (read-only; QEMU: 62.5 MHz)
 *   CNTV_TVAL_EL0 countdown value — interrupt fires when it hits 0
 *   CNTV_CTL_EL0  control: bit 0 enables the timer
 *
 * (The 'V' means we use the *virtual* timer, which is the one that's
 * always available to a kernel at EL1. Its interrupt is PPI 27.)
 */

#include "timer.h"
#include "gic.h"

#define VIRTUAL_TIMER_IRQ  27u

static uint64_t freq;             /* counter ticks per second */

/* Written by the interrupt handler, read by normal code — `volatile`
 * tells the compiler it can change at any moment. */
static volatile uint64_t ticks;

void timer_init(void)
{
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    gic_enable_interrupt(VIRTUAL_TIMER_IRQ);

    /* Arm the first deadline: freq/TIMER_HZ counter-ticks = 10 ms. */
    asm volatile("msr cntv_tval_el0, %0" :: "r"(freq / TIMER_HZ));
    asm volatile("msr cntv_ctl_el0, %0"  :: "r"(1UL));
}

void timer_handle_irq(void)
{
    ticks++;
    /* The timer is one-shot: writing TVAL re-arms the next deadline. */
    asm volatile("msr cntv_tval_el0, %0" :: "r"(freq / TIMER_HZ));
}

uint64_t timer_ticks(void)
{
    return ticks;
}

uint64_t timer_uptime_ms(void)
{
    return ticks * (1000 / TIMER_HZ);
}
