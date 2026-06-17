#!/bin/sh
# run.sh — portable launcher for MicroOS under QEMU.
#
# MicroOS targets QEMU's `virt` machine (virtio + ramfb), so the way to
# run it on ANY host — your Mac, a Raspberry Pi running Linux, or an
# Android tablet via Termux — is to emulate that machine here. This script
# auto-picks an audio backend and a display so the same command works
# everywhere. Copy microos.bin + disk.img + run.sh to the device and run.
#
# Usage:
#   ./run.sh                 native GUI window (auto display + audio)
#   ./run.sh --vnc           serve the desktop over VNC on :5900 (phones)
#   ./run.sh --text          text shell only, no GUI (serial on stdio)
#   ./run.sh --kvm           add hardware acceleration (Linux w/ /dev/kvm)
#   ./run.sh --check         print the QEMU command and exit (don't run)
#   AUDIO=none ./run.sh      override the audio backend (none|pa|alsa|...)

set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# locate the kernel image and disk next to the script (or in build/)
BIN=""
for c in "$DIR/microos.bin" "$DIR/build/microos.bin"; do
    [ -f "$c" ] && BIN="$c" && break
done
[ -n "$BIN" ] || { echo "run.sh: microos.bin not found (run 'make' first)"; exit 1; }

DISK="$DIR/disk.img"
if [ ! -f "$DISK" ]; then
    echo "run.sh: creating a blank 4 MiB disk.img (MicroOS will format it)"
    dd if=/dev/zero of="$DISK" bs=1048576 count=4 2>/dev/null
fi

QEMU=${QEMU:-qemu-system-aarch64}

# --- pick an audio backend: macOS -> coreaudio, Linux -> pa/alsa, else none
if [ -z "${AUDIO:-}" ]; then
    case "$(uname -s)" in
        Darwin) AUDIO=coreaudio ;;
        Linux)  if [ -n "${PULSE_SERVER:-}" ] || [ -S "/run/user/$(id -u)/pulse/native" ] 2>/dev/null; then
                    AUDIO=pa
                elif [ -d /proc/asound ]; then AUDIO=alsa
                else AUDIO=none; fi ;;
        *)      AUDIO=none ;;
    esac
fi

# --- parse flags
DISPLAY_MODE=native; KVM=""; CHECK=""
for a in "$@"; do
    case "$a" in
        --vnc)   DISPLAY_MODE=vnc ;;
        --text)  DISPLAY_MODE=text ;;
        --kvm)   KVM="-enable-kvm -cpu host" ;;
        --check) CHECK=1 ;;
        *) echo "run.sh: unknown option '$a'"; exit 1 ;;
    esac
done

# --- display / serial wiring
case "$DISPLAY_MODE" in
    vnc)  DISP="-display vnc=:0";      SER="-serial stdio" ;;
    text) DISP="-nographic";           SER="" ;;          # -nographic owns stdio
    *)    DISP="-display default,show-cursor=on"; SER="-serial mon:stdio" ;;
esac

# CPU: with KVM use the host CPU, otherwise emulate a Cortex-A72
CPU="-cpu cortex-a72"
[ -n "$KVM" ] && CPU=""

set -- $QEMU -M virt $CPU $KVM -smp 2 -m 128M \
    -global virtio-mmio.force-legacy=false \
    -drive "if=none,file=$DISK,format=raw,id=hd,file.locking=off" \
    -device virtio-blk-device,drive=hd \
    -device ramfb \
    -device virtio-tablet-device \
    -device virtio-keyboard-device \
    -netdev user,id=n0 -device virtio-net-device,netdev=n0 \
    -audiodev "$AUDIO,id=a0" -device virtio-sound-device,audiodev=a0 \
    -kernel "$BIN" $DISP $SER

if [ -n "$CHECK" ]; then
    echo "audio=$AUDIO  display=$DISPLAY_MODE${KVM:+  +kvm}"
    echo "$@"
    exit 0
fi

[ "$DISPLAY_MODE" = vnc ] && echo "MicroOS desktop on VNC :5900 — connect a VNC viewer to localhost:5900"
exec "$@"
