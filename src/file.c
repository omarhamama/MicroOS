/*
 * file.c — open file objects and the three things behind a descriptor:
 * the console, a disk file (inode), and a pipe.
 *
 * Read/write don't know or care which backend they're talking to —
 * file_read/file_write switch on the type. That polymorphism, behind a
 * plain integer, is why a shell can wire any program's output to any
 * other program's input without either program knowing.
 */

#include "file.h"
#include "fs.h"
#include "mem.h"
#include "lib.h"
#include "uart.h"
#include "pipe.h"
#include "kprintf.h"

#define NFILE 64        /* system-wide open-file objects */

static struct file ftable[NFILE];

/* The console is a single shared object — fd 0/1/2 of every process
 * point at it (until the shell redirects them through a pipe). */
static struct file console = {
    .type = FD_CONSOLE, .refcount = 1, .readable = 1, .writable = 1,
};

struct file *file_console(void)
{
    return file_dup(&console);
}

static struct file *file_alloc(void)
{
    for (int i = 0; i < NFILE; i++)
        if (ftable[i].refcount == 0) {
            memset(&ftable[i], 0, sizeof(ftable[i]));
            ftable[i].refcount = 1;
            return &ftable[i];
        }
    return 0;
}

struct file *file_dup(struct file *f)
{
    f->refcount++;
    return f;
}

void file_close(struct file *f)
{
    if (!f || --f->refcount > 0)
        return;
    if (f->type == FD_PIPE)
        pipe_close(f->pipe, f->pipe_writer);    /* may signal EOF */
    f->type = FD_NONE;                          /* refcount already 0 */
}

struct file *file_open(const char *path, int flags)
{
    struct fs_node *n = fs_resolve(fs_root, path);

    if (!n && (flags & 0x100 /* O_CREATE */)) {
        char leaf[FS_NAME_MAX];
        struct fs_node *dir = fs_resolve_parent(fs_root, path, leaf);
        if (dir)
            n = fs_create(dir, leaf, FS_FILE);
    }
    if (!n || n->type != FS_FILE)
        return 0;

    if (flags & 0x200 /* O_TRUNC */)
        fs_set_content(n, "", 0);

    struct file *f = file_alloc();
    if (!f)
        return 0;
    f->type = FD_INODE;
    f->node = n;
    f->off = 0;
    f->readable = 1;
    f->writable = 1;
    return f;
}

/* ---- the console backend ------------------------------------------- */

static long console_read(char *buf, long n)
{
    /* Line-buffered: read until newline or the buffer fills, echoing
     * as we go (so the human sees what they type). Returns the line. */
    long i = 0;
    while (i < n) {
        char c = uart_getc();
        if (c == '\r')
            c = '\n';
        uart_putc(c);                   /* echo */
        buf[i++] = c;
        if (c == '\n')
            break;
    }
    return i;
}

static void console_write(const char *buf, long n)
{
    for (long i = 0; i < n; i++) {
        if (buf[i] == '\n')
            uart_putc('\r');            /* terminals want \r\n */
        uart_putc(buf[i]);
    }
}

/* ---- the inode backend (a disk file) ------------------------------- */

static long inode_read(struct file *f, char *buf, long n)
{
    if (f->off >= f->node->size)
        return 0;                       /* EOF */
    long avail = (long)(f->node->size - f->off);
    if (n > avail)
        n = avail;
    memcpy(buf, f->node->data + f->off, (uint64_t)n);
    f->off += (uint64_t)n;
    return n;
}

static long inode_write(struct file *f, const char *buf, long n)
{
    /* Grow the file to cover [off, off+n), preserving what's there,
     * then hand the whole new content to the fs (which writes it
     * through to disk). Quadratic for many small appends — fine at our
     * scale; a real fs writes only the changed blocks. */
    uint64_t end = f->off + (uint64_t)n;
    uint64_t newsize = end > f->node->size ? end : f->node->size;

    char *tmp = kmalloc(newsize ? newsize : 1);
    if (!tmp)
        return -1;
    memcpy(tmp, f->node->data, f->node->size);
    if (end > f->node->size)            /* zero any gap we skipped over */
        memset(tmp + f->node->size, 0, end - f->node->size);
    memcpy(tmp + f->off, buf, (uint64_t)n);

    int rc = fs_set_content(f->node, tmp, newsize);
    kfree(tmp);
    if (rc < 0)
        return -1;
    f->off = end;
    return n;
}

/* ---- the dispatchers ----------------------------------------------- */

long file_read(struct file *f, void *buf, long n)
{
    if (!f || !f->readable || n < 0)
        return -1;
    switch (f->type) {
    case FD_CONSOLE: return console_read(buf, n);
    case FD_INODE:   return inode_read(f, buf, n);
    case FD_PIPE:    return pipe_read(f->pipe, buf, n);
    default:         return -1;
    }
}

long file_write(struct file *f, const void *buf, long n)
{
    if (!f || !f->writable || n < 0)
        return -1;
    switch (f->type) {
    case FD_CONSOLE: console_write(buf, n); return n;
    case FD_INODE:   return inode_write(f, buf, n);
    case FD_PIPE:    return pipe_write(f->pipe, buf, n);
    default:         return -1;
    }
}

/* Pipe descriptors are built by the pipe syscall; expose the pool
 * allocator so pipe.c can wrap a struct pipe in two file objects. */
struct file *file_alloc_pipe(struct pipe *p, int writer)
{
    struct file *f = file_alloc();
    if (!f)
        return 0;
    f->type = FD_PIPE;
    f->pipe = p;
    f->pipe_writer = writer;
    f->readable = !writer;
    f->writable = writer;
    return f;
}
