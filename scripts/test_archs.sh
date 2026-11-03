#!/usr/bin/env bash
# Run every cross-built TWRP v2 on an emulated CPU (qemu-user) - each one on the
# oldest CPU its build supports:
#   1. the self-tests              out/<arch>/twrpv2_selftest  (TW_SELFTEST=1 scripts/build.sh <arch>)
#   2. the recovery UI             simulator mode, headless: boots, renders, screenshot compared with the host build
#   3. the twrp command line       super metadata, file check, image flash and a full GSI flash (.img.xz)
#
#   scripts/test_archs.sh [arch...]     default: every architecture that has out/<arch>/recovery
# Needs qemu-user-static (apt install qemu-user-static) and python3 (+ Pillow for the screenshot comparison).
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

declare -A QEMU=([arm64]=qemu-aarch64 [arm]=qemu-arm [armv5]=qemu-arm [x86]=qemu-i386 [x86_64]=qemu-x86_64 [riscv64]=qemu-riscv64 [mips]=qemu-mipsel)
declare -A QCPU=([arm64]=cortex-a53 [arm]=cortex-a8 [armv5]=arm926 [x86]=core2duo [x86_64]=qemu64 [riscv64]=rv64 [mips]=24Kf)

qemu_for() {
  local q="${QEMU[$1]}"
  command -v "$q-static" >/dev/null && { echo "$q-static"; return; }
  command -v "$q" >/dev/null && { echo "$q"; return; }
  return 1
}

if [ $# -eq 0 ]; then
  for a in arm64 arm armv5 x86 x86_64 riscv64 mips; do [ -f "out/$a/recovery" ] && set -- "$@" "$a"; done
fi
[ $# -gt 0 ] || { echo "No builds in out/ - run: TW_SELFTEST=1 scripts/build.sh all" >&2; exit 1; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
# Reference screenshot from the host build (if there is one).
REF=""
HOSTBIN=""
for h in build-host/twrpv2 out/sim/twrpv2; do [ -x "$h" ] && { HOSTBIN="$h"; break; }; done
if [ -n "$HOSTBIN" ]; then
  "$HOSTBIN" --sim --headless --sim-root "$T/sim-host" --script tools/scripts/smoke.txt --out "$T/shots-host" > /dev/null 2>&1 && REF="$T/shots-host/01_home.png"
fi

compare() {  # $1 png, $2 reference -> prints "same NN.N%"
  python3 - "$1" "$2" <<'EOF' 2>/dev/null || echo "png ok"
import sys
from PIL import Image, ImageChops
a, b = Image.open(sys.argv[1]).convert("RGB"), Image.open(sys.argv[2]).convert("RGB")
if a.size != b.size:
    print("size differs"); sys.exit(0)
d = ImageChops.difference(a, b).convert("L")
w, h = a.size
# the status bar (clock, battery, temperature) changes from run to run: compare below it
d = d.crop((0, 60, w, h))
hist = d.histogram()
total = sum(hist)
close = sum(hist[:9])            # channel difference <= 8/255
print(f"{100.0 * close / total:.1f}% same")
EOF
}

FAIL=0
printf '%-8s %-12s %-10s %-22s %-16s %s\n' arch cpu selftest UI "CLI + GSI flash" size
for a in "$@"; do
  bin="out/$a/recovery"
  [ -f "$bin" ] || { printf '%-8s no build (scripts/build.sh %s)\n' "$a" "$a"; FAIL=1; continue; }
  q="$(qemu_for "$a")" || { printf '%-8s %s not installed\n' "$a" "${QEMU[$a]}"; FAIL=1; continue; }
  run() { "$q" -cpu "${QCPU[$a]}" "$@"; }
  sim="$T/sim-$a"

  st="-"
  if [ -f "out/$a/twrpv2_selftest" ]; then
    if (cd "$T" && run "$ROOT/out/$a/twrpv2_selftest") > "$T/selftest-$a.log" 2>&1 && grep -q "All self-tests passed" "$T/selftest-$a.log"; then st=pass
    else st=FAIL; FAIL=1; fi
  fi

  ui=FAIL
  if run "$bin" --sim --headless --sim-root "$sim" --script tools/scripts/smoke.txt --out "$T/shots-$a" > "$T/ui-$a.log" 2>&1 && [ -f "$T/shots-$a/01_home.png" ]; then
    ui="ok"
    [ -n "$REF" ] && ui="ok, $(compare "$T/shots-$a/01_home.png" "$REF")"
    cp "$T/shots-$a/01_home.png" "$T/home-$a.png"
  else FAIL=1; fi

  # Outputs go to files and are checked afterwards (a `cmd | grep -q` can fail on SIGPIPE under pipefail).
  cli=FAIL
  gsi=/sdcard/Download/system-td-arm64-ab-vanilla.img.xz want="flags 0 -> 3"
  [ -f "$sim/data/media/0/Download/system-td-arm64-ab-vanilla.img.xz" ] || { gsi=/sdcard/Download/lineage-22.1-arm64_bvN-vndklite.img.gz; want="Verified"; }
  {
    run "$bin" --sim --sim-root "$sim" twrp lp list > "$T/lp-$a.log" 2>&1 && grep -q "system_a" "$T/lp-$a.log" &&
    run "$bin" --sim --sim-root "$sim" twrp check "/sdcard/Download/GSI_A15_download(1)" > "$T/check-$a.log" 2>&1 &&
    grep -q "doesn't end in" "$T/check-$a.log" &&
    run "$bin" --sim --sim-root "$sim" twrp flash /sdcard/Download/kernel-boot.img boot > "$T/flash-$a.log" 2>&1 &&
    grep -q "Verified" "$T/flash-$a.log" &&
    run "$bin" --sim --sim-root "$sim" twrp gsi "$gsi" > "$T/gsi-$a.log" 2>&1 &&
    grep -q "Verified" "$T/gsi-$a.log" && grep -q "$want" "$T/gsi-$a.log"
  } > "$T/cli-$a.log" 2>&1 && cli=ok || FAIL=1

  printf '%-8s %-12s %-10s %-22s %-16s %s\n' "$a" "${QCPU[$a]}" "$st" "$ui" "$cli" "$(du -h "$bin" | cut -f1)"
  if [ "$st" = FAIL ]; then grep -E "FAIL" "$T/selftest-$a.log" | head -5 | sed 's/^/           /'; fi
  if [ "$cli" = FAIL ]; then tail -5 "$T/gsi-$a.log" 2>/dev/null | sed 's/^/           /'; fi
done
mkdir -p out/arch-shots && cp "$T"/home-*.png out/arch-shots/ 2>/dev/null
[ "$FAIL" = 0 ] && echo "All architectures passed (screenshots in out/arch-shots/)" || { echo "Some architectures FAILED"; exit 1; }
