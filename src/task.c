/*
 * task.c — tasks and the scheduler.
 *
 * Multitasking on one CPU is an illusion built from two parts:
 *
 *   1. cpu_switch_to (switch.S) — mechanism. Freeze this task, thaw
 *      another. ~25 instructions.
 *   2. schedule() — policy. WHICH task next? Ours is round-robin:
 *      everyone gets a 10 ms turn, no priorities. (Real schedulers are
 *      this loop plus forty years of fairness research.)
 *
 * The illusion becomes PREEMPTIVE because the timer interrupt calls
 * scheduler_tick(): a task doesn't have to agree to give up the CPU —
 * the clock takes it. That's the line between "cooperative" (Windows
 * 3.1, classic Mac OS — one hung program froze everything) and
 * "preemptive" (everything since). MicroOS does both: task_sleep() and
 * blocking are voluntary handoffs; the timer is the involuntary one.
 *
 * Concurrency rule of this kernel: schedule() must be entered with
 * IRQs MASKED. Anyone may mask-call-unmask; the timer path arrives
 * masked already (hardware masks IRQs on exception entry).
 */

#include "task.h"
#include "mem.h"
#include "mmu.h"
#include "lib.h"
#include "timer.h"
#include "kprintf.h"

static struct task tasks[MAX_TASKS];
struct task *current;
static uint64_t idle_ticks;     /* ticks when NO task wanted the CPU */
static int in_idle;             /* the idle loop below is spinning */

/* in switch.S */
void cpu_switch_to(struct context *old, struct context *new);
extern char task_trampoline[];

static void irq_mask(void)   { asm volatile("msr daifset, #2"); }
static void irq_unmask(void) { asm volatile("msr daifclr, #2"); }

void task_init(void)
{
    /* The code running RIGHT NOW — boot flow, soon the shell — simply
     * declares itself task 0. No magic: a "task" is just a bookkeeping
     * entry plus a stack, and we already have both. */
    struct task *t = &tasks[0];
    t->state = TASK_RUNNING;
    t->id = 0;
    memcpy(t->name, "shell", 6);
    current = t;
}

static int task_create_sz(const char *name, void (*fn)(uint64_t), uint64_t arg,
                          uint32_t stack_size)
{
    irq_mask();

    struct task *t = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_ZOMBIE) {
            t = &tasks[i];
            t->id = i;
            break;
        }
    }
    if (!t) {
        irq_unmask();
        return -1;                      /* task table full */
    }

    kfree(t->stack);                    /* reuse a zombie's slot... */
    if (t->pgtable) {                   /* ...and its address space.
                                         * Safe: by the time anyone calls
                                         * task_create, the scheduler has
                                         * long since switched away from
                                         * the zombie's tables. */
        mmu_user_table_free(t->pgtable);
        t->pgtable = 0;
    }
    t->stack = kmalloc(stack_size);
    t->stack_size = stack_size;
    if (!t->stack) {
        t->state = TASK_UNUSED;
        irq_unmask();
        return -1;
    }

    /*
     * Forge a saved context that LOOKS like the task once called
     * cpu_switch_to from task_trampoline: lr = the trampoline, sp = a
     * fresh stack, x19/x20 = what the trampoline should call. The
     * first switch to it "returns" into code that never ran. Time
     * travel by bookkeeping.
     */
    memset(&t->ctx, 0, sizeof(t->ctx));
    t->ctx.lr  = (uint64_t)task_trampoline;
    t->ctx.sp  = (uint64_t)(t->stack + stack_size) & ~15UL;
    t->ctx.x19 = (uint64_t)fn;
    t->ctx.x20 = arg;

    int i = 0;
    while (name[i] && i < (int)sizeof(t->name) - 1) {
        t->name[i] = name[i];
        i++;
    }
    t->name[i] = '\0';
    t->wake_tick = t->ticks_run = t->scratch = 0;
    t->parent = current;
    t->exit_code = 0;
    for (int j = 0; j < NOFILE; j++)
        t->ofile[j] = 0;                /* fresh table; proc/fork fills it */
    t->state = TASK_RUNNABLE;           /* last: now the scheduler may take it */

    irq_unmask();
    return t->id;
}

int task_create(const char *name, void (*fn)(uint64_t), uint64_t arg)
{
    return task_create_sz(name, fn, arg, TASK_STACK_SIZE);
}

int task_create_stack(const char *name, void (*fn)(uint64_t), uint64_t arg,
                      uint32_t stack_size)
{
    return task_create_sz(name, fn, arg, stack_size);
}

/* fork()'s helper: grab a slot + kernel stack, mark the caller as
 * parent, clear the fd table. The CONTEXT (where it resumes) and the
 * RUNNABLE state are left to proc_fork, which forges a trap frame so
 * the child returns from fork() in EL0. Returned in TASK_BLOCKED so
 * the scheduler won't run it half-built. */
struct task *task_alloc_child(const char *name)
{
    irq_mask();

    struct task *t = 0;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_ZOMBIE) {
            t = &tasks[i];
            t->id = i;
            break;
        }
    if (!t) {
        irq_unmask();
        return 0;
    }

    kfree(t->stack);
    if (t->pgtable) {
        mmu_user_table_free(t->pgtable);
        t->pgtable = 0;
    }
    t->stack = kmalloc(TASK_STACK_SIZE);
    t->stack_size = TASK_STACK_SIZE;
    if (!t->stack) {
        t->state = TASK_UNUSED;
        irq_unmask();
        return 0;
    }

    int i = 0;
    while (name[i] && i < (int)sizeof(t->name) - 1) {
        t->name[i] = name[i];
        i++;
    }
    t->name[i] = '\0';
    t->wake_tick = t->ticks_run = t->scratch = 0;
    t->parent = current;
    t->exit_code = 0;
    for (int j = 0; j < NOFILE; j++)
        t->ofile[j] = 0;
    t->state = TASK_BLOCKED;            /* half-built: not yet schedulable */

    irq_unmask();
    return t;
}

void task_set_exit_code(int code)
{
    current->exit_code = code;
}

/* Wait for ONE specific child (by id) to finish and reap it. The shell
 * uses this to run a foreground command without accidentally reaping a
 * different background child that happened to exit first. */
int task_waitpid(int pid, int *code_out)
{
    if (pid < 1 || pid >= MAX_TASKS)
        return -1;
    for (;;) {
        irq_mask();
        if (tasks[pid].parent != current) {    /* not ours (or gone) */
            irq_unmask();
            return -1;
        }
        if (tasks[pid].state == TASK_ZOMBIE) {
            if (code_out)
                *code_out = tasks[pid].exit_code;
            tasks[pid].state = TASK_UNUSED;
            tasks[pid].parent = 0;
            irq_unmask();
            return pid;
        }
        task_block(current);                    /* a child wakes us on exit */
        irq_unmask();
    }
}

/* wait(): block until one of our children becomes a zombie, reap it
 * (free its slot for reuse), and return its pid + exit code. Stubbed
 * minimally here; Layer 3 relies on it once fork() exists. */
int task_wait(int *code_out)
{
    for (;;) {
        irq_mask();
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].parent == current && tasks[i].state == TASK_ZOMBIE) {
                int pid = tasks[i].id;
                if (code_out)
                    *code_out = tasks[i].exit_code;
                tasks[i].state = TASK_UNUSED;   /* reaped */
                tasks[i].parent = 0;
                irq_unmask();
                return pid;
            }
        }
        /* any children left to wait for? */
        int have_child = 0;
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].parent == current && tasks[i].state != TASK_UNUSED)
                have_child = 1;
        if (!have_child) {
            irq_unmask();
            return -1;
        }
        task_block(current);            /* a child wakes us in task_exit */
        irq_unmask();
    }
}

void task_exit(void)
{
    /* Closing our descriptors here is what makes pipes work: when a
     * program ends, its write end of a pipe closes, and the reader on
     * the far side finally sees EOF. */
    for (int i = 0; i < NOFILE; i++)
        if (current->ofile[i]) {
            file_close(current->ofile[i]);
            current->ofile[i] = 0;
        }

    irq_mask();
    current->state = TASK_ZOMBIE;       /* stack freed when slot is reused */
    task_wakeup(current->parent);       /* a waiting parent can reap us */
    schedule();                         /* never returns to a zombie */
    for (;;)
        asm volatile("wfe");
}

int task_kill(int id)
{
    if (id <= 0 || id >= MAX_TASKS || current->id == id)
        return -1;                      /* no killing the shell or yourself */
    irq_mask();
    int ok = (tasks[id].state == TASK_RUNNABLE ||
              tasks[id].state == TASK_SLEEPING);
    if (ok)
        tasks[id].state = TASK_ZOMBIE;
    irq_unmask();
    return ok ? 0 : -1;
}

void task_sleep(uint64_t ticks)
{
    irq_mask();
    current->wake_tick = timer_ticks() + ticks;
    current->state = TASK_SLEEPING;
    schedule();
    irq_unmask();
}

/*
 * Blocking on events: until now, a task waiting for a keystroke or a
 * disk completion sat in a wfi loop — harmless, but it stayed RUNNING
 * and woke on every stray interrupt to re-check. With channels, the
 * waiter leaves the run queue entirely and the interrupt that
 * produces the event wakes exactly the tasks that wanted it.
 *
 * The classic lost-wakeup race (event fires between "check condition"
 * and "go to sleep") is closed by the calling convention: check and
 * block with IRQs masked, in a loop —
 *
 *     irq_mask();
 *     while (!condition)
 *         task_block(chan);      <- IRQs stay masked until the switch
 *     irq_unmask();
 *
 * The waking interrupt can only run once we've safely switched away.
 * (On a multi-core kernel this dance needs a real lock — that story
 * is in the SMP lesson.)
 */
void task_block(void *chan)
{
    current->chan = (uint64_t)chan;
    current->state = TASK_BLOCKED;
    schedule();                         /* returns once woken + rescheduled */
    current->chan = 0;
}

void task_wakeup(void *chan)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_BLOCKED && tasks[i].chan == (uint64_t)chan)
            tasks[i].state = TASK_RUNNABLE;
}

static struct task *pick_next(void)
{
    /* Wake anyone whose alarm has rung. */
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_SLEEPING &&
            timer_ticks() >= tasks[i].wake_tick)
            tasks[i].state = TASK_RUNNABLE;

    /* Round-robin: scan starting AFTER the current task, so everyone
     * gets a turn before anyone gets two. */
    for (int i = 1; i <= MAX_TASKS; i++) {
        struct task *t = &tasks[(current->id + i) % MAX_TASKS];
        if (t->state == TASK_RUNNABLE)
            return t;
    }
    if (current->state == TASK_RUNNING)
        return current;                 /* nobody else wants the CPU */
    return 0;                           /* everyone is asleep */
}

void schedule(void)
{
    struct task *next;

    /* If every task is sleeping, THIS loop is the idle loop: unmask,
     * halt until the next interrupt (probably the timer), re-check.
     * The in_idle flag stops scheduler_tick() from re-entering
     * schedule() out of those very interrupts — each re-entry would
     * nest a fresh stack frame, and a long-enough idle stretch would
     * overflow the kernel stack. (Found the hard way: ~150 idle ticks
     * = ~150 nested frames = a corpse with PC=0.) The loop itself
     * re-checks pick_next() after every wakeup, so nothing is lost. */
    in_idle = 1;
    while (!(next = pick_next())) {
        asm volatile("msr daifclr, #2; wfi; msr daifset, #2");
    }
    in_idle = 0;

    if (next == current)
        return;

    struct task *prev = current;
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_RUNNABLE;    /* preempted, not blocked */
    next->state = TASK_RUNNING;
    current = next;

    /* Address-space switch: if the next task lives in a different
     * world, point the MMU at it. We compare against mmu_current_table()
     * — the SHARED record of TTBR0 — not a private static, so a table
     * the program loader switched to directly is still accounted for.
     * Kernel mappings are identical in every table, so this very code
     * keeps working across the switch. */
    uint64_t *want = next->pgtable ? next->pgtable : mmu_kernel_table();
    if (want != mmu_current_table())
        mmu_switch(want);

    cpu_switch_to(&prev->ctx, &next->ctx);
    /* ...and when someone switches back to us, we resume right here. */
}

void scheduler_tick(void)
{
    if (!current)
        return;                         /* timer fired before task_init */
    /* Charge the tick to whoever is actually RUNNING. If the tick
     * landed in the idle loop (current task asleep, wfi in schedule),
     * it's idle time — charging it to the sleeping task would invent
     * CPU usage out of thin air. */
    if (current->state == TASK_RUNNING)
        current->ticks_run++;
    else
        idle_ticks++;
    if (in_idle)
        return;             /* the idle loop will re-check on its own */
    schedule();                         /* a 10 ms quantum: one tick, next task */
}

int task_snapshot(struct task_info *out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_TASKS && n < max; i++) {
        if (tasks[i].state == TASK_UNUSED)
            continue;
        out[n].id = tasks[i].id;
        out[n].state = (int)tasks[i].state;
        out[n].ticks = tasks[i].ticks_run;
        out[n].scratch = tasks[i].scratch;
        memcpy(out[n].name, tasks[i].name, sizeof(out[n].name));
        n++;
    }
    return n;
}

uint64_t task_idle_ticks(void)
{
    return idle_ticks;
}

void task_dump(void)
{
    static const char *names[] = {
        "unused", "RUNNABLE", "RUNNING", "SLEEPING", "BLOCKED", "zombie"
    };
    kprintf(" ID  STATE     TICKS    SCRATCH  NAME\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED)
            continue;
        kprintf("  %d  %s\t%lu\t%lu\t%s%s\n",
                t->id, names[t->state], t->ticks_run, t->scratch,
                t->name, t == current ? " <- you" : "");
    }
    kprintf("idle: %lu ticks (CPU halted, nobody runnable)\n", idle_ticks);
}
