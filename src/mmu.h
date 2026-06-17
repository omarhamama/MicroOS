#pragma once

#include <stdint.h>

/* Build identity-mapped page tables and switch the MMU (and caches) on. */
void mmu_init(void);
void mmu_enable_this_core(void);    /* per-core register setup (SMP) */

/*
 * Per-process address spaces. Each user process gets its own L1 table
 * whose kernel entries are SHARED with the boot tables (same physical
 * L2/L3 tables — the kernel looks identical from every process) and
 * whose user region (0x8000_0000+) maps that process's private pages.
 */
/* Page permission classes for mmu_user_map (real W^X per ELF segment). */
#define MMU_PERM_RW  0      /* read/write, no-execute (data, stack) */
#define MMU_PERM_RX  1      /* read-only, executable (code) */
#define MMU_PERM_RO  2      /* read-only, no-execute (rodata) */

uint64_t *mmu_kernel_table(void);
uint64_t *mmu_user_table_new(void);                 /* NULL if out of pages */
int  mmu_user_map(uint64_t *l1, uint64_t va, void *frame, int perm);
int  mmu_user_copy(uint64_t *dst, uint64_t *src);   /* fork: clone user pages */
void mmu_user_table_free(uint64_t *l1);             /* frees frames too */
void mmu_switch(uint64_t *l1);                      /* load TTBR0 + flush TLB */
uint64_t *mmu_current_table(void);                  /* what TTBR0 holds now */

#define USER_BASE       0x80000000UL    /* user code is linked here */
#define USER_STACK_VA   0x80100000UL    /* 8 pages of stack... */
#define USER_STACK_TOP  0x80108000UL    /* ...growing down from here (32 KiB) */
#define USER_REGION_END 0x80200000UL    /* for syscall pointer checks */
