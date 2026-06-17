#pragma once

#include <stdint.h>

int      dtb_init(uint64_t addr);   /* validate + scan; 0 = ok */
uint64_t dtb_ram_base(void);        /* 0 if no valid DTB */
uint64_t dtb_ram_size(void);
uint64_t dtb_uart_base(void);
int      dtb_virtio_slots(void);
void     dtb_dump(void);            /* print the whole tree (the `dtb` cmd) */

/* Find a "simple-framebuffer" the bootloader left running (R36S/RK3326).
 * Returns 0 + fills the outputs, or -1. Works without dtb_init. */
int      dtb_find_simplefb(const void *dtb, uint64_t *addr, uint32_t *w,
                           uint32_t *h, uint32_t *stride);
