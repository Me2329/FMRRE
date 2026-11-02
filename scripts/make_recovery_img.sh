#!/usr/bin/env bash
# Build a flashable/bootable TWRP v2 image from your device's stock (or any
# working) recovery image by swapping the recovery binary in its ramdisk.
#
#   scripts/make_recovery_img.sh <recovery.img|boot.img|vendor_boot.img> [recovery-binary] [twrp.fstab]
#
# Needs magiskboot on the PC: extract lib/x86_64/libmagiskboot.so from the
# Magisk APK, rename it to magiskboot, chmod +x, and put it in PATH (or set MAGISKBOOT).
#
# Then test WITHOUT flashing first:   fastboot boot twrpv2-<image>.img
# and only if it works:               fastboot flash recovery twrpv2-recovery.img
set -euo pipefail
IMG="$(realpath "${1:?usage: $0 <image> [binary] [fstab]}")"
BIN="$(realpath "${2:-out/arm64/recovery}")"
FSTAB="${3:-}"
[ -n "$FSTAB" ] && FSTAB="$(realpath "$FSTAB")"
MB="${MAGISKBOOT:-magiskboot}"
command -v "$MB" >/dev/null || { echo "magiskboot not found (set MAGISKBOOT=/path/to/magiskboot)" >&2; exit 1; }
[ -f "$BIN" ] || { echo "recovery binary $BIN not found - run scripts/build.sh arm64 first" >&2; exit 1; }

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
echo "Test it first:  fastboot boot $OUT     (A/B devices without recovery partition: flash to boot/vendor_boot of the inactive slot)"
