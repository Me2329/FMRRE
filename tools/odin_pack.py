#!/usr/bin/env python3
"""Samsung packaging for TWRP v2 (and a disabled vbmeta for any device).

  odin_pack.py tar   --recovery twrpv2-recovery.img [--vbmeta FILE | --vbmeta-disabled] -o twrpv2.tar.md5
  odin_pack.py vbmeta-disabled -o vbmeta_disabled.img
  odin_pack.py vbmeta-patch    stock_vbmeta.img -o vbmeta_patched.img
  odin_pack.py check  twrpv2.tar.md5

The .tar.md5 goes into Odin's AP slot (Windows). On Linux/macOS use Heimdall
(scripts/install.sh does both). A Samsung with Android 9+ needs vbmeta with
verification disabled next to a custom recovery, or it refuses to boot it.
"""
import argparse
import hashlib
import io
import os
import struct
import sys
import tarfile
import time

AVB_MAGIC = b"AVB0"


def vbmeta_disabled(size=4096):
    """Unsigned vbmeta with no descriptors and flags=3 (like `avbtool make_vbmeta_image --flags 3`)."""
    h = bytearray(256)
    h[0:4] = AVB_MAGIC
    struct.pack_into(">II", h, 4, 1, 0)          # required libavb 1.0
    # auth/aux block sizes, algorithm NONE and every offset/size stay 0
    struct.pack_into(">I", h, 120, 3)             # HASHTREE_DISABLED | VERIFICATION_DISABLED
    rel = b"avbtool 1.3.0 (twrpv2)"
    h[128:128 + len(rel)] = rel
    return bytes(h) + bytes(size - len(h))


def vbmeta_patch(data):
    if data[:4] != AVB_MAGIC:
        raise SystemExit("not a vbmeta image (no AVB0 header)")
    out = bytearray(data)
    flags = struct.unpack_from(">I", out, 120)[0] | 3
    struct.pack_into(">I", out, 120, flags)
    return bytes(out)


def boot_magic(path):
    with open(path, "rb") as f:
        return f.read(8)


def add_bytes(tar, name, data):
    ti = tarfile.TarInfo(name)
    ti.size = len(data)
    ti.mode = 0o644
    ti.uname = ti.gname = "dpi"   # what Samsung's own firmware tars use
    ti.mtime = int(time.time())
    tar.addfile(ti, io.BytesIO(data))


def make_tar(images, out):
    buf = io.BytesIO()
    # Odin only understands plain ustar (no GNU/PAX extensions).
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, data in images:
            add_bytes(t, name, data)
    body = buf.getvalue()
    md5 = hashlib.md5(body).hexdigest()
    tar_name = os.path.basename(out)
    if tar_name.endswith(".md5"):
        tar_name = tar_name[:-4]
    with open(out, "wb") as f:
        f.write(body)
        f.write(f"{md5}  {tar_name}\n".encode())
    return md5


def check(path):
    data = open(path, "rb").read()
    # A tar is a whole number of 512-byte blocks; the md5 line ("<md5>  <name>.tar\n") follows it.
    body_len = len(data) - len(data) % 512
    body, line = data[:body_len], data[body_len:].decode("ascii", "replace").strip()
    md5, _, name = line.partition("  ")
    if not line:
        print("No md5 line: this is a plain .tar (Odin accepts it, but can't verify it)")
    ok = bool(line) and hashlib.md5(body).hexdigest() == md5
    with tarfile.open(fileobj=io.BytesIO(body)) as t:
        names = [(m.name, m.size) for m in t.getmembers()]
    for n, s in names:
        print(f"  {n:24} {s:>12} bytes")
    print(("MD5 OK " if ok else "MD5 MISMATCH ") + md5)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tar", help="build an Odin AP .tar.md5")
    t.add_argument("--recovery", required=True)
    g = t.add_mutually_exclusive_group()
    g.add_argument("--vbmeta", help="stock vbmeta.img: verification is disabled in the copy that goes in the tar")
    g.add_argument("--vbmeta-disabled", action="store_true", help="add a generated disabled vbmeta (default)")
    g.add_argument("--no-vbmeta", action="store_true", help="recovery only")
    t.add_argument("-o", "--out", default="twrpv2-recovery.tar.md5")
    v = sub.add_parser("vbmeta-disabled")
    v.add_argument("-o", "--out", default="vbmeta_disabled.img")
    p = sub.add_parser("vbmeta-patch")
    p.add_argument("vbmeta")
    p.add_argument("-o", "--out", default="vbmeta_patched.img")
    c = sub.add_parser("check")
    c.add_argument("file")
    a = ap.parse_args()

    if a.cmd == "vbmeta-disabled":
        open(a.out, "wb").write(vbmeta_disabled())
        print("==>", a.out)
    elif a.cmd == "vbmeta-patch":
        open(a.out, "wb").write(vbmeta_patch(open(a.vbmeta, "rb").read()))
        print("==>", a.out)
    elif a.cmd == "check":
        sys.exit(0 if check(a.file) else 1)
    else:
        if boot_magic(a.recovery) != b"ANDROID!":
            raise SystemExit(f"{a.recovery} is not an Android boot/recovery image")
        images = [("recovery.img", open(a.recovery, "rb").read())]
        if a.vbmeta:
            images.append(("vbmeta.img", vbmeta_patch(open(a.vbmeta, "rb").read())))
        elif not a.no_vbmeta:
            images.append(("vbmeta.img", vbmeta_disabled()))
        md5 = make_tar(images, a.out)
        print("==>", a.out, f"({', '.join(n for n, _ in images)}; md5 {md5})")


if __name__ == "__main__":
    main()
