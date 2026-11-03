#!/usr/bin/env bash
# Put TWRP v2 on a phone from a computer (Linux / macOS; Windows: scripts/install.ps1).
#
#   scripts/install.sh [options] [twrpv2-recovery.img]
#
# Fastboot (Pixel, Xiaomi, OnePlus, Motorola, Nothing, Sony, ...):
#   default    fastboot boot  - runs TWRP v2 once, writes NOTHING to the phone
#   --permanent               - flashes it (recovery, vendor_boot or boot - detected)
# Samsung Download mode:
#   always builds an Odin package (AP slot, Windows) and, if Heimdall is installed,
#   flashes RECOVERY (+ VBMETA) with it. Samsung has no "test boot" - it is always permanent.
#
# Options
#   --mode auto|fastboot|samsung   how to talk to the phone (default: auto-detect)
#   --permanent                    fastboot: flash instead of boot once
#   --partition NAME               fastboot: force recovery | vendor_boot | boot | init_boot
#   --both-slots                   fastboot A/B: flash _a and _b
#   --vbmeta FILE                  also flash this stock vbmeta.img with verification disabled
#   --disable-vbmeta               also flash a generated empty vbmeta (flags 3) - GSI/Samsung style
#   --no-vbmeta                    Samsung: don't put vbmeta in the package
#   --build-from STOCK.img         first build the image from your stock recovery/boot/vendor_boot image
#                                  (the CPU architecture is detected from it; TW_BINARY=out/<arch>/recovery overrides)
#   --odin-only                    Samsung: only build the .tar.md5, don't flash
#   -y, --yes                      don't ask for confirmation
#   --dry-run                      print the fastboot/heimdall commands instead of running them
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

MODE=auto PERMANENT=0 PARTITION="" BOTH=0 VBMETA="" DISABLE_VBMETA=0 NO_VBMETA=0 BUILD_FROM="" ODIN_ONLY=0 YES=0 DRY=0 IMG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE="$2"; shift ;;
    --permanent) PERMANENT=1 ;;
    --partition) PARTITION="$2"; shift ;;
    --both-slots) BOTH=1 ;;
    --vbmeta) VBMETA="$(realpath "$2")"; shift ;;
    --disable-vbmeta) DISABLE_VBMETA=1 ;;
    --no-vbmeta) NO_VBMETA=1 ;;
    --build-from) BUILD_FROM="$(realpath "$2")"; shift ;;
    --odin-only) ODIN_ONLY=1; MODE=samsung ;;
    -y|--yes) YES=1 ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) IMG="$(realpath "$1")" ;;
  esac
  shift
done

FASTBOOT="${FASTBOOT:-fastboot}" ADB="${ADB:-adb}" HEIMDALL="${HEIMDALL:-heimdall}" PY="${PYTHON:-python3}"
say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m  %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m  %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
run()  { echo "   \$ $*"; [ "$DRY" = 1 ] || "$@"; }
confirm() {
  [ "$YES" = 1 ] && return 0
  printf '%s [y/N] ' "$1"; read -r a; [ "$a" = y ] || [ "$a" = Y ] || die "Cancelled."
}

# ---- image ------------------------------------------------------------------
if [ -n "$BUILD_FROM" ]; then
  say "Building TWRP v2 from $(basename "$BUILD_FROM")"
  # "auto": the CPU architecture is taken from the stock ramdisk (and built if needed).
  scripts/make_recovery_img.sh "$BUILD_FROM" "${TW_BINARY:-auto}"
  IMG="$ROOT/twrpv2-$(basename "$BUILD_FROM")"
fi
if [ -z "$IMG" ]; then
  for c in twrpv2-recovery.img twrpv2-boot.img twrpv2-vendor_boot.img out/twrpv2-recovery.img; do
    [ -f "$c" ] && { IMG="$ROOT/$c"; break; }
  done
fi
[ -n "$IMG" ] && [ -f "$IMG" ] || die "No TWRP v2 image. Build one from your stock image:
     scripts/install.sh --build-from recovery.img      (or boot.img / vendor_boot.img of YOUR phone)
   or pass an existing one: scripts/install.sh twrpv2-recovery.img"
MAGIC="$(head -c 8 "$IMG" | tr -d '\0')"
case "$MAGIC" in
  ANDROID!) KIND=boot ;;
  VNDRBOOT) KIND=vendor_boot ;;
  *) die "$(basename "$IMG") is not an Android boot/recovery image" ;;
esac
say "Image: $(basename "$IMG") ($KIND format, $(du -h "$IMG" | cut -f1))"

# ---- detect the phone -----------------------------------------------------------
# Output is captured first and parsed afterwards: with pipefail, `tool | grep -q` can fail when grep
# exits early and the tool gets SIGPIPE writing its next line (fastboot always prints "Finished...").
fb_devices()  { have "$FASTBOOT" && [ -n "$("$FASTBOOT" devices 2>/dev/null)" ]; }
heim_detect() { have "$HEIMDALL" && "$HEIMDALL" detect >/dev/null 2>&1 ; }
adb_state()   { have "$ADB" && "$ADB" get-state 2>/dev/null || true; }

wait_for() {  # $1 = fastboot|samsung, waits up to 90 s
  for _ in $(seq 1 90); do
    if [ "$1" = fastboot ]; then fb_devices && return 0; else heim_detect && return 0; fi
    sleep 1
  done
  return 1
}

if [ "$MODE" = auto ]; then
  if fb_devices; then MODE=fastboot
  elif heim_detect; then MODE=samsung
  else
    st="$(adb_state)"
    if [ "$st" = device ] || [ "$st" = recovery ]; then
      man="$("$ADB" shell getprop ro.product.manufacturer 2>/dev/null | tr -d '\r' | tr 'A-Z' 'a-z')"
      if [ "$man" = samsung ]; then
        MODE=samsung
        if [ "$ODIN_ONLY" = 0 ] && have "$HEIMDALL"; then
          say "Samsung phone on adb - rebooting to Download mode"
          run "$ADB" reboot download
          [ "$DRY" = 1 ] || wait_for samsung || die "The phone didn't show up in Download mode."
        fi
      else
        MODE=fastboot
        say "Phone on adb ($man) - rebooting to the bootloader"
        run "$ADB" reboot bootloader
        [ "$DRY" = 1 ] || wait_for fastboot || die "The phone didn't show up in fastboot. Is the USB driver installed?"
      fi
    fi
  fi
fi
if [ "$MODE" = auto ]; then
  die "No phone found. Put it in one of these modes and plug it in:
     Fastboot (most phones): power off, hold Volume Down + Power
     Download (Samsung):     power off, hold Volume Up + Volume Down, plug the USB cable in, then press Volume Up
   or enable USB debugging in Android so this script can reboot it for you."
fi

# ---- Samsung ------------------------------------------------------------------
if [ "$MODE" = samsung ]; then
  [ "$KIND" = boot ] || die "Samsung needs a recovery.img (build it from the stock recovery.img in your firmware's AP file)."
  mkdir -p out/samsung
  TAR="out/samsung/twrpv2-recovery-odin.tar.md5"
  VBARG=(--vbmeta-disabled)
  [ -n "$VBMETA" ] && VBARG=(--vbmeta "$VBMETA")
  [ "$NO_VBMETA" = 1 ] && VBARG=(--no-vbmeta)
  "$PY" tools/odin_pack.py tar --recovery "$IMG" "${VBARG[@]}" -o "$TAR"
  cat <<EOF

   Odin (Windows): AP = $TAR, Options: UNTICK "Auto Reboot", press Start.
EOF
  if [ "$ODIN_ONLY" = 1 ] || ! have "$HEIMDALL"; then
    [ "$ODIN_ONLY" = 1 ] || warn "Heimdall is not installed - flash the package with Odin, or install heimdall and run this again."
    MODE=done
  else
    cat <<EOF
   Before flashing a Samsung:
     - Settings > Developer options > OEM unlocking was ON and the bootloader is UNLOCKED
     - KG/RMM state is not "Prenormal" (the bootloader would reject the image)
     - this ERASES nothing, but Samsung's encryption usually requires Format Data in TWRP v2 afterwards
EOF
    confirm "Flash TWRP v2 to RECOVERY with Heimdall?"
    [ "$DRY" = 1 ] || heim_detect || die "No Samsung in Download mode."
    ARGS=(flash --RECOVERY "$IMG")
    if [ "$NO_VBMETA" = 0 ]; then
      VBF="out/samsung/vbmeta.img"
      if [ -n "$VBMETA" ]; then "$PY" tools/odin_pack.py vbmeta-patch "$VBMETA" -o "$VBF" >/dev/null
      else "$PY" tools/odin_pack.py vbmeta-disabled -o "$VBF" >/dev/null; fi
      ARGS+=(--VBMETA "$VBF")
    fi
    run "$HEIMDALL" "${ARGS[@]}" --no-reboot
    cat <<EOF

==> Flashed. Now boot STRAIGHT into recovery, or stock Android replaces TWRP v2 on its first boot:
     1. hold Volume Down + Power until the screen goes black
     2. immediately switch to Volume Up + Power (+ Bixby on older models) and hold until TWRP v2 appears
EOF
  fi
  exit 0
fi

# ---- Fastboot ---------------------------------------------------------------------
[ "$MODE" = fastboot ] || die "unknown --mode $MODE"
have "$FASTBOOT" || die "fastboot not found. Install Android platform-tools: https://developer.android.com/tools/releases/platform-tools"
[ "$DRY" = 1 ] || fb_devices || die "No phone in fastboot mode."
gv() {  # value of a fastboot variable, "" if the bootloader doesn't have it
  local out line
  out="$("$FASTBOOT" getvar "$1" 2>&1)" || true
  while IFS= read -r line; do
    line="${line%$'\r'}"
    if [[ "$line" == "$1:"* ]]; then
      line="${line#"$1:"}"
      echo "${line#"${line%%[! ]*}"}"
      return 0
    fi
  done <<< "$out"
}
PRODUCT="$(gv product)"; UNLOCKED="$(gv unlocked)"; SLOT="$(gv current-slot)"; USERSPACE="$(gv is-userspace)"
say "Phone: ${PRODUCT:-unknown}  unlocked: ${UNLOCKED:-?}  slot: ${SLOT:-none}"
if [ "$UNLOCKED" = no ]; then
  die "The bootloader is LOCKED. Unlocking erases the phone:  fastboot flashing unlock   (some brands: fastboot oem unlock)"
fi
if [ "$USERSPACE" = yes ]; then
  say "In fastbootd (userspace) - switching to the bootloader"
  run "$FASTBOOT" reboot bootloader
  [ "$DRY" = 1 ] || wait_for fastboot || die "Bootloader didn't come back."
fi
haspart() { [[ "$(gv "partition-size:$1")" == 0x* ]]; }
if [ -z "$PARTITION" ]; then
  if [ "$KIND" = vendor_boot ]; then PARTITION=vendor_boot
  elif haspart recovery || haspart "recovery_${SLOT:-a}"; then PARTITION=recovery
  else PARTITION=boot; fi
fi
say "Target partition: $PARTITION"

if [ "$PERMANENT" = 0 ]; then
  [ "$KIND" = vendor_boot ] && die "vendor_boot images can't be test-booted. Use --permanent (or build from recovery.img/boot.img)."
  say "Test boot: TWRP v2 runs once from RAM, nothing on the phone is changed."
  run "$FASTBOOT" boot "$IMG"
  echo
  echo "   If it works:   scripts/install.sh --permanent $(basename "$IMG")"
  exit 0
fi

if [ "$PARTITION" = boot ] || [ "$PARTITION" = vendor_boot ] || [ "$PARTITION" = init_boot ]; then
  warn "This phone has no recovery partition: TWRP v2 goes into '$PARTITION'."
  warn "That is only safe if the image was built from THIS phone's own $PARTITION.img (scripts/install.sh --build-from $PARTITION.img)."
  warn "Keep the stock $PARTITION.img to restore with:  fastboot flash $PARTITION $PARTITION.img"
fi
WHERE="$PARTITION"
if [ -n "$SLOT" ]; then [ "$BOTH" = 1 ] && WHERE="$PARTITION (both slots)" || WHERE="$PARTITION (slot $SLOT)"; fi
confirm "Flash $(basename "$IMG") to $WHERE?"
if [ "$BOTH" = 1 ] && [ -n "$SLOT" ]; then
  run "$FASTBOOT" flash --slot=all "$PARTITION" "$IMG"
else
  run "$FASTBOOT" flash "$PARTITION" "$IMG"
fi
if [ -n "$VBMETA" ]; then
  run "$FASTBOOT" --disable-verity --disable-verification flash vbmeta "$VBMETA"
elif [ "$DISABLE_VBMETA" = 1 ]; then
  "$PY" tools/odin_pack.py vbmeta-disabled -o out/vbmeta_disabled.img >/dev/null
  run "$FASTBOOT" --disable-verity --disable-verification flash vbmeta out/vbmeta_disabled.img
fi
say "Rebooting into TWRP v2"
run "$FASTBOOT" reboot recovery || { warn "This bootloader can't 'reboot recovery': hold Volume Up + Power while it restarts."; run "$FASTBOOT" reboot; }
