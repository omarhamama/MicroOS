/*
 * linker-rk3326.ld — layout for the R36S / Rockchip RK3326 build.
 *
 * Same as linker-raspi4.ld: link at 0x8_0000 with DRAM based at 0. The
 * device's U-Boot loads/relocates an arm64 Image to (RAM base + the
 * header's text_offset = 0x8_0000), which is where we link, so the
 * identity-mapped kernel's absolute addresses are correct. UNVERIFIED.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;
    __kernel_start = .;

    .text : {
        KEEP(*(.text.boot))
        *(.text*)
    }
    .rodata : { *(.rodata*) }

    . = ALIGN(4096);
    __ro_end = .;

    .data : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x4000;
    __stack_top = .;

    __heap_start = .;
}
