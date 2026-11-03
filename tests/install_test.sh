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
b = bytearray(1 << 20); b[0:8] = b"ANDROID!"; open(T + "/twrpv2-recovery.img", "wb").write(b)
v = bytearray(b"VNDRBOOT") + bytearray(4096); open(T + "/twrpv2-vendor_boot.img", "wb").write(v)
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
IMG="$T/twrpv2-recovery.img"
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
touch "$T/fastboot_on"; inst "$T/twrpv2-vendor_boot.img"
[ "$(rc)" != 0 ] && pass "test boot refused" || fail "vendor_boot test boot was attempted"
touch "$T/fastboot_on"; inst --permanent -y "$T/twrpv2-vendor_boot.img"
expect_call "flashes vendor_boot" "fastboot flash vendor_boot $T/twrpv2-vendor_boot.img"

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

scenario "Samsung on adb -> Download mode -> Heimdall RECOVERY + VBMETA"
touch "$T/adb_on"; ADB_MAN=samsung inst -y "$IMG"
expect_call "reboots to download" "adb reboot download"
expect_call "heimdall flash" "heimdall flash --RECOVERY $IMG --VBMETA out/samsung/vbmeta.img --no-reboot"
python3 tools/odin_pack.py check out/samsung/twrpv2-recovery-odin.tar.md5 > "$T/chk" && pass "Odin package md5 valid" || fail "Odin package invalid"
grep -q "recovery.img" "$T/chk" && grep -q "vbmeta.img" "$T/chk" && pass "package has recovery.img + vbmeta.img" || fail "package content"

scenario "Samsung --odin-only without Heimdall"
HEIMDALL=/nonexistent inst --odin-only --no-vbmeta "$IMG"
[ "$(rc)" = 0 ] && pass "exit 0" || fail "exit $(rc)"
python3 tools/odin_pack.py check out/samsung/twrpv2-recovery-odin.tar.md5 > "$T/chk" && ! grep -q vbmeta "$T/chk" && pass "recovery-only package" || fail "--no-vbmeta package"

scenario "nothing connected"
inst "$IMG"
[ "$(rc)" != 0 ] && grep -q "No phone found" "$T/out" && pass "explains how to connect" || fail "no-phone message"

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
