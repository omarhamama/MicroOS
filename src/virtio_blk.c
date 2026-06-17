/*
 * virtio_blk.c — a disk driver (virtio block device over MMIO).
 *
 * This was the first "real driver" in MicroOS, with all three things
 * every modern storage driver has:
 *
 *   1. DISCOVERY  — find the device and check what it is
 *   2. HANDSHAKE  — negotiate features, agree on a protocol
 *   3. DMA RINGS  — the device reads/writes OUR RAM directly; we just
 *                   exchange descriptions of what to transfer
 *
 * What is virtio? VMs could emulate a real SATA controller, but that's
 * slow and complicated for everyone. Virtio is an honest standard
 * interface designed FOR virtual machines — and once the network,
 * tablet and sound devices joined, the shared 80% of it moved to
 * virtio.c. What stays here is the block personality:
 *
 *   a request = THREE chained descriptors
 *     [0] header  (read or write? which sector?)   device reads it
 *     [1] data    (512 bytes)                      device reads/writes
 *     [2] status  (1 byte)                         device writes: 0=ok
 *
 * Note what's missing: no "data register". The device pulls bytes
 * straight out of our RAM at addresses we put in descriptors. That's
 * DMA — the CPU never touches the data.
 */

#include "virtio_blk.h"
#include "virtio.h"
#include "task.h"
#include "kprintf.h"

#define QSIZE 8

static struct virtio_dev dev;
static struct virtq vq;

/* Ring memory: sizes/alignments dictated by the virtio spec. */
static struct virtq_desc desc[QSIZE]        __attribute__((aligned(16)));
static uint8_t avail_mem[6 + 2 * QSIZE]     __attribute__((aligned(2)));
static uint8_t used_mem[6 + 8 * QSIZE]      __attribute__((aligned(4)));

struct blk_req_header {
    uint32_t type;                  /* 0 = read, 1 = write */
    uint32_t reserved;
    uint64_t sector;
};

static struct blk_req_header req;
static volatile uint8_t      req_status;
static uint64_t              capacity;

int vblk_present(void)       { return dev.base != 0; }
uint64_t vblk_capacity(void) { return capacity; }
uint64_t vblk_mmio_base(void){ return dev.base; }
uint32_t vblk_irq(void)      { return dev.base ? dev.irq : 0; }

/* The disk's interrupt has exactly one job: wake whoever is blocked
 * in vblk_transfer below. The used ring is the source of truth. */
static void vblk_isr(void)
{
    virtio_irq_ack(&dev);
    task_wakeup(&vq);
}

int vblk_init(void)
{
    if (virtio_find(2 /* block */, 0, &dev) < 0)
        return -1;
    if (virtio_queue_init(&dev, &vq, 0, QSIZE, desc, avail_mem, used_mem) < 0)
        return -1;
    virtio_set_irq_handler(&dev, vblk_isr);
    virtio_driver_ok(&dev);

    /* Config space: capacity in 512-byte sectors (two 32-bit halves). */
    capacity = virtio_cfg32(&dev, 0) | ((uint64_t)virtio_cfg32(&dev, 4) << 32);
    return 0;
}

/* One request at a time, then SLEEP until the used ring says done —
 * the disk's interrupt (or worst case the next timer tick) wakes us. */
static int vblk_transfer(uint64_t sector, void *buf, int is_write)
{
    if (!dev.base || sector >= capacity)
        return -1;

    req.type     = is_write ? 1 : 0;
    req.reserved = 0;
    req.sector   = sector;
    req_status   = 0xFF;            /* poison: device must overwrite */

    desc[0] = (struct virtq_desc){ (uint64_t)&req, sizeof(req),
                                   VIRTQ_DESC_F_NEXT, 1 };
    desc[1] = (struct virtq_desc){ (uint64_t)buf, VBLK_SECTOR_SIZE,
                                   VIRTQ_DESC_F_NEXT |
                                   (is_write ? 0 : VIRTQ_DESC_F_WRITE), 2 };
    desc[2] = (struct virtq_desc){ (uint64_t)&req_status, 1,
                                   VIRTQ_DESC_F_WRITE, 0 };

    virtq_kick(&vq, 0);

    /* Block until the disk's interrupt wakes us (see task.c for the
     * mask/loop convention). The CPU is free for other tasks the
     * whole time the disk is seeking. */
    uint32_t id, len;
    asm volatile("msr daifset, #2");
    while (virtq_pop_used(&vq, &id, &len) < 0)
        task_block(&vq);
    asm volatile("msr daifclr, #2");

    return req_status == 0 ? 0 : -1;
}

int vblk_read(uint64_t sector, void *buf)
{
    return vblk_transfer(sector, buf, 0);
}

int vblk_write(uint64_t sector, const void *buf)
{
    return vblk_transfer(sector, (void *)buf, 1);
}
