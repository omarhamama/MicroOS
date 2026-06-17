#pragma once

#include <stdint.h>
#include "file.h"       /* for NOFILE and struct file (the fd table) */

#define MAX_TASKS        8
#define TASK_STACK_SIZE  (16 * 1024)

enum task_state {
    TASK_UNUSED,
    TASK_RUNNABLE,      /* ready — waiting for its turn on the CPU */
    TASK_RUNNING,       /* on the CPU right now (exactly one, ever) */
    TASK_SLEEPING,      /* off the run queue until wake_tick */
    TASK_BLOCKED,       /* off the run queue until task_wakeup(chan) */
    TASK_ZOMBIE,        /* finished; slot reusable */
};

/* The callee-saved CPU state that defines a paused task (see switch.S). */
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp, lr, sp;
};

/* The full register set saved on a trap, EXACTLY matching the layout
 * kernel_entry builds in vectors.S (offsets 0..271). fork() copies one
 * of these to make the child resume where the parent was; the syscall
 * handler reads/writes args and results in x[]. sp_el0 is the user
 * stack pointer — saved per-trap so two EL0 processes don't share it. */
struct trap_frame {
    uint64_t x[31];     /* x0..x30          (offset 0)   */
    uint64_t elr;       /* resume PC        (offset 248) */
    uint64_t spsr;      /* saved pstate     (offset 256) */
    uint64_t sp_el0;    /* user stack ptr   (offset 264) */
};

/* in switch.S / vectors.S */
extern char ret_to_user[];   /* trampoline: eret into a trap frame */

struct task {
    struct context  ctx;
    enum task_state state;
    int             id;
    char            name[16];
    uint64_t        wake_tick;   /* when to wake a SLEEPING task */
    uint64_t        chan;        /* what a BLOCKED task is waiting on */
    uint64_t        ticks_run;   /* timer ticks spent on the CPU */
    uint64_t        scratch;     /* free for the task to publish stats */
    char           *stack;       /* kmalloc'd kernel stack (task 0: boot stack) */
    uint32_t        stack_size;  /* its size (most tasks TASK_STACK_SIZE) */
    uint64_t       *pgtable;     /* private address space (0 = kernel's) */
    struct file    *ofile[NOFILE];   /* open files: fd -> object (Unix's table) */
    struct task    *parent;      /* who fork()ed us — for wait() */
    int             exit_code;   /* status a parent collects via wait() */
};

extern struct task *current;

void task_init(void);                /* adopt the boot flow as task 0 */
int  task_create(const char *name, void (*fn)(uint64_t), uint64_t arg);
/* Like task_create but with a custom kernel-stack size — for tasks that
 * recurse deeply (the browser runs the JS interpreter and TLS crypto). */
int  task_create_stack(const char *name, void (*fn)(uint64_t), uint64_t arg,
                       uint32_t stack_size);

/* Allocate a child task slot + kernel stack for fork() (parent set to
 * the caller, fds cleared, context left for proc_fork to forge).
 * Returns the task in a not-yet-runnable state, or NULL. */
struct task *task_alloc_child(const char *name);
int  task_wait(int *code_out);       /* reap any finished child; -1 if none */
int  task_waitpid(int pid, int *code_out);   /* reap one specific child */
void task_exit(void) __attribute__((noreturn));   /* current task is done */
void task_set_exit_code(int code);   /* record status for the parent's wait() */
int  task_kill(int id);              /* 0 ok, -1 bad id / refuses 0+self */
void task_sleep(uint64_t ticks);     /* yield the CPU for >= ticks */

/* Event waiting (xv6's sleep/wakeup). A "channel" is just a unique
 * pointer both sides agree on — usually the address of the thing
 * being waited for. Call task_block with IRQs MASKED, in a loop that
 * re-checks the condition. task_wakeup is safe from any context,
 * including interrupt handlers. */
void task_block(void *chan);
void task_wakeup(void *chan);
void schedule(void);                 /* pick + switch; call with IRQs MASKED */
void scheduler_tick(void);           /* called from the timer interrupt */
void task_dump(void);                /* the `ps` command */

/* A copy of the task table for display code (the GUI's task list). */
struct task_info {
    int      id;
    int      state;                  /* enum task_state */
    uint64_t ticks, scratch;
    char     name[16];
};
int      task_snapshot(struct task_info *out, int max);
uint64_t task_idle_ticks(void);
