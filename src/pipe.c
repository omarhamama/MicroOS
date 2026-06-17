/*
 * pipe.c — a pipe: a one-way, in-memory byte stream between processes.
 *
 * `cat file | wc -l` is two programs and one of these in the middle:
 * cat writes bytes into the pipe's buffer, wc reads them out. The
 * magic is entirely in the blocking rules, which turn a fixed buffer
 * into an infinite stream:
 *
 *   - a reader on an EMPTY pipe SLEEPS until a writer adds data
 *     (or until every writer has closed — then read returns 0 = EOF,
 *      which is exactly how wc knows the input ended)
 *   - a writer on a FULL pipe SLEEPS until a reader drains some
 *
 * Those two waits are why a 4 KB buffer can carry a gigabyte: the
 * producer and consumer take turns automatically. We block on the
 * scheduler's sleep/wakeup channels (task_block/task_wakeup), the same
 * mechanism the disk and keyboard drivers use.
 */

#include "pipe.h"
#include "file.h"
#include "mem.h"
#include "lib.h"
#include "task.h"

#define PIPE_SIZE 4096

struct pipe {
    char     data[PIPE_SIZE];
    uint32_t nread;             /* total bytes consumed */
    uint32_t nwrite;            /* total bytes produced  */
    int      readers;           /* read ends still open  */
    int      writers;           /* write ends still open */
};

/* file.c hands us file objects to wrap our two ends in. */
struct file *file_alloc_pipe(struct pipe *p, int writer);

int pipe_new(struct file **read_end, struct file **write_end)
{
    struct pipe *p = kmalloc(sizeof(*p));
    if (!p)
        return -1;
    memset(p, 0, sizeof(*p));
    p->readers = 1;
    p->writers = 1;

    *read_end = file_alloc_pipe(p, 0);
    *write_end = file_alloc_pipe(p, 1);
    if (!*read_end || !*write_end) {
        kfree(p);
        return -1;
    }
    return 0;
}

long pipe_write(struct pipe *p, const void *buf, long n)
{
    const char *src = buf;
    long i = 0;

    asm volatile("msr daifset, #2");            /* the sleep/wakeup dance */
    while (i < n) {
        if (p->readers == 0) {                  /* nobody will ever read */
            asm volatile("msr daifclr, #2");
            return i ? i : -1;                  /* "broken pipe" */
        }
        if (p->nwrite - p->nread == PIPE_SIZE) { /* full: wake readers, wait */
            task_wakeup(&p->nread);
            task_block(&p->nwrite);
            continue;
        }
        p->data[p->nwrite++ % PIPE_SIZE] = src[i++];
    }
    task_wakeup(&p->nread);                      /* data is available */
    asm volatile("msr daifclr, #2");
    return i;
}

long pipe_read(struct pipe *p, void *buf, long n)
{
    char *dst = buf;
    long i = 0;

    asm volatile("msr daifset, #2");
    while (p->nread == p->nwrite) {             /* empty */
        if (p->writers == 0) {                  /* ...and no more is coming */
            asm volatile("msr daifclr, #2");
            return 0;                           /* EOF */
        }
        task_block(&p->nwrite);                 /* wait for a writer */
    }
    while (i < n && p->nread < p->nwrite)
        dst[i++] = p->data[p->nread++ % PIPE_SIZE];
    task_wakeup(&p->nwrite);                     /* room for writers now */
    asm volatile("msr daifclr, #2");
    return i;
}

void pipe_close(struct pipe *p, int writer)
{
    asm volatile("msr daifset, #2");
    if (writer)
        p->writers--;
    else
        p->readers--;
    /* Wake the other side: a reader must learn writers hit 0 (EOF),
     * a writer must learn readers hit 0 (broken pipe). */
    task_wakeup(&p->nread);
    task_wakeup(&p->nwrite);
    int done = (p->readers == 0 && p->writers == 0);
    asm volatile("msr daifclr, #2");

    if (done)
        kfree(p);
}
