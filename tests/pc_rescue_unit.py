#!/usr/bin/env python3
"""Cross-platform tests for pc/fmrre_rescue.py (Windows, macOS, Linux - CI runs it on all three).

Fake adb / fastboot / heimdall are one Python script behind a small wrapper (.bat on Windows, a shell
script elsewhere), named by ADB= FASTBOOT= HEIMDALL=, so the tool runs exactly as it would with the real
platform-tools. tests/pc_rescue_test.sh covers many more scenarios on Linux; this one checks that the
basics work everywhere: finding the phone, the lock state, a repair, a refusal, Romanian output, the
vbmeta it generates and a Samsung AP with an LZ4 image.

  python3 tests/pc_rescue_unit.py
"""
import io
import json
import os
import struct
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "pc", "fmrre_rescue.py")

FAKE = r'''
import json, os, sys
tool, args = sys.argv[1], sys.argv[2:]
d = os.path.dirname(os.path.abspath(__file__))
st = json.load(open(os.path.join(d, "state.json")))
with open(os.path.join(d, "calls.txt"), "a") as f:
    f.write(tool + " " + " ".join(args) + "\n")
if args[:1] == ["-s"]:
    args = args[2:]
mode = st["mode"]
def save():
    json.dump(st, open(os.path.join(d, "state.json"), "w"))
if tool == "fastboot":
    if mode != "fastboot":
        sys.exit(0 if args[:1] == ["devices"] else 1)
    if args[:1] == ["devices"]:
        print("FAKE123\tfastboot")
    elif args[:2] == ["getvar", "all"]:
        for k, v in st["getvar"].items():
            sys.stderr.write("(bootloader) %s:%s\n" % (k, v))
    elif args[:1] == ["getvar"]:
        v = st["getvar"].get(args[1])
        sys.stderr.write(("%s: %s\n" % (args[1], v)) if v is not None else ("getvar:%s FAILED (remote: 'not found')\n" % args[1]))
    elif args[:1] == ["reboot"]:
        st["mode"] = "none"; save()
    else:
        sys.stderr.write("OKAY [  0.1s]\n")
elif tool == "adb":
    if mode == "sideload":
        if args[:1] == ["devices"]:
            print("List of devices attached\nFAKE123   sideload product:husky model:Pixel_8_Pro device:husky\n")
        elif args[:1] == ["get-state"]:
            print("sideload")
        elif args[:1] == ["sideload"]:
            print("serving: '%s'  (~100%%)" % os.path.basename(args[1]))
            print("Total xfer: 1.00x")
            st["mode"] = "none"; save()
        sys.exit(0)
    if mode != "android":
        if args[:1] == ["devices"]:
            print("List of devices attached\n")
            sys.exit(0)
        sys.stderr.write("error: no devices/emulators found\n"); sys.exit(1)
    if args[:1] == ["devices"]:
        print("List of devices attached\nFAKE123   device product:husky model:Pixel_8_Pro device:husky\n")
    elif args[:1] == ["get-state"]:
        print("device")
    elif args[:2] == ["shell", "getprop"]:
        for k, v in st["props"].items():
            print("[%s]: [%s]" % (k, v))
    elif args[:2] == ["reboot", "sideload"]:
        st["mode"] = "sideload"; save()
elif tool == "heimdall":
    if args[:1] == ["detect"]:
        sys.exit(0 if mode == "download" else 1)
    if mode != "download":
        sys.exit(1)
    if args[:1] == ["flash"]:
        for a, b in zip(args, args[1:]):
            if a.startswith("--") and os.path.isfile(b):
                open(os.path.join(d, "flashed_" + a[2:]), "wb").write(open(b, "rb").read())
        print("100%")
sys.exit(0)
'''

FAILS = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        FAILS.append(name)
        if detail:
            print("     " + detail.replace("\n", "\n     ")[:3000])


def lz4_frame(data):
    """An LZ4 frame with one raw block and one compressed block (literals + an overlapping match)."""
    half = data[:len(data) // 2]
    comp = bytes([0x2F]) + b"ab" + bytes([0x01, 0x00]) + bytes([200]) + bytes([0x10]) + b"z"  # "ab" then 219x "b", then "z"
    out = bytearray(b"\x04\x22\x4d\x18") + bytes([0x60, 0x40, 0x00])
    out += struct.pack("<I", len(half) | 0x80000000) + half
    out += struct.pack("<I", len(comp)) + comp
    out += b"\0" * 4
    return bytes(out), half + b"ab" + b"b" * 219 + b"z"


def main():
    work = tempfile.mkdtemp(prefix="fmrre-pc-unit-")
    fake = os.path.join(work, "fake")
    os.makedirs(fake)
    open(os.path.join(fake, "fake_tool.py"), "w").write(FAKE)
    env = dict(os.environ, HOME=work, USERPROFILE=work, FMRRE_LANG="en", FMRRE_WAIT="2", NO_COLOR="1", PYTHONIOENCODING="utf-8")
    env.pop("ANDROID_SERIAL", None)
    for t in ("adb", "fastboot", "heimdall"):
        if os.name == "nt":
            w = os.path.join(fake, t + ".bat")
            open(w, "w").write('@"%s" "%%~dp0fake_tool.py" %s %%*\r\n' % (sys.executable, t))
        else:
            w = os.path.join(fake, t)
            open(w, "w").write('#!/bin/sh\nexec "%s" "$(dirname "$0")/fake_tool.py" %s "$@"\n' % (sys.executable, t))
            os.chmod(w, 0o755)
        env[t.upper()] = w

    def phone(mode, getvar=None, props=None):
        json.dump({"mode": mode, "getvar": getvar or {}, "props": props or {}}, open(os.path.join(fake, "state.json"), "w"))
        open(os.path.join(fake, "calls.txt"), "w").close()

    def run(*args, stdin=""):
        p = subprocess.run([sys.executable, TOOL] + list(args), input=stdin.encode(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env=env, timeout=120)
        return p.returncode, p.stdout.decode("utf-8", "replace")

    def calls():
        return open(os.path.join(fake, "calls.txt")).read()

    slot_a_broken = {"product": "husky", "unlocked": "yes", "current-slot": "a", "slot-unbootable:a": "yes", "slot-successful:a": "no",
                     "slot-retry-count:a": "0", "slot-unbootable:b": "no", "slot-successful:b": "yes", "slot-retry-count:b": "3"}

    phone("none")
    rc, out = run("status")
    check("no phone: exit 3 and what to do", rc == 3 and "No phone found" in out and "Volume Down + Power" in out, out)

    phone("fastboot", slot_a_broken)
    rc, out = run("diagnose")
    check("bootloader: model from the codename, unlocked, the broken slot",
          "Pixel 8 Pro" in out and "UNLOCKED" in out and "Slot A is marked unbootable; slot B booted fine before" in out, out)
    rc, out = run("fix", "--yes")
    check("fix: rolls back to slot B and reboots", rc == 0 and "set_active b" in calls() and "fastboot -s FAKE123 reboot" in calls(),
          out + "\n" + calls())

    phone("fastboot", dict(slot_a_broken, unlocked="no"))
    rc, out = run("fix", "--yes")
    check("locked: refused, nothing written", rc == 2 and "LOCKED" in out and "set_active" not in calls(), out)

    phone("fastboot", slot_a_broken)
    rc, out = run("--lang", "ro", "diagnose")
    check("Romanian (UTF-8 through a pipe)", "DEBLOCAT" in out and "Slotul A este marcat că nu pornește" in out, out)

    phone("fastboot", slot_a_broken)
    rc, out = run("disable-vbmeta", "--yes", "--workdir", os.path.join(work, "w"))
    vb = os.path.join(work, "w", "vbmeta_disabled.img")
    ok = rc == 0 and os.path.isfile(vb) and struct.unpack_from(">I", open(vb, "rb").read(), 120)[0] == 3
    check("disable-vbmeta: generated image with flags 3", ok and "--disable-verification flash vbmeta" in calls(), out)

    phone("android", props={"ro.product.manufacturer": "Google", "ro.product.model": "Pixel 8 Pro", "ro.product.device": "husky",
                            "ro.build.version.release": "15", "ro.boot.flash.locked": "1", "ro.boot.verifiedbootstate": "green",
                            "sys.oem_unlock_allowed": "1"})
    rc, out = run("status")
    check("Android: model, LOCKED, how to unlock", "Google Pixel 8 Pro" in out and "LOCKED" in out and "fastboot flashing unlock" in out
          and "OEM unlocking is already allowed" in out, out)

    # ---- OEM-unlock diagnosis (helping the owner of the phone) ----
    sam = {"ro.product.manufacturer": "Samsung", "ro.product.model": "SM-S938B", "ro.product.device": "pa3q",
           "ro.build.version.release": "16", "ro.boot.flash.locked": "1", "ro.boot.verifiedbootstate": "green",
           "sys.oem_unlock_allowed": "0"}
    phone("android", props=sam)
    rc, out = run("check-unlock")
    check("check-unlock: Samsung international, toggle hidden by VaultKeeper (only hidden, not gone)",
          rc == 0 and "VaultKeeper" in out and "international" in out and "7-day" in out, out)

    phone("android", props=dict(sam, **{"ro.product.model": "SM-S938U"}))
    rc, out = run("check-unlock")
    check("check-unlock: Samsung US (SM-...U) can't be unlocked, no bypass offered",
          "can't be unlocked" in out and "US carrier" in out and "bypass" in out, out)

    phone("android", props=dict(sam, **{"sys.oem_unlock_allowed": "1"}))
    rc, out = run("check-unlock")
    check("check-unlock: toggle already on -> you can unlock", "you can unlock" in out, out)

    phone("download")
    rc, out = run("check-unlock")
    check("check-unlock: Samsung Download mode -> read OEM LOCK / KG STATE on the screen",
          "OEM LOCK" in out and "KG STATE" in out, out)

    phone("android", props=sam)
    rc, out = run("--lang", "ro", "check-unlock")
    check("check-unlock: Romanian", "VaultKeeper" in out and ("debloc" in out.lower()), out)

    # A locked phone: the official OTA goes in with adb sideload (the stock recovery checks the signature).
    import zipfile
    ota = os.path.join(work, "husky-ota.zip")
    with zipfile.ZipFile(ota, "w") as z:
        z.writestr("META-INF/com/android/metadata", "ota-type=AB\npre-device=husky\n")
        z.writestr("payload.bin", "CrAU")
    phone("android", props={"ro.product.manufacturer": "Google", "ro.product.model": "Pixel 8 Pro", "ro.product.device": "husky",
                            "ro.boot.flash.locked": "1", "ro.boot.verifiedbootstate": "green"})
    rc, out = run("fix", "--yes", "--ota", ota)
    check("locked phone: official OTA with adb sideload", rc == 0 and "reboot sideload" in calls() and "sideload " + ota in calls(),
          out + calls())

    phone("download")
    boot = b"ANDROID!" + bytes(range(256)) * 40
    frame, expect = lz4_frame(boot)
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        ti = tarfile.TarInfo("boot.img.lz4")
        ti.size = len(frame)
        t.addfile(ti, io.BytesIO(frame))
    ap = os.path.join(work, "AP_TEST.tar.md5")
    body = buf.getvalue()
    open(ap, "wb").write(body + b"0123456789abcdef0123456789abcdef  AP_TEST.tar\n")
    rc, out = run("samsung-stock", ap, "--yes", "--unlocked", "--workdir", os.path.join(work, "w1"))
    check("Samsung: an AP whose MD5 doesn't match is refused", rc == 2 and "MD5 doesn't match" in out and "heimdall flash" not in calls(), out)
    import hashlib
    open(ap, "wb").write(body + ("%s  AP_TEST.tar\n" % hashlib.md5(body).hexdigest()).encode())
    rc, out = run("samsung-stock", ap, "--yes", "--workdir", os.path.join(work, "w2"))
    check("Samsung: refused until the lock state is confirmed", rc == 2 and "heimdall flash" not in calls(), out)
    rc, out = run("samsung-stock", ap, "--yes", "--unlocked", "--workdir", os.path.join(work, "w2"))
    got = os.path.join(fake, "flashed_BOOT")
    check("Samsung: boot.img.lz4 unpacked and flashed", rc == 0 and os.path.isfile(got) and open(got, "rb").read() == expect, out + calls())

    bad = bytearray(frame)
    bad[-9], bad[-8] = 0xFF, 0xFF  # the match offset now points far before the start of the block
    ap = os.path.join(work, "AP_PLAIN.tar")  # a plain tar (no MD5 line): the LZ4 decoder has to catch it
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        ti = tarfile.TarInfo("boot.img.lz4")
        ti.size = len(bad)
        t.addfile(ti, io.BytesIO(bytes(bad)))
    open(ap, "wb").write(buf.getvalue())
    rc, out = run("samsung-stock", ap, "--yes", "--unlocked", "--workdir", os.path.join(work, "w3"))
    check("Samsung: a damaged AP file is refused with a clear message", rc == 2 and "is damaged" in out and "Traceback" not in out, out)

    # The window opens and closes (skipped where tkinter or a display is missing).
    probe = ("import sys, tkinter as tk\n"
             "sys.path.insert(0, %r)\n"
             "try:\n    tk.Tk().destroy()\nexcept tk.TclError:\n    sys.exit(77)\n"
             "import fmrre_rescue as fr\n"
             "real = tk.Tk.mainloop\n"
             "def quick(self, n=0):\n    self.after(2500, self.destroy)\n    real(self, n)\n"
             "tk.Tk.mainloop = quick\n"
             "sys.exit(fr.main(['gui']))\n") % os.path.join(ROOT, "pc")
    try:
        import tkinter  # noqa: F401
        p = subprocess.run([sys.executable, "-c", probe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=120)
        if p.returncode == 77:
            print("skip window test (no display)")
        else:
            check("the window opens and closes", p.returncode == 0, p.stdout.decode("utf-8", "replace"))
    except ImportError:
        print("skip window test (no tkinter)")

    print()
    if FAILS:
        print("%d PC rescue unit test(s) FAILED" % len(FAILS))
        return 1
    print("All PC rescue unit tests passed (%s)" % sys.platform)
    return 0


if __name__ == "__main__":
    sys.exit(main())
