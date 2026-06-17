#pragma once

#include <stdint.h>

/*
 * Shared virtio-MMIO plumbing. One device family, many personalities:
 * the same discovery/handshake/virtqueue machinery drives our disk
 * (ID 2), network card (ID 1), tablet (ID 18) and sound card (ID 25).
 */

/* ---- virtqueue structures (layouts fixed by the virtio spec) ------- */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VIRTQ_DESC_F_NEXT   1       /* chain continues at .next */
#define VIRTQ_DESC_F_WRITE  2       /* DEVICE writes this buffer */

struct virtq_used_elem { uint32_t id, len; };

struct virtio_dev {
    uint64_t base;                  /* MMIO slot base, 0 = not found */
    uint32_t irq;                   /* GIC INTID (48 + slot on virt) */
};

struct virtq {
    struct virtio_dev *dev;
    uint32_t index;                 /* queue number on the device */
    uint16_t size;
    uint16_t last_used;             /* our cursor into the used ring */
    struct virtq_desc *desc;
    /* avail/used rings as raw pointers (their tails are sized by
     * queue size, so fixed structs don't fit every queue) */
    volatile uint16_t *avail_flags, *avail_idx, *avail_ring;
    volatile uint16_t *used_flags, *used_idx;
    volatile struct virtq_used_elem *used_ring;
};

/* Find an unclaimed device of this type and run the handshake up to
 * FEATURES_OK (negotiating VERSION_1 + want_features). 0 = ok. */
int virtio_find(uint32_t device_id, uint64_t want_features,
                struct virtio_dev *out);

/* Wire queue `index` to caller-provided ring memory:
 *   desc:  16*size bytes, 16-aligned
 *   avail:  6+2*size bytes, 2-aligned
 *   used:   6+8*size bytes, 4-aligned */
int virtio_queue_init(struct virtio_dev *d, struct virtq *q, uint32_t index,
                      uint16_t size, void *desc, void *avail, void *used);

void virtio_driver_ok(struct virtio_dev *d);    /* go live */

uint8_t  virtio_cfg8(struct virtio_dev *d, uint32_t off);
uint32_t virtio_cfg32(struct virtio_dev *d, uint32_t off);

/* Publish descriptor chain starting at `head` and ring the doorbell. */
void virtq_kick(struct virtq *q, uint16_t head);

/* Pop one completion from the used ring; -1 if none pending. */
int virtq_pop_used(struct virtq *q, uint32_t *id, uint32_t *len);

/* IRQ routing: drivers register a handler; exceptions.c calls
 * virtio_dispatch_irq for any interrupt it doesn't recognize. */
void virtio_set_irq_handler(struct virtio_dev *d, void (*fn)(void));
int  virtio_dispatch_irq(uint32_t intid);       /* 1 = was a virtio device */
void virtio_irq_ack(struct virtio_dev *d);
