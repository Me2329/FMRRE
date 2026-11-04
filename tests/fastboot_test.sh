#!/usr/bin/env bash
# The built-in fastbootd against the real `fastboot` client (apt install fastboot), over fastboot's
# TCP transport, on the simulator: getvar, raw + sparse + split-sparse flashing (with automatic
# logical partition resizing), logical partition commands, fetch, erase, set_active, oem fmrre / oem twrp,
# wipe-super / update (super_empty.img), error replies and reboot recovery.
#   tests/fastboot_test.sh [path/to/fmrre]
set -uo pipefail
cd "$(dirname "$0")/.."
BIN="$(realpath "${1:-build-host/fmrre}")"
command -v fastboot > /dev/null || { echo "SKIP: no fastboot client (apt install fastboot)"; exit 0; }
T="$(mktemp -d)"
R="$T/sim"
PORT=$((20000 + RANDOM % 20000))
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2> /dev/null; rm -rf "$T"; }
trap cleanup EXIT
pass=0
fail=0
FB() { fastboot -s "tcp:127.0.0.1:$PORT" "$@" > "$T/out" 2>&1; }
has() { grep -q -- "$1" "$T/out"; }
check() {
    local what="$1"
    shift
    if "$@"; then echo "  ok    $what"; pass=$((pass + 1))
    else echo "  FAIL  $what"; fail=$((fail + 1)); tail -4 "$T/out" | sed 's/^/        /'; fi
}
same() { cmp -s <(head -c "$(stat -c %s "$2")" "$1") "$2"; }
part() { echo "$R/dev/block/$1"; }

"$BIN" --sim --sim-root "$R" fmrre help > /dev/null
"$BIN" --sim --sim-root "$R" fmrre lp export --empty /sdcard/super_empty.img > /dev/null
start() {
    "$BIN" --sim --sim-root "$R" fmrre fastboot --tcp "$PORT" > "$T/server.log" 2>&1 &
    SRV=$!
    for _ in $(seq 50); do FB getvar version && return 0; sleep 0.1; done
    return 1
}
check "fastbootd starts and answers" start

echo "getvar"
check "is-userspace: yes" eval 'FB getvar is-userspace && has "is-userspace: yes"'
check "current-slot: a" eval 'FB getvar current-slot && has "current-slot: a"'
check "getvar all lists logical partitions" eval 'FB getvar all && has "is-logical:system_a:yes" && has "has-slot:boot:yes"'
check "unknown variable is a FAIL" eval 'FB getvar nonsense; has "FAILED (remote: .Variable not found.)"'

echo "flash"
python3 - "$T" <<'PY'
import random, sys
r = random.Random(1)
open(sys.argv[1] + '/boot.img', 'wb').write(b'ANDROID!' + bytes(r.getrandbits(8) for _ in range(3 * 1024 * 1024 - 8)))
b = bytearray()
for i in range(2048):  # 8 MiB, every third block data: the client sends it as several sparse pieces with -S 1M
    b += (bytes(r.getrandbits(8) for _ in range(64)) * 64) if i % 3 == 0 else bytes(4096)
open(sys.argv[1] + '/product.img', 'wb').write(b)
PY
check "raw image to boot_a" eval 'FB flash boot "$T/boot.img" && same "$(part by-name/boot_a)" "$T/boot.img"'
check "8 MB raw image in 1 MB sparse pieces to product_a (resized)" \
    eval 'FB -S 1M flash product "$T/product.img" && has "Resizing .product_a" && has "Sending sparse .product_a. 3/3" && same "$(part mapper/product_a)" "$T/product.img"'
check "partition-size follows the resize" eval 'FB getvar partition-size:product_a && has "0x800000"'
GSI="$R/data/media/0/Download/GSI_A15_download(1)"
check "sparse ext4 GSI to system_a" eval 'FB flash system "$GSI" && python3 - "$GSI" "$(part mapper/system_a)" <<'"'"'PY'"'"'
import struct, sys
d = open(sys.argv[1], "rb").read(); dev = open(sys.argv[2], "rb")
_, _, _, fh, ch, bs, _, chunks, _ = struct.unpack("<IHHHHIIII", d[:28]); pos, out = fh, 0
for _ in range(chunks):
    t, _, n, total = struct.unpack("<HHII", d[pos:pos + 12]); body = d[pos + ch:pos + total]
    dev.seek(out)
    if t == 0xCAC1: assert dev.read(len(body)) == body
    if t == 0xCAC2: assert dev.read(n * bs) == body[:4] * (n * bs // 4)
    out += n * bs; pos += total
PY'
check "unknown partition is a FAIL" eval '! FB flash nosuchpart "$T/boot.img" && has "No partition named nosuchpart"'

echo "partitions"
check "create-logical-partition" eval 'FB create-logical-partition scratch_test 1048576 && FB getvar partition-size:scratch_test && has "0x100000"'
check "resize-logical-partition" eval 'FB resize-logical-partition scratch_test 3145728 && FB getvar partition-size:scratch_test && has "0x300000"'
check "delete-logical-partition" eval 'FB delete-logical-partition scratch_test && { FB getvar partition-size:scratch_test; has "Variable not found"; }'
check "fetch boot_a" eval 'FB fetch boot_a "$T/fetched.img" && same "$T/fetched.img" "$T/boot.img"'
check "erase cache" eval 'FB erase cache && has "OKAY"'
check "set_active b (booted slot stays a)" eval 'FB set_active b && FB getvar current-slot && has "current-slot: a"'
check "oem fmrre runs FMRRE commands" eval 'FB oem fmrre lp list && has "(bootloader) system_a"'
check "oem twrp still works (alias)" eval 'FB oem twrp lp list && has "(bootloader) system_a"'
check "oem log" eval 'FB oem log 5 && has "(bootloader)"'

echo "super_empty.img"
check "wipe-super resets the layout" eval 'FB wipe-super "$R/data/media/0/super_empty.img" && FB getvar partition-size:system_a && has "0x0$"'
mkdir -p "$T/upd"
cp "$T/boot.img" "$T/upd/boot.img"
head -c 4194304 /dev/urandom > "$T/upd/system.img"
cp "$R/data/media/0/super_empty.img" "$T/upd/super_empty.img"
echo "require product=husky" > "$T/upd/android-info.txt"
(cd "$T/upd" && python3 -c "import zipfile,os; z=zipfile.ZipFile('../update.zip','w'); [z.write(f) for f in os.listdir('.')]")
# Newer clients build the whole super image on the computer and flash `super` ("optimized"); older ones
# send update-super and then resize + flash each partition. Either way system_a must end up right.
check "fastboot update (factory-image style)" \
    eval 'FB --skip-reboot update "$T/update.zip" && same "$(part mapper/system_a)" "$T/upd/system.img" && same "$(part by-name/boot_a)" "$T/upd/boot.img"'

# Older clients send update-super instead: speak the protocol directly (FB01 handshake, 8-byte lengths).
raw_update_super() {
    python3 - "$PORT" "$R/data/media/0/super_empty.img" > "$T/out" 2>&1 <<'PY'
import socket, struct, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"FB01"); assert s.recv(4) == b"FB01"
def msg(b): s.sendall(struct.pack(">Q", len(b)) + b)
def reply():
    n = struct.unpack(">Q", s.recv(8, socket.MSG_WAITALL))[0]
    return s.recv(n, socket.MSG_WAITALL).decode()
img = open(sys.argv[2], "rb").read()
msg(b"download:%08x" % len(img)); r = reply(); assert r.startswith("DATA"), r
msg(img); r = reply(); assert r.startswith("OKAY"), r
msg(b"update-super:super"); r = reply(); print(r)
PY
}
check "update-super keeps the other slot's partitions" \
    eval 'FB create-logical-partition keep_b 1048576 && raw_update_super && has "^OKAY" && FB getvar partition-size:keep_b && has "0x100000" && FB getvar partition-size:system_a && has "0x0$"'

echo "exit"
check "reboot recovery leaves fastboot mode" eval 'FB reboot recovery && wait "$SRV" && grep -q "back to FMRRE" "$T/server.log"'
SRV=""
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
