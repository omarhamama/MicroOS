#pragma once

#include <stdint.h>

#define VBLK_SECTOR_SIZE 512

int      vblk_init(void);           /* 0 = disk ready, -1 = none found */
int      vblk_present(void);
uint64_t vblk_capacity(void);       /* in 512-byte sectors */
uint64_t vblk_mmio_base(void);
uint32_t vblk_irq(void);            /* GIC INTID of the disk, 0 if none */

/* Transfer exactly one 512-byte sector. Return 0 on success. */
int vblk_read(uint64_t sector, void *buf);
int vblk_write(uint64_t sector, const void *buf);
