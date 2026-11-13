#!/usr/bin/env bash
# Tests for scripts/install.sh and tools/odin_pack.py against fake fastboot / adb / heimdall
# that answer like real phones and log every call. Nothing touches a real device.
set -uo pipefail
cd "$(dirname "$0")/.."
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin"
FAILS=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }
expect_call() { grep -qF -- "$2" "$T/log" && pass "$1" || { fail "$1 (missing: $2)"; sed 's/^/       /' "$T/log"; }; }
expect_no_call() { grep -qE -- "$2" "$T/log" && fail "$1 (unexpected: $2)" || pass "$1"; }

python3 - "$T" <<'EOF'
import sys
T = sys.argv[1]
b = bytearray(1 << 20); b[0:8] = b"ANDROID!"; open(T + "/fmrre-recovery.img", "wb").write(b)
b[-64:-60] = b"AVBf"; open(T + "/fmrre-recovery-avb.img", "wb").write(b)  # a recovery verified by AVB (Android 9+)
v = bytearray(b"VNDRBOOT") + bytearray(4096); open(T + "/fmrre-vendor_boot.img", "wb").write(v)
m = bytearray(4096); m[0:4] = b"AVB0"; open(T + "/vbmeta.img", "wb").write(m)
open(T + "/junk.img", "wb").write(b"\0" * 4096)
EOF

# Fake fastboot. FB_* variables describe the phone; $T/fastboot_on gates whether it is connected.
cat > "$T/bin/fastboot" <<'EOF'
#!/bin/bash
echo "fastboot $*" >> "$T/log"
[ -f "$T/fastboot_on" ] || exit 0
case "$1" in
  devices) echo "FAKE123	fastboot" ;;
  getvar)
    case "$2" in
      product) echo "product: ${FB_PRODUCT:-husky}" >&2 ;;
      unlocked) echo "unlocked: ${FB_UNLOCKED:-yes}" >&2 ;;
      current-slot) [ -n "${FB_SLOT-a}" ] && echo "current-slot: ${FB_SLOT-a}" >&2 ;;
      is-userspace) echo "is-userspace: no" >&2 ;;
      partition-size:recovery*) [ "${FB_RECOVERY:-0}" = 1 ] && echo "$2: 0x4000000" >&2 || echo "getvar:$2 FAILED (remote: 'Variable not found')" >&2 ;;
    esac
    # Like a real phone over USB: the summary line comes a moment later. A `fastboot | grep -q`
    # under pipefail breaks on this (grep exits, fastboot gets SIGPIPE) - the installer must not.
    sleep 0.05
    echo "Finished. Total time: 0.001s" >&2 ;;
esac
exit 0
EOF
cat > "$T/bin/heimdall" <<'EOF'
#!/bin/bash
echo "heimdall $*" >> "$T/log"
[ "$1" = detect ] && { [ -f "$T/download_on" ] && { echo "Device detected"; exit 0; }; exit 1; }
exit 0
EOF
cat > "$T/bin/adb" <<'EOF'
#!/bin/bash
echo "adb $*" >> "$T/log"
[ -f "$T/adb_on" ] || { echo "error: no devices/emulators found" >&2; exit 1; }
case "$1" in
  get-state) echo device ;;
  shell) printf '%s\r\n' "${ADB_MAN:-Google}" ;;
  reboot) rm -f "$T/adb_on"; [ "$2" = download ] && touch "$T/download_on"; [ "$2" = bootloader ] && touch "$T/fastboot_on" ;;
esac
exit 0
EOF
chmod +x "$T/bin/"*
export T PATH="$T/bin:$PATH"
IMG="$T/fmrre-recovery.img"
scenario() { : > "$T/log"; rm -f "$T"/*_on; echo "--- $1"; }
inst() { scripts/install.sh "$@" > "$T/out" 2>&1; echo $? > "$T/rc"; }
rc() { cat "$T/rc"; }

scenario "fastboot, default = test boot only"
touch "$T/fastboot_on"; inst "$IMG"
[ "$(rc)" = 0 ] && pass "exit 0" || fail "exit $(rc)"
expect_call "boots the image" "fastboot boot $IMG"
expect_no_call "writes nothing" "fastboot (.* )?flash"

scenario "fastboot --permanent, phone with a recovery partition"
touch "$T/fastboot_on"; FB_RECOVERY=1 inst --permanent -y "$IMG"
expect_call "flashes recovery" "fastboot flash recovery $IMG"
expect_call "reboots to recovery" "fastboot reboot recovery"

scenario "fastboot --permanent, A/B without recovery partition, both slots + stock vbmeta"
touch "$T/fastboot_on"; inst --permanent --both-slots --vbmeta "$T/vbmeta.img" -y "$IMG"
expect_call "flashes boot on both slots" "fastboot flash --slot=all boot $IMG"
expect_call "vbmeta with verification disabled" "fastboot --disable-verity --disable-verification flash vbmeta $T/vbmeta.img"
grep -q "no recovery partition" "$T/out" && pass "warns about boot" || fail "no warning about flashing boot"

scenario "vendor_boot image: no test boot, permanent goes to vendor_boot"
touch "$T/fastboot_on"; inst "$T/fmrre-vendor_boot.img"
[ "$(rc)" != 0 ] && pass "test boot refused" || fail "vendor_boot test boot was attempted"
touch "$T/fastboot_on"; inst --permanent -y "$T/fmrre-vendor_boot.img"
expect_call "flashes vendor_boot" "fastboot flash vendor_boot $T/fmrre-vendor_boot.img"

scenario "locked bootloader"
touch "$T/fastboot_on"; FB_UNLOCKED=no inst --permanent -y "$IMG"
[ "$(rc)" != 0 ] && pass "refuses" || fail "continued on a locked bootloader"
expect_no_call "writes nothing" "fastboot (.* )?flash"

scenario "not a boot image"
inst "$T/junk.img"
[ "$(rc)" != 0 ] && pass "rejected" || fail "accepted junk"

scenario "Android phone on adb (Xiaomi) -> bootloader -> test boot"
touch "$T/adb_on"; FB_SLOT= ADB_MAN=Xiaomi inst "$IMG"
expect_call "reboots to bootloader" "adb reboot bootloader"
expect_call "then test boots" "fastboot boot $IMG"

scenario "Samsung (AVB, Android 9+) on adb -> Download mode -> Heimdall RECOVERY + VBMETA"
AVBIMG="$T/fmrre-recovery-avb.img"
touch "$T/adb_on"; ADB_MAN=samsung inst -y "$AVBIMG"
expect_call "reboots to download" "adb reboot download"
expect_call "heimdall flash" "heimdall flash --RECOVERY $AVBIMG --VBMETA out/samsung/vbmeta.img --no-reboot"
python3 tools/odin_pack.py check out/samsung/fmrre-recovery-odin.tar.md5 > "$T/chk" && pass "Odin package md5 valid" || fail "Odin package invalid"
grep -q "recovery.img" "$T/chk" && grep -q "vbmeta.img" "$T/chk" && pass "package has recovery.img + vbmeta.img" || fail "package content"

scenario "Samsung without AVB (Galaxy S8 era) -> recovery only, no VBMETA"
touch "$T/adb_on"; ADB_MAN=samsung inst -y "$IMG"
expect_call "heimdall flashes the recovery alone" "heimdall flash --RECOVERY $IMG --no-reboot"
expect_no_call "no VBMETA (the phone has no such partition)" "--VBMETA"
python3 tools/odin_pack.py check out/samsung/fmrre-recovery-odin.tar.md5 > "$T/chk" && ! grep -q vbmeta "$T/chk" && pass "Odin package: recovery.img only" || fail "S8 package content"
grep -q "no vbmeta partition" "$T/out" && pass "says why" || fail "no explanation"

scenario "Samsung without AVB but --disable-vbmeta: forced in"
HEIMDALL=/nonexistent inst --odin-only --disable-vbmeta "$IMG"
python3 tools/odin_pack.py check out/samsung/fmrre-recovery-odin.tar.md5 > "$T/chk" && grep -q vbmeta "$T/chk" && pass "vbmeta added on request" || fail "--disable-vbmeta ignored"

scenario "Samsung --odin-only without Heimdall"
HEIMDALL=/nonexistent inst --odin-only --no-vbmeta "$IMG"
[ "$(rc)" = 0 ] && pass "exit 0" || fail "exit $(rc)"
python3 tools/odin_pack.py check out/samsung/fmrre-recovery-odin.tar.md5 > "$T/chk" && ! grep -q vbmeta "$T/chk" && pass "recovery-only package" || fail "--no-vbmeta package"

scenario "nothing connected"
inst "$IMG"
[ "$(rc)" != 0 ] && grep -q "No phone found" "$T/out" && pass "explains how to connect" || fail "no-phone message"

echo "--- make_recovery_img.sh: picks the binary for the phone's CPU"
# Fake magiskboot: the "ramdisk" is a folder (rd/) next to ramdisk.cpio; repack writes a manifest.
cat > "$T/bin/magiskboot" <<'MAGISKBOOT'
#!/bin/bash
cmd="$1"; shift
case "$cmd" in
  unpack) cp -a "$FAKE_RAMDISK" rd; : > ramdisk.cpio ;;
  repack)
    { printf 'ANDROID!\n'
      for f in system/bin/recovery system/bin/fmrre system/bin/twrp; do
        if [ -L "rd/$f" ]; then echo "$f -> $(readlink "rd/$f")"
        elif [ -f "rd/$f" ]; then echo "$f $(sha256sum < "rd/$f" | cut -c1-16)"; fi
      done; } > "$2" ;;
  cpio)
    shift
    for c in "$@"; do
      set -- $c
      case "$1" in
        exists) [ -e "rd/$2" ] || [ -L "rd/$2" ] || exit 1 ;;
        mv) mv "rd/$2" "rd/$3" ;;
        mkdir) mkdir -p "rd/$3" ;;
        add) mkdir -p "$(dirname "rd/$3")"; cp "$4" "rd/$3" ;;
        ln) ln -s "$2" "rd/$3" ;;
        extract) { [ -e "rd/$2" ] || [ -L "rd/$2" ]; } || exit 1; cp -P "rd/$2" "$3" ;;
      esac
    done ;;
esac
exit 0
MAGISKBOOT
chmod +x "$T/bin/magiskboot"
# Minimal stock binaries for each CPU (ARM ones carry real .ARM.attributes: v7 vs v5TE).
python3 - "$T" <<'PYELF'
import os, struct, sys
T = sys.argv[1]
def elf64(machine):
    return b"\x7fELF" + bytes([2, 1, 1]) + bytes(9) + struct.pack("<HHI", 2, machine, 1) + bytes(64 - 24)
def elf32(machine, arm_arch=None):
    hdr = bytearray(b"\x7fELF" + bytes([1, 1, 1]) + bytes(9) + struct.pack("<HHI", 2, machine, 1) + bytes(52 - 24))
    if arm_arch is None:
        return bytes(hdr)
    attrs = bytes([5]) + b"cpu\0" + bytes([6, arm_arch])                 # Tag_CPU_name, Tag_CPU_arch
    filesub = bytes([1]) + struct.pack("<I", 1 + 4 + len(attrs)) + attrs  # Tag_File
    sub = b"aeabi\0" + filesub
    sec = b"A" + struct.pack("<I", 4 + len(sub)) + sub
    shoff = 52 + len(sec)
    shoff += (-shoff) % 4
    sh_attr = struct.pack("<10I", 0, 0x70000003, 0, 0, 52, len(sec), 0, 0, 1, 0)
    struct.pack_into("<I", hdr, 0x20, shoff)            # e_shoff
    struct.pack_into("<HHH", hdr, 0x2E, 40, 2, 0)       # e_shentsize, e_shnum, e_shstrndx
    out = bytes(hdr) + sec
    return out + bytes(shoff - len(out)) + bytes(40) + sh_attr
stock = {"arm64": elf64(183), "arm": elf32(40, 10), "armv5": elf32(40, 4), "x86": elf32(3), "x86_64": elf64(62), "riscv64": elf64(243)}
for arch, data in stock.items():
    os.makedirs(f"{T}/rd-{arch}/system/bin")
    open(f"{T}/rd-{arch}/system/bin/recovery", "wb").write(data)                 # the phone's stock recovery
    os.makedirs(f"{T}/builds/{arch}", exist_ok=True)
    open(f"{T}/builds/{arch}/recovery", "wb").write(data + b"TWRPv2-" + arch.encode())  # "our" build for that CPU
os.makedirs(f"{T}/rd-none/lib/modules")                 # vendor_boot style ramdisk: no binaries
open(f"{T}/stock.img", "wb").write(b"ANDROID!" + bytes(1024))
PYELF
MK="$PWD/scripts/make_recovery_img.sh"
mk() { (cd "$T" && FAKE_RAMDISK="$T/rd-$1" TW_OUT_DIR="$T/builds" "$MK" stock.img ${2:+"$2"}) > "$T/mk.log" 2>&1; }
for arch in arm64 arm armv5 x86 x86_64 riscv64; do
  rm -f "$T/fmrre-stock.img"
  mk "$arch"
  want="$(sha256sum < "$T/builds/$arch/recovery" | cut -c1-16)"
  if grep -q "system/bin/recovery $want" "$T/fmrre-stock.img" 2>/dev/null && grep -q "Architecture: $arch" "$T/mk.log"; then
    pass "stock $arch ramdisk -> out/$arch/recovery"
  else fail "stock $arch ramdisk"; sed 's/^/       /' "$T/mk.log"; fi
done
grep -q "system/bin/fmrre -> /system/bin/recovery" "$T/fmrre-stock.img" && pass "fmrre CLI symlink added" || fail "fmrre symlink"
grep -q "system/bin/twrp -> /system/bin/recovery" "$T/fmrre-stock.img" && pass "twrp alias symlink added" || fail "twrp symlink"
mk arm "$T/builds/arm64/recovery" && fail "arm64 binary accepted for an armeabi-v7a phone" || pass "wrong-CPU binary refused"
grep -q "would never start" "$T/mk.log" && pass "explains the CPU mismatch" || fail "mismatch message"
mk arm "$T/builds/armv5/recovery" && pass "armeabi binary allowed on an ARMv7 phone" || { fail "armeabi on ARMv7"; sed 's/^/       /' "$T/mk.log"; }
(cd "$T" && FAKE_RAMDISK="$T/rd-none" TW_OUT_DIR="$T/builds" PATH="$T/bin:/usr/bin:/bin" "$MK" stock.img) > "$T/mk.log" 2>&1
grep -q "assuming arm64" "$T/mk.log" && pass "no binaries in the ramdisk -> warns and assumes arm64" || { fail "fallback"; sed 's/^/       /' "$T/mk.log"; }

echo "--- odin_pack"
python3 tools/odin_pack.py vbmeta-disabled -o "$T/vd.img" >/dev/null
python3 - "$T/vd.img" <<'EOF' && pass "disabled vbmeta: AVB0, libavb 1.0, flags 3" || fail "disabled vbmeta layout"
import struct, sys
d = open(sys.argv[1], "rb").read()
assert d[:4] == b"AVB0" and struct.unpack_from(">II", d, 4) == (1, 0) and struct.unpack_from(">I", d, 120)[0] == 3 and len(d) == 4096
EOF
python3 tools/odin_pack.py tar --recovery "$IMG" -o "$T/p.tar.md5" >/dev/null
python3 -c "import sys;p=sys.argv[1];d=bytearray(open(p,'rb').read());d[700]^=1;open(p,'wb').write(d)" "$T/p.tar.md5"
python3 tools/odin_pack.py check "$T/p.tar.md5" >/dev/null && fail "corrupted package passed" || pass "corrupted package detected"

[ "$FAILS" = 0 ] && echo "All installer tests passed" || { echo "$FAILS installer test(s) FAILED"; exit 1; }
