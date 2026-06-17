/*
 * mem.c — a free-list memory allocator.
 *
 * Version 1 of this file was a "bump allocator": a pointer that only
 * moves forward, so free() was impossible. That was fine until the
 * filesystem arrived — `rm` has to give memory back!
 *
 * The classic fix, used in some form by almost every allocator ever
 * written: put a small hidden HEADER in front of every allocation
 * recording its size. Then:
 *
 *   kfree(p)    — look at the header to see how big the block is, and
 *                 push it onto a linked list of free blocks.
 *   kmalloc(n)  — first walk that free list looking for a block big
 *                 enough to reuse ("first fit"). Only if none fits do
 *                 we bump-allocate fresh memory from the end.
 *
 * What the user's pointer really looks like in memory:
 *
 *        +--------------------+----------------------------+
 *        | header (16 bytes)  |  payload (what you asked)  |
 *        +--------------------+----------------------------+
 *        ^ kernel sees this    ^ kmalloc returns this
 *
 * The free list is kept SORTED BY ADDRESS, which makes the allocator
 * COALESCING: when a block is freed, its neighbors in the list are by
 * definition the blocks adjacent to it in memory, so merging freed
 * neighbors into one big block is two pointer checks. Without this,
 * heaps "fragment": free 1000 small blocks and you have plenty of free
 * bytes but can't satisfy one large allocation. Run `memtest` to watch
 * three 1 KiB blocks merge back into one 3 KiB block.
 */

#include "mem.h"
#include "lib.h"
#include "kprintf.h"

/* These symbols are defined by linker.ld — the linker fills in their
 * addresses. Declaring them as arrays gives us their *location*. */
extern char __kernel_start[];
extern char __bss_end[];
extern char __heap_start[];

#define RAM_BASE   0x40000000UL
#define RAM_SIZE   (128UL * 1024 * 1024)        /* matches -m 128M */
#define HEAP_SIZE  (12UL * 1024 * 1024)

/* Above the byte-granular heap sits the PAGE POOL: 1024 whole 4 KiB
 * frames with a used/free byte each. Two allocators, two currencies:
 * kmalloc deals in bytes for kernel objects; page_alloc deals in
 * frames for the MMU (page tables, user code, user stacks), which
 * must be 4 KiB-sized, 4 KiB-aligned physical memory — nothing else
 * will fit in a page-table entry. */
#define PAGE_POOL_PAGES 1024
static uint8_t  page_used[PAGE_POOL_PAGES];
static uint64_t page_pool_base;
static uint64_t pages_out;

struct block {
    uint64_t size;              /* payload bytes (not counting header) */
    struct block *next_free;    /* only meaningful while on the free list */
};

static uint64_t bump_offset;        /* high-water mark of fresh memory */
static struct block *free_list;     /* singly-linked list of freed blocks */
static uint64_t total_allocs, total_frees;

void *kmalloc(uint64_t size)
{
    /* Round up to 16 bytes: keeps every allocation aligned for any
     * type, and guarantees room for the header when the block is
     * later freed and recycled. */
    size = (size + 15) & ~15UL;
    if (size == 0)
        size = 16;

    /* First choice: recycle a freed block (first one that fits). */
    struct block **prev = &free_list;
    for (struct block *b = free_list; b; prev = &b->next_free, b = b->next_free) {
        if (b->size < size)
            continue;

        *prev = b->next_free;           /* unlink it from the free list */

        /* If the block is much bigger than needed, split it: keep the
         * front, hand the tail back to the free list as a new block —
         * inserted at the same list position to keep address order. */
        if (b->size >= size + sizeof(struct block) + 16) {
            struct block *rest = (struct block *)((char *)(b + 1) + size);
            rest->size = b->size - size - sizeof(struct block);
            rest->next_free = *prev;
            *prev = rest;
            b->size = size;
        }

        total_allocs++;
        return b + 1;                   /* +1 skips the header */
    }

    /* No reusable block: bump-allocate fresh memory. */
    if (bump_offset + sizeof(struct block) + size > HEAP_SIZE)
        return 0;                       /* out of memory */

    struct block *b = (struct block *)(__heap_start + bump_offset);
    bump_offset += sizeof(struct block) + size;
    b->size = size;
    total_allocs++;
    return b + 1;
}

/* Does block `a` end exactly where block `b` begins? */
static int adjacent(struct block *a, struct block *b)
{
    return (char *)(a + 1) + a->size == (char *)b;
}

void kfree(void *p)
{
    if (!p)
        return;

    /* Step back over the payload to find the header we hid there. */
    struct block *b = (struct block *)p - 1;
    total_frees++;

    /* Walk to this block's position in the address-ordered list,
     * remembering the block before it. */
    struct block *prev = 0, *next = free_list;
    while (next && next < b) {
        prev = next;
        next = next->next_free;
    }

    /* Merge forward: [b][next] touching? Swallow next into b. */
    if (next && adjacent(b, next)) {
        b->size += sizeof(struct block) + next->size;
        b->next_free = next->next_free;
    } else {
        b->next_free = next;
    }

    /* Merge backward: [prev][b] touching? Swallow b into prev. */
    if (prev && adjacent(prev, b)) {
        prev->size += sizeof(struct block) + b->size;
        prev->next_free = b->next_free;
    } else if (prev) {
        prev->next_free = b;
    } else {
        free_list = b;
    }
}

void mem_usage(uint64_t *claimed, uint64_t *heap_total, uint64_t *live_allocs)
{
    *claimed = bump_offset;
    *heap_total = HEAP_SIZE;
    *live_allocs = total_allocs - total_frees;
}

void *page_alloc(void)
{
    if (!page_pool_base)
        page_pool_base = ((uint64_t)__heap_start + HEAP_SIZE + 4095) & ~4095UL;

    for (uint64_t i = 0; i < PAGE_POOL_PAGES; i++) {
        if (!page_used[i]) {
            page_used[i] = 1;
            pages_out++;
            void *p = (void *)(page_pool_base + i * 4096);
            memset(p, 0, 4096);
            return p;
        }
    }
    return 0;                       /* out of frames */
}

void page_free(void *page)
{
    if (!page)
        return;
    uint64_t i = ((uint64_t)page - page_pool_base) / 4096;
    if (i < PAGE_POOL_PAGES && page_used[i]) {
        page_used[i] = 0;
        pages_out--;
    }
}

uint64_t page_pool_used(void)
{
    return pages_out;
}

uint64_t mem_free_block_count(void)
{
    uint64_t n = 0;
    for (struct block *b = free_list; b; b = b->next_free)
        n++;
    return n;
}

void mem_print_stats(void)
{
    uint64_t kernel_size = (uint64_t)(__bss_end - __kernel_start);
    uint64_t free_blocks = 0, free_bytes = 0;

    for (struct block *b = free_list; b; b = b->next_free) {
        free_blocks++;
        free_bytes += b->size;
    }

    kprintf("RAM:    %lu MiB at %p\n", RAM_SIZE >> 20, (void *)RAM_BASE);
    kprintf("Kernel: %p - %p (%lu KiB: code + data + bss)\n",
            __kernel_start, __bss_end, kernel_size >> 10);
    kprintf("Heap:   %p, %lu bytes claimed of %lu MiB\n",
            __heap_start, bump_offset, HEAP_SIZE >> 20);
    kprintf("        %lu live allocations (%lu made, %lu freed)\n",
            total_allocs - total_frees, total_allocs, total_frees);
    kprintf("        free list: %lu block%s, %lu bytes awaiting reuse\n",
            free_blocks, free_blocks == 1 ? "" : "s", free_bytes);
}
