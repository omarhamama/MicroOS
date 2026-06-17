/*
 * linker-raspi4.ld — layout for the Raspberry Pi 4 build.
 *
 * Identical to linker.ld except the load address: the Pi's GPU firmware
 * loads the kernel ("kernel8.img") at physical 0x8_0000 with DRAM based
 * at 0, whereas QEMU's virt board bases RAM at 0x4000_0000. Because the
 * MMU map is an identity map, the kernel must be LINKED at the address it
 * is LOADED at — hence a separate script for each board.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;
    __kernel_start = .;

    .text : {
        KEEP(*(.text.boot))   /* boot.S first — it's the entry point */
        *(.text*)
    }

    .rodata : { *(.rodata*) }

    . = ALIGN(4096);          /* W^X page boundary: code|rodata vs data */
    __ro_end = .;

    .data : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x4000;              /* 16 KiB boot stack */
    __stack_top = .;

    __heap_start = .;
}
