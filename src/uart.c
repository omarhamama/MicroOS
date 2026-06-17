/*
 * uart.c — driver for the PL011 UART (serial port), interrupt-driven.
 *
 * This is our keyboard and screen in one. QEMU's "virt" board has no
 * VGA card by default; instead it wires a serial port to your terminal
 * window. Everything you type goes in through this device, everything
 * we print goes out through it.
 *
 * THE key idea of this file: there are no functions to call to talk to
 * hardware. Hardware is controlled by reading and writing special
 * physical addresses — "memory-mapped I/O" (MMIO). The board maps the
 * PL011's registers at physical address 0x0900_0000, so
 * `*(volatile uint32_t *)0x09000000 = 'A'` literally sends an 'A' out
 * the serial line.
 *
 * INPUT used to be polled: uart_getc() spun in a loop asking "char
 * yet? char yet?" millions of times a second — 100% CPU to do nothing.
 * Now it's interrupt-driven, the way real OSes work:
 *
 *   keystroke -> UART raises IRQ 33 -> GIC -> vectors.S -> handle_irq()
 *      -> uart_handle_irq() pushes the char into a RING BUFFER
 *
 *   uart_getc() pops from that buffer; while it's empty it executes
 *   `wfi` (Wait For Interrupt) — the CPU genuinely SLEEPS, drawing
 *   near-zero host CPU, until any interrupt arrives.
 *
 * The ring buffer is the classic producer/consumer structure: the
 * interrupt handler is the producer (writes at `head`), normal code is
 * the consumer (reads at `tail`), and they never touch each other's
 * index — which is what makes it safe without locks on one core.
 */

#include <stdint.h>
#include "uart.h"
#include "gic.h"
#include "gui.h"
#include "task.h"
#include "board.h"

#define UART0_BASE  BOARD_UART_BASE     /* PL011: 0x09000000 (virt) / 0xFE201000 (Pi) */
#define UART0_IRQ   BOARD_UART_IRQ

/*
 * Two UART flavours behind one tiny set of helpers. QEMU virt and the
 * Raspberry Pi have a PL011; the Rockchip RK3326 (R36S) has a DesignWare
 * 8250 (16550-style, registers 32-bit spaced). The shared code below
 * (ring buffer, getc, putc) only calls hw_*().
 */
#define REG(off)    (*(volatile uint32_t *)(UART0_BASE + (off)))

#if BOARD_UART_DW
/* DesignWare 8250 (RK3326). reg-shift 2 → registers at offset*4. */
#define DW_RBR  0x00    /* read: received byte   */
#define DW_THR  0x00    /* write: transmit byte  */
#define DW_IER  0x04    /* interrupt enable      */
#define DW_FCR  0x08    /* FIFO control (write)  */
#define DW_LSR  0x14    /* line status           */
#define LSR_DR    (1u << 0)     /* data ready            */
#define LSR_THRE  (1u << 5)     /* TX holding reg empty  */

static inline int  hw_tx_full(void)  { return !(REG(DW_LSR) & LSR_THRE); }
static inline void hw_write(char c)  { REG(DW_THR) = (uint8_t)c; }
static inline int  hw_rx_empty(void) { return !(REG(DW_LSR) & LSR_DR); }
static inline char hw_read(void)     { return (char)(REG(DW_RBR) & 0xFF); }
static inline void hw_init(void)     { REG(DW_FCR) = 0x07; }   /* enable+clear FIFOs */
static inline void hw_rx_irq_on(void){ REG(DW_IER) = 0x01; }   /* ERBFI */
static inline void hw_irq_clear(void){ (void)REG(0x08); }      /* read IIR */

#else
/* PL011 (QEMU virt, Raspberry Pi). */
#define PL_DR    0x00
#define PL_FR    0x18
#define PL_CR    0x30
#define PL_IMSC  0x38
#define PL_ICR   0x44
#define FR_TXFF  (1u << 5)
#define FR_RXFE  (1u << 4)
#define INT_RX   (1u << 4)
#define INT_RT   (1u << 6)

static inline int  hw_tx_full(void)  { return REG(PL_FR) & FR_TXFF; }
static inline void hw_write(char c)  { REG(PL_DR) = (uint32_t)c; }
static inline int  hw_rx_empty(void) { return REG(PL_FR) & FR_RXFE; }
static inline char hw_read(void)     { return (char)(REG(PL_DR) & 0xFF); }
static inline void hw_init(void)     { REG(PL_CR) = (1u<<0)|(1u<<8)|(1u<<9); }
static inline void hw_rx_irq_on(void){ REG(PL_ICR) = 0x7FF; REG(PL_IMSC) = INT_RX | INT_RT; }
static inline void hw_irq_clear(void){ REG(PL_ICR) = INT_RX | INT_RT; }
#endif

/* ---- the keyboard ring buffer -------------------------------------- */

#define RX_BUF_SIZE 256         /* generous typeahead; FIFO was 16 */

static volatile char     rx_buf[RX_BUF_SIZE];
static volatile uint32_t rx_head;   /* producer (IRQ handler) writes here */
static volatile uint32_t rx_tail;   /* consumer (uart_getc) reads here */

void uart_init(void)
{
    /* The bootloader/QEMU already set the baud rate; just (re)enable. */
    hw_init();
}

/* Called once the GIC is up (interrupts need a route to the CPU). */
void uart_irq_init(void)
{
    gic_enable_interrupt(UART0_IRQ);
    hw_rx_irq_on();                 /* interrupt us on received data */

    /*
     * Subtle but classic: a character may have arrived BEFORE this
     * function ran (QEMU delivers piped input the instant the machine
     * powers on, while we're still printing the banner). Its interrupt
     * fired into the void — and with the holding register now full, no
     * NEW interrupt will ever come. Without this manual first drain,
     * the OS sleeps in wfi forever waiting for an event that already
     * happened. Every interrupt-driven driver has a moment like this:
     * "handle what arrived before we started listening".
     */
    uart_handle_irq();
}

/* IRQ context! Keep it short: move bytes, get out.
 *
 * ORDER MATTERS: acknowledge FIRST, then drain. The first version
 * drained first and cleared last — and a byte that arrived in the gap
 * between "FIFO looks empty" and the clear had its interrupt wiped
 * while it sat in the FIFO. No interrupt, FIFO full, QEMU politely
 * waiting for us to read: input dead forever. Clear-first has no such
 * window: a byte landing after the clear raises a fresh interrupt; a
 * byte landing before it is caught by the drain. (A genuine race bug,
 * found the way they usually are: a test that froze one build in
 * three.) */
/* Push one character into the console input queue and wake any reader.
 * This is the ONE place input enters the system — and now there are TWO
 * sources: the serial port (this file's IRQ) and the GUI keyboard
 * (virtio_input.c). Both call here, so the shell reads keystrokes from
 * the QEMU window and the serial terminal identically. (Both run in
 * IRQ context on core 0, never concurrently, so no lock is needed.) */
void uart_input_push(char c)
{
    uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {          /* drop on overflow */
        rx_buf[rx_head] = c;
        rx_head = next;
    }
    task_wakeup((void *)rx_buf);
}

void uart_handle_irq(void)
{
    hw_irq_clear();

    while (!hw_rx_empty())           /* reading the data reg pops each char */
        uart_input_push(hw_read());
}

int uart_haschar(void)
{
    return rx_tail != rx_head;
}

void uart_debug_state(uint32_t *fr, uint32_t *ris, uint32_t *mis,
                      uint32_t *head, uint32_t *tail)
{
#if BOARD_UART_DW
    *fr = REG(DW_LSR); *ris = 0; *mis = 0;
#else
    *fr = REG(PL_FR); *ris = REG(0x3C); *mis = REG(0x40);
#endif
    *head = rx_head;
    *tail = rx_tail;
}

char uart_getc(void)
{
    /*
     * Generation three of this function. v1 busy-polled (100% CPU).
     * v2 looped on wfi (CPU slept, but the task stayed RUNNING and
     * re-checked on every interrupt). v3 BLOCKS: the task leaves the
     * run queue entirely and the UART interrupt wakes it — the kernel
     * does zero work for an idle shell. The mask/loop shape is the
     * lost-wakeup dance documented in task.c.
     */
    asm volatile("msr daifset, #2");
    while (rx_tail == rx_head)
        task_block((void *)rx_buf);
    asm volatile("msr daifclr, #2");

    char c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return c;
}

void uart_putc(char c)
{
    /* Every character that leaves the serial port also lands in the
     * GUI's console window — one output stream, two screens. */
    gui_console_feed(c);

    /* Output stays polled: QEMU drains it instantly, and "wait until
     * there's room" almost never actually waits. (Buffered, interrupt-
     * driven TX is a nice exercise though.) */
    while (hw_tx_full()) { }
    hw_write(c);
}

void uart_puts(const char *s)
{
    while (*s) {
        /* Serial terminals want "\r\n" for a new line, C strings use '\n'.
         * Translate on the way out so kernel code can stay idiomatic. */
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}
