/* Linker script for MicroOS user programs.
 *
 * Every program is linked to run at 0x8000_0000 — and every program
 * can use the same address, because each process gets its own page
 * tables: the SAME virtual address resolves to DIFFERENT physical
 * pages per process. (Before virtual memory, programs had to be
 * relocated to wherever free memory happened to be.)
 *
 * Sections are PAGE-ALIGNED so the ELF loader can give each its own
 * permissions: .text read-only+executable, .data read/write+no-exec.
 * That separation is what makes per-process W^X possible. */

ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    .text   : { KEEP(*(.text.start)) *(.text*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
