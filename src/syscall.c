/*
 * syscall.c — the kernel's front desk.
 *
 * User code at EL0 cannot touch hardware, kernel memory, or privileged
 * registers — the MMU and the privilege system forbid it. The ONLY
 * door in is the `svc` instruction: a deliberate synchronous exception.
 * The program leaves a number in x8 and args in x0-x2; the CPU vaults
 * to EL1 here; we do the work WITH FULL PRIVILEGES but ON OUR TERMS;
 * the result goes back in x0 and eret drops back to EL0.
 *
 * Every read()/write()/open()/fork() you've ever called is this, at
 * the bottom. The descriptor-based I/O calls all funnel through the
 * file layer (file.c), so a program neither knows nor cares whether
 * fd 1 is the console, a disk file, or a pipe.
 */

#include <stdint.h>
#include "syscall.h"
#include "task.h"
#include "file.h"
#include "pipe.h"
#include "fs.h"
#include "mmu.h"
#include "proc.h"
#include "timer.h"
#include "lib.h"
#include "kprintf.h"

/* A user pointer is only trustworthy if it lies inside the caller's own
 * address space. Skipping this is the "confused deputy" hole: a program
 * hands the kernel a kernel address and tricks it into reading/writing
 * privileged memory on the program's behalf. */
static int user_ok(uint64_t p, uint64_t len)
{
    return p >= USER_BASE && p + len >= p && p + len <= USER_REGION_END;
}

static struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return 0;
    return current->ofile[fd];
}

static int fd_alloc(struct file *f)
{
    for (int i = 0; i < NOFILE; i++)
        if (!current->ofile[i]) {
            current->ofile[i] = f;
            return i;
        }
    return -1;
}

/* in proc.c — fork/exec live there, next to the address-space code */
int  proc_fork(struct trap_frame *f);
int  proc_exec_syscall(struct trap_frame *f, const char *path, char **argv);
int  task_wait(int *code_out);

static long sys_readdir(uint64_t path, int idx, uint64_t namebuf)
{
    if (!user_ok(path, 1) || !user_ok(namebuf, FS_NAME_MAX))
        return -1;
    struct fs_node *d = fs_resolve(fs_root, (const char *)path);
    if (!d || d->type != FS_DIR)
        return -1;
    int i = 0;
    for (struct fs_node *c = d->children; c; c = c->next, i++)
        if (i == idx) {
            memcpy((void *)namebuf, c->name, FS_NAME_MAX);
            return c->type == FS_DIR ? 1 : 0;
        }
    return -1;                          /* past the last entry */
}

void handle_el0_sync(struct trap_frame *f, uint64_t esr)
{
    uint32_t ec = (esr >> 26) & 0x3F;

    if (ec == 0x15) {                   /* SVC: a system call */
        uint64_t a0 = f->x[0], a1 = f->x[1], a2 = f->x[2];

        switch (f->x[8]) {
        case SYS_EXIT:
            task_set_exit_code((int)a0);
            task_exit();                /* no return */

        case SYS_WRITE: {
            struct file *file = fd_get((int)a0);
            if (!file || !user_ok(a1, a2)) { f->x[0] = (uint64_t)-1; return; }
            f->x[0] = (uint64_t)file_write(file, (const void *)a1, (long)a2);
            return;
        }
        case SYS_READ: {
            struct file *file = fd_get((int)a0);
            if (!file || !user_ok(a1, a2)) { f->x[0] = (uint64_t)-1; return; }
            f->x[0] = (uint64_t)file_read(file, (void *)a1, (long)a2);
            return;
        }
        case SYS_OPEN: {
            if (!user_ok(a0, 1)) { f->x[0] = (uint64_t)-1; return; }
            struct file *file = file_open((const char *)a0, (int)a1);
            if (!file) { f->x[0] = (uint64_t)-1; return; }
            int fd = fd_alloc(file);
            if (fd < 0) { file_close(file); f->x[0] = (uint64_t)-1; return; }
            f->x[0] = (uint64_t)fd;
            return;
        }
        case SYS_CLOSE: {
            struct file *file = fd_get((int)a0);
            if (!file) { f->x[0] = (uint64_t)-1; return; }
            file_close(file);
            current->ofile[a0] = 0;
            f->x[0] = 0;
            return;
        }
        case SYS_SLEEP:
            task_sleep(a0);
            f->x[0] = 0;
            return;
        case SYS_TICKS:
            f->x[0] = timer_ticks();
            return;
        case SYS_GETPID:
            f->x[0] = (uint64_t)current->id;
            return;
        case SYS_READDIR:
            f->x[0] = (uint64_t)sys_readdir(a0, (int)a1, a2);
            return;

        case SYS_FORK:
            f->x[0] = (uint64_t)proc_fork(f);
            return;
        case SYS_EXEC: {
            if (!user_ok(a0, 1)) { f->x[0] = (uint64_t)-1; return; }
            f->x[0] = (uint64_t)proc_exec_syscall(f, (const char *)a0,
                                                  (char **)a1);
            return;     /* on success proc_exec_syscall does not return here */
        }
        case SYS_WAIT: {
            int code;
            int pid = task_wait(&code);
            if (pid >= 0 && a0 && user_ok(a0, 4))
                *(int *)a0 = code;
            f->x[0] = (uint64_t)pid;
            return;
        }
        case SYS_DUP2: {
            struct file *file = fd_get((int)a0);
            int nfd = (int)a1;
            if (!file || nfd < 0 || nfd >= NOFILE) { f->x[0] = (uint64_t)-1; return; }
            if (current->ofile[nfd])
                file_close(current->ofile[nfd]);
            current->ofile[nfd] = file_dup(file);
            f->x[0] = (uint64_t)nfd;
            return;
        }
        case SYS_PIPE: {
            /* Make a pipe and hand back its two descriptors. The shell
             * (or any program) then forks, and each child keeps just
             * one end — the foundation of `a | b`. */
            if (!user_ok(a0, 2 * sizeof(int))) { f->x[0] = (uint64_t)-1; return; }
            struct file *rf, *wf;
            if (pipe_new(&rf, &wf) < 0) { f->x[0] = (uint64_t)-1; return; }
            int rfd = fd_alloc(rf);
            int wfd = fd_alloc(wf);
            if (rfd < 0 || wfd < 0) {
                file_close(rf);
                file_close(wf);
                if (rfd >= 0) current->ofile[rfd] = 0;
                if (wfd >= 0) current->ofile[wfd] = 0;
                f->x[0] = (uint64_t)-1;
                return;
            }
            int *fds = (int *)a0;
            fds[0] = rfd;               /* read end  */
            fds[1] = wfd;               /* write end */
            f->x[0] = 0;
            return;
        }

        default:
            kprintf("[el0] unknown syscall %lu\n", f->x[8]);
            f->x[0] = (uint64_t)-1;
            return;
        }
    }

    /* Not a syscall: the program FAULTED. With per-process isolation
     * this is a contained event — kill the offender, kernel lives on. */
    uint64_t far;
    asm volatile("mrs %0, far_el1" : "=r"(far));
    kprintf("[el0] task '%s' faulted at EL0: EC=0x%x ELR=0x%lx FAR=0x%lx\n",
            current->name, ec, f->elr, far);
    kprintf("[el0] killing it; the kernel and other tasks are unharmed\n");
    task_set_exit_code(-1);
    task_exit();
}
