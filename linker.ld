/*
 * linker.ld — tells the linker how to lay out our kernel in memory.
 *
 * QEMU's "virt" machine puts RAM at physical address 0x4000_0000.
 * QEMU also copies the Device Tree Blob (DTB) to the very start of RAM,
 * so we load our kernel a bit higher, at 0x4008_0000 (the conventional
 * ARM64 kernel load offset), leaving room for the DTB below us.
 *
 * Memory map we are building:
 *
 *   0x4000_0000  +------------------+
 *                | DTB (from QEMU)  |
 *   0x4008_0000  +------------------+
 *                | .text   (code)   |  <- _start is the very first byte
 *                | .rodata (consts) |
 *                | .data   (globals)|
 *                | .bss    (zeroed) |
 *                +------------------+
 *                | stack (16 KiB)   |  <- grows DOWN from __stack_top
 *                +------------------+
 *                | heap ...         |  <- kmalloc() hands out from here
 *                +------------------+
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;
    __kernel_start = .;

    .text : {
        KEEP(*(.text.boot))   /* boot.S must come first — it's our entry point */
        *(.text*)
    }

    .rodata : { *(.rodata*) }

    /* Page-align the boundary between read-only and writable: MMU
     * permissions are per 4 KiB page, so code+constants and data must
     * not share a page if we want W^X ("writable xor executable"). */
    . = ALIGN(4096);
    __ro_end = .;

    .data : { *(.data*) }

    /* .bss holds uninitialized globals. The C standard says they start
     * as zero — but nobody zeroes RAM for us, so boot.S must do it.
     * These two symbols tell boot.S where .bss begins and ends. */
    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    /* Reserve 16 KiB for the kernel stack. AArch64 requires the stack
     * pointer to stay 16-byte aligned. */
    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x4000;
    __stack_top = .;

    /* Everything after the stack is heap territory. */
    __heap_start = .;
}
