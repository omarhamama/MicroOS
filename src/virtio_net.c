/*
 * virtio_net.c — the network card.
 *
 * A NIC moves Ethernet FRAMES — flat byte arrays with a 14-byte header
 * (destination MAC, source MAC, protocol type) and a payload. That's
 * the entire job: frames out, frames in. Everything that makes it feel
 * like "the internet" — IP, TCP, DNS — is layered software OVER this
 * (see net.c for the first two layers of that tower).
 *
 * Both directions ride virtqueues:
 *
 *   RX (queue 0): like the tablet, the world speaks whenever it wants,
 *       so we keep empty buffers posted and the device fills one per
 *       incoming frame — the conveyor belt pattern again.
 *   TX (queue 1): like the disk, send is a request we submit and the
 *       device completes.
 *
 * Each frame travels with a 12-byte virtio-net header in front (it
 * carries checksum/TSO offload info; we use none of those features,
 * so ours is always zero — but the slot must be there).
 */

#include "virtio_net.h"
#include "virtio.h"
#include "task.h"
#include "lib.h"

#define QSIZE 8
#define HDR_LEN 12                  /* struct virtio_net_hdr_v1 */
#define RXBUF_LEN (HDR_LEN + ETH_MAX_FRAME)

#define FEATURE_MAC (1UL << 5)      /* "config space contains the MAC" */

static struct virtio_dev dev;
static struct virtq rxq, txq;

static struct virtq_desc rx_desc[QSIZE]    __attribute__((aligned(16)));
static uint8_t rx_avail[6 + 2 * QSIZE]     __attribute__((aligned(2)));
static uint8_t rx_used[6 + 8 * QSIZE]      __attribute__((aligned(4)));
static struct virtq_desc tx_desc[QSIZE]    __attribute__((aligned(16)));
static uint8_t tx_avail[6 + 2 * QSIZE]     __attribute__((aligned(2)));
static uint8_t tx_used[6 + 8 * QSIZE]      __attribute__((aligned(4)));

static uint8_t rx_bufs[QSIZE][RXBUF_LEN];
static uint8_t tx_buf[HDR_LEN + ETH_MAX_FRAME];

static uint8_t mac[6];
static uint64_t rx_count, tx_count;

int vnet_present(void)          { return dev.base != 0; }
const uint8_t *vnet_mac(void)   { return mac; }
uint64_t vnet_rx_count(void)    { return rx_count; }
uint64_t vnet_tx_count(void)    { return tx_count; }

static void post_rx(uint16_t i)
{
    rx_desc[i] = (struct virtq_desc){ (uint64_t)rx_bufs[i], RXBUF_LEN,
                                      VIRTQ_DESC_F_WRITE, 0 };
    virtq_kick(&rxq, i);
}

static void vnet_isr(void)
{
    virtio_irq_ack(&dev);
    task_wakeup(&rxq);      /* the net task blocks here between frames */
    task_wakeup(&txq);      /* a sender may be waiting for completion */
}

int vnet_init(void)
{
    if (virtio_find(1 /* network */, FEATURE_MAC, &dev) < 0)
        return -1;
    if (virtio_queue_init(&dev, &rxq, 0, QSIZE, rx_desc, rx_avail, rx_used) < 0 ||
        virtio_queue_init(&dev, &txq, 1, QSIZE, tx_desc, tx_avail, tx_used) < 0)
        return -1;
    virtio_set_irq_handler(&dev, vnet_isr);
    virtio_driver_ok(&dev);

    for (int i = 0; i < 6; i++)
        mac[i] = virtio_cfg8(&dev, (uint32_t)i);

    for (uint16_t i = 0; i < QSIZE; i++)
        post_rx(i);
    return 0;
}

int vnet_send(const void *frame, uint32_t len)
{
    if (!dev.base || len > ETH_MAX_FRAME)
        return -1;

    memset(tx_buf, 0, HDR_LEN);             /* no offloads: header = zeros */
    memcpy(tx_buf + HDR_LEN, frame, len);

    tx_desc[0] = (struct virtq_desc){ (uint64_t)tx_buf, HDR_LEN + len, 0, 0 };
    virtq_kick(&txq, 0);

    uint32_t id, ulen;
    asm volatile("msr daifset, #2");
    while (virtq_pop_used(&txq, &id, &ulen) < 0)
        task_block(&txq);
    asm volatile("msr daifclr, #2");
    tx_count++;
    return 0;
}

/* The channel the net task should block on while no frames wait. */
void *vnet_rx_chan(void)
{
    return &rxq;
}

int vnet_recv(void *buf)
{
    uint32_t id, len;
    if (virtq_pop_used(&rxq, &id, &len) < 0)
        return -1;

    int out = -1;
    if (id < QSIZE && len > HDR_LEN) {
        out = (int)(len - HDR_LEN);
        if (out > ETH_MAX_FRAME)
            out = ETH_MAX_FRAME;
        memcpy(buf, rx_bufs[id] + HDR_LEN, (uint32_t)out);
        rx_count++;
    }
    post_rx((uint16_t)id);                  /* belt keeps moving */
    return out;
}
