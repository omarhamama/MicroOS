#pragma once

#include <stdint.h>

/*
 * The file-descriptor layer — the great Unix unifier.
 *
 * A program reads and writes through small integers (0, 1, 2, ...)
 * without caring what's behind them: a disk file, the console, or a
 * pipe to another program. "Everything is a file." A `struct file` is
 * the object a descriptor points at; the per-process table `ofile[]`
 * maps the integers to those objects.
 *
 * Files are reference-counted and SHARED: when a process forks, parent
 * and child both point at the same struct file (so they share a file's
 * read offset, and a pipe stays open as long as either holds it). The
 * object frees only when the last descriptor closes.
 */

#define NOFILE 16       /* open files per process */

struct fs_node;
struct pipe;

enum file_type { FD_NONE, FD_CONSOLE, FD_INODE, FD_PIPE };

struct file {
    enum file_type type;
    int   refcount;
    int   readable, writable;
    /* FD_INODE: */
    struct fs_node *node;
    uint64_t off;
    /* FD_PIPE: */
    struct pipe *pipe;
    int   pipe_writer;          /* 1 = the write end, 0 = the read end */
};

struct file *file_console(void);                /* the shared console object */
struct file *file_open(const char *path, int flags);   /* FD_INODE, or NULL */
struct file *file_dup(struct file *f);          /* refcount++ */
void  file_close(struct file *f);               /* refcount--; free at 0 */
long  file_read(struct file *f, void *buf, long n);     /* bytes; 0 = EOF */
long  file_write(struct file *f, const void *buf, long n);
