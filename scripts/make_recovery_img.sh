#!/usr/bin/env bash
# Build a flashable/bootable TWRP v2 image from your device's stock (or any
# working) recovery image by swapping the recovery binary in its ramdisk.
#
#   scripts/make_recovery_img.sh <recovery.img|boot.img|vendor_boot.img> [recovery-binary|auto] [twrp.fstab]
#
# The binary defaults to "auto": the CPU architecture is read from the binaries
# already in the stock ramdisk (arm64, armeabi-v7a, armeabi, x86, x86_64, riscv64,
# mips) and the matching out/<arch>/recovery is used - built first if it is missing.
# A binary for the wrong CPU is refused (the recovery would never start); set
# TW_FORCE_ARCH=1 to override.
#
# Needs magiskboot on the PC: extract lib/x86_64/libmagiskboot.so from the
# Magisk APK, rename it to magiskboot, chmod +x, and put it in PATH (or set MAGISKBOOT).
#
# Then test WITHOUT flashing first:   scripts/install.sh twrpv2-<image>.img   (fastboot boot)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$(realpath "${1:?usage: $0 <image> [binary|auto] [fstab]}")"
BIN_ARG="${2:-auto}"
FSTAB="${3:-}"
[ -n "$FSTAB" ] && FSTAB="$(realpath "$FSTAB")"
[ "$BIN_ARG" = auto ] || BIN_ARG="$(realpath "$BIN_ARG")"
MB="${MAGISKBOOT:-magiskboot}"
PY="${PYTHON:-python3}"
OUTDIR="${TW_OUT_DIR:-$ROOT/out}"   # where scripts/build.sh puts out/<arch>/recovery
command -v "$MB" >/dev/null || { echo "magiskboot not found (set MAGISKBOOT=/path/to/magiskboot)" >&2; exit 1; }

OUT="$PWD/twrpv2-$(basename "$IMG")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
cp "$IMG" in.img
"$MB" unpack -h in.img

# Recovery ramdisk location differs by image type.
CPIO=""
for c in ramdisk.cpio vendor_ramdisk/recovery.cpio vendor_ramdisk/ramdisk.cpio; do
  [ -f "$c" ] && { CPIO="$c"; break; }
done
[ -n "$CPIO" ] || { echo "No ramdisk found in $(basename "$IMG")" >&2; exit 1; }

# ---- which CPU is this for? --------------------------------------------------------
STOCK_ARCH="" FROM=""
for c in system/bin/recovery sbin/recovery system/bin/init init system/bin/linker64 system/bin/linker; do
  rm -f probe.bin
  "$MB" cpio "$CPIO" "extract $c probe.bin" >/dev/null 2>&1 || continue
  [ -f probe.bin ] && [ ! -L probe.bin ] || continue
  if a="$("$PY" "$ROOT/tools/elf_abi.py" probe.bin 2>/dev/null)"; then STOCK_ARCH="$a"; FROM="$c"; break; fi
done
if [ -z "$STOCK_ARCH" ] && command -v adb >/dev/null; then
  # vendor_boot ramdisks often hold no binaries: ask the phone if it is connected.
  abi="$(adb shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r' || true)"
  if [ -n "$abi" ] && a="$("$PY" "$ROOT/tools/elf_abi.py" --abi "$abi")"; then STOCK_ARCH="$a"; FROM="adb (ro.product.cpu.abi=$abi)"; fi
fi
[ -n "$STOCK_ARCH" ] && echo "Architecture: $STOCK_ARCH (from $FROM)"

if [ "$BIN_ARG" = auto ]; then
  if [ -z "$STOCK_ARCH" ]; then
    STOCK_ARCH=arm64
    echo "warning: couldn't tell the CPU architecture from this image - assuming arm64." >&2
    echo "         Pass the binary explicitly (e.g. out/arm/recovery) if your phone is 32-bit or x86." >&2
  fi
  BIN="$OUTDIR/$STOCK_ARCH/recovery"
  if [ ! -f "$BIN" ]; then
    echo "Building TWRP v2 for $STOCK_ARCH first"
    (cd "$ROOT" && scripts/build.sh "$STOCK_ARCH")
  fi
else
  BIN="$BIN_ARG"
  [ -f "$BIN" ] || { echo "recovery binary $BIN not found - run scripts/build.sh <arch> first" >&2; exit 1; }
  MINE="$("$PY" "$ROOT/tools/elf_abi.py" "$BIN" 2>/dev/null || echo unknown)"
  if [ -n "$STOCK_ARCH" ] && [ "$MINE" != "$STOCK_ARCH" ]; then
    ok=0
    # A 32-bit ARM build can run on these CPUs, but never the other way round.
    [ "$STOCK_ARCH" = arm ] && [ "$MINE" = armv5 ] && ok=1
    if [ "$ok" = 0 ] && [ "${TW_FORCE_ARCH:-0}" != 1 ]; then
      echo "error: $BIN is a $MINE binary but this phone's ramdisk is $STOCK_ARCH - it would never start." >&2
      echo "       Use out/$STOCK_ARCH/recovery (scripts/build.sh $STOCK_ARCH), or set TW_FORCE_ARCH=1 if you are sure." >&2
      exit 1
    fi
  fi
fi
echo "Using $BIN ($("$PY" "$ROOT/tools/elf_abi.py" "$BIN" 2>/dev/null || echo '?'))"
echo "Patching $CPIO"

if "$MB" cpio "$CPIO" "exists system/bin/recovery"; then
  "$MB" cpio "$CPIO" "mv system/bin/recovery system/bin/recovery.stock"
  TARGET=system/bin/recovery
elif "$MB" cpio "$CPIO" "exists sbin/recovery"; then
  "$MB" cpio "$CPIO" "mv sbin/recovery sbin/recovery.stock"   # very old (pre-Q) layout
  TARGET=sbin/recovery
else
  echo "warning: no existing recovery binary in ramdisk; adding system/bin/recovery" >&2
  "$MB" cpio "$CPIO" "mkdir 0755 system" "mkdir 0755 system/bin" || true
  TARGET=system/bin/recovery
fi
"$MB" cpio "$CPIO" "add 0755 $TARGET $BIN"
# `twrp` command line tool (adb shell twrp ...): the same binary, selected by its name.
"$MB" cpio "$CPIO" "ln /$TARGET $(dirname "$TARGET")/twrp" 2>/dev/null || echo "  (could not add the twrp symlink - use: recovery twrp ...)"
if [ -n "$FSTAB" ]; then
  "$MB" cpio "$CPIO" "mkdir 0755 system/etc" 2>/dev/null || true
  "$MB" cpio "$CPIO" "add 0644 system/etc/twrp.fstab $FSTAB"
fi
"$MB" repack in.img out.img
cp out.img "$OUT"
echo "==> $OUT"
echo "Test it first:  scripts/install.sh $(basename "$OUT")     (fastboot boot: nothing is written)"
