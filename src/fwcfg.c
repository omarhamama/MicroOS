/*
 * fwcfg.c — driver for QEMU's fw_cfg ("firmware configuration") device.
 *
 * fw_cfg is a tiny side-channel between QEMU and the guest: a list of
 * named FILES (like "etc/ramfb" or "bootorder") that firmware can read
 * — and a few that it can WRITE to configure the machine. Our DTB
 * lists it at 0x0902_0000 (`dtb` shows the node).
 *
 * Interface (MMIO flavor):
 *   +0x08  selector  (16-bit, BIG-endian) — choose an entry
 *   +0x00  data      — read the selected entry's bytes, streaming
 *   +0x10  DMA       (64-bit, BIG-endian) — point it at a descriptor
 *                     and QEMU performs a whole read/write in one go
 *
 * Everything is big-endian (firmware-land tradition, like the DTB).
 *
 * Why we care: the "ramfb" display device has no register block at
 * all. You configure it by DMA-WRITING a small struct into the
 * "etc/ramfb" fw_cfg file: framebuffer address, resolution, format.
 * QEMU then scans out YOUR RAM as the screen.
 */

#include "fwcfg.h"
#include "lib.h"

#define FWCFG_BASE  0x09020000UL
#define FWCFG_DATA      (FWCFG_BASE + 0x00)
#define FWCFG_SELECTOR  (FWCFG_BASE + 0x08)
#define FWCFG_DMA       (FWCFG_BASE + 0x10)

#define FW_CFG_FILE_DIR 0x0019      /* the directory of named files */

#define DMA_CTL_ERROR   1u
#define DMA_CTL_READ    2u
#define DMA_CTL_SELECT  8u
#define DMA_CTL_WRITE   16u

struct fwcfg_file {                 /* one 64-byte directory entry */
    uint32_t size;                  /* big-endian */
    uint16_t select;                /* big-endian */
    uint16_t reserved;
    char     name[56];
};

struct fwcfg_dma {                  /* the DMA descriptor, all BE */
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t be32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
static uint64_t be64(uint64_t v)
{
    return ((uint64_t)be32((uint32_t)v) << 32) | be32((uint32_t)(v >> 32));
}

static void select_entry(uint16_t key)
{
    *(volatile uint16_t *)FWCFG_SELECTOR = be16(key);
}

static void read_bytes(void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = *(volatile uint8_t *)FWCFG_DATA;     /* streaming reads */
}

uint16_t fwcfg_find(const char *name)
{
    select_entry(FW_CFG_FILE_DIR);

    uint32_t count;
    read_bytes(&count, 4);
    count = be32(count);
    if (count > 256)
        return 0;                   /* implausible: not a fw_cfg device? */

    for (uint32_t i = 0; i < count; i++) {
        struct fwcfg_file f;
        read_bytes(&f, sizeof(f));
        if (strcmp(f.name, name) == 0)
            return be16(f.select);
    }
    return 0;
}

int fwcfg_dma_write(uint16_t selector, const void *buf, uint32_t len)
{
    /* `volatile static` so the compiler really writes the fields and
     * QEMU really sees them — the device reads this struct from RAM. */
    static volatile struct fwcfg_dma dma;

    dma.control = be32(((uint32_t)selector << 16) | DMA_CTL_SELECT | DMA_CTL_WRITE);
    dma.length  = be32(len);
    dma.address = be64((uint64_t)buf);

    asm volatile("dsb sy" ::: "memory");

    /* Hand the descriptor's address to the device — BE, high half
     * first; the write to the LOW half is the trigger. */
    uint64_t a = (uint64_t)&dma;
    *(volatile uint32_t *)(FWCFG_DMA)     = be32((uint32_t)(a >> 32));
    *(volatile uint32_t *)(FWCFG_DMA + 4) = be32((uint32_t)a);

    asm volatile("dsb sy" ::: "memory");
    return (be32(dma.control) & DMA_CTL_ERROR) ? -1 : 0;
}
