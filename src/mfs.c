/*
 * mfs.c — MicroFS v3: a journaled on-disk filesystem.
 *
 * The disk is carved into regions:
 *
 *   block 0          SUPERBLOCK   who am I, where is everything
 *   blocks 1-2       BLOCK BITMAP one bit per block: free or used?
 *   blocks 3-34      INODE TABLE  128 slots of "what is this file"
 *   blocks 35-8127   DATA         the actual bytes
 *   blocks 8128-8191 JOURNAL      the crash-safety machinery (below)
 *
 * The two ideas that make every Unix filesystem tick:
 *
 *   INODE — a file IS its inode: type, size, and a list of which
 *   blocks hold its bytes — 24 "direct" pointers, plus one INDIRECT
 *   pointer to a block that is itself 128 more pointers. Files scale
 *   by adding levels of indirection, not by being contiguous.
 *
 *   DIRECTORY = A FILE OF (name, inode#) PAIRS — nothing more.
 *
 * THE JOURNAL — why filesystems survive power cuts:
 *
 * One logical operation ("create a file") touches several blocks:
 * the bitmap, an inode, the parent directory. A crash BETWEEN those
 * writes leaves the metadata lying about itself — the historical
 * reason fsck took minutes on every unclean boot. The fix is
 * write-ahead logging, the same trick as databases:
 *
 *   1. write all of a transaction's blocks to the JOURNAL area
 *   2. write one COMMIT record naming them      <- the atomic moment
 *   3. copy them to their real homes
 *   4. clear the commit record
 *
 * Crash before 2: journal ignored, old state intact. Crash after 2:
 * boot REPLAYS the journal and finishes the operation. Either way,
 * never half. (File DATA is written before the commit, "ordered
 * mode" — same policy as ext4's default.)
 *
 * All disk I/O goes through bcache.c; the RAM tree in fs.c remains
 * the directory cache. Run `crashfs` to watch a replay happen.
 */

#include "mfs.h"
#include "bcache.h"
#include "virtio_blk.h"
#include "mem.h"
#include "lib.h"
#include "kprintf.h"

#define MFS_MAGIC      0x33534675u      /* "uFS3", little-endian */
#define TOTAL_BLOCKS   8192             /* 4 MiB / 512 */
#define BITMAP_START   1
#define BITMAP_BLOCKS  2
#define ITABLE_START   3
#define INODE_COUNT    128
#define ITABLE_BLOCKS  32
#define DATA_START     (ITABLE_START + ITABLE_BLOCKS)   /* 35 */
#define JOURNAL_START  8128             /* header + up to 62 blocks */
#define JOURNAL_MAX    62
#define DATA_END       JOURNAL_START
#define NDIRECT        24
#define NINDIRECT      128              /* one block of u32 pointers */

#define JRN_MAGIC      0x4e524a75u      /* "uJRN" */

struct superblock {
    uint32_t magic, version;
    uint32_t total_blocks, bitmap_start, bitmap_blocks;
    uint32_t itable_start, inode_count, data_start, root_ino;
    uint32_t journal_start;
};

struct dinode {                         /* exactly 128 bytes */
    uint16_t type;                      /* 0 free, 1 file, 2 dir */
    uint16_t nlink;
    uint32_t reserved;
    uint64_t size;
    uint32_t direct[NDIRECT];
    uint32_t indirect;                  /* block of 128 more pointers */
    uint8_t  pad[12];
};

struct dirent {
    uint32_t ino;
    char     name[28];
};

struct jheader {
    uint32_t magic;
    uint32_t n;
    uint32_t sectors[JOURNAL_MAX];
};

static int mounted;
static int loading;                     /* mount in progress: hooks off */
static uint8_t bitmap[BITMAP_BLOCKS * 512];

int mfs_mounted(void) { return mounted; }

/* ---- the transaction layer ----------------------------------------- */
/* Between tx_begin and tx_commit, metadata writes are STAGED here in
 * RAM instead of hitting the disk. Commit pushes them through the
 * journal protocol above. Reads check the staging area first so a
 * transaction always sees its own writes. */

static int      tx_active;
static int      tx_skip_apply;          /* crashtest hook: stop after commit */
static uint32_t tx_n;
static uint32_t tx_sectors[JOURNAL_MAX];
static uint8_t  tx_data[JOURNAL_MAX][512];
static uint64_t tx_commits, tx_blocks_logged;

static void tx_begin(void)
{
    tx_active = 1;
    tx_n = 0;
}

static int tx_write(uint32_t sector, const void *buf)
{
    if (!tx_active)
        return bwrite(sector, buf);

    for (uint32_t i = 0; i < tx_n; i++)        /* coalesce repeats */
        if (tx_sectors[i] == sector) {
            memcpy(tx_data[i], buf, 512);
            return 0;
        }
    if (tx_n >= JOURNAL_MAX)
        return -1;                              /* transaction too big */
    tx_sectors[tx_n] = sector;
    memcpy(tx_data[tx_n], buf, 512);
    tx_n++;
    return 0;
}

static int tx_read(uint32_t sector, void *dst)
{
    if (tx_active)
        for (uint32_t i = 0; i < tx_n; i++)
            if (tx_sectors[i] == sector) {
                memcpy(dst, tx_data[i], 512);
                return 0;
            }
    return bread(sector, dst);
}

static int tx_commit(void)
{
    tx_active = 0;
    if (!tx_n)
        return 0;

    /* 1. the transaction's blocks, parked in the journal area */
    for (uint32_t i = 0; i < tx_n; i++)
        if (bwrite(JOURNAL_START + 1 + i, tx_data[i]) < 0)
            return -1;

    /* 2. the commit record — THE atomic moment. Before this write the
     * operation never happened; after it, it's guaranteed. */
    static uint8_t sector[512];
    struct jheader hdr = { JRN_MAGIC, tx_n, {0} };
    for (uint32_t i = 0; i < tx_n; i++)
        hdr.sectors[i] = tx_sectors[i];
    memset(sector, 0, 512);
    memcpy(sector, &hdr, sizeof(hdr));
    if (bwrite(JOURNAL_START, sector) < 0)
        return -1;

    tx_commits++;
    tx_blocks_logged += tx_n;

    if (tx_skip_apply)
        return 0;                       /* crashtest: "power fails" here */

    /* 3. the real writes */
    for (uint32_t i = 0; i < tx_n; i++)
        bwrite(tx_sectors[i], tx_data[i]);

    /* 4. retire the journal entry */
    memset(sector, 0, 512);
    return bwrite(JOURNAL_START, sector);
}

static long journal_replay(void)
{
    uint8_t sector[512];
    struct jheader hdr;

    if (bread(JOURNAL_START, sector) < 0)
        return -1;
    memcpy(&hdr, sector, sizeof(hdr));
    if (hdr.magic != JRN_MAGIC || hdr.n > JOURNAL_MAX)
        return 0;                       /* clean shutdown: nothing logged */

    /* A committed transaction never finished applying. Finish it —
     * replay is idempotent, so even a crash DURING replay is fine. */
    for (uint32_t i = 0; i < hdr.n; i++) {
        if (bread(JOURNAL_START + 1 + i, sector) < 0)
            return -1;
        bwrite(hdr.sectors[i], sector);
    }
    memset(sector, 0, 512);
    bwrite(JOURNAL_START, sector);
    return (long)hdr.n;
}

/* ---- block + inode layer (all metadata via the tx layer) ------------ */

static int bitmap_sync(uint32_t blockno)
{
    uint32_t which = blockno / (512 * 8);
    return tx_write(BITMAP_START + which, bitmap + which * 512);
}

static uint32_t balloc(void)
{
    for (uint32_t b = DATA_START; b < DATA_END; b++) {
        if (!(bitmap[b / 8] & (1u << (b % 8)))) {
            bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
            if (bitmap_sync(b) < 0)
                return 0;
            return b;
        }
    }
    return 0;
}

static void bfree(uint32_t b)
{
    bitmap[b / 8] &= (uint8_t)~(1u << (b % 8));
    bitmap_sync(b);
}

static int iread(uint32_t ino, struct dinode *di)
{
    uint8_t sector[512];
    if (tx_read(ITABLE_START + ino / 4, sector) < 0)
        return -1;
    memcpy(di, sector + (ino % 4) * 128, sizeof(*di));
    return 0;
}

static int iwrite(uint32_t ino, const struct dinode *di)
{
    uint8_t sector[512];
    if (tx_read(ITABLE_START + ino / 4, sector) < 0)
        return -1;
    memcpy(sector + (ino % 4) * 128, di, sizeof(*di));
    return tx_write(ITABLE_START + ino / 4, sector);
}

static uint32_t ialloc(uint16_t type)
{
    struct dinode di;
    for (uint32_t i = 1; i < INODE_COUNT; i++) {
        if (iread(i, &di) < 0)
            return 0;
        if (di.type == 0) {
            memset(&di, 0, sizeof(di));
            di.type = type;
            return iwrite(i, &di) < 0 ? 0 : i;
        }
    }
    return 0;
}

static void free_blocks(struct dinode *di)
{
    for (uint32_t i = 0; i < NDIRECT && di->direct[i]; i++) {
        bfree(di->direct[i]);
        di->direct[i] = 0;
    }
    if (di->indirect) {
        uint32_t ind[NINDIRECT];
        if (tx_read(di->indirect, ind) == 0)
            for (uint32_t j = 0; j < NINDIRECT && ind[j]; j++)
                bfree(ind[j]);
        bfree(di->indirect);
        di->indirect = 0;
    }
}

/* Replace an inode's content. File data goes straight to disk BEFORE
 * the metadata commits ("ordered mode"); directory data rides inside
 * the transaction, because a directory's content IS metadata. */
static int write_content(uint32_t ino, const char *data, uint64_t len,
                         int journal_data)
{
    if (len > MFS_MAX_FILE)
        return -1;

    struct dinode di;
    if (iread(ino, &di) < 0)
        return -1;
    free_blocks(&di);

    uint32_t nblocks = (uint32_t)((len + 511) / 512);
    uint32_t ind[NINDIRECT];
    memset(ind, 0, sizeof(ind));

    uint8_t sector[512];
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t b = balloc();
        if (!b) {
            kprintf("[mfs] write: block allocation failed at %u/%u\n",
                    i, nblocks);
            return -1;
        }
        uint64_t chunk = len - i * 512 < 512 ? len - i * 512 : 512;
        memset(sector, 0, 512);
        memcpy(sector, data + i * 512, chunk);
        if ((journal_data ? tx_write(b, sector) : bwrite(b, sector)) < 0)
            return -1;
        if (i < NDIRECT)
            di.direct[i] = b;
        else
            ind[i - NDIRECT] = b;
    }

    if (nblocks > NDIRECT) {            /* the file outgrew its inode */
        di.indirect = balloc();
        if (!di.indirect || tx_write(di.indirect, ind) < 0)
            return -1;
    }

    di.size = len;
    return iwrite(ino, &di);
}

static int read_content(const struct dinode *di, char *out)
{
    uint32_t ind[NINDIRECT];
    memset(ind, 0, sizeof(ind));
    if (di->indirect && bread(di->indirect, (void *)ind) < 0)
        return -1;

    uint8_t sector[512];
    for (uint64_t off = 0; off < di->size; off += 512) {
        uint32_t idx = (uint32_t)(off / 512);
        uint32_t b = idx < NDIRECT ? di->direct[idx] : ind[idx - NDIRECT];
        if (!b || bread(b, sector) < 0)
            return -1;
        uint64_t chunk = di->size - off < 512 ? di->size - off : 512;
        memcpy(out + off, sector, chunk);
    }
    return 0;
}

static int update_dir(struct fs_node *dir)
{
    uint32_t n = 0;
    for (struct fs_node *c = dir->children; c; c = c->next)
        n++;
    if (n * sizeof(struct dirent) > MFS_MAX_FILE)
        return -1;

    struct dirent *ents = kmalloc(n ? n * sizeof(struct dirent) : 1);
    if (!ents)
        return -1;

    uint32_t k = 0;
    for (struct fs_node *c = dir->children; c; c = c->next, k++) {
        ents[k].ino = c->ino;
        memset(ents[k].name, 0, sizeof(ents[k].name));
        for (int i = 0; c->name[i] && i < 27; i++)
            ents[k].name[i] = c->name[i];
    }

    int rc = write_content(dir->ino, (const char *)ents,
                           n * sizeof(struct dirent), 1);
    kfree(ents);
    return rc;
}

/* ---- the write-through hooks (called from fs.c) -------------------- */

int mfs_on_create(struct fs_node *n)
{
    if (!mounted || loading)
        return 0;
    tx_begin();
    uint32_t ino = ialloc(n->type == FS_DIR ? 2 : 1);
    if (!ino) {
        tx_commit();
        return -1;
    }
    n->ino = ino;
    int rc = update_dir(n->parent);
    return tx_commit() < 0 ? -1 : rc;
}

int mfs_on_content(struct fs_node *n)
{
    if (!mounted || loading)
        return 0;
    tx_begin();
    int rc = write_content(n->ino, n->data, n->size, 0);
    return tx_commit() < 0 ? -1 : rc;
}

int mfs_on_unlink(struct fs_node *n)
{
    if (!mounted || loading)
        return 0;
    tx_begin();
    struct dinode di;
    if (iread(n->ino, &di) == 0) {
        free_blocks(&di);
        memset(&di, 0, sizeof(di));
        iwrite(n->ino, &di);
    }
    int rc = update_dir(n->parent);
    return tx_commit() < 0 ? -1 : rc;
}

/* ---- mount / format ------------------------------------------------ */

static int format(void)
{
    uint8_t sector[512];

    memset(bitmap, 0, sizeof(bitmap));
    for (uint32_t b = 0; b < DATA_START; b++)
        bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
    for (uint32_t b = JOURNAL_START; b < TOTAL_BLOCKS; b++)
        bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
    for (uint32_t i = 0; i < BITMAP_BLOCKS; i++)
        if (bwrite(BITMAP_START + i, bitmap + i * 512) < 0)
            return -1;

    memset(sector, 0, 512);
    for (uint32_t i = 0; i < ITABLE_BLOCKS; i++)
        if (bwrite(ITABLE_START + i, sector) < 0)
            return -1;
    if (bwrite(JOURNAL_START, sector) < 0)      /* empty journal */
        return -1;

    struct dinode root = { 0 };
    root.type = 2;
    tx_active = 0;
    if (iwrite(0, &root) < 0)
        return -1;

    struct superblock sb = {
        MFS_MAGIC, 3, TOTAL_BLOCKS, BITMAP_START, BITMAP_BLOCKS,
        ITABLE_START, INODE_COUNT, DATA_START, 0, JOURNAL_START
    };
    memset(sector, 0, 512);
    memcpy(sector, &sb, sizeof(sb));
    return bwrite(0, sector);
}

static long load_dir(uint32_t ino, struct fs_node *dir, long count)
{
    struct dinode di;
    if (count < 0 || iread(ino, &di) < 0 || di.type != 2)
        return -1;

    char *buf = kmalloc(di.size ? di.size : 1);
    if (!buf || read_content(&di, buf) < 0) {
        kfree(buf);
        return -1;
    }

    struct dirent *ents = (struct dirent *)buf;
    for (uint64_t i = 0; i < di.size / sizeof(struct dirent); i++) {
        struct dinode cdi;
        if (iread(ents[i].ino, &cdi) < 0 || cdi.type == 0)
            continue;

        ents[i].name[27] = '\0';
        struct fs_node *child = fs_create(dir, ents[i].name,
                                          cdi.type == 2 ? FS_DIR : FS_FILE);
        if (!child)
            continue;
        child->ino = ents[i].ino;
        count++;

        if (cdi.type == 2) {
            count = load_dir(ents[i].ino, child, count);
        } else if (cdi.size) {
            char *data = kmalloc(cdi.size);
            if (data && read_content(&cdi, data) == 0)
                fs_set_content(child, data, cdi.size);
            kfree(data);
        }
    }
    kfree(buf);
    return count;
}

long mfs_mount(void)
{
    if (!vblk_present())
        return -1;

    uint8_t sector[512];
    if (bread(0, sector) < 0)
        return -1;

    struct superblock sb;
    memcpy(&sb, sector, sizeof(sb));

    if (sb.magic != MFS_MAGIC || sb.version != 3) {
        kprintf("[mfs] no MicroFS v3 on disk — formatting fresh\n");
        if (format() < 0)
            return -1;
        mounted = 1;
        fs_root->ino = 0;
        return 0;
    }

    long replayed = journal_replay();
    if (replayed > 0)
        kprintf("[mfs] unclean shutdown: journal REPLAYED %ld blocks — "
                "metadata healed\n", replayed);

    for (uint32_t i = 0; i < BITMAP_BLOCKS; i++)
        if (bread(BITMAP_START + i, bitmap + i * 512) < 0)
            return -1;

    mounted = 1;
    loading = 1;
    fs_root->ino = 0;
    long n = load_dir(0, fs_root, 0);
    loading = 0;

    if (n < 0) {
        kprintf("[mfs] disk metadata damaged — mounted what was readable\n");
        return 0;
    }
    return n;
}

int mfs_crashtest(void)
{
    if (!mounted)
        return -1;

    struct fs_node *f = fs_resolve(fs_root, "/welcome.txt");
    if (!f)
        return -1;

    struct dinode di;
    if (iread(f->ino, &di) < 0 || !di.direct[0])
        return -1;

    /* Stage new content for welcome.txt's first block, write it to
     * the journal, COMMIT — and "lose power" before applying. */
    static const char msg[] =
        "THIS TEXT ARRIVED VIA JOURNAL REPLAY.\n"
        "It was committed to the journal, then the machine 'crashed'\n"
        "before the real write happened. Boot found the commit record\n"
        "and finished the job. That is what a journal is for.\n";
    uint8_t sector[512];
    memset(sector, ' ', 512);
    memcpy(sector, msg, sizeof(msg) - 1);

    tx_begin();
    tx_write(di.direct[0], sector);
    tx_skip_apply = 1;
    int rc = tx_commit();
    tx_skip_apply = 0;
    return rc;
}

void mfs_block_usage(uint32_t *used, uint32_t *total)
{
    uint32_t u = 0;
    if (mounted)
        for (uint32_t b = 0; b < TOTAL_BLOCKS; b++)
            if (bitmap[b / 8] & (1u << (b % 8)))
                u++;
    *used = u;
    *total = mounted ? TOTAL_BLOCKS : 0;
}

void mfs_stat(void)
{
    if (!mounted) {
        kprintf("MicroFS: not mounted\n");
        return;
    }

    uint32_t used_blocks = 0;
    for (uint32_t b = 0; b < TOTAL_BLOCKS; b++)
        if (bitmap[b / 8] & (1u << (b % 8)))
            used_blocks++;

    uint32_t used_inodes = 0;
    struct dinode di;
    for (uint32_t i = 0; i < INODE_COUNT; i++)
        if (iread(i, &di) == 0 && di.type != 0)
            used_inodes++;

    uint64_t hits, misses;
    bcache_stats(&hits, &misses);

    kprintf("MicroFS v3 (write-through, journaled — survives crashes)\n");
    kprintf("  layout:  sb=0, bitmap=1-2, inodes=3-34, data=35+, "
            "journal=%u+\n", JOURNAL_START);
    kprintf("  blocks:  %u used / %u (%u KiB free); files up to %u KiB\n",
            used_blocks, TOTAL_BLOCKS, (TOTAL_BLOCKS - used_blocks) / 2,
            MFS_MAX_FILE / 1024);
    kprintf("  inodes:  %u used / %u\n", used_inodes, INODE_COUNT);
    kprintf("  journal: %lu commits, %lu blocks logged\n",
            tx_commits, tx_blocks_logged);
    kprintf("  bcache:  %lu hits / %lu misses (%lu%% hit rate)\n",
            hits, misses, hits + misses ? hits * 100 / (hits + misses) : 0);
}
