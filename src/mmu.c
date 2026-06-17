/*
 * mmu.c — turning on the MMU: the biggest rite of passage in OS dev.
 *
 * Until now every address was a PHYSICAL address: `*(int *)0x40080000`
 * went straight to that RAM cell, and nothing could stop any code from
 * reading or writing (or executing!) anything. The MMU (Memory
 * Management Unit) inserts a translation step in front of EVERY memory
 * access:
 *
 *    virtual address --[page tables]--> physical address + permissions
 *
 * The page tables are a radix tree in ordinary RAM that WE build, and
 * each entry carries permission bits. That one mechanism is the
 * foundation of nearly everything an OS does for safety:
 *
 *    - kernel code becomes read-only        (no more self-corruption)
 *    - data becomes non-executable          ("W^X" — stops whole bug classes)
 *    - address 0 becomes unmapped           (null derefs now CRASH LOUDLY)
 *    - and later: each process gets its own private tables = isolation
 *
 * We keep virtual == physical everywhere (an "identity map") so no
 * pointer changes meaning when we flip the switch — we only gain
 * permissions. Per-process address spaces come later.
 *
 * The table tree for our 4 GiB view (4 KiB pages, "granule"):
 *
 *   L1 (1 GiB/entry)      L2 (2 MiB/entry)        L3 (4 KiB/entry)
 *   [0] devices ------->  [64..511] device blocks
 *                         [0..63]   INVALID        <- null page lives here
 *   [1] RAM     ------->  [0] -------------------> 512 fine-grained pages:
 *                         [1..63] RW+XN blocks        DTB: read/write, XN
 *                         [64..511] INVALID           text+rodata: READ-ONLY
 *   [2],[3] INVALID                                   data/bss/...: RW + XN
 */

#include <stdint.h>
#include "mmu.h"
#include "kprintf.h"
#include "board.h"

#include "mem.h"

extern char __kernel_start[];   /* kernel text+rodata begin (link address) */
extern char __ro_end[];         /* page-aligned end of read-only region */

/* ---- descriptor bits (ARMv8-A VMSA, the long format) --------------- */
#define DESC_TABLE      3UL             /* L1/L2 entry -> next-level table */
#define DESC_BLOCK      1UL             /* L1/L2 entry -> big block of memory */
#define DESC_PAGE       3UL             /* L3 entry    -> one 4 KiB page */

#define ATTR_DEVICE     (0UL << 2)      /* MAIR slot 0: device memory */
#define ATTR_NORMAL     (1UL << 2)      /* MAIR slot 1: normal, cacheable */
#define AP_RW_EL1       (0UL << 6)      /* kernel read/write, EL0 no access */
#define AP_RW_ALL       (1UL << 6)      /* read/write at EL1 AND EL0 */
#define AP_RO_EL1       (2UL << 6)      /* kernel read-only,  EL0 no access */
#define AP_RO_ALL       (3UL << 6)      /* read-only at EL1 AND EL0 */
#define SH_INNER        (3UL << 8)      /* shareability (matters with caches) */
#define ACCESS_FLAG     (1UL << 10)     /* "has been used" — must preset or fault */
#define PXN             (1UL << 53)     /* privileged execute-never */
#define UXN             (1UL << 54)     /* unprivileged execute-never */

/* The flavors of memory MicroOS knows about: */
#define MMIO    (DESC_BLOCK | ATTR_DEVICE | AP_RW_EL1 | ACCESS_FLAG | PXN | UXN)
#define RAM_RW  (ATTR_NORMAL | AP_RW_EL1 | SH_INNER | ACCESS_FLAG | PXN | UXN)
/* Kernel text: read-only, executable, EL1-ONLY. (It was briefly
 * EL0-visible as a teaching shortcut before the program loader
 * existed; with real processes, user mode sees nothing of the kernel.) */
#define RAM_RX  (ATTR_NORMAL | AP_RO_EL1 | SH_INNER | ACCESS_FLAG | UXN)
/* User process pages, now PER-SEGMENT — real W^X, because the ELF
 * loader tells us which bytes are code and which are data:
 *   TEXT   read-only + executable at EL0   (can't be overwritten)
 *   RODATA read-only + never executable
 *   DATA   read/write + never executable   (also used for stack)
 * PXN on all three: the KERNEL must never execute user-controlled
 * bytes, even when it's the one that wrote them. */
#define RAM_USER_TEXT  (ATTR_NORMAL | AP_RO_ALL | SH_INNER | ACCESS_FLAG | PXN)
#define RAM_USER_RODAT (ATTR_NORMAL | AP_RO_ALL | SH_INNER | ACCESS_FLAG | PXN | UXN)
#define RAM_USER_DATA  (ATTR_NORMAL | AP_RW_ALL | SH_INNER | ACCESS_FLAG | PXN | UXN)

/* Page tables are just arrays in RAM — 4 KiB-aligned, 512 entries. */
static uint64_t l1[512]     __attribute__((aligned(4096)));
static uint64_t l2_dev[512] __attribute__((aligned(4096)));
static uint64_t l2_ram[512] __attribute__((aligned(4096)));
static uint64_t l3_low[512] __attribute__((aligned(4096)));

void mmu_init(void)
{
    /* The map is built from board.h so the SAME code serves QEMU virt
     * (RAM @ 0x4000_0000, devices @ 0x0800_0000) and the Raspberry Pi
     * (RAM @ 0, peripherals + GIC in the top GiB). Each 1 GiB L1 entry
     * points at an L2 of 2 MiB blocks; the RAM gig's first 2 MiB is
     * split into 4 KiB pages so the kernel image gets per-page W^X. */

    /* ---- the device (MMIO) window --------------------------------- */
    /* Map from BOARD_DEV_BASE to the end of the 1 GiB it sits in. Entries
     * below it in that gig stay INVALID — on virt that's what turns a
     * NULL dereference into a loud fault (on the Pi, RAM page 0 does). */
    uint64_t dev_l1    = BOARD_DEV_BASE >> 30;
    uint64_t dev_first = (BOARD_DEV_BASE >> 21) & 0x1FF;
    for (uint64_t i = dev_first; i < 512; i++)
        l2_dev[i] = ((dev_l1 << 30) + (i << 21)) | MMIO;
    l1[dev_l1] = (uint64_t)l2_dev | DESC_TABLE;

    /* ---- RAM ------------------------------------------------------- */
    uint64_t ram_l1 = BOARD_RAM_BASE >> 30;
    uint64_t blocks = BOARD_RAM_SIZE >> 21;       /* 2 MiB blocks of RAM */
    if (blocks > 512) blocks = 512;
#if defined(BOARD_RASPI4) || defined(BOARD_RK3326)
    blocks = 512;   /* map the whole low GiB: the GPU/bootloader places the
                     * framebuffer somewhere in it, and we must reach it */
#endif
    for (uint64_t i = 1; i < blocks; i++)
        l2_ram[i] = (BOARD_RAM_BASE + (i << 21)) | DESC_BLOCK | RAM_RW;
    l2_ram[0] = (uint64_t)l3_low | DESC_TABLE;    /* first 2 MiB: fine-grained */
    l1[ram_l1] = (uint64_t)l2_ram | DESC_TABLE;

    /* ---- the fine-grained first 2 MiB of RAM (holds the kernel) ---- */
    uint64_t ro_start = (uint64_t)__kernel_start >> 12;   /* page numbers */
    uint64_t ro_end   = (uint64_t)__ro_end >> 12;
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t page = (BOARD_RAM_BASE >> 12) + i;
        uint64_t addr = page << 12;
        if (addr == 0)                                /* page 0 stays unmapped */
            l3_low[i] = 0;                            /* (null-deref trap on Pi) */
        else if (page >= ro_start && page < ro_end)
            l3_low[i] = addr | DESC_PAGE | RAM_RX;    /* code: RO + exec */
        else
            l3_low[i] = addr | DESC_PAGE | RAM_RW;    /* data: RW + XN  */
    }

    mmu_enable_this_core();

    kprintf("[boot] MMU on (%s): identity map, W^X, null page unmapped\n",
            BOARD_NAME);
}

/*
 * ---- flip the switch (PER CORE — the tables are shared, but every
 * core has its own MMU registers and must be configured itself; core 1
 * calls this too when it wakes) ---------------------------------------
 * MAIR: a palette of 8 memory types; descriptors pick by index.
 *   slot 0 = 0x00 Device-nGnRnE (no caching, no reordering — MMIO)
 *   slot 1 = 0xFF Normal, write-back cacheable (real RAM)
 * TCR: shape of the address space — 4 GiB (T0SZ=32), 4 KiB granule,
 *   table walks cacheable, TTBR1 (the "high half") disabled.
 * TTBR0: physical address of our L1 table — the root pointer.
 */
void mmu_enable_this_core(void)
{
    asm volatile("msr mair_el1, %0"  :: "r"(0xFF00UL));
    asm volatile("msr tcr_el1, %0"   :: "r"(
        32UL            /* T0SZ: 64-32 = 32 bits of VA = 4 GiB */
        | (1UL << 8) | (1UL << 10)      /* walks: write-back cacheable */
        | (3UL << 12)                   /* walks: inner shareable */
        | (1UL << 23)                   /* EPD1: no high-half tables */
        | (2UL << 32)));                /* IPS: 40-bit physical space */
    asm volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l1));

    asm volatile("dsb ish; isb");           /* tables visible before use */
    asm volatile("tlbi vmalle1; dsb nsh; isb");     /* no stale translations */

    /* SCTLR: M = MMU on, C = data cache on, I = instruction cache on.
     * The instruction after this isb executes TRANSLATED. Because the
     * mapping is identity, the program counter still makes sense. */
    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    asm volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr));
}

/* ------------------------------------------------------------------ */
/* Per-process address spaces                                          */
/* ------------------------------------------------------------------ */
/*
 * Until processes existed, one set of tables served everyone. Now
 * each user process gets a PRIVATE L1 whose entries 0 and 1 point at
 * the SAME kernel L2 tables as everyone else's (the kernel is
 * identical in every address space — that's why a syscall can run
 * without switching tables), and whose entry 2 (VA 0x8000_0000+)
 * leads to that process's own pages. Two processes can both believe
 * they own address 0x80000000, because the MMU resolves it through
 * whichever table TTBR0 points at. THAT is what a process is.
 */

uint64_t *mmu_kernel_table(void)
{
    return l1;
}

uint64_t *mmu_user_table_new(void)
{
    uint64_t *ul1 = page_alloc();
    if (!ul1)
        return 0;
    ul1[0] = l1[0];                 /* devices: shared with the kernel */
    ul1[1] = l1[1];                 /* RAM: shared with the kernel */
    return ul1;                     /* entries 2..3: this process's own */
}

int mmu_user_map(uint64_t *ul1, uint64_t va, void *frame, int perm)
{
    if (va < USER_BASE || va >= 0xC0000000UL)
        return -1;

    uint64_t l1i = (va >> 30) & 3;
    uint64_t l2i = (va >> 21) & 0x1FF;
    uint64_t l3i = (va >> 12) & 0x1FF;

    uint64_t *l2t;
    if (!(ul1[l1i] & 1)) {          /* no L2 yet: grow the tree */
        l2t = page_alloc();
        if (!l2t)
            return -1;
        ul1[l1i] = (uint64_t)l2t | DESC_TABLE;
    } else {
        l2t = (uint64_t *)(ul1[l1i] & ~0xFFFUL);
    }

    uint64_t *l3t;
    if (!(l2t[l2i] & 1)) {
        l3t = page_alloc();
        if (!l3t)
            return -1;
        l2t[l2i] = (uint64_t)l3t | DESC_TABLE;
    } else {
        l3t = (uint64_t *)(l2t[l2i] & ~0xFFFUL);
    }

    uint64_t bits = perm == MMU_PERM_RX ? RAM_USER_TEXT
                  : perm == MMU_PERM_RO ? RAM_USER_RODAT
                  : RAM_USER_DATA;
    l3t[l3i] = (uint64_t)frame | DESC_PAGE | bits;
    return 0;
}

/*
 * fork() in one function: clone every mapped user page of `src` into
 * `dst`, allocating fresh frames and copying the bytes, preserving
 * each page's permissions. This is the SIMPLE, eager version — it
 * copies the whole address space up front. The famous optimization is
 * copy-on-write: share the pages read-only and only duplicate one when
 * a process writes it, so fork()+exec() (which throws the copy away
 * immediately) costs nothing. COW is on the robustness roadmap; eager
 * copy is correct and easy to read. Returns 0, or -1 if it runs out
 * of frames (leaving dst for the caller to free).
 */
int mmu_user_copy(uint64_t *dst, uint64_t *src)
{
    for (int i = 2; i < 4; i++) {           /* user half only */
        if (!(src[i] & 1))
            continue;
        uint64_t *s2 = (uint64_t *)(src[i] & ~0xFFFUL);
        for (int j = 0; j < 512; j++) {
            if (!(s2[j] & 1))
                continue;
            uint64_t *s3 = (uint64_t *)(s2[j] & ~0xFFFUL);
            for (int k = 0; k < 512; k++) {
                if (!(s3[k] & 1))
                    continue;
                uint64_t va = ((uint64_t)i << 30) | ((uint64_t)j << 21)
                            | ((uint64_t)k << 12);
                void *frame = page_alloc();
                if (!frame)
                    return -1;
                /* copy the page contents via the kernel identity map */
                for (int b = 0; b < 512; b++)
                    ((uint64_t *)frame)[b] =
                        ((uint64_t *)(s3[k] & ~0xFFFUL & 0xFFFFFFFFFFFFUL))[b];
                /* recover this page's permission class from its bits */
                uint64_t ap = s3[k] & (3UL << 6);
                uint64_t uxn = s3[k] & UXN;
                int perm = (ap == AP_RO_ALL && !uxn) ? MMU_PERM_RX
                         : (ap == AP_RO_ALL)         ? MMU_PERM_RO
                         :                             MMU_PERM_RW;
                if (mmu_user_map(dst, va, frame, perm) < 0) {
                    page_free(frame);
                    return -1;
                }
            }
        }
    }
    return 0;
}

void mmu_user_table_free(uint64_t *ul1)
{
    /* Walk only the user half (entries 2..3 — 0/1 are the shared
     * kernel tables, NOT ours to free) and return every frame and
     * table page to the pool. */
    for (int i = 2; i < 4; i++) {
        if (!(ul1[i] & 1))
            continue;
        uint64_t *l2t = (uint64_t *)(ul1[i] & ~0xFFFUL);
        for (int j = 0; j < 512; j++) {
            if (!(l2t[j] & 1))
                continue;
            uint64_t *l3t = (uint64_t *)(l2t[j] & ~0xFFFUL);
            for (int k = 0; k < 512; k++)
                if (l3t[k] & 1)
                    page_free((void *)(l3t[k] & ~0xFFFUL & 0xFFFFFFFFFFFFUL));
            page_free(l3t);
        }
        page_free(l2t);
    }
    page_free(ul1);
}

/*
 * THE single source of truth for which table is in TTBR0. Both the
 * scheduler and the program loader change address spaces, so the
 * "what's active now?" answer must live in ONE place they both update
 * — keeping it as a private static in the scheduler let the loader's
 * direct switch go unrecorded, and the scheduler would then SKIP a
 * needed switch, leaving a freed user table live in TTBR0. (That bug
 * corrupted the kernel page tables on the second program launch; the
 * cure is this shared variable.) Core 0 runs all user tasks; core 1
 * sets its TTBR0 once and never calls here, so one global suffices.
 */
static uint64_t *active_table;

uint64_t *mmu_current_table(void)
{
    return active_table;
}

void mmu_switch(uint64_t *table)
{
    active_table = table;
    /* New translations, new world: load the root pointer and flush
     * the TLB (cached translations from the old table would otherwise
     * leak between processes). Flushing EVERYTHING is the blunt
     * instrument — tagging entries per-process (ASIDs) so switches
     * don't flush at all is a worthy exercise. */
    asm volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n" :: "r"((uint64_t)table));
}
