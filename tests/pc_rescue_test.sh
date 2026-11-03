#!/usr/bin/env bash
# Tests for pc/twrpv2_rescue.py against fake adb / fastboot / heimdall that answer like a phone in
# each mode (Android, TWRP v2, stock recovery, bootloader, Samsung Download mode) and log every call.
# Nothing touches a real device.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
T="$(mktemp -d)"
trap '[ -n "${KEEP:-}" ] && echo "kept $T" || rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/home"
FAILS=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }
has_out() { grep -qF -- "$2" "$T/out" && pass "$1" || { fail "$1 (output lacks: $2)"; sed 's/^/       /' "$T/out" | head -40; }; }
called() { grep -qF -- "$2" "$T/log" && pass "$1" || { fail "$1 (missing call: $2)"; sed 's/^/       /' "$T/log"; }; }
not_called() { grep -qE -- "$2" "$T/log" && fail "$1 (unexpected call: $2)" || pass "$1"; }
rc_is() { [ "$(cat "$T/rc")" = "$2" ] && pass "$1" || { fail "$1 (exit $(cat "$T/rc"), wanted $2)"; sed 's/^/       /' "$T/out" | head -30; }; }

# ---- fixtures: images, a Samsung AP with lz4-packed images, and the LZ4 decoder checks
python3 - "$T" "$ROOT/pc" <<'EOF'
import hashlib, io, os, random, struct, sys, tarfile
T, PC = sys.argv[1], sys.argv[2]
sys.path.insert(0, PC)
import twrpv2_rescue as tr

def compress_block(data, prefix=b""):
    """Greedy LZ4 block compressor (test only) - matches may reach into `prefix` and overlap themselves."""
    src = prefix + data
    n, start = len(src), len(prefix)
    out, table = bytearray(), {}
    for p in range(max(0, start - 65535), max(0, start - 3)):
        table[src[p:p + 4]] = p
    def put_len(x):
        while x >= 255:
            out.append(255); x -= 255
        out.append(x)
    i = anchor = start
    while i < n - 12:
        key = src[i:i + 4]
        j = table.get(key)
        table[key] = i
        if j is not None and i - j <= 65535:
            ml = 4
            while i + ml < n - 5 and src[j + ml] == src[i + ml]:
                ml += 1
            lit = src[anchor:i]
            out.append((min(len(lit), 15) << 4) | min(ml - 4, 15))
            if len(lit) >= 15: put_len(len(lit) - 15)
            out += lit + struct.pack("<H", i - j)
            if ml - 4 >= 15: put_len(ml - 4 - 15)
            i += ml
            anchor = i
        else:
            i += 1
    lit = src[anchor:]
    out.append(min(len(lit), 15) << 4)
    if len(lit) >= 15: put_len(len(lit) - 15)
    return bytes(out + lit)

def frame(data, block=1 << 16, independent=True, block_sum=False, content_sum=False, size=False, raw_every=0):
    flg = 0x40 | (0x20 * independent) | (0x10 * block_sum) | (0x08 * size) | (0x04 * content_sum)
    out = bytearray(b"\x04\x22\x4d\x18") + bytes([flg, 0x40]) + (struct.pack("<Q", len(data)) if size else b"") + b"\x00"
    prev = b""
    for k, off in enumerate(range(0, len(data), block)):
        chunk = data[off:off + block]
        if raw_every and k % raw_every == 0:
            out += struct.pack("<I", len(chunk) | 0x80000000) + chunk
        else:
            c = compress_block(chunk, b"" if independent else prev)
            out += struct.pack("<I", len(c)) + c
        if block_sum: out += b"\0" * 4
        prev = (prev + chunk)[-65536:]
    out += b"\0" * 4 + (b"\0" * 4 if content_sum else b"")
    return bytes(out)

def decode(b):
    o = io.BytesIO()
    tr.lz4_decode(io.BytesIO(b), o)
    return o.getvalue()

rnd = random.Random(7)
boot = bytearray(b"ANDROID!" + bytes(rnd.getrandbits(8) for _ in range(40000)))
boot += b"abc" * 30000 + bytes(70000) + b"TWRP v2 rescue " * 5000 + bytes(rnd.getrandbits(8) for _ in range(30000))
boot = bytes(boot)
checks = {
    "independent blocks": frame(boot),
    "dependent blocks + block checksums": frame(boot, independent=False, block_sum=True),
    "content size + content checksum + raw blocks": frame(boot, size=True, content_sum=True, raw_every=2),
    "dependent with raw blocks": frame(boot, independent=False, raw_every=3),
    "skippable frame + two frames": struct.pack("<II", 0x184D2A51, 3) + b"xyz" + frame(boot[:100000]) + frame(boot[100000:]),
}
bad = 0
for name, b in checks.items():
    ok = decode(b) == boot
    print(("ok   " if ok else "FAIL ") + "lz4: " + name)
    bad += not ok
assert len(frame(boot)) < len(boot) * 0.8, "the test compressor should really compress"
try:
    tr.lz4_block(b"\x0f\x01\x00", b"")  # match offset 1 with no output yet
    print("FAIL lz4: corrupt data must raise"); bad += 1
except (ValueError, IndexError):
    print("ok   lz4: corrupt data is refused")
open(T + "/lz4_fails", "w").write(str(bad))

# images
img = bytearray(1 << 16); img[0:8] = b"ANDROID!"
open(T + "/twrpv2-recovery.img", "wb").write(img)
open(T + "/twrpv2-vendor_boot.img", "wb").write(b"VNDRBOOT" + bytes(4096))
open(T + "/junk.img", "wb").write(b"\0" * 4096)
vb = bytearray(4096); vb[0:4] = b"AVB0"
open(T + "/vbmeta.img", "wb").write(vb)
open(T + "/boot_orig.img", "wb").write(boot)
rec = b"ANDROID!" + b"stock recovery " * 4000
open(T + "/recovery_orig.img", "wb").write(rec)
# Samsung AP: lz4 images like real firmware, plus files that must be left alone
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
    for name, data in (("boot.img.lz4", frame(boot, independent=False, content_sum=True)), ("vbmeta.img", bytes(vb)),
                       ("recovery.img.lz4", frame(rec)), ("super.img.lz4", frame(b"super" * 1000)), ("meta-data/fota.zip", b"PK")):
        ti = tarfile.TarInfo(name); ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
body = buf.getvalue()
open(T + "/AP_TEST.tar.md5", "wb").write(body + ("%s  AP_TEST.tar\n" % hashlib.md5(body).hexdigest()).encode())
EOF
[ "$(cat "$T/lz4_fails" 2>/dev/null)" = 0 ] || fail "lz4 decoder"

# ---- fake tools. State files in $T: fastboot_on, adb_on (+ adb_mode), download_on, boot_ok, installed_twrp
cat > "$T/bin/fastboot" <<'EOF'
#!/bin/bash
echo "fastboot $*" >> "$T/log"
[ "$1" = -s ] && shift 2
if [ ! -f "$T/fastboot_on" ]; then [ "$1" = devices ] && exit 0; echo "< waiting for any device >" >&2; exit 1; fi
case "$1" in
  devices) echo "FAKE123	fastboot" ;;
  getvar)
    if [ "$2" = all ]; then cat "$T/getvar_all" >&2
    else
      v="$(grep -F "(bootloader) $2:" "$T/getvar_all" | head -1)"
      [ -n "$v" ] && echo "$2: ${v#*"$2":}" >&2 || echo "getvar:$2 FAILED (remote: 'GetVar Variable Not found')" >&2
    fi
    echo "Finished. Total time: 0.002s" >&2 ;;
  oem) [ -f "$T/oem_info" ] && cat "$T/oem_info" >&2; echo "OKAY [  0.001s]" >&2 ;;
  boot)
    if [ -f "$T/boot_ok" ]; then rm -f "$T/fastboot_on"; touch "$T/adb_on"; echo twrp > "$T/adb_mode"; echo "Booting OKAY [  0.3s]" >&2
    else echo "FAILED (remote: 'unknown command')" >&2; exit 1; fi ;;
  reboot)
    rm -f "$T/fastboot_on"
    [ "${2:-}" = bootloader ] && touch "$T/fastboot_on"
    if [ "${2:-}" = recovery ] && [ -f "$T/installed_twrp" ]; then touch "$T/adb_on"; echo twrp > "$T/adb_mode"; fi ;;
  *) echo "OKAY [  0.1s]" >&2 ;;
esac
exit 0
EOF
cat > "$T/bin/adb" <<'EOF'
#!/bin/bash
echo "adb $*" >> "$T/log"
[ "$1" = -s ] && shift 2
mode="$(cat "$T/adb_mode" 2>/dev/null)"
if [ ! -f "$T/adb_on" ]; then
  case "$1" in devices) printf 'List of devices attached\n\n'; exit 0 ;; *) echo "error: no devices/emulators found" >&2; exit 1 ;; esac
fi
case "$mode" in twrp|stock_recovery) state=recovery ;; unauthorized) state=unauthorized ;; *) state=device ;; esac
case "$1" in
  devices) printf 'List of devices attached\nFAKE123                %s usb:1-1 product:husky model:Pixel_8_Pro device:husky transport_id:3\n\n' "$state" ;;
  get-state) echo "$state" ;;
  shell)
    shift
    case "$*" in
      getprop) cat "$T/props" ;;
      "/system/bin/recovery twrp help") [ "$mode" = twrp ] && echo "Commands (OpenRecoveryScript + TWRP v2):" || { echo "sh: /system/bin/recovery: not found"; exit 127; } ;;
      "/system/bin/recovery twrp autofix --dry-run")
        printf '> autofix --dry-run\nThe last boot failed: Android gave up after repeated crashes (Rescue Party)\n[ok] Boot image (boot_a)          OK\n[! ] Magisk / KernelSU modules    3 enabled\n[x] disable_modules    Disable all Magisk / KernelSU modules  (Can be undone)\n[x] wipe_caches        Clear the app caches (ART / Dalvik and /cache)  (Safe)\n' ;;
      "/system/bin/recovery twrp autofix "*) printf '[1/2] Disable all Magisk / KernelSU modules\n  done\nRepairs done. If the phone still does not start, come back here.\n' ;;
      "/system/bin/recovery twrp rescue save") echo "Logs saved to /sdcard/TWRP/logs/2026-11-03--19-00-00" ;;
    esac ;;
  reboot)
    rm -f "$T/adb_on"
    case "${2:-}" in
      bootloader) touch "$T/fastboot_on" ;;
      recovery) [ -f "$T/installed_twrp" ] && { touch "$T/adb_on"; echo twrp > "$T/adb_mode"; } ;;
      download) touch "$T/download_on" ;;
    esac ;;
  pull) mkdir -p "$3/$(basename "$2")"; echo "$2: 1 file pulled" ;;
  logcat) echo "11-03 19:00:00.000 E AndroidRuntime: FATAL EXCEPTION" ;;
esac
exit 0
EOF
cat > "$T/bin/heimdall" <<'EOF'
#!/bin/bash
echo "heimdall $*" >> "$T/log"
case "$1" in
  detect) [ -f "$T/download_on" ] && { echo "Device detected"; exit 0; }; echo "ERROR: Failed to detect compatible download-mode device."; exit 1 ;;
  print-pit) [ -f "$T/download_on" ] || exit 1
    printf -- '--- Entry #0 ---\nPartition Name: BOOT\nFlash Filename: boot.img\n\n--- Entry #1 ---\nPartition Name: VBMETA\nFlash Filename: vbmeta.img\n' ;;
  flash) [ -f "$T/download_on" ] || { echo "ERROR: Failed to detect compatible download-mode device."; exit 1; }
    prev=""
    for a in "$@"; do [ -n "$prev" ] && [ -f "$a" ] && cp "$a" "$T/flashed_${prev#--}"; prev="$a"; done
    echo "Uploading RECOVERY"; echo "100%"
    # The user then holds the key combination: TWRP v2 comes up on adb.
    if [[ " $* " == *" --RECOVERY "* ]] && [ -f "$T/flashed_RECOVERY" ] && cmp -s "$T/flashed_RECOVERY" "$T/twrpv2-recovery.img"; then
      rm -f "$T/download_on"; touch "$T/adb_on"; echo twrp > "$T/adb_mode"
    fi ;;
esac
exit 0
EOF
chmod +x "$T/bin/"*
export T HOME="$T/home" PATH="$T/bin:$PATH" TWRPV2_WAIT=3 NO_COLOR=1
unset ANDROID_SERIAL
PC="python3 $ROOT/pc/twrpv2_rescue.py"
IMG="$T/twrpv2-recovery.img"
scenario() { : > "$T/log"; rm -f "$T"/*_on "$T/adb_mode" "$T/boot_ok" "$T/installed_twrp" "$T/oem_info" "$T"/flashed_*; echo "--- $1"; }
run() { $PC "$@" > "$T/out" 2>&1 < "${STDIN:-/dev/null}"; echo $? > "$T/rc"; }
props() {  # key=value ... -> getprop output
  : > "$T/props"
  for kv in "$@"; do printf '[%s]: [%s]\n' "${kv%%=*}" "${kv#*=}" >> "$T/props"; done
}
pixel_props() { props ro.product.manufacturer=Google "ro.product.model=Pixel 8 Pro" ro.product.device=husky ro.build.version.release=15 \
  ro.boot.slot_suffix=_a "$@"; }
getvar_all() { printf '(bootloader) %s\n' "$@" > "$T/getvar_all"; }

scenario "no phone"
run status
rc_is "exit 3" 3
has_out "says so" "No phone found"
has_out "explains the key combo" "Volume Down + Power"

scenario "Android, unlocked: model and lock state"
touch "$T/adb_on"; echo android > "$T/adb_mode"
pixel_props ro.boot.flash.locked=0 ro.boot.verifiedbootstate=orange sys.oem_unlock_allowed=1
run status
rc_is "exit 0" 0
has_out "model" "Google Pixel 8 Pro (husky)"
has_out "unlocked" "UNLOCKED"
has_out "mode" "Android is running"
run status --json
python3 -c "import json,sys; d=json.load(open('$T/out')); sys.exit(0 if d['locked'] is False and d['model']=='Pixel 8 Pro' else 1)" \
  && pass "--json" || fail "--json"
[ -f "$T/home/.twrpv2_rescue.json" ] && pass "remembers the model by serial" || fail "model cache"

scenario "Android, locked: says how to unlock, changes nothing"
touch "$T/adb_on"; echo android > "$T/adb_mode"
pixel_props ro.boot.flash.locked=1 ro.boot.verifiedbootstate=green sys.oem_unlock_allowed=1
run fix --yes
rc_is "refused (exit 2)" 2
has_out "locked" "LOCKED"
has_out "OEM unlocking already allowed" "OEM unlocking is already allowed"
has_out "unlock command" "fastboot flashing unlock"
has_out "official tool" "pixelrepair.withgoogle.com"
not_called "no reboot" "adb .*reboot"

scenario "bootloader: slot A unbootable, slot B booted fine -> rollback"
touch "$T/fastboot_on"
getvar_all product:husky unlocked:yes current-slot:a slot-count:2 slot-unbootable:a:yes slot-successful:a:no slot-retry-count:a:0 \
  slot-unbootable:b:no slot-successful:b:yes slot-retry-count:b:3 is-userspace:no "version-bootloader:ripcurrent-16.0"
run status
has_out "model from the codename / cache" "Pixel 8 Pro"
has_out "unlocked" "UNLOCKED"
has_out "slot health" "A: unbootable, never booted fully, 0 tries left; B: booted fine, 3 tries left"
run diagnose
has_out "finding" "Slot A is marked unbootable; slot B booted fine before"
run fix --yes --dry-run
has_out "dry run shows the command" "$ fastboot -s FAKE123 set_active b"
not_called "dry run changes nothing" "set_active"
run fix --yes
rc_is "exit 0" 0
called "switches to slot B" "fastboot -s FAKE123 set_active b"
called "reboots" "fastboot -s FAKE123 reboot"

scenario "bootloader, locked -> refused"
touch "$T/fastboot_on"
getvar_all product:husky unlocked:no current-slot:a slot-unbootable:a:yes slot-successful:b:yes
run fix --yes --recovery "$IMG"
rc_is "refused" 2
has_out "explains" "the bootloader is LOCKED"
not_called "no writes" "fastboot .*(set_active|flash|boot |-w)"

scenario "bootloader, no lock variable: asks oem device-info"
touch "$T/fastboot_on"
getvar_all product:lmi current-slot:a
printf '(bootloader) Verity mode: true\n(bootloader) Device unlocked: true\n(bootloader) Device critical unlocked: false\n' > "$T/oem_info"
run status
has_out "unlocked via oem device-info" "oem device-info: Device unlocked: true"

scenario "bootloader + TWRP v2 image: start it from RAM, Auto repair from the inside"
touch "$T/fastboot_on" "$T/boot_ok"
getvar_all product:husky unlocked:yes current-slot:a slot-successful:a:yes slot-successful:b:yes
run fix --yes --recovery "$IMG"
rc_is "exit 0" 0
called "fastboot boot" "fastboot -s FAKE123 boot $IMG"
called "asks TWRP v2 what is wrong" "adb -s FAKE123 shell /system/bin/recovery twrp autofix --dry-run"
called "repairs" "adb -s FAKE123 shell /system/bin/recovery twrp autofix --allow-slot-switch --reboot"
has_out "shows TWRP v2's diagnosis" "Rescue Party"
not_called "nothing flashed" "fastboot .*flash"

scenario "vendor_boot image can't be test-booted"
touch "$T/fastboot_on"
getvar_all product:husky unlocked:yes current-slot:a
run boot-twrp "$T/twrpv2-vendor_boot.img" --yes
rc_is "refused" 2
has_out "explains" "vendor_boot image"

scenario "bootloader, no image, TWRP v2 installed: its recovery is started"
touch "$T/fastboot_on" "$T/installed_twrp"
getvar_all product:husky unlocked:yes current-slot:a slot-successful:a:yes
run fix --yes
rc_is "exit 0" 0
called "reboot recovery" "fastboot -s FAKE123 reboot recovery"
called "auto repair" "twrp autofix --allow-slot-switch --reboot"

scenario "bootloader, no image, no TWRP v2: says what it needs"
touch "$T/fastboot_on"
getvar_all product:husky unlocked:yes current-slot:a slot-successful:a:yes
run fix --yes
rc_is "refused" 2
has_out "asks for an image" "--recovery twrpv2-recovery.img"

scenario "Android -> recovery -> Auto repair"
touch "$T/adb_on" "$T/installed_twrp"; echo android > "$T/adb_mode"
pixel_props ro.boot.flash.locked=0 ro.boot.verifiedbootstate=orange
run fix --yes --no-reboot
rc_is "exit 0" 0
called "reboots to recovery" "adb -s FAKE123 reboot recovery"
called "no reboot at the end" "twrp autofix --allow-slot-switch"
not_called "--no-reboot honoured" "autofix --allow-slot-switch --reboot"

scenario "TWRP v2 running: diagnose + logs"
touch "$T/adb_on"; echo twrp > "$T/adb_mode"
pixel_props ro.boot.flash.locked=0 ro.boot.verifiedbootstate=orange
run diagnose
has_out "recovery is TWRP v2" "TWRP v2"
has_out "headline" "The last boot failed: Android gave up after repeated crashes"
has_out "planned fixes" "Planned fixes: disable_modules"
run logs "$T/logs"
rc_is "logs saved" 0
called "rescue save" "twrp rescue save"
called "pulls the folder" "adb -s FAKE123 pull /data/media/0/TWRP/logs/2026-11-03--19-00-00"
[ -f "$T/logs/phone.json" ] && pass "phone.json" || fail "phone.json"

scenario "stock recovery: points to sideload"
touch "$T/adb_on"; echo stock_recovery > "$T/adb_mode"
pixel_props ro.boot.flash.locked=0
run diagnose
has_out "not TWRP v2" "A recovery other than TWRP v2"
has_out "sideload advice" "adb sideload"

scenario "USB debugging not allowed"
touch "$T/adb_on"; echo unauthorized > "$T/adb_mode"
run diagnose
has_out "explains the prompt" "Allow USB debugging"

scenario "disable vbmeta"
touch "$T/fastboot_on"
getvar_all product:husky unlocked:yes current-slot:a
run disable-vbmeta --yes --workdir "$T/w"
rc_is "exit 0" 0
called "flashes a disabled vbmeta" "fastboot -s FAKE123 --disable-verity --disable-verification flash vbmeta $T/w/vbmeta_disabled.img"
python3 -c "import struct; d=open('$T/w/vbmeta_disabled.img','rb').read(); exit(0 if d[:4]==b'AVB0' and struct.unpack_from('>I',d,120)[0]==3 else 1)" \
  && pass "vbmeta flags = 3" || fail "vbmeta content"
run disable-vbmeta --yes --both-slots --vbmeta "$T/vbmeta.img"
called "stock vbmeta, both slots" "flash --slot=all vbmeta $T/vbmeta.img"
run disable-vbmeta --yes --vbmeta "$T/junk.img"
rc_is "not vbmeta: refused" 2

scenario "update still merging: no flashing"
touch "$T/fastboot_on"
getvar_all product:husky unlocked:yes current-slot:a snapshot-update-status:merging
run flash boot "$IMG" --yes
rc_is "refused" 2
has_out "explains" "still being merged"

scenario "flash and wipe"
touch "$T/fastboot_on"
getvar_all product:husky unlocked:yes current-slot:a
run flash boot "$T/junk.img" --yes
rc_is "wrong image refused" 2
run flash boot "$IMG" --yes
called "flashes" "fastboot -s FAKE123 flash boot $IMG"
echo nope > "$T/answer"; STDIN="$T/answer" run wipe-data --yes
rc_is "wipe needs ERASE typed, even with --yes" 2
not_called "not wiped" "fastboot .* -w"
echo ERASE > "$T/answer"; STDIN="$T/answer" run wipe-data
called "wiped after ERASE" "fastboot -s FAKE123 -w"

scenario "Samsung Download mode: lock state is on the screen"
touch "$T/download_on"
run status
has_out "mode" "Samsung Download mode"
has_out "read the screen" "OEM LOCK: OFF (U)"
run fix --yes --recovery "$IMG"
rc_is "needs --unlocked" 2
not_called "nothing flashed" "heimdall flash"

scenario "Samsung: install TWRP v2, then Auto repair"
touch "$T/download_on"
run fix --yes --unlocked --recovery "$IMG" --workdir "$T/w2"
rc_is "exit 0" 0
called "recovery + vbmeta" "heimdall flash --RECOVERY $IMG --VBMETA $T/w2/vbmeta.img --no-reboot"
python3 -c "import struct; d=open('$T/flashed_VBMETA','rb').read(); exit(0 if struct.unpack_from('>I',d,120)[0]==3 else 1)" \
  && pass "vbmeta flashed with flags 3" || fail "vbmeta flags"
called "auto repair after TWRP v2 came up" "twrp autofix --allow-slot-switch --reboot"

scenario "Samsung: stock images from an AP .tar.md5 (lz4)"
touch "$T/download_on"
run samsung-stock "$T/AP_TEST.tar.md5" --yes --unlocked --workdir "$T/w3"
rc_is "exit 0" 0
called "partition table read" "heimdall print-pit --no-reboot"
called "flashes boot, recovery, vbmeta (PIT names, session resumed)" \
  "heimdall flash --BOOT $T/w3/boot.img --RECOVERY $T/w3/recovery.img --VBMETA $T/w3/vbmeta.img --resume"
cmp -s "$T/flashed_BOOT" "$T/boot_orig.img" && pass "boot.img.lz4 unpacked exactly" || fail "boot.img content"
cmp -s "$T/flashed_RECOVERY" "$T/recovery_orig.img" && pass "recovery.img.lz4 unpacked exactly" || fail "recovery.img content"
not_called "super left alone" "super"
run samsung-stock "$T/AP_TEST.tar.md5" --yes --unlocked --parts boot
called "--parts" "heimdall flash --BOOT"

# ---- the window: click "Fix it for me" on a bootlooping phone (needs tkinter and Xvfb; skipped otherwise)
TKPY=""
for p in python3 /usr/bin/python3 /usr/bin/python3.12 /usr/bin/python3.11; do
  command -v "$p" > /dev/null 2>&1 && "$p" -c "import tkinter" > /dev/null 2>&1 && { TKPY="$p"; break; }
done
if [ -n "$TKPY" ] && command -v xvfb-run > /dev/null; then
  scenario "window: Fix it for me (slot A unbootable)"
  touch "$T/fastboot_on"
  getvar_all product:husky unlocked:yes current-slot:a slot-unbootable:a:yes slot-successful:a:no slot-retry-count:a:0 \
    slot-unbootable:b:no slot-successful:b:yes slot-retry-count:b:3
  cat > "$T/gui_drive.py" <<'EOF'
import os, sys, tkinter as tk
from tkinter import messagebox
sys.path.insert(0, sys.argv[1])
import twrpv2_rescue as tr
asked, out = [], {}
messagebox.askyesno = lambda title, q, parent=None: (asked.append(q), True)[1]
real_mainloop = tk.Tk.mainloop

def find(w, cls=None, text=None):
    for c in w.winfo_children():
        try:
            if (text is None or c.cget("text") == text) and (cls is None or c.winfo_class() == cls):
                return c
        except tk.TclError:
            pass
        r = find(c, cls, text)
        if r is not None:
            return r

def drive(root):
    state = {"t": 0, "clicked": False}
    def step():
        state["t"] += 1
        log = find(root, cls="Text")  # "What was found" comes first ...
        texts = []
        def all_text(w):
            for c in w.winfo_children():
                if c.winfo_class() == "Text":
                    texts.append(c.get("1.0", "end"))
                all_text(c)
        all_text(root)
        joined = "\n".join(texts)
        if not state["clicked"] and "Slot A is marked unbootable" in joined:
            find(root, text="Fix it for me").invoke()
            state["clicked"] = True
        if state["clicked"] and "==> done" in joined:
            out["text"] = joined
            if os.environ.get("SHOT"):
                os.system("import -window root %s" % os.environ["SHOT"])
            root.destroy()
            return
        if state["t"] > 300:
            out["text"] = joined
            root.destroy()
            return
        root.after(100, step)
    root.after(500, step)

def mainloop(self, n=0):
    drive(self)
    real_mainloop(self, n)
tk.Tk.mainloop = mainloop
tr.main(["gui"])
print("ASKED:", " | ".join(asked))
print(out.get("text", "(no output)"))
EOF
  xvfb-run -a -s "-screen 0 1000x860x24" "$TKPY" "$T/gui_drive.py" "$ROOT/pc" > "$T/out" 2>&1
  has_out "asked before switching" "Make slot B active"
  has_out "shows the repair in the log" "$ fastboot -s FAKE123 set_active b"
  has_out "finished" "==> done"
  called "switched slots" "fastboot -s FAKE123 set_active b"
else
  echo "skip window test (no tkinter or no xvfb-run)"
fi

echo
if [ "$FAILS" = 0 ]; then echo "All PC rescue tests passed"; else echo "$FAILS PC rescue test(s) FAILED"; exit 1; fi
