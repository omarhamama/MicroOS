#pragma once

/*
 * board.h — the one place that knows which machine we're running on.
 *
 * MicroOS was born on QEMU's "virt" board (virtio everything, RAM at
 * 0x4000_0000). To run NATIVELY on other ARM64 machines the addresses and
 * a few capabilities differ, so everything machine-specific funnels
 * through the constants below. Select the target at build time:
 *
 *     make                 # QEMU virt        (default, fully verified)
 *     make BOARD=raspi4    # Raspberry Pi 4    (verified in QEMU raspi4b)
 *     make BOARD=rk3326    # R36S handheld     (SPECULATIVE — UNVERIFIED)
 *
 * Only addresses, sizes, and capability flags live here — the drivers read
 * these instead of hard-coding a memory map, so the same C builds for all.
 *
 * Capability flags each board defines:
 *   BOARD_HAS_VIRTIO   virtio disk/net/input/sound present?
 *   BOARD_HAS_PSCI     PSCI CPU_ON via HVC works (SMP)?
 *   BOARD_FB_MAILBOX   framebuffer via the VideoCore mailbox (Pi)
 *   BOARD_FB_SIMPLEFB  reuse the bootloader's framebuffer (simple-framebuffer)
 *   BOARD_UART_DW      UART is a DesignWare 8250 (16550-style), not a PL011
 *   BOARD_FB_W/H       native screen size
 */

#include <stdint.h>

#if defined(BOARD_RASPI4)
/* ---- Raspberry Pi 4 (BCM2711) — also QEMU's `raspi4b` ---------------- */
#define BOARD_NAME        "Raspberry Pi 4 (BCM2711)"
#define BOARD_RAM_BASE    0x00000000UL
#define BOARD_RAM_SIZE    (256UL << 20)
#define BOARD_KERNEL_BASE 0x00080000UL
#define BOARD_UART_BASE   0xFE201000UL    /* PL011 UART0 */
#define BOARD_UART_IRQ    153u
#define BOARD_GICD_BASE   0xFF841000UL    /* GIC-400 */
#define BOARD_GICC_BASE   0xFF842000UL
#define BOARD_DEV_BASE    0xC0000000UL
#define BOARD_MBOX_BASE   0xFE00B880UL
#define BOARD_HAS_VIRTIO  0
#define BOARD_HAS_PSCI    0
#define BOARD_FB_MAILBOX  1
#define BOARD_FB_SIMPLEFB 0
#define BOARD_UART_DW     0
#define BOARD_FB_W        1024
#define BOARD_FB_H        768

#elif defined(BOARD_RK3326)
/* ---- R36S handheld — Rockchip RK3326 (Cortex-A35) -------------------- *
 * SPECULATIVE / UNVERIFIED: there is no emulator for this SoC, so none of
 * these addresses or the drivers below have been run. They are taken from
 * the public RK3326 TRM and the mainline rk3326.dtsi. Boot via the
 * device's own U-Boot (`booti`); it passes a DTB in x0 from which we read
 * the framebuffer the bootloader already set up (simple-framebuffer),
 * sidestepping the Rockchip VOP/DSI panel bring-up. The console is the
 * DesignWare 8250 UART2 (the one ArkOS logs to). Addresses to re-check on
 * real hardware: UART IRQ, GIC bases, and the simplefb format. */
#define BOARD_NAME        "R36S (Rockchip RK3326)"
#define BOARD_RAM_BASE    0x00000000UL    /* DDR3L at 0; 1 GiB total       */
#define BOARD_RAM_SIZE    (512UL << 20)   /* map 512 MiB (leave room hi)   */
#define BOARD_KERNEL_BASE 0x00080000UL    /* RAM base + 0x80000 (booti default) */
#define BOARD_UART_BASE   0xFF160000UL    /* UART2 (DesignWare 8250)       */
#define BOARD_UART_IRQ    100u            /* GIC SPI for UART2 (verify!)   */
#define BOARD_GICD_BASE   0xFF8C1000UL    /* GIC-400 distributor           */
#define BOARD_GICC_BASE   0xFF8C2000UL    /* GIC-400 CPU interface         */
#define BOARD_DEV_BASE    0xC0000000UL    /* whole top GiB = MMIO          */
#define BOARD_HAS_VIRTIO  0
#define BOARD_HAS_PSCI    0               /* RK PSCI is SMC (ATF), not HVC */
#define BOARD_FB_MAILBOX  0
#define BOARD_FB_SIMPLEFB 1               /* reuse U-Boot's framebuffer    */
#define BOARD_UART_DW     1               /* 16550-style, not PL011        */
#define BOARD_FB_W        640
#define BOARD_FB_H        480

#else
/* ---- QEMU `virt` (the original, default target) --------------------- */
#define BOARD_NAME        "QEMU virt"
#define BOARD_RAM_BASE    0x40000000UL
#define BOARD_RAM_SIZE    (128UL << 20)
#define BOARD_KERNEL_BASE 0x40080000UL
#define BOARD_UART_BASE   0x09000000UL
#define BOARD_UART_IRQ    33u             /* SPI 1 */
#define BOARD_GICD_BASE   0x08000000UL
#define BOARD_GICC_BASE   0x08010000UL
#define BOARD_DEV_BASE    0x08000000UL
#define BOARD_HAS_VIRTIO  1
#define BOARD_HAS_PSCI    1
#define BOARD_FB_MAILBOX  0
#define BOARD_FB_SIMPLEFB 0
#define BOARD_UART_DW     0
#define BOARD_FB_W        1024
#define BOARD_FB_H        768
#endif
