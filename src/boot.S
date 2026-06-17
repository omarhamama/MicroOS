/*
 * boot.S — the very first code the CPU executes.
 *
 * When QEMU "powers on" the virtual machine it loads our ELF kernel into
 * RAM and jumps to _start with:
 *
 *   - the CPU in EL1 (Exception Level 1 = "kernel mode" on ARM64)
 *   - the MMU off (all addresses are physical addresses)
 *   - all interrupts masked
 *   - x0 = physical address of the Device Tree Blob (DTB), a data
 *     structure describing the hardware
 *
 * C code can't run yet: there is no stack, and global variables that
 * should be zero (.bss) contain random garbage. Fixing that is this
 * file's whole job. Then we jump to kernel_main().
 */

.section ".text.boot"

/*
 * Step 0: masquerade as a Linux kernel.
 *
 * Firmware/QEMU only follows the full ARM64 boot protocol (load the
 * Device Tree into RAM, pass its address in x0) for images that start
 * with the 64-byte Linux "Image" header. Without it, QEMU just jumps
 * to our entry point and we never get a DTB. So we wear the costume:
 * the header below is data, except the first word, which is a real
 * instruction that hops over it.
 */
.global _start
_start:
    b       header_end              /* code0: jump over the header */
    .long   0                       /* code1 (unused) */
    .quad   0x80000                 /* text_offset: load us 512 KiB into RAM */
    .quad   __heap_start - _start   /* image_size: code+data+bss+stack */
    .quad   2                       /* flags: little-endian, 4 KiB pages */
    .quad   0, 0, 0                 /* reserved */
    .ascii  "ARM\x64"               /* magic */
    .long   0                       /* reserved */
header_end:

    /*
     * Step 1: park every CPU core except core 0.
     *
     * Even on multi-core systems, early boot is single-threaded.
     * MPIDR_EL1 contains the core's ID in its low bits. Core 0
     * continues; everyone else sleeps forever in a wfe loop
     * ("wait for event" — a low-power halt).
     *
     * Note we leave x0 untouched the whole way through this file:
     * it still holds the DTB pointer, which becomes the first
     * argument to kernel_main(dtb).
     */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, core0
park:
    wfe
    b       park

core0:
    /*
     * Step 1.5: if we booted at EL2, drop to EL1.
     *
     * QEMU's virt board hands control to us already in EL1, but the
     * Raspberry Pi's firmware (and QEMU's raspi4b) starts the kernel in
     * EL2 (the hypervisor level). The rest of MicroOS is written for EL1,
     * so we configure EL2 just enough and `eret` down one level. x0 (the
     * DTB pointer) is preserved across the drop — we only touch x1.
     */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    in_el1                  /* already EL1 (virt): skip the drop */

    mov     x1, #(1 << 31)          /* HCR_EL2.RW = 1: EL1 runs AArch64 */
    msr     hcr_el2, x1
    mrs     x1, cnthctl_el2         /* let EL1 read the timers without trapping */
    orr     x1, x1, #3              /*   EL1PCTEN | EL1PCEN */
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr        /* virtual timer offset = 0 */
    ldr     x1, =0x30d00800         /* a sane SCTLR_EL1 (RES1 bits, MMU off) */
    msr     sctlr_el1, x1
    mov     x1, #0x3c5              /* SPSR: EL1h, DAIF (all interrupts) masked */
    msr     spsr_el2, x1
    adr     x1, in_el1
    msr     elr_el2, x1
    eret                            /* ... now running at EL1 ... */

in_el1:
    /*
     * Step 2: give ourselves a stack.
     *
     * C compiles to code that pushes/pops local variables and return
     * addresses on a stack — without sp set, the first C function call
     * would scribble over random memory. __stack_top comes from
     * linker.ld. Stacks grow DOWNWARD on ARM64.
     */
    ldr     x1, =__stack_top
    mov     sp, x1

    /*
     * Step 3: zero out the .bss section.
     *
     * The C standard promises that `int counter;` at global scope
     * starts as 0. On a real machine, fresh RAM is garbage — the
     * "promise" is actually kept by code like this, storing zeros
     * over the whole region 8 bytes at a time.
     */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
bss_loop:
    cmp     x1, x2
    b.hs    bss_done
    str     xzr, [x1], #8       /* *x1 = 0; x1 += 8 */
    b       bss_loop
bss_done:

    /*
     * Step 4: install our exception vector table (see vectors.S).
     *
     * VBAR_EL1 ("Vector Base Address Register") tells the CPU where
     * to jump when an interrupt or fault occurs. Until this is set,
     * any exception sends the CPU into the weeds.
     */
    ldr     x1, =vector_table
    msr     vbar_el1, x1

    /*
     * Step 4.5: turn the FPU/SIMD unit ON (CPACR_EL1.FPEN = 0b11).
     *
     * The kernel is built -mgeneral-regs-only and never touches floating-
     * point registers; interrupt handlers don't save them. We enable the
     * FPU for exactly ONE consumer — the music player's MP3 decoder
     * (mp3.c, the only file compiled WITH the FPU). That is safe precisely
     * because nothing else uses FP registers: no other task or interrupt
     * can clobber them mid-decode, so there is nothing to save or restore
     * on a context switch. (A general-purpose FPU would need lazy FP-state
     * save/restore in the trap path; keeping FP single-user avoids it.)
     */
    mrs     x1, cpacr_el1
    orr     x1, x1, #(3 << 20)      /* FPEN = 0b11: don't trap FP at EL0/EL1 */
    msr     cpacr_el1, x1
    isb

    /*
     * Step 5: enter C. x0 still holds the DTB pointer, which the
     * AArch64 calling convention treats as the first argument.
     */
    bl      kernel_main

    /* kernel_main should never return — but if it does, halt politely. */
halt:
    wfe
    b       halt

/*
 * Where SECONDARY cores enter, years (well, milliseconds) after core 0
 * parked them up top. smp.c asks PSCI firmware to start core 1 here,
 * and PSCI delivers the context argument in x0 — we pass the new
 * core's stack top through it. C does the rest (smp.c).
 */
.global secondary_start
secondary_start:
    mov     sp, x0
    bl      secondary_main
1:  wfe
    b       1b
