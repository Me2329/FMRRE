#!/usr/bin/env bash
# Run TWRP v2 on a phone that is ALREADY in a recovery with root adb (stock
# TWRP, OrangeFox, LineageOS recovery with adb...). Nothing is flashed: the
# binary runs from /tmp and disappears on reboot.
set -euo pipefail
BIN="${1:-out/arm64/recovery}"
adb wait-for-recovery
adb push "$BIN" /tmp/twrpv2
adb shell chmod 755 /tmp/twrpv2
echo "Stopping the current recovery UI and starting TWRP v2 (Ctrl+C to quit)..."
adb shell 'stop recovery; sleep 1; /tmp/twrpv2'
