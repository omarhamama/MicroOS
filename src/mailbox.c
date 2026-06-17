/*
 * mailbox.c — talk to the VideoCore GPU to get a framebuffer.
 *
 * The mailbox is a tiny FIFO at a fixed MMIO address. We build a
 * "property" message — a length-prefixed list of tag/size/value triples
 * (set resolution, set depth, allocate the buffer, ask the pitch) — and
 * hand the GPU the message's address ORed with the channel number (8).
 * The GPU fills the values in place and signals back. One quirk: the GPU
 * sees RAM through an uncached alias at 0xC000_0000, so the buffer
 * address we pass (and the framebuffer address we get back) are "bus"
 * addresses; we translate to/from the ARM's view by masking.
 */

#include "board.h"

#if BOARD_FB_MAILBOX

#include "mailbox.h"

#define MBOX  ((volatile uint32_t *)BOARD_MBOX_BASE)
#define MB_READ    (0x00 / 4)
#define MB_STATUS  (0x18 / 4)
#define MB_WRITE   (0x20 / 4)
#define MB_FULL    0x80000000u
#define MB_EMPTY   0x40000000u
#define CH_PROP    8

/* The message buffer must be 16-byte aligned (the low 4 bits carry the
 * channel). It lives in normal RAM the GPU can reach via its bus alias. */
static volatile uint32_t __attribute__((aligned(16))) mbuf[40];

static void barrier(void) { asm volatile("dsb sy" ::: "memory"); }

static int mbox_call(void)
{
    uint32_t addr = ((uint32_t)(uintptr_t)mbuf & ~0xFu) | CH_PROP;
    addr |= 0xC0000000u;                    /* GPU's uncached bus alias */
    barrier();
    while (MBOX[MB_STATUS] & MB_FULL) { }
    MBOX[MB_WRITE] = addr;
    for (;;) {
        while (MBOX[MB_STATUS] & MB_EMPTY) { }
        if (MBOX[MB_READ] == addr) {
            barrier();
            return mbuf[1] == 0x80000000u;  /* response code = success */
        }
    }
}

uint32_t *mailbox_fb_init(int w, int h, int *pitch)
{
    int i = 0, fb_idx, pitch_idx;
    mbuf[i++] = 0;                                   /* total size (set below) */
    mbuf[i++] = 0;                                   /* request */
    mbuf[i++] = 0x48003; mbuf[i++] = 8; mbuf[i++] = 8; mbuf[i++] = w; mbuf[i++] = h;
    mbuf[i++] = 0x48004; mbuf[i++] = 8; mbuf[i++] = 8; mbuf[i++] = w; mbuf[i++] = h;
    mbuf[i++] = 0x48005; mbuf[i++] = 4; mbuf[i++] = 4; mbuf[i++] = 32;   /* depth */
    mbuf[i++] = 0x48006; mbuf[i++] = 4; mbuf[i++] = 4; mbuf[i++] = 0;    /* BGR: matches our 0x00RRGGBB pixels in little-endian memory */
    mbuf[i++] = 0x40001; mbuf[i++] = 8; mbuf[i++] = 8;                   /* alloc FB */
    fb_idx = i; mbuf[i++] = 4096; mbuf[i++] = 0;     /* in: align; out: addr,size */
    mbuf[i++] = 0x40008; mbuf[i++] = 4; mbuf[i++] = 4;                   /* get pitch */
    pitch_idx = i; mbuf[i++] = 0;
    mbuf[i++] = 0;                                   /* end tag */
    mbuf[0] = (uint32_t)(i * 4);

    if (!mbox_call() || mbuf[fb_idx] == 0)
        return 0;
    *pitch = (int)mbuf[pitch_idx];
    return (uint32_t *)(uintptr_t)(mbuf[fb_idx] & 0x3FFFFFFF);   /* bus -> phys */
}

#endif /* BOARD_FB_MAILBOX */
