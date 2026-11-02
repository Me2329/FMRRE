#!/usr/bin/env bash
# Build TWRP v2.
#   scripts/build.sh arm64    static aarch64 recovery binary (Zig, no NDK needed)   -> out/arm64/recovery
#   scripts/build.sh arm      static armv7 recovery binary                          -> out/arm/recovery
#   scripts/build.sh ndk      static arm64 with the Android NDK ($ANDROID_NDK)      -> out/ndk-arm64/recovery
#   scripts/build.sh sim      desktop simulator (X11 window)                        -> out/sim/twrpv2
set -euo pipefail
cd "$(dirname "$0")/.."
target="${1:-arm64}"
jobs="$(nproc 2>/dev/null || echo 4)"

zig_build() {
  local zt="$1" out="$2"
  if [ -z "${ZIG:-}" ] && ! command -v zig >/dev/null && ! python3 -c "import ziglang" 2>/dev/null; then
    echo "Zig not found. Install it with:  pip install ziglang   (or set ZIG=/path/to/zig)" >&2
    exit 1
  fi
  if [ -z "${ZIG:-}" ] && command -v zig >/dev/null; then export ZIG=zig; fi
  export ZIG_TARGET="$zt"
  cmake -S . -B "build-$out" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=tools/toolchain/zig-android.cmake -DZIG_TARGET="$zt"
  cmake --build "build-$out" -j"$jobs"
  mkdir -p "out/$out"
  cp "build-$out/twrpv2" "out/$out/recovery"
  echo "==> out/$out/recovery"
  file "out/$out/recovery" 2>/dev/null || true
}

case "$target" in
  arm64) zig_build aarch64-linux-musl arm64 ;;
  arm)   zig_build arm-linux-musleabihf arm ;;
  x86_64-android) zig_build x86_64-linux-musl x86_64 ;;
  ndk)
    : "${ANDROID_NDK:?set ANDROID_NDK to your NDK path}"
    cmake -S . -B build-ndk-arm64 -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
          -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DTW_STATIC=ON -DTW_X11=OFF
    cmake --build build-ndk-arm64 -j"$jobs"
    mkdir -p out/ndk-arm64 && cp build-ndk-arm64/twrpv2 out/ndk-arm64/recovery
    echo "==> out/ndk-arm64/recovery" ;;
  sim)
    cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
    cmake --build build-host -j"$jobs"
    mkdir -p out/sim && cp build-host/twrpv2 out/sim/twrpv2
    echo "==> out/sim/twrpv2   (run: out/sim/twrpv2 --sim)" ;;
  *) echo "unknown target: $target" >&2; exit 1 ;;
esac
