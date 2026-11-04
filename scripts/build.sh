#!/usr/bin/env bash
# Build FMRRE.
#   scripts/build.sh <arch> [<arch>...]   static recovery binary (Zig, no NDK needed) -> out/<arch>/recovery
#   scripts/build.sh all                  every architecture in the table below
#   scripts/build.sh list                 print the table
#   scripts/build.sh ndk [abi]            with the Android NDK ($ANDROID_NDK)            -> out/ndk-<abi>/recovery
#                                          abi: arm64-v8a (default), armeabi-v7a, x86, x86_64, riscv64
#   scripts/build.sh sim                  desktop simulator (X11 window)                  -> out/sim/fmrre
#   TW_SELFTEST=1 scripts/build.sh <arch> also builds out/<arch>/fmrre_selftest (run with qemu-user)
#
#   arch     Android ABI   zig target                CPU it needs (oldest supported)
#   arm64    arm64-v8a     aarch64-linux-musl        ARMv8-A               - almost every phone since ~2015
#   arm      armeabi-v7a   arm-linux-musleabihf      ARMv7-A + VFPv3 + NEON - 32-bit phones 2011-2016 (NDK default)
#   armv5    armeabi       arm-linux-musleabi        ARMv5TE, soft-float   - very old phones (Android 2.x-4.x)
#   x86      x86           x86-linux-musl            i686 + SSSE3 (Core 2) - Intel Atom phones/tablets, emulators
#   x86_64   x86_64        x86_64-linux-musl         x86-64 (v1)           - emulators, Chromebooks, Android-x86
#   riscv64  riscv64       riscv64-linux-musl        RV64GC                - RISC-V Android boards
#   mips     mips          mipsel-linux-musleabihf   MIPS32r2 + FPU (LE)   - Android 4.x MIPS tablets
# Aliases: aarch64, arm64-v8a | armv7, armhf, armeabi-v7a | armeabi, armel | i686, i386 | amd64, x64 | riscv | mipsel
set -euo pipefail
cd "$(dirname "$0")/.."
jobs="$(nproc 2>/dev/null || echo 4)"

ARCHS=(arm64 arm armv5 x86 x86_64 riscv64 mips)
declare -A TARGET=([arm64]=aarch64-linux-musl [arm]=arm-linux-musleabihf [armv5]=arm-linux-musleabi [x86]=x86-linux-musl
                   [x86_64]=x86_64-linux-musl [riscv64]=riscv64-linux-musl [mips]=mipsel-linux-musleabihf)
declare -A CPU=([arm64]=baseline [arm]=cortex_a8 [armv5]=arm926ej_s [x86]=core2 [x86_64]=x86_64 [riscv64]=baseline [mips]=mips32r2)
declare -A ABI=([arm64]=arm64-v8a [arm]=armeabi-v7a [armv5]=armeabi [x86]=x86 [x86_64]=x86_64 [riscv64]=riscv64 [mips]=mips)

canon() {
  case "$1" in
    arm64|aarch64|arm64-v8a) echo arm64 ;;
    arm|armv7|armv7a|armhf|armeabi-v7a) echo arm ;;
    armv5|armv5te|armv6|armeabi|armel) echo armv5 ;;
    x86|i686|i386|ia32) echo x86 ;;
    x86_64|x86-64|amd64|x64|x86_64-android) echo x86_64 ;;
    riscv64|riscv|rv64) echo riscv64 ;;
    mips|mipsel|mips32) echo mips ;;
    *) return 1 ;;
  esac
}

zig_setup() {
  if [ -z "${ZIG:-}" ] && ! command -v zig >/dev/null && ! python3 -c "import ziglang" 2>/dev/null; then
    echo "Zig not found. Install it with:  pip install ziglang   (or set ZIG=/path/to/zig)" >&2
    exit 1
  fi
  if [ -z "${ZIG:-}" ] && command -v zig >/dev/null; then export ZIG=zig; fi
}

zig_build() {  # $1 = canonical arch. Every step is checked: set -e doesn't apply in `( ... ) || ...`.
  local a="$1" dir="build-$1" want="${TARGET[$1]} ${CPU[$1]}"
  zig_setup || return 1
  # Objects built for another CPU must not be reused.
  if [ -d "$dir" ] && { [ ! -f "$dir/.zig-target" ] || [ "$(cat "$dir/.zig-target")" != "$want" ]; }; then rm -rf "$dir"; fi
  export ZIG_TARGET="${TARGET[$a]}" ZIG_CPU="${CPU[$a]}"
  echo "==> $a (${ABI[$a]}): $ZIG_TARGET, cpu $ZIG_CPU"
  cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=tools/toolchain/zig-android.cmake \
        -DZIG_TARGET="$ZIG_TARGET" -DZIG_CPU="$ZIG_CPU" -DTW_SELFTEST="$( [ "${TW_SELFTEST:-0}" = 1 ] && echo ON || echo OFF)" \
        > /dev/null || { echo "==> $a: configure FAILED" >&2; rm -rf "$dir"; return 1; }
  echo "$want" > "$dir/.zig-target"
  cmake --build "$dir" -j"$jobs" || { echo "==> $a: build FAILED" >&2; return 1; }
  mkdir -p "out/$a" && cp "$dir/fmrre" "out/$a/recovery" || return 1
  if [ "${TW_SELFTEST:-0}" = 1 ]; then cp "$dir/fmrre_selftest" "out/$a/fmrre_selftest" || return 1; fi
  echo "${ABI[$a]}" > "out/$a/ABI"
  echo "==> out/$a/recovery"
  file "out/$a/recovery" 2>/dev/null || true
}

[ $# -gt 0 ] || set -- arm64
case "$1" in
  list)
    printf '%-8s %-12s %-26s %s\n' arch "Android ABI" "zig target" cpu
    for a in "${ARCHS[@]}"; do printf '%-8s %-12s %-26s %s\n' "$a" "${ABI[$a]}" "${TARGET[$a]}" "${CPU[$a]}"; done
    exit 0 ;;
  all) set -- "${ARCHS[@]}" ;;
  ndk)
    : "${ANDROID_NDK:?set ANDROID_NDK to your NDK path}"
    abi="${2:-arm64-v8a}"
    api=29; [ "$abi" = riscv64 ] && api=35
    cmake -S . -B "build-ndk-$abi" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
          -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$api" -DTW_STATIC=ON -DTW_X11=OFF
    cmake --build "build-ndk-$abi" -j"$jobs"
    mkdir -p "out/ndk-$abi" && cp "build-ndk-$abi/fmrre" "out/ndk-$abi/recovery"
    echo "==> out/ndk-$abi/recovery"
    exit 0 ;;
  sim)
    cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
    cmake --build build-host -j"$jobs"
    mkdir -p out/sim && cp build-host/fmrre out/sim/fmrre
    echo "==> out/sim/fmrre   (run: out/sim/fmrre --sim)"
    exit 0 ;;
esac

todo=()
for arg in "$@"; do
  a="$(canon "$arg")" || { echo "unknown architecture: $arg   (scripts/build.sh list)" >&2; exit 1; }
  todo+=("$a")
done
if [ ${#todo[@]} -eq 1 ]; then zig_build "${todo[0]}"; exit 0; fi
# Several: keep going when one fails, report at the end.
failed=()
for a in "${todo[@]}"; do ( zig_build "$a" ) || failed+=("$a"); done
echo
for a in "${todo[@]}"; do
  if printf '%s\n' "${failed[@]:-}" | grep -qx "$a"; then echo "FAILED  $a"; else echo "ok      $a  -> out/$a/recovery"; fi
done
[ ${#failed[@]} -eq 0 ] || exit 1
