/*
 * kernel.c — kernel_main(), where boot.S hands over to C.
 *
 * Boot order matters and is worth reading as a story:
 *   1. UART first       — without a console we're debugging blind
 *   2. GIC next         — the interrupt controller must be listening...
 *   3. timer + UART IRQ — ...before any device starts raising interrupts
 *   4. unmask IRQs      — only now does the CPU accept interruptions
 *   5. mount the fs     — build the in-RAM file tree (needs kmalloc)
 *   6. run the shell    — forever
 */

#include <stdint.h>
#include "uart.h"
#include "kprintf.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"
#include "task.h"
#include "fs.h"
#include "virtio_blk.h"
#include "virtio_input.h"
#include "virtio_snd.h"
#include "proc.h"
#include "smp.h"
#include "net.h"
#include "mfs.h"
#include "dtb.h"
#include "board.h"
#include "fb.h"
#include "gui.h"
#include "browser.h"
#include "player.h"
#include "fonts.h"
#include "shell.h"

static const char *banner =
    "\n"
    "  __  __ _              ___  ____\n"
    " |  \\/  (_) ___ _ __ ___/ _ \\/ ___|\n"
    " | |\\/| | |/ __| '__/ _ \\ | | \\___ \\\n"
    " | |  | | | (__| | | (_) | |_| |___) |\n"
    " |_|  |_|_|\\___|_|  \\___/ \\___/|____/\n"
    "\n";

void kernel_main(uint64_t dtb)
{
    uart_init();
    uart_puts(banner);
    kprintf("Hello from your own kernel!\n\n");

    mmu_init();         /* virtual memory + caches before anything else */

    gic_init();
    timer_init();
    uart_irq_init();    /* keyboard input becomes interrupt-driven */
    task_init();        /* this very code path becomes task 0, "shell" */

    /* The 'I' bit in DAIF masks IRQs; clearing it (daifclr, #2) is the
     * moment the timer interrupt can actually reach handle_irq(). */
    asm volatile("msr daifclr, #2");

    kprintf("[boot] interrupt controller up, timer ticking at %u Hz\n", TIMER_HZ);
    kprintf("[boot] console on UART IRQ %u — the CPU now sleeps when idle\n",
            BOARD_UART_IRQ);

    /* Ask the firmware's device tree what machine this actually is,
     * instead of trusting our hard-coded addresses. (`dtb` to browse.)
     * QEMU's raspi board doesn't hand us a DTB pointer in x0, and the Pi
     * build trusts board.h anyway, so this is virtio-boards only. */
#if BOARD_HAS_VIRTIO
    if (dtb_init(dtb) == 0)
        kprintf("[boot] device tree: RAM %lu MiB @ 0x%lx, UART @ 0x%lx, "
                "%d virtio slots\n",
                dtb_ram_size() >> 20, dtb_ram_base(),
                dtb_uart_base(), dtb_virtio_slots());
    else
        kprintf("[boot] no valid device tree at %p!\n", (void *)dtb);
#else
    (void)dtb;
#endif

    fs_init();          /* needs kmalloc, so after memory exists = always */

#if BOARD_HAS_VIRTIO
    /* Mount: if a disk with a MicroFS v2 is attached, the file tree
     * comes from there (a blank disk gets formatted). Every change
     * from here on writes through to disk immediately. */
    if (vblk_init() == 0) {
        kprintf("[boot] virtio disk: %lu KiB at MMIO %p, IRQ %u\n",
                vblk_capacity() / 2, (void *)vblk_mmio_base(), vblk_irq());
        long n = mfs_mount();
        if (n > 0) {
            kprintf("[boot] MicroFS v3 mounted: %ld entries restored\n\n", n);
        } else if (n == 0) {
            fs_populate_defaults();     /* written through as they're made */
            kprintf("[boot] fresh filesystem — starter files created\n\n");
        } else {
            fs_populate_defaults();
            kprintf("[boot] disk unusable — files are RAM-only\n\n");
        }
    } else {
        fs_populate_defaults();
        kprintf("[boot] no disk attached — files are RAM-only\n\n");
    }
#else
    /* No virtio block device on this board (e.g. the Raspberry Pi): the
     * filesystem lives in RAM only until an SD-card driver exists. */
    fs_populate_defaults();
    kprintf("[boot] %s: RAM-only filesystem (no SD driver yet)\n\n", BOARD_NAME);
#endif

    proc_install_binaries();    /* user programs land in /bin */

#if BOARD_HAS_VIRTIO
    if (vinput_init() == 0)
        kprintf("[boot] mouse: virtio tablet, IRQ via shared dispatch\n");

    net_init();         /* NIC + the polling task (prints if found) */

    if (vsnd_init() == 0)
        kprintf("[boot] sound: virtio-snd — try 'beep'\n");
#endif

#if BOARD_FB_SIMPLEFB
    /* R36S/RK3326: reuse the framebuffer the bootloader (U-Boot) already
     * set up, found in the DTB it passed in x0 — sidesteps the Rockchip
     * display controller entirely. */
    {
        uint64_t fbaddr; uint32_t fw, fh, fstride;
        if (dtb_find_simplefb((const void *)dtb, &fbaddr, &fw, &fh, &fstride) == 0) {
            fb_set_simplefb(fbaddr, (int)fstride);
            kprintf("[boot] simple-framebuffer: %ux%u @ 0x%lx, stride %u\n",
                    fw, fh, fbaddr, fstride);
        } else {
            kprintf("[boot] no simple-framebuffer in DTB — serial console only\n");
        }
    }
#endif

    fonts_init();       /* build the system fonts before anything paints */
    gui_init();         /* the desktop, painted by its own kernel task */
    browser_init();     /* the web browser's fetch task + home page */
    player_init();      /* the music player's audio task + playlist scan */

#if BOARD_HAS_PSCI
    smp_init();         /* wake core 1 — see smp.c for the fine print */
#endif

    shell_run();        /* never returns */
}
