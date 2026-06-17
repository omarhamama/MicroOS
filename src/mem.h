#pragma once

#include <stdint.h>

void  *kmalloc(uint64_t size);
void   kfree(void *p);          /* kfree(0) is a safe no-op, like free() */
void   mem_print_stats(void);
uint64_t mem_free_block_count(void);
void     mem_usage(uint64_t *claimed, uint64_t *heap_total,
                   uint64_t *live_allocs);

/* The page-frame allocator: whole 4 KiB pages, 4 KiB-aligned, zeroed.
 * Page tables and user-process memory live here — the MMU requires
 * page-aligned physical frames, which kmalloc can't promise. */
void    *page_alloc(void);          /* NULL when the pool is empty */
void     page_free(void *page);
uint64_t page_pool_used(void);      /* pages out, for the monitor */
