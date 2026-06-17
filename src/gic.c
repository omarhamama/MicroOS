/*
 * gic.c — driver for the GIC (Generic Interrupt Controller), version 2.
 *
 * Devices don't talk to the CPU directly. When the timer expires or the
 * UART receives a byte, the device raises a line into the GIC, and the
 * GIC decides whether/when to interrupt the CPU. It has two halves:
 *
 *   Distributor (GICD) — one per system. Collects all interrupt lines,
 *       lets you enable/disable each one, routes them to CPU cores.
 *
 *   CPU interface (GICC) — one per core. The core asks it "who
 *       interrupted me?" (IAR) and tells it "done handling" (EOIR).
 *
 * Interrupt IDs on ARM are conventionally numbered:
 *      0-15  SGIs — software-generated (cores poking each other)
 *     16-31  PPIs — private to each core (e.g. that core's timer)
 *     32+    SPIs — shared peripherals (UART, disks, network...)
 *
 * Like the UART, it's all memory-mapped registers. Addresses are from
 * QEMU's virt board memory map.
 */

#include "gic.h"
#include "board.h"

#define GICD_BASE  BOARD_GICD_BASE   /* virt 0x08000000 / Pi GIC-400 0xFF841000 */
#define GICC_BASE  BOARD_GICC_BASE   /* virt 0x08010000 / Pi GIC-400 0xFF842000 */

#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER  ((volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_ITARGETSR  ((volatile uint8_t  *)(GICD_BASE + 0x800))

#define GICC_CTLR       (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR        (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR        (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR       (*(volatile uint32_t *)(GICC_BASE + 0x010))

void gic_init(void)
{
    GICD_CTLR = 1;          /* distributor: start forwarding interrupts */

    /* Shared interrupts (SPIs, id 32+) carry a TARGET byte: a bitmask
     * of which cores may receive them. With one CPU this is moot and
     * resets target-everything; boot a second core and suddenly the
     * reset value of 0 means "deliver to NOBODY" — the disk interrupt
     * vanishes and every blocked task sleeps forever. Found the day
     * -smp 2 arrived. Route all SPIs to core 0, which runs the kernel. */
    for (int i = 32; i < 256; i++)
        GICD_ITARGETSR[i] = 0x01;

    /* Priority mask: only interrupts with priority HIGHER (numerically
     * lower) than this reach the CPU. 0xFF = "let everything through". */
    GICC_PMR = 0xFF;

    GICC_CTLR = 1;          /* CPU interface: start signaling the core */
}

void gic_enable_interrupt(uint32_t intid)
{
    /* ISENABLER is a bitmap: 32 interrupts per 32-bit register.
     * Writing 1 to a bit enables that interrupt (writing 0 does
     * nothing — there's a separate ICENABLER for disabling, which
     * makes enable/disable race-free without read-modify-write). */
    GICD_ISENABLER[intid / 32] = 1u << (intid % 32);
}

uint32_t gic_acknowledge(void)
{
    /* Reading IAR returns the highest-priority pending interrupt ID
     * and marks it "active". 1023 means spurious — ignore it. */
    return GICC_IAR & 0x3FFu;
}

void gic_end_of_interrupt(uint32_t intid)
{
    GICC_EOIR = intid;
}
