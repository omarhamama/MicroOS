/*
 * switch.S — the context switch: the beating heart of multitasking.
 *
 * cpu_switch_to(old, new) saves the CPU state of the current task into
 * `old`, loads the state of `new`, and RETURNS — but because it also
 * swapped the stack pointer and link register, it returns into the
 * middle of whatever the NEW task was doing when IT last called
 * cpu_switch_to. Every task is "paused inside this function"; running
 * is just not being paused right now.
 *
 * Why only x19-x30 and sp? The AArch64 calling convention says a
 * function may freely destroy x0-x18 ("caller-saved") — so whoever
 * called cpu_switch_to already expects those to be gone. Only the
 * "callee-saved" registers must survive a function call, so only they
 * define a paused task. (Everything else is on the task's own stack.)
 */

// void cpu_switch_to(struct context *old, struct context *new)
//                    x0 = old           x1 = new
.global cpu_switch_to
cpu_switch_to:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]     /* fp + lr: where we'll resume */
    mov     x9, sp
    str     x9, [x0, #96]

    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x9, [x1, #96]
    mov     sp, x9
    ret                              /* "return"... into the new task */

/*
 * Where brand-new tasks begin. task_create() forges a context whose
 * saved lr points here and whose x19/x20 hold the entry function and
 * its argument. The first time the scheduler switches to the task,
 * cpu_switch_to "returns" to this label.
 */
.global task_trampoline
task_trampoline:
    msr     daifclr, #2              /* new tasks start with IRQs on */
    mov     x0, x20                  /* the argument */
    blr     x19                      /* call the task's entry function */
    bl      task_exit                /* it returned: clean up */
1:  wfe                              /* task_exit never returns; just in case */
    b       1b
