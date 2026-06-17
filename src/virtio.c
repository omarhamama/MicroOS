/*
 * virtio.c — the shared core of every virtio driver.
 *
 * When the disk driver was our only virtio device, its discovery and
 * handshake code lived inline. Then came the network card, the tablet
 * and the sound card — same magic number, same status dance, same
 * ring layout. This file is that common 80%; each driver keeps only
 * its personality (what the buffers MEAN).
 *
 * See virtio_blk.c for the full guided tour of the concepts —
 * descriptors, avail/used rings, DMA. It reads best first.
 */

#include "virtio.h"
#include "gic.h"
#include "kprintf.h"

#define MMIO_MAGIC            0x000
#define MMIO_VERSION          0x004
#define MMIO_DEVICE_ID        0x008
#define MMIO_DEVICE_FEATURES  0x010
#define MMIO_DEVICE_FEAT_SEL  0x014
#define MMIO_DRIVER_FEATURES  0x020
#define MMIO_DRIVER_FEAT_SEL  0x024
#define MMIO_QUEUE_SEL        0x030
#define MMIO_QUEUE_NUM_MAX    0x034
#define MMIO_QUEUE_NUM        0x038
#define MMIO_QUEUE_READY      0x044
#define MMIO_QUEUE_NOTIFY     0x050
#define MMIO_INTERRUPT_STATUS 0x060
#define MMIO_INTERRUPT_ACK    0x064
#define MMIO_STATUS           0x070
#define MMIO_QUEUE_DESC_LOW   0x080
#define MMIO_QUEUE_AVAIL_LOW  0x090
#define MMIO_QUEUE_USED_LOW   0x0a0
#define MMIO_CONFIG           0x100

#define STATUS_ACKNOWLEDGE  1
#define STATUS_DRIVER       2
#define STATUS_DRIVER_OK    4
#define STATUS_FEATURES_OK  8
#define STATUS_FAILED       0x80

#define FEATURE_VERSION_1   (1UL << 32)

#define REG(d, off) (*(volatile uint32_t *)((d)->base + (off)))
#define mb()  asm volatile("dsb sy" ::: "memory")

#define NSLOTS 32
static uint8_t claimed[NSLOTS];

/* irq -> handler routing for all virtio devices */
static struct { uint32_t irq; void (*fn)(void); } irq_table[8];
static int irq_count;

int virtio_find(uint32_t device_id, uint64_t want_features,
                struct virtio_dev *out)
{
    out->base = 0;

    for (int i = 0; i < NSLOTS; i++) {
        uint64_t b = 0x0a000000UL + (uint64_t)i * 0x200;
        if (claimed[i] ||
            *(volatile uint32_t *)(b + MMIO_MAGIC) != 0x74726976 ||
            *(volatile uint32_t *)(b + MMIO_DEVICE_ID) != device_id)
            continue;
        if (*(volatile uint32_t *)(b + MMIO_VERSION) != 2) {
            kprintf("[virtio] legacy device in slot %d — need "
                    "-global virtio-mmio.force-legacy=false\n", i);
            return -1;
        }
        claimed[i] = 1;
        out->base = b;
        out->irq = 48 + (uint32_t)i;
        break;
    }
    if (!out->base)
        return -1;

    struct virtio_dev *d = out;
    REG(d, MMIO_STATUS) = 0;                            /* reset */
    mb();
    REG(d, MMIO_STATUS) = STATUS_ACKNOWLEDGE;
    REG(d, MMIO_STATUS) = STATUS_ACKNOWLEDGE | STATUS_DRIVER;

    uint64_t want = want_features | FEATURE_VERSION_1;
    for (int half = 0; half < 2; half++) {
        REG(d, MMIO_DEVICE_FEAT_SEL) = (uint32_t)half;
        uint32_t offered = REG(d, MMIO_DEVICE_FEATURES);
        uint32_t ask = (uint32_t)(want >> (32 * half));
        if ((offered & ask) != ask) {
            kprintf("[virtio] device %u refused features %x/%x\n",
                    device_id, ask, offered);
            goto fail;
        }
        REG(d, MMIO_DRIVER_FEAT_SEL) = (uint32_t)half;
        REG(d, MMIO_DRIVER_FEATURES) = ask;
    }

    REG(d, MMIO_STATUS) = STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK;
    if (!(REG(d, MMIO_STATUS) & STATUS_FEATURES_OK)) {
        kprintf("[virtio] device %u rejected feature selection\n", device_id);
        goto fail;
    }
    return 0;

fail:
    REG(d, MMIO_STATUS) = STATUS_FAILED;
    out->base = 0;
    return -1;
}

int virtio_queue_init(struct virtio_dev *d, struct virtq *q, uint32_t index,
                      uint16_t size, void *desc, void *avail, void *used)
{
    REG(d, MMIO_QUEUE_SEL) = index;
    if (REG(d, MMIO_QUEUE_NUM_MAX) < size)
        return -1;

    q->dev = d;
    q->index = index;
    q->size = size;
    q->last_used = 0;
    q->desc = desc;

    uint8_t *a = avail;                 /* flags u16, idx u16, ring[] */
    q->avail_flags = (volatile uint16_t *)a;
    q->avail_idx   = (volatile uint16_t *)(a + 2);
    q->avail_ring  = (volatile uint16_t *)(a + 4);

    uint8_t *u = used;                  /* flags u16, idx u16, ring[] */
    q->used_flags = (volatile uint16_t *)u;
    q->used_idx   = (volatile uint16_t *)(u + 2);
    q->used_ring  = (volatile struct virtq_used_elem *)(u + 4);

    *q->avail_flags = 0;
    *q->avail_idx = 0;

    REG(d, MMIO_QUEUE_NUM) = size;
    REG(d, MMIO_QUEUE_DESC_LOW)      = (uint32_t)(uint64_t)desc;
    REG(d, MMIO_QUEUE_DESC_LOW + 4)  = (uint32_t)((uint64_t)desc >> 32);
    REG(d, MMIO_QUEUE_AVAIL_LOW)     = (uint32_t)(uint64_t)avail;
    REG(d, MMIO_QUEUE_AVAIL_LOW + 4) = (uint32_t)((uint64_t)avail >> 32);
    REG(d, MMIO_QUEUE_USED_LOW)      = (uint32_t)(uint64_t)used;
    REG(d, MMIO_QUEUE_USED_LOW + 4)  = (uint32_t)((uint64_t)used >> 32);
    mb();
    REG(d, MMIO_QUEUE_READY) = 1;
    return 0;
}

void virtio_driver_ok(struct virtio_dev *d)
{
    REG(d, MMIO_STATUS) = STATUS_ACKNOWLEDGE | STATUS_DRIVER
                        | STATUS_FEATURES_OK | STATUS_DRIVER_OK;
}

uint8_t  virtio_cfg8(struct virtio_dev *d, uint32_t off)
{
    return *(volatile uint8_t *)(d->base + MMIO_CONFIG + off);
}

uint32_t virtio_cfg32(struct virtio_dev *d, uint32_t off)
{
    return REG(d, MMIO_CONFIG + off);
}

void virtq_kick(struct virtq *q, uint16_t head)
{
    q->avail_ring[*q->avail_idx % q->size] = head;
    mb();
    (*q->avail_idx)++;
    mb();
    REG(q->dev, MMIO_QUEUE_NOTIFY) = q->index;
}

int virtq_pop_used(struct virtq *q, uint32_t *id, uint32_t *len)
{
    mb();
    if (*q->used_idx == q->last_used)
        return -1;
    volatile struct virtq_used_elem *e = &q->used_ring[q->last_used % q->size];
    *id = e->id;
    *len = e->len;
    q->last_used++;
    return 0;
}

void virtio_irq_ack(struct virtio_dev *d)
{
    REG(d, MMIO_INTERRUPT_ACK) = REG(d, MMIO_INTERRUPT_STATUS);
}

void virtio_set_irq_handler(struct virtio_dev *d, void (*fn)(void))
{
    if (irq_count < (int)(sizeof(irq_table) / sizeof(irq_table[0]))) {
        irq_table[irq_count].irq = d->irq;
        irq_table[irq_count].fn = fn;
        irq_count++;
        gic_enable_interrupt(d->irq);
    }
}

int virtio_dispatch_irq(uint32_t intid)
{
    for (int i = 0; i < irq_count; i++) {
        if (irq_table[i].irq == intid) {
            irq_table[i].fn();
            return 1;
        }
    }
    return 0;
}
