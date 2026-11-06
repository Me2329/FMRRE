#!/usr/bin/env bash
# Rebuild the README screenshots (docs/screenshots/*.png) from the simulator, in fresh sandboxes.
#   tools/make_screenshots.sh            needs out/sim/fmrre (scripts/build.sh sim) and ImageMagick (montage, convert)
# The PC rescue window (pc_rescue.png) is made separately: it needs tkinter, Xvfb and the fake phone of
# tests/pc_rescue_test.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN="${BIN:-out/sim/fmrre}"
[ -x "$BIN" ] || { echo "build the simulator first: scripts/build.sh sim" >&2; exit 1; }
command -v montage > /dev/null || { echo "ImageMagick (montage) is needed" >&2; exit 1; }
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
OUT=docs/screenshots
BG="#0d1117"

sandbox() {  # sandbox NAME [setting=value ...]: a fresh simulator with these settings
  local d="$W/$1"; shift
  "$BIN" --sim --sim-root "$d" fmrre help > /dev/null 2>&1
  : > "$d/data/media/0/FMRRE/.fmrre.conf"
  for kv in "$@"; do echo "$kv" >> "$d/data/media/0/FMRRE/.fmrre.conf"; done
  echo "$d"
}
shoot() {  # shoot SANDBOX SCRIPT_TEXT
  printf '%s\n' "$2" > "$W/script.txt"
  "$BIN" --sim --headless --sim-root "$1" --script "$W/script.txt" --out "$W/shots" > /dev/null
}
row() {  # row OUT.png tile files...: phone screens side by side
  local out="$1"; shift
  montage "$@" -tile "$#x1" -geometry 324x702+7+14 -background "$BG" "$W/row.png"
  convert "$W/row.png" -bordercolor "$BG" -border 7x0 -depth 8 -strip "$out"
}
mkdir -p "$W/shots"

# ---- overview + tools: the docs walk-through (home, install, flashing, backup, wipe, dialogs, monitor, terminal, files)
S="$(sandbox docs)"
"$BIN" --sim --headless --sim-root "$S" --script tools/scripts/docs.txt --out "$W/shots" > /dev/null
# PIN lock screen, light style
L="$(sandbox lock style=3 accent=2 "pin_hash=$(printf '1234:twrpv2' | sha256sum | cut -c1-64)")"
shoot "$L" "wait 1600
tap 148 459
tap 270 459
wait 300
shot pin.png"
cd "$W/shots"
row "$OLDPWD/$OUT/overview.png" home.png install.png flashing.png backup.png wipe.png
row "$OLDPWD/$OUT/tools.png" dialog.png monitor.png terminal.png files.png pin.png
cd "$OLDPWD"

# ---- themes: settings, then the home screen in three accent / style combinations
for t in "violet_aurora accent=1 style=2" "ocean_light accent=2 style=3" "teal_amoled accent=0 style=1"; do
  set -- $t
  name="$1"; shift
  shoot "$(sandbox "$name" "$@")" "wait 1700
shot theme_$name.png"
done
row "$OUT/themes.png" "$W/shots/settings.png" "$W/shots/theme_violet_aurora.png" "$W/shots/theme_ocean_light.png" "$W/shots/theme_teal_amoled.png"

# ---- GSI wizard, find & flash (a typo'd path, then the found file), root
S="$(sandbox gsi)"
shoot "$S" "wait 1200
open gsi /sdcard/Download/system-td-arm64-ab-vanilla.img.xz
shot g1_gsi.png
key back
open manual /sdcard/Download/GSI_A15_downlod.img
shot g2_manual.png
key back
open manual /sdcard/Download/GSI_A15_download(1)
swipe 270 900 270 300 300
wait 900
shot g3_manual_found.png
key back
open root
shot g4_root.png"
row "$OUT/gsi_root.png" "$W/shots/g1_gsi.png" "$W/shots/g2_manual.png" "$W/shots/g3_manual_found.png" "$W/shots/g4_root.png"

# ---- A/B OTA, fastbootd, bootloop rescue, the home screen in Romanian
S="$(sandbox ota)"
shoot "$S" "wait 1200
open payload /sdcard/Download/lineage-22.2-20261101-nightly-husky-signed.zip
shot o1_payload.png
key back
open fastboot --no-start
wait 600
shot o2_fastboot.png
key back
open rescue
shot o3_rescue.png"
R="$(sandbox ro lang=ro)"
shoot "$R" "wait 1700
shot o4_home_ro.png"
row "$OUT/ota_fastboot_rescue.png" "$W/shots/o1_payload.png" "$W/shots/o2_fastboot.png" "$W/shots/o3_rescue.png" "$W/shots/o4_home_ro.png"

# ---- Auto repair: the home card after a failed boot, the diagnosis, the repair, in Romanian
S="$(sandbox autofix)"
shoot "$S" "wait 1600
shot a1_home.png
tap 270 330
wait 900
shot a2_autofix.png
swipe 60 1005 520 1005 600
wait 400
waittask
shot a3_done.png"
R="$(sandbox autofix_ro lang=ro)"
shoot "$R" "wait 1600
open autofix
swipe 270 900 270 600 300
wait 900
shot a4_ro.png"
row "$OUT/autofix.png" "$W/shots/a1_home.png" "$W/shots/a2_autofix.png" "$W/shots/a3_done.png" "$W/shots/a4_ro.png"

# ---- Zip signatures: a ROM signed with another key in the queue, whose key it is, an update of the installed ROM,
# then auto repair after that ROM was flashed without wiping data. The keys are made with openssl (tests/signapk.py).
S="$(sandbox sig)"
python3 - "$S" "$W/keys" <<'PY'
import io, os, sys, zipfile
sys.path.insert(0, 'tests')
import signapk
root, keys = sys.argv[1], sys.argv[2]
os.makedirs(keys, exist_ok=True)
lin_c, lin_k = signapk.make_key('rsa:4096', '/O=LineageOS/CN=LineageOS', keys + '/lineage')
cr_c, cr_k = signapk.make_key('rsa:2048', '/O=crDroid/CN=crDroid release key', keys + '/crdroid')
sec = root + '/system_root/system/etc/security'  # the installed system (LineageOS) trusts its own key
os.makedirs(sec, exist_ok=True)
with zipfile.ZipFile(sec + '/otacerts.zip', 'w') as z:
    z.write(lin_c, 'releasekey.x509.pem')
def rom(name):
    b = io.BytesIO()
    with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('META-INF/com/google/android/update-binary', '#!/sbin/sh\n')
        z.writestr('META-INF/com/google/android/updater-script', 'ui_print("Installing %s");\n' % name)
        z.writestr('system.transfer.list', '4\n0\n0\n0\n')
        z.writestr('system.new.dat', os.urandom(1 << 20))
    return b.getvalue()
dl = root + '/data/media/0/Download/'
open(dl + 'crDroid-11.2-husky.zip', 'wb').write(signapk.sign_zip(rom('crDroid 11.2'), cr_c, cr_k))
open(dl + 'lineage-22.2-20261108-husky.zip', 'wb').write(signapk.sign_zip(rom('LineageOS 22.2'), lin_c, lin_k))
PY
shoot "$S" "wait 1200
open install /sdcard/Download/crDroid-11.2-husky.zip
wait 1500
shot s1_queue.png
open signature /sdcard/Download/crDroid-11.2-husky.zip
wait 900
shot s2_other.png
key back
open signature /sdcard/Download/lineage-22.2-20261108-husky.zip
wait 900
shot s3_same.png"
"$BIN" --sim --sim-root "$S" fmrre install /sdcard/Download/crDroid-11.2-husky.zip > /dev/null 2>&1
shoot "$S" "wait 1200
open autofix
swipe 270 900 270 330 400
wait 900
shot s4_autofix.png"
row "$OUT/signatures.png" "$W/shots/s1_queue.png" "$W/shots/s2_other.png" "$W/shots/s3_same.png" "$W/shots/s4_autofix.png"

ls -la "$OUT"
