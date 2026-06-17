#pragma once

#include <stdint.h>

/* The block cache: all filesystem disk I/O goes through here. */
int  bread(uint32_t sector, void *dst);     /* 512 bytes */
int  bwrite(uint32_t sector, const void *src);
void bcache_stats(uint64_t *hits, uint64_t *misses);
