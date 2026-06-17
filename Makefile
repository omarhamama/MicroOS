# Makefile for MicroOS
#
#   make        build the kernel (build/microos.elf)
#   make run    build and boot it in QEMU  (exit: Ctrl-A then X)
#   make clean  remove build artifacts
#
# We must use a CROSS-compiler: your Mac's normal clang/gcc targets
# macOS (Mach-O binaries, system calls, libc). The kernel needs
# "aarch64-elf" — ARM64 CPU, ELF format, no operating system beneath it.

CROSS   := aarch64-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

# Compiler flags, each one meaningful for kernel code:
#   -ffreestanding      no OS below us: no libc, main() isn't special
#   -mgeneral-regs-only no floating-point/SIMD registers (we don't save
#                       them on interrupts, so the kernel mustn't use them)
#   -nostdlib           link nothing we didn't write ourselves
# -MMD -MP: emit header-dependency files alongside each object, so
# editing a .h rebuilds every .c that includes it. (Discovered the
# hard way: a changed constant in mfs.h silently didn't reach fs.o.)
# -mno-outline-atomics: inline the ldaxr/stlxr loops for atomics
# instead of calling libgcc helpers we don't link.
# BOARD selects the target machine: empty = QEMU virt (default), or
# `make BOARD=raspi4` for the Raspberry Pi 4 (also QEMU's raspi4b). Each
# board gets its own build dir, link address, and -D so a Pi build and a
# virt build never mix objects.
BOARD ?=
ifeq ($(BOARD),raspi4)
  BOARDFLAG := -DBOARD_RASPI4
  LDSCRIPT  := linker-raspi4.ld
  B         := build-raspi4
else ifeq ($(BOARD),rk3326)
  BOARDFLAG := -DBOARD_RK3326
  LDSCRIPT  := linker-rk3326.ld
  B         := build-rk3326
else
  BOARDFLAG :=
  LDSCRIPT  := linker.ld
  B         := build
endif

CFLAGS  := -ffreestanding -mgeneral-regs-only -nostdlib \
           -mno-outline-atomics -Wall -Wextra -O2 -g -MMD -MP $(BOARDFLAG)
# --no-warn-rwx-segments: the linker warns that our kernel is readable,
# writable AND executable at once. It's right — with the MMU off there
# is no memory protection at all. Turning the MMU on fixes this for real.
LDFLAGS := -nostdlib -T $(LDSCRIPT) -Wl,--no-warn-rwx-segments

SRC_C   := $(wildcard src/*.c)
SRC_S   := $(wildcard src/*.S)
OBJ     := $(SRC_C:src/%.c=$(B)/%.o) $(SRC_S:src/%.S=$(B)/%.o)

# User programs: compiled SEPARATELY from the kernel (their own libc in
# user/, linked at 0x80000000 by user/user.ld), kept as real ELF files
# (.uelf), then wrapped as objects so the kernel can install them into
# /bin at boot and load them with its ELF loader. MicroOS ships its
# software, in a real executable format, on its own filesystem.
USERPROGS := hello rogue echo cat ls forkdemo wc pipedemo lisp
OBJ += $(USERPROGS:%=$(B)/%_blob.o)

KERNEL  := $(B)/microos.elf
IMAGE   := $(B)/microos.bin

# The disk: a plain 4 MiB file on your Mac, handed to the VM as a
# virtio block device. force-legacy=false selects the MODERN virtio
# interface our driver speaks (virtio_blk.c explains the difference).
DISK      := disk.img

QEMU      := qemu-system-aarch64
# -rtc base=localtime: the PL031 reflects YOUR Mac's wall clock (default
# is UTC). MicroOS reads it for the menu-bar clock and the `date` command.
QEMUBASE  := -M virt -cpu cortex-a72 -smp 2 -m 128M -rtc base=localtime \
             -global virtio-mmio.force-legacy=false \
             -drive if=none,file=$(DISK),format=raw,id=hd,file.locking=off \
             -device virtio-blk-device,drive=hd \
             -device ramfb \
             -device virtio-tablet-device \
             -device virtio-keyboard-device \
             -netdev user,id=n0 \
             -device virtio-net-device,netdev=n0 \
             -audiodev coreaudio,id=a0 \
             -device virtio-sound-device,audiodev=a0 \
             -kernel $(IMAGE)

all: $(IMAGE)

$(B):
	mkdir -p $(B)

$(B)/%.o: src/%.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# The MP3 decoder is the ONE kernel file allowed to use the FPU (the boot
# code enables it; the player task is its sole user — see boot.S). Compile
# it WITHOUT -mgeneral-regs-only so the float DSP can use FP/SIMD registers.
# -w: it's adapted vendored code (PDMP3), kept close to upstream, so we
# don't hold it to the tree's -Wall -Wextra cleanliness.
$(B)/mp3.o: src/mp3.c | $(B)
	$(CC) $(filter-out -mgeneral-regs-only -Wall -Wextra,$(CFLAGS)) -w -c $< -o $@

$(B)/%.o: src/%.S | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/u_%.o: user/%.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/u_crt0.o: user/crt0.S | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# Link each program: crt0 first (the entry point), then the program,
# then the user libc. -z max-page-size=4096 keeps ELF segments page-
# aligned so the loader can map each with its own permissions.
$(B)/%.uelf: $(B)/u_%.o $(B)/u_crt0.o $(B)/u_ulib.o user/user.ld
	$(CC) -nostdlib -Wl,-z,max-page-size=4096 -Wl,--no-warn-rwx-segments \
	    -T user/user.ld $(B)/u_crt0.o $(B)/u_$*.o $(B)/u_ulib.o -o $@

# Wrap the ELF as a linkable blob; objcopy names the symbols
# _binary_<name>_uelf_start/_end from the filename (run in $(B)).
$(B)/%_blob.o: $(B)/%.uelf
	cd $(B) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $*.uelf $*_blob.o

.SECONDARY:     # keep the intermediate .uelf files

$(KERNEL): $(OBJ) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

# QEMU boots a *raw binary* (with the Linux Image header from boot.S),
# not the ELF: that's what makes it follow the ARM64 boot protocol and
# hand us the Device Tree address in x0. The ELF still exists for
# debugging — it carries the symbols gdb and objdump need.
$(IMAGE): $(KERNEL)
	$(CROSS)objcopy -O binary $(KERNEL) $(IMAGE)

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1048576 count=4

# `run` opens the GUI window; the shell stays in THIS terminal (the
# window mirrors it). `run-term` is the old terminal-only experience.
run: $(IMAGE) $(DISK)
	$(QEMU) $(QEMUBASE) -serial mon:stdio

run-term: $(IMAGE) $(DISK)
	$(QEMU) $(QEMUBASE) -nographic

# Build + boot the Raspberry Pi 4 image in QEMU's raspi4b machine. No
# virtio (the Pi has none): the console is the PL011 UART and the display
# is the VideoCore framebuffer. serial0 = mini-UART (unused) -> null,
# serial1 = PL011 -> our console on stdio.
run-raspi:
	$(MAKE) BOARD=raspi4
	$(QEMU) -M raspi4b -m 2G -kernel build-raspi4/microos.bin \
	    -serial mon:stdio -serial null

# `dist` packages everything needed to run MicroOS on another machine
# (a Raspberry Pi, an Android tablet via Termux, any Linux box): the
# kernel image, a fresh disk, and the portable launcher. Copy the tarball
# over, extract, and run `./run.sh` — no toolchain needed on the device.
dist: $(IMAGE)
	rm -rf dist && mkdir -p dist
	cp $(IMAGE) dist/microos.bin
	cp run.sh dist/run.sh && chmod +x dist/run.sh
	dd if=/dev/zero of=dist/disk.img bs=1048576 count=4 2>/dev/null
	printf '%s\n' \
	  'MicroOS — run with QEMU on any host.' \
	  '  sudo apt install qemu-system-arm   # (Raspberry Pi / Linux)' \
	  '  pkg install qemu-system-aarch64    # (Android / Termux)' \
	  '  ./run.sh         # native GUI window' \
	  '  ./run.sh --vnc   # desktop over VNC :5900 (tablets/phones)' \
	  '  ./run.sh --text  # text shell only' \
	  '  ./run.sh --kvm   # hardware acceleration (Pi 4/5, Linux)' \
	  > dist/README.txt
	tar -czf microos-dist.tar.gz -C dist .
	@echo "wrote microos-dist.tar.gz ($$(du -h microos-dist.tar.gz | cut -f1)) — copy it to the Pi/tablet"

# `dist-raspi` packages the files for booting on a REAL Raspberry Pi 4:
# our kernel as kernel8.img plus a config.txt. Copy these (with the Pi's
# firmware: start4.elf + fixup4.dat from the raspberrypi/firmware repo)
# onto a FAT32 SD card. HDMI shows the desktop; UART (GPIO 14/15) is the
# console. (Input needs USB HID — not built yet; use the serial console.)
dist-raspi:
	$(MAKE) BOARD=raspi4
	rm -rf dist-raspi && mkdir -p dist-raspi
	cp build-raspi4/microos.bin dist-raspi/kernel8.img
	printf '%s\n' 'arm_64bit=1' 'kernel=kernel8.img' 'enable_uart=1' \
	  'dtoverlay=disable-bt' 'disable_overscan=1' > dist-raspi/config.txt
	printf '%s\n' \
	  'MicroOS on a real Raspberry Pi 4 — copy onto a FAT32 SD card:' \
	  '  kernel8.img, config.txt  (here) +' \
	  '  start4.elf, fixup4.dat    (from github.com/raspberrypi/firmware/boot)' \
	  'HDMI shows the desktop; the console is the PL011 UART on GPIO 14/15' \
	  '(3.3V USB-serial, 115200 8N1). Input devices (USB) are not yet built.' \
	  > dist-raspi/README.txt
	@echo "wrote dist-raspi/ (kernel8.img + config.txt) — add the Pi firmware + an SD card"

# `dist-rk3326` packages the R36S handheld build (Rockchip RK3326). This
# is SPECULATIVE/UNVERIFIED — there's no emulator for the chip. Boot it
# from the device's U-Boot (Ctrl-C during boot to reach the prompt):
#   load mmc 1:1 0x80000 microos.bin ; booti 0x80000 - ${fdtcontroladdr}
# HDMI shows the desktop IF U-Boot left a simple-framebuffer in its DTB;
# the console is UART2 (DesignWare 8250) on the device's serial pads.
dist-rk3326:
	$(MAKE) BOARD=rk3326
	rm -rf dist-rk3326 && mkdir -p dist-rk3326
	cp build-rk3326/microos.bin dist-rk3326/microos.bin
	printf '%s\n' \
	  'MicroOS for the R36S (Rockchip RK3326) — SPECULATIVE / UNVERIFIED.' \
	  'Copy microos.bin to the SD card. At the U-Boot prompt (serial):' \
	  '  load mmc 1:1 0x80000 microos.bin' \
	  '  booti 0x80000 - ${fdtcontroladdr}' \
	  'Console = UART2 (DesignWare 8250, 1.5 Mbaud or 115200). The desktop' \
	  'appears only if U-Boot passes a simple-framebuffer node in its DTB.' \
	  'No input driver yet (gamepad = GPIO/ADC); drive it over serial.' \
	  > dist-rk3326/README.txt
	@echo "wrote dist-rk3326/ — UNVERIFIED; expect to debug on hardware"

# Handy for learning: disassemble the kernel to see what the compiler did.
disasm: $(KERNEL)
	$(CROSS)objdump -d $(KERNEL) | less

-include $(wildcard $(B)/*.d)

# `clean` deliberately leaves disk.img alone — it holds YOUR files.
clean:
	rm -rf build build-raspi4 build-rk3326

wipedisk:
	rm -f $(DISK)

.PHONY: all run run-term run-raspi dist dist-raspi dist-rk3326 disasm clean wipedisk
