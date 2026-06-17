/*
 * vectors.S — the exception vector table.
 *
 * When something interrupts normal execution — a hardware interrupt
 * (IRQ), a fault like dividing by zero, or a deliberate trap — the ARM64
 * CPU jumps to a fixed slot in this table. Which slot depends on:
 *
 *   1. WHAT happened:
 *        Synchronous  — caused by the instruction itself (page fault, brk)
 *        IRQ          — normal hardware interrupt (our timer!)
 *        FIQ          — "fast" interrupt (rarely used; we don't)
 *        SError       — system error (bad bus access)
 *
 *   2. WHERE the CPU was:
 *        group 0: EL1 while using SP_EL0   (we never do this)
 *        group 1: EL1 using SP_EL1         <- kernel code: this is us
 *        group 2: EL0 (user mode, AArch64) (no user mode yet)
 *        group 3: EL0 in AArch32 mode      (not us either)
 *
 * 4 groups x 4 kinds = 16 slots. Each slot is exactly 0x80 bytes (32
 * instructions) and the whole table must be 2 KiB aligned (hence the
 * .align 11 = 2^11 below). Our slots just branch to handlers.
 */

/*
 * Saving context: an interrupt can fire between ANY two instructions.
 * The interrupted code expects every register to still hold its value
 * when it resumes — so before calling any C handler we push all 31
 * general-purpose registers, and restore them after. This save area
 * is called a "trap frame" — and switching stacks (each holding its
 * own trap frame) between tasks is exactly how the scheduler
 * multitasks.
 *
 * We also save ELR_EL1 (where to resume) and SPSR_EL1 (the saved
 * processor state). These live in CPU registers, and there is only
 * ONE of each: if we switch to another task inside an interrupt
 * handler and THAT task takes its own interrupt, the CPU overwrites
 * them. Without stashing them per-task on the stack, the first task
 * would eventually eret into the wrong place. (Forgetting this is a
 * rite-of-passage scheduler bug.)
 */
.macro kernel_entry
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]
    mrs     x10, elr_el1
    mrs     x11, spsr_el1
    stp     x10, x11, [sp, #248]
    mrs     x10, sp_el0             /* the USER stack pointer... */
    str     x10, [sp, #264]         /* ...saved per-task so EL0 procs don't share it */
.endm

.macro kernel_exit
    ldp     x10, x11, [sp, #248]
    msr     elr_el1, x10
    msr     spsr_el1, x11
    ldr     x10, [sp, #264]
    msr     sp_el0, x10            /* restore THIS task's user stack pointer */
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]
    add     sp, sp, #272
    eret                        /* "exception return" — resume what was running */
.endm

/*
 * ret_to_user — fork()'s landing pad. The scheduler's cpu_switch_to
 * "returns" here for a brand-new child: sp already points at a trap
 * frame the child was given (a copy of the parent's, with x0 = 0), and
 * kernel_exit restores it and erets into EL0. The child wakes up as if
 * it had just returned from the fork() syscall — but returning 0.
 */
.global ret_to_user
ret_to_user:
    kernel_exit

/* Each table slot is 128 bytes; .align 7 = 2^7 pads to the next slot. */
.macro ventry  label
.align 7
    b       \label
.endm

.align 11
.global vector_table
vector_table:
    /* Group 0: from EL1 with SP_EL0 — unexpected */
    ventry  vec_bad_0           /* synchronous */
    ventry  vec_bad_1           /* IRQ */
    ventry  vec_bad_2           /* FIQ */
    ventry  vec_bad_3           /* SError */

    /* Group 1: from EL1 with SP_EL1 — OUR kernel. The two that matter: */
    ventry  vec_el1_sync        /* faults, brk instructions, ... */
    ventry  vec_el1_irq         /* hardware interrupts (timer) */
    ventry  vec_bad_6           /* FIQ */
    ventry  vec_bad_7           /* SError */

    /* Group 2: from EL0 AArch64 — USER MODE enters the kernel here */
    ventry  vec_el0_sync        /* svc syscalls, user faults */
    ventry  vec_el0_irq         /* timer fires while user code runs */
    ventry  vec_bad_10
    ventry  vec_bad_11

    /* Group 3: from EL0 AArch32 — never */
    ventry  vec_bad_12
    ventry  vec_bad_13
    ventry  vec_bad_14
    ventry  vec_bad_15

/*
 * A hardware interrupt while the kernel runs: save everything, let C
 * figure out which device it was, restore everything, resume.
 */
vec_el1_irq:
    kernel_entry
    bl      handle_irq
    kernel_exit

/*
 * A synchronous exception: something our own code did (a fault or a
 * deliberate `brk`). We pass C the two registers that explain it:
 *   ESR_EL1 = Exception Syndrome Register: WHAT happened
 *   ELR_EL1 = Exception Link Register: the address WHERE it happened
 * handle_sync() prints a crash report and halts, so no need to return.
 */
vec_el1_sync:
    kernel_entry
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    bl      handle_sync
    kernel_exit

/*
 * Entry from USER MODE (EL0). Two notes that make this work:
 *
 *  - The CPU lands here at EL1 using SP_EL1 — the kernel stack of the
 *    task that was running, which is exactly where its frame belongs.
 *    The user's own stack pointer is parked untouched in SP_EL0.
 *
 *  - For a syscall, the C handler needs to READ arguments from and
 *    WRITE results into the saved registers, so we hand it a pointer
 *    to the whole trap frame (which is simply sp after kernel_entry).
 */
vec_el0_sync:
    kernel_entry
    mov     x0, sp                  /* the trap frame */
    mrs     x1, esr_el1
    bl      handle_el0_sync
    kernel_exit

vec_el0_irq:
    kernel_entry
    bl      handle_irq              /* same dispatcher; may preempt */
    kernel_exit

/* All 12 "shouldn't happen" slots: report which slot fired, then halt. */
.macro bad_handler  label, index
\label:
    kernel_entry
    mov     x0, #\index
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    bl      handle_bad_exception    /* never returns */
1:  wfe
    b       1b
.endm

bad_handler vec_bad_0,  0
bad_handler vec_bad_1,  1
bad_handler vec_bad_2,  2
bad_handler vec_bad_3,  3
bad_handler vec_bad_6,  6
bad_handler vec_bad_7,  7
bad_handler vec_bad_10, 10
bad_handler vec_bad_11, 11
bad_handler vec_bad_12, 12
bad_handler vec_bad_13, 13
bad_handler vec_bad_14, 14
bad_handler vec_bad_15, 15
