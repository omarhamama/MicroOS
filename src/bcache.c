/*
 * bcache.c — the block cache: RAM remembering the disk.
 *
 * The filesystem re-reads the same sectors constantly — the bitmap on
 * every allocation, an inode-table block on every stat, a directory on
 * every lookup. Going to the (virtual) disk each time costs a request
 * round-trip; going to RAM costs a memcpy. So we keep the 32 most
 * recently used sectors in RAM:
 *
 *   bread:  cache hit -> memcpy and done. Miss -> evict the least-
 *           recently-used slot, fetch from disk, remember.
 *   bwrite: update the cached copy AND the disk ("write-through" —
 *           the disk is never stale, so a crash loses nothing).
 *
 * The eviction policy is the eternal classic, LRU: the slot whose
 * last use is oldest gets recycled, on the theory that the recent
 * past predicts the near future. The same idea, scaled up a million
 * times, is your Mac's page cache — RAM "free" is RAM wasted.
 *
 * (A write-BACK cache — buffer writes in RAM, flush later — is faster
 * still, and is exactly why "safely remove hardware" exists. Ours
 * stays write-through; the journal below it handles crash atomicity.)
 */

#include "bcache.h"
#include "virtio_blk.h"
#include "lib.h"

#define NBUF 32

static struct buf {
    uint32_t sector;
    uint8_t  valid;
    uint64_t lastuse;
    uint8_t  data[512];
} bufs[NBUF];

static uint64_t useclock;           /* logical time for LRU */
static uint64_t hits, misses;

static struct buf *lookup(uint32_t sector)
{
    for (int i = 0; i < NBUF; i++)
        if (bufs[i].valid && bufs[i].sector == sector)
            return &bufs[i];
    return 0;
}

static struct buf *evict_lru(void)
{
    struct buf *victim = &bufs[0];
    for (int i = 1; i < NBUF; i++)
        if (!bufs[i].valid)
            return &bufs[i];        /* an empty slot beats any victim */
        else if (bufs[i].lastuse < victim->lastuse)
            victim = &bufs[i];
    return victim;
}

int bread(uint32_t sector, void *dst)
{
    struct buf *b = lookup(sector);
    if (b) {
        hits++;
    } else {
        misses++;
        b = evict_lru();
        if (vblk_read(sector, b->data) < 0) {
            b->valid = 0;
            return -1;
        }
        b->sector = sector;
        b->valid = 1;
    }
    b->lastuse = ++useclock;
    memcpy(dst, b->data, 512);
    return 0;
}

int bwrite(uint32_t sector, const void *src)
{
    struct buf *b = lookup(sector);
    if (!b) {
        b = evict_lru();
        b->sector = sector;
        b->valid = 1;
    }
    memcpy(b->data, src, 512);
    b->lastuse = ++useclock;
    return vblk_write(sector, src);     /* through to the disk, always */
}

void bcache_stats(uint64_t *h, uint64_t *m)
{
    *h = hits;
    *m = misses;
}
