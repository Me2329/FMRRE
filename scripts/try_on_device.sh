#!/usr/bin/env bash
# Run FMRRE on a phone that is ALREADY in a recovery with root adb (stock
# TWRP, OrangeFox, LineageOS recovery with adb...). Nothing is flashed: the
# binary runs from /tmp and disappears on reboot.
#
#   scripts/try_on_device.sh [out/<arch>/recovery]
# Without an argument the phone is asked for its CPU (ro.product.cpu.abi) and the
# matching build is used (built first if it is missing).
set -euo pipefail
cd "$(dirname "$0")/.."
adb wait-for-recovery
BIN="${1:-}"
if [ -z "$BIN" ]; then
  abi="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
  [ -n "$abi" ] || abi="$(adb shell uname -m | tr -d '\r')"
  arch="$(python3 tools/elf_abi.py --abi "$abi")" || { echo "No FMRRE build for CPU '$abi' (scripts/build.sh list)" >&2; exit 1; }
  echo "Phone CPU: $abi -> out/$arch/recovery"
  BIN="out/$arch/recovery"
  [ -f "$BIN" ] || scripts/build.sh "$arch"
fi
adb push "$BIN" /tmp/fmrre
adb shell chmod 755 /tmp/fmrre
echo "Stopping the current recovery UI and starting FMRRE (Ctrl+C to quit)..."
adb shell 'stop recovery; sleep 1; /tmp/fmrre'
