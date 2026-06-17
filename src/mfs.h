#pragma once

#include <stdint.h>
#include "fs.h"

/* 24 direct + 128 single-indirect blocks = 76 KiB per file */
#define MFS_MAX_FILE ((24 + 128) * 512)

/* Mount the disk: returns entries restored (>= 0), or -1 if there is
 * no usable disk. A blank/foreign disk is formatted automatically and
 * counts as 0 entries restored. */
long mfs_mount(void);
int  mfs_mounted(void);

/* Write-through hooks — fs.c calls these after every mutation. Each
 * is a no-op when no filesystem is mounted (RAM-only mode). */
int  mfs_on_create(struct fs_node *n);
int  mfs_on_content(struct fs_node *n);
int  mfs_on_unlink(struct fs_node *n);

void mfs_stat(void);                /* for the `disk` command */
void mfs_block_usage(uint32_t *used, uint32_t *total);  /* cheap, from RAM bitmap */

/* Test hook: stage a visible change in the journal, COMMIT it, but
 * "crash" before applying. The replay at next boot proves itself. */
int mfs_crashtest(void);
