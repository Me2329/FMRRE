#!/usr/bin/env python3
"""Tests for pc/fmrre_mkrecovery.py (the Windows-friendly recovery image builder). Standard library only, so they
run on Windows too; the lz4 module, the lz4 tool and bsdtar/cpio are used for extra cross-checks when present.

Stock images are made here independently of the tool (own boot header and cpio writers), like real ones: a
Galaxy S8 recovery (v0 + Samsung DT blob + SEANDROIDENFORCE, gzip ramdisk, /sbin/recovery) inside an AP .tar.md5
with LZ4, a newer Samsung (v2 + AVB footer, legacy LZ4 ramdisk with the kernel's size_append), a v3 GKI-style
recovery with an xz ramdisk, a v4 one with a boot signature.
"""
import hashlib
import io
import lzma
import gzip
import os
import random
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "pc"))
import fmrre_mkrecovery as mk  # noqa: E402

FAILS = 0


def check(cond, what):
    global FAILS
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS += 1


def elf(machine, cls=2, interp=False):
    """A minimal ELF header (+ one program header) for the given e_machine."""
    b = bytearray(256)
    b[0:4] = b"\x7fELF"
    b[4], b[5], b[6] = cls, 1, 1
    struct.pack_into("<HH", b, 16, 2, machine)
    if cls == 2:
        struct.pack_into("<Q", b, 32, 64)
        struct.pack_into("<HH", b, 54, 56, 1)
        ph = 64
    else:
        struct.pack_into("<I", b, 28, 52)
        struct.pack_into("<HH", b, 42, 32, 1)
        ph = 52
    struct.pack_into("<I", b, ph, 3 if interp else 1)
    return bytes(b) + os.urandom(64) + b"payload" * 500


def newc(entries):
    """Independent cpio (newc) writer: (name, mode, data) tuples."""
    out = bytearray()
    for ino, (name, mode, data) in enumerate(list(entries) + [("TRAILER!!!", 0, b"")]):
        nb = name.encode() + b"\0"
        out += ("070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X" %
                (ino + 300, mode, 0, 0, 1, 1700000000, len(data), 0, 0, 0, 0, len(nb), 0)).encode() + nb
        out += b"\0" * (-len(out) % 4) + data
        out += b"\0" * (-len(out) % 4)
    return bytes(out)


def unnewc(buf):
    """Independent cpio reader -> {name: (mode, data)}; later copies win, like the kernel."""
    out, i = {}, 0
    while i + 110 <= len(buf) and buf[i:i + 6] in (b"070701", b"070702"):
        mode = int(buf[i + 14:i + 22], 16)
        size = int(buf[i + 54:i + 62], 16)
        nsize = int(buf[i + 94:i + 102], 16)
        name = buf[i + 110:i + 110 + nsize - 1].decode()
        i = (i + 110 + nsize + 3) & ~3
        data = buf[i:i + size]
        i = (i + size + 3) & ~3
        if name != "TRAILER!!!":
            out[name] = (mode, data)
    return out


def pad(n, p):
    return (n + p - 1) // p * p


def bootimg(version, kernel, ramdisk, page=2048, second=b"", dt=b"", rdtbo=b"", dtb=b"", sig=b"", seandroid=False,
            avb_total=0, sha256=False):
    """Independent boot image writer (AOSP mkbootimg layout; dt = Samsung/QCOM DT blob in a v0 header)."""
    if version >= 3:
        page = 4096
        hdr = bytearray(4096)
        hdr[0:8] = b"ANDROID!"
        struct.pack_into("<IIII", hdr, 8, len(kernel), len(ramdisk), (11 << 25) | (2026 - 2000 << 4) | 10,
                         1580 if version == 3 else 1584)
        struct.pack_into("<I", hdr, 40, version)
        hdr[44:60] = b"console=ttyMSM0 "
        if version == 4:
            struct.pack_into("<I", hdr, 1580, len(sig))
        secs = [kernel, ramdisk] + ([sig] if version == 4 else [])
    else:
        hdr = bytearray(page)
        hdr[0:8] = b"ANDROID!"
        struct.pack_into("<10I", hdr, 8, len(kernel), 0x10008000, len(ramdisk), 0x11000000, len(second), 0x10f00000,
                         0x10000100, page, len(dt) if dt else version, (9 << 25) | (2021 - 2000 << 4) | 3)
        hdr[48:59] = b"SRPQB28A001"
        hdr[64:84] = b"buildvariant=user   "
        secs = [kernel, ramdisk, second]
        if dt:
            secs.append(dt)
        if version >= 1:
            struct.pack_into("<I", hdr, 1632, len(rdtbo))
            struct.pack_into("<I", hdr, 1644, 1648 if version == 1 else 1660)
            secs.append(rdtbo)
        if version >= 2:
            struct.pack_into("<IQ", hdr, 1648, len(dtb), 0x10000000)
            secs.append(dtb)
        h = hashlib.sha256() if sha256 else hashlib.sha1()
        for s in secs:
            h.update(s)
            h.update(struct.pack("<I", len(s)))
        hdr[576:608] = h.digest().ljust(32, b"\0")
    out = bytearray(hdr)
    for k, s in enumerate(secs):
        if version in (1, 2) and s is rdtbo and rdtbo:
            struct.pack_into("<Q", out, 1636, len(out))
        out += s + b"\0" * (-len(s) % page)
    if seandroid:
        out += b"SEANDROIDENFORCE" + os.urandom(300)  # Samsung's signature block follows the marker
    if avb_total:
        vb = b"AVB0" + os.urandom(1020)
        body = len(out)
        voff = pad(body, 4096)
        out += b"\0" * (voff - body) + vb
        out += b"\0" * (avb_total - 64 - len(out))
        out += b"AVBf" + struct.pack(">IIQQQ", 1, 0, body, voff, len(vb)) + b"\0" * 28
    return bytes(out)


def parse(img):
    """Minimal independent header read: (version, page, {section: bytes}, header bytes)."""
    hv = struct.unpack_from("<I", img, 40)[0]
    if hv in (3, 4):
        page, sizes = 4096, [("kernel", 8), ("ramdisk", 12)] + ([("sig", 1580)] if hv == 4 else [])
    else:
        page = struct.unpack_from("<I", img, 36)[0]
        sizes = [("kernel", 8), ("ramdisk", 16), ("second", 24)]
        if hv > 4:
            sizes.append(("dt", 40))
        if hv in (1, 2):
            sizes.append(("rdtbo", 1632))
        if hv == 2:
            sizes.append(("dtb", 1648))
    off, secs = page, {}
    for name, o in sizes:
        n = struct.unpack_from("<I", img, o)[0]
        secs[name] = img[off:off + n]
        off += pad(n, page)
    return hv, page, secs, off


def run(args):
    """The tool's main(), output captured."""
    old = sys.stdout
    sys.stdout = io.StringIO()
    try:
        rc = mk.main(args)
        text = sys.stdout.getvalue()
    finally:
        sys.stdout = old
    return rc, text


def odin_members(path):
    data = open(path, "rb").read()
    body = data[:len(data) - len(data) % 512]
    line = data[len(body):].decode().strip()
    md5_ok = hashlib.md5(body).hexdigest() == line.split()[0]
    with tarfile.open(fileobj=io.BytesIO(body)) as t:
        return md5_ok, {m.name: t.extractfile(m).read() for m in t.getmembers()}, t.getmembers()[0].uname


def lz4_tests():
    m = mk._lz4_module()
    check(mk.xxh32(b"") == 0x02CC5D05, "xxh32 of nothing is the reference value")
    rnd = random.Random(7)
    samples = [b"", b"a", b"abcdefgh" * 3, bytes(rnd.getrandbits(8) for _ in range(5000)),
               b"FMRRE lz4 block " * 4000, b"\0" * 300000,
               bytes(rnd.choice(b"ACGT") for _ in range(70000)) + b"tail-literals"]
    ok = True
    for s in samples:
        out = bytearray()
        mk.lz4_block_decode(mk.lz4_block_encode(s), out)
        ok = ok and bytes(out) == s
    check(ok, "pure-Python LZ4 blocks round-trip (empty, tiny, random, runs, overlaps)")
    big = b"".join(struct.pack("<I", i * 2654435761 & 0xFFFFFFFF) + b"recovery " * (i % 7) for i in range(200000))
    fr = mk.lz4_frame_encode(big)
    check(mk.lz4_decode_py(fr) == big, "pure LZ4 frame (content checksum) round-trips %d bytes" % len(big))
    info = {}
    leg = mk.lz4_legacy_encode(big, True)
    check(mk.lz4_decode_py(leg, info) == big and info.get("size_appended"),
          "legacy LZ4 with the kernel's size_append is read and recognised")
    bad = bytearray(fr)
    bad[6] ^= 1
    try:
        mk.lz4_decode_py(bytes(bad))
        check(False, "damaged frame header refused")
    except mk.Fail as e:
        check("header" in str(e), "damaged frame header refused")
    if m:
        lib = m.frame.compress(big, content_checksum=True, block_linked=True)
        check(mk.lz4_decode_py(lib) == big, "frames from the official lz4 library (linked blocks) decode in Python")
        check(m.frame.decompress(_pure_frame(big)) == big, "the official library reads the pure-Python frames")
        blk = mk.lz4_block_encode(big[:LEGACY_SAMPLE])
        check(m.block.decompress(blk, uncompressed_size=LEGACY_SAMPLE) == big[:LEGACY_SAMPLE],
              "the official library reads the pure-Python blocks")
    else:
        print("skip official lz4 library cross-checks (python lz4 module not installed)")
    tool = shutil.which("lz4")
    if tool:
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.lz4")
            open(p, "wb").write(_pure_frame(big))
            r = subprocess.run([tool, "-t", "-q", p])
            check(r.returncode == 0, "the lz4 tool accepts the pure-Python frames")
    else:
        print("skip lz4 tool cross-check (not installed)")


LEGACY_SAMPLE = 1 << 20


def _pure_frame(data):
    saved = mk._lz4_module
    mk._lz4_module = lambda: None
    try:
        return mk.lz4_frame_encode(data)
    finally:
        mk._lz4_module = saved


def scenarios(d):
    arm64, arm = elf(183), elf(40, cls=1)
    fmrre = os.path.join(d, "fmrre-arm64")
    open(fmrre, "wb").write(elf(183) + b"FMRRE binary " * 3000)
    fmrre_bytes = open(fmrre, "rb").read()
    kernel = os.urandom(300000)
    dt = os.urandom(70000)

    # 1. Galaxy S8: v0 + DT blob, gzip ramdisk, /sbin/recovery, a real /etc folder, SEANDROIDENFORCE, inside an AP.
    stock_rec = arm64 + b"stock samsung recovery"
    cp = newc([("sbin", 0o40750, b""), ("sbin/recovery", 0o100750, stock_rec), ("init", 0o100750, arm64),
               ("etc", 0o40755, b""), ("etc/recovery.fstab", 0o100644, b"/system ext4 /dev/block/SYSTEM\n"),
               ("init.rc", 0o100750, b"service recovery /sbin/recovery\n"), ("system", 0o40755, b"")])
    s8 = bootimg(0, kernel, gzip.compress(cp), page=2048, dt=dt, seandroid=True)
    ap = os.path.join(d, "AP_G950FXXUCDUD1_CL1_FA90_user_low_ship.tar.md5")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, blob in (("boot.img.lz4", _pure_frame(os.urandom(5000))), ("recovery.img.lz4", _pure_frame(s8)),
                           ("system.img.ext4.lz4", _pure_frame(b"\0" * 9000))):
            ti = tarfile.TarInfo(name)
            ti.size = len(blob)
            t.addfile(ti, io.BytesIO(blob))
    body = buf.getvalue()
    open(ap, "wb").write(body + ("%s  %s\n" % (hashlib.md5(body).hexdigest(), os.path.basename(ap)[:-4])).encode())
    out = os.path.join(d, "s8", "fmrre-recovery.img")
    os.makedirs(os.path.dirname(out))
    rc, text = run([ap, "--fmrre", fmrre, "--device", "dreamlte", "-o", out])
    check(rc == 0, "S8: AP -> FMRRE image (exit %d)" % rc)
    if rc:
        print(text)
        return
    check("MD5 OK" in text and "Samsung DT blob" in text and "SEANDROIDENFORCE" in text, "S8: AP checked, header seen")
    img = open(out, "rb").read()
    hv, page, secs, end = parse(img)
    check(hv == len(dt) and page == 2048 and secs["kernel"] == kernel and secs["dt"] == dt,
          "S8: kernel and DT blob unchanged, DT size still in the header")
    check(img[48:576] == s8[48:576] and img[8:16] == s8[8:16] and img[20:48] == s8[20:48],
          "S8: addresses, name, cmdline, OS version/patch level as stock")
    h = hashlib.sha1()
    for k in ("kernel", "ramdisk", "second", "dt"):
        h.update(secs[k])
        h.update(struct.pack("<I", len(secs[k])))
    check(img[576:608] == h.digest().ljust(32, b"\0"), "S8: image ID recomputed (SHA-1 incl. the DT blob)")
    check(img[end:end + 16] == b"SEANDROIDENFORCE" and len(img) == end + 16, "S8: SEANDROIDENFORCE kept, old signature dropped")
    files = unnewc(gzip.decompress(secs["ramdisk"]))
    check(files["sbin/recovery"][1] == fmrre_bytes and files["sbin/recovery"][0] == 0o100750,
          "S8: /sbin/recovery is FMRRE (stock permissions)")
    check(files["sbin/recovery.stock"][1] == stock_rec, "S8: stock program kept as /sbin/recovery.stock")
    check(files["sbin/fmrre"] == (0o120777, b"/sbin/recovery") and files["sbin/twrp"][1] == b"/sbin/recovery",
          "S8: fmrre and twrp links")
    want_fstab = open(os.path.join(HERE, "..", "device", "dreamlte", "twrp.fstab"), "rb").read().replace(b"\r\n", b"\n")
    check(files.get("etc/twrp.fstab", (0, b""))[1] == want_fstab, "S8: device/dreamlte/twrp.fstab in /etc (a real folder here)")
    check(files["init.rc"][1] == b"service recovery /sbin/recovery\n" and files["init"][1] == arm64, "S8: rest of the ramdisk untouched")
    ok, members, uname = odin_members(os.path.join(d, "s8", "fmrre-recovery.tar.md5"))
    check(ok and list(members) == ["recovery.img"] and members["recovery.img"] == img and uname == "dpi",
          "S8: Odin package = recovery.img only (no vbmeta partition on this phone), MD5 line valid")
    ok, members, _ = odin_members(os.path.join(d, "s8", "stock-recovery.tar.md5"))
    check(ok and members.get("recovery.img") == s8, "S8: stock-recovery.tar.md5 puts the stock recovery back")
    for tool in ("bsdtar", "cpio"):
        if shutil.which(tool):
            with tempfile.TemporaryDirectory() as x:
                c = os.path.join(x, "r.cpio")
                open(c, "wb").write(gzip.decompress(secs["ramdisk"]))
                cmd = [tool, "-tvf", c] if tool == "bsdtar" else [tool, "-itv", "-F", c]
                r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                listing = r.stdout.decode("utf-8", "replace")
                check(r.returncode == 0 and "sbin/recovery.stock" in listing and "sbin/fmrre -> /sbin/recovery" in listing,
                      "S8: %s reads the new ramdisk" % tool)

    # Running it again on its own output: FMRRE replaced, the stock program not overwritten.
    out2 = os.path.join(d, "s8", "again.img")
    rc, text = run([out, "--fmrre", fmrre, "-o", out2, "--odin", "no"])
    files2 = unnewc(gzip.decompress(parse(open(out2, "rb").read())[2]["ramdisk"]))
    check(rc == 0 and files2["sbin/recovery.stock"][1] == stock_rec and files2["sbin/recovery"][1] == fmrre_bytes,
          "rebuilding an FMRRE image keeps the original stock program")

    # Damaged AP: refused before anything is written.
    bad = bytearray(open(ap, "rb").read())
    bad[700] ^= 0x10
    badp = os.path.join(d, "AP_bad.tar.md5")
    open(badp, "wb").write(bad)
    rc, _ = run([badp, "--fmrre", fmrre, "-o", os.path.join(d, "s8", "never.img")])
    check(rc == 1 and not os.path.exists(os.path.join(d, "s8", "never.img")), "damaged AP (MD5) refused")

    # Wrong CPU: a 32-bit ARM phone, an arm64 FMRRE.
    cp32 = newc([("sbin", 0o40750, b""), ("sbin/recovery", 0o100750, arm)])
    a32 = os.path.join(d, "arm32.img")
    open(a32, "wb").write(bootimg(0, kernel, gzip.compress(cp32)))
    rc, _ = run([a32, "--fmrre", fmrre, "-o", os.path.join(d, "x.img")])
    check(rc == 1, "arm64 FMRRE refused for a 32-bit phone")
    rc, _ = run([a32, "--fmrre", fmrre, "-o", os.path.join(d, "x.img"), "--force"])
    check(rc == 0, "... unless --force")

    # 2. Newer Samsung: v2 header, recovery_dtbo + dtb, legacy LZ4 ramdisk with size_append, /system/bin/recovery,
    #    /etc -> /system/etc, AVB footer in a 6 MiB partition image, SHA-256 ID.
    stock2 = arm64 + b"stock v2"
    cp2 = newc([("system", 0o40755, b""), ("system/bin", 0o40755, b""), ("system/bin/recovery", 0o100755, stock2),
                ("system/etc", 0o40755, b""), ("etc", 0o120777, b"/system/etc"), ("init", 0o100750, arm64)])
    rd2 = mk.lz4_legacy_encode(cp2, True)
    rdtbo, dtb = os.urandom(40000), os.urandom(50000)
    v2 = bootimg(2, kernel, rd2, page=4096, rdtbo=rdtbo, dtb=dtb, seandroid=True, avb_total=6 << 20, sha256=True)
    p2 = os.path.join(d, "recovery_v2.img")
    open(p2, "wb").write(v2)
    out = os.path.join(d, "v2", "fmrre-recovery.img")
    os.makedirs(os.path.dirname(out))
    rc, text = run([p2, "--fmrre", fmrre, "--fstab", os.path.join(HERE, "..", "device", "dreamlte", "twrp.fstab"),
                    "-o", out])
    check(rc == 0, "v2 + AVB: built (exit %d)" % rc)
    if rc:
        print(text)
        return
    img = open(out, "rb").read()
    hv, page, secs, end = parse(img)
    check(hv == 2 and secs["rdtbo"] == rdtbo and secs["dtb"] == dtb and secs["kernel"] == kernel,
          "v2: recovery_dtbo and dtb unchanged")
    rdo = struct.unpack_from("<Q", img, 1636)[0]
    check(img[rdo:rdo + len(rdtbo)] == rdtbo, "v2: recovery_dtbo offset in the header points at it")
    h = hashlib.sha256()
    for k in ("kernel", "ramdisk", "second", "rdtbo", "dtb"):
        h.update(secs[k])
        h.update(struct.pack("<I", len(secs[k])))
    check(img[576:608] == h.digest(), "v2: SHA-256 image ID recomputed")
    raw = secs["ramdisk"]
    check(struct.unpack_from("<I", raw, 0)[0] == mk.LZ4_LEGACY and
          struct.unpack_from("<I", raw, len(raw) - 4)[0] == len(mk.lz4_decode_py(raw)),
          "v2: ramdisk stays legacy LZ4, with the uncompressed size appended like the stock one")
    files = unnewc(mk.lz4_decode_py(raw))
    check(files["system/bin/recovery"][1] == fmrre_bytes and files["system/bin/recovery.stock"][1] == stock2 and
          files["system/bin/fmrre"][1] == b"/system/bin/recovery", "v2: FMRRE in /system/bin")
    check("system/etc/twrp.fstab" in files and "etc/twrp.fstab" not in files, "v2: fstab follows the /etc link")
    check(len(img) == 6 << 20 and img[-64:-60] == b"AVBf", "v2: same partition size, AVB footer at the end")
    _, _, orig, voff, vsize = struct.unpack_from(">IIQQQ", img, len(img) - 60)
    check(orig == end + 16 and voff % 4096 == 0 and img[voff:voff + 4] == b"AVB0" and v2[-64:] != img[-64:],
          "v2: AVB footer moved past the new contents")
    ok, members, _ = odin_members(os.path.join(d, "v2", "fmrre-recovery.tar.md5"))
    check(ok and sorted(members) == ["recovery.img", "vbmeta.img"] and
          struct.unpack_from(">I", members["vbmeta.img"], 120)[0] == 3,
          "v2: AVB phone -> Odin package carries a disabled vbmeta")
    big = os.path.join(d, "v2", "big.img")
    open(big, "wb").write(elf(183) + os.urandom(7 << 20))
    rc, text = run([p2, "--fmrre", big, "-o", os.path.join(d, "v2", "nofit.img")])
    check(rc == 1, "v2: refused when FMRRE doesn't fit the recovery partition")

    # 3. v3 (Android 11+ GKI-style recovery), xz ramdisk with CRC32, no Samsung marks -> no Odin files.
    cp3 = newc([("system", 0o40755, b""), ("system/bin", 0o40755, b""), ("system/bin/recovery", 0o100755, arm64),
                ("init", 0o100750, arm64)])
    rd3 = lzma.compress(cp3, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32)
    p3 = os.path.join(d, "recovery_v3.img")
    open(p3, "wb").write(bootimg(3, kernel, rd3))
    out = os.path.join(d, "v3", "fmrre-recovery.img")
    os.makedirs(os.path.dirname(out))
    rc, text = run([p3, "--fmrre", fmrre, "-o", out])
    img = open(out, "rb").read() if rc == 0 else b""
    hv, page, secs, end = parse(img) if img else (0, 0, {}, 0)
    rdx = secs.get("ramdisk", b"")
    check(rc == 0 and hv == 3 and secs["kernel"] == kernel and rdx[:6] == b"\xfd7zXZ\x00" and rdx[7] & 0xF == 1 and
          unnewc(lzma.decompress(rdx))["system/bin/recovery"][1] == fmrre_bytes and img[44:60] == b"console=ttyMSM0 ",
          "v3: xz (CRC32 kept for the kernel), FMRRE in place, cmdline kept")
    check(not os.path.exists(os.path.join(d, "v3", "fmrre-recovery.tar.md5")) and "fastboot boot" in text,
          "v3: not a Samsung -> no Odin package, fastboot instructions")

    # 4. v4 with a boot signature section, plain (uncompressed) cpio ramdisk.
    sig = os.urandom(4096)
    p4 = os.path.join(d, "recovery_v4.img")
    open(p4, "wb").write(bootimg(4, kernel, cp3, sig=sig))
    out = os.path.join(d, "v4.img")
    rc, _ = run([p4, "--fmrre", fmrre, "-o", out, "--odin", "no"])
    hv, page, secs, end = parse(open(out, "rb").read()) if rc == 0 else (0, 0, {}, 0)
    check(rc == 0 and hv == 4 and secs["sig"] == sig and unnewc(secs["ramdisk"])["system/bin/recovery"][1] == fmrre_bytes,
          "v4: signature section kept, uncompressed ramdisk patched")

    # 5. TWRP already on the phone, dumped with dd: LZMA ramdisk, /etc/twrp.fstab, zeros to the partition's end.
    twrp_bin = elf(183) + b"TWRP 3.7.0"
    cp5 = newc([("sbin", 0o40755, b""), ("sbin/recovery", 0o100755, twrp_bin), ("sbin/twrp", 0o100755, b"#!/sbin/sh\n"),
                ("etc", 0o40755, b""), ("etc/twrp.fstab", 0o100644, b"/data ext4 /dev/block/USERDATA\n"), ("init", 0o100750, arm64)])
    t5 = bootimg(0, kernel, lzma.compress(cp5, format=lzma.FORMAT_ALONE), page=2048, dt=dt, seandroid=True)
    p5 = os.path.join(d, "twrp_dump.img")
    open(p5, "wb").write(t5 + b"\0" * ((8 << 20) - len(t5)))
    out = os.path.join(d, "t5", "fmrre-recovery.img")
    os.makedirs(os.path.dirname(out))
    rc, text = run([p5, "--fmrre", fmrre, "-o", out])
    files = unnewc(lzma.decompress(parse(open(out, "rb").read())[2]["ramdisk"])) if rc == 0 else {}
    check(rc == 0 and "This is TWRP" in text and files.get("sbin/recovery", (0, b""))[1] == fmrre_bytes and
          files.get("etc/twrp.fstab", (0, b""))[1] == b"/data ext4 /dev/block/USERDATA\n" and files["sbin/recovery.stock"][1] == twrp_bin,
          "TWRP base: recognised, FMRRE in its place, TWRP's fstab kept (LZMA ramdisk)")
    ok, members, _ = odin_members(os.path.join(d, "t5", "twrp-recovery.tar.md5")) if rc == 0 else (False, {}, "")
    back = members.get("recovery.img", b"")
    check(ok and len(back) < (1 << 20) and t5.startswith(back.rstrip(b"\0")) and back.startswith(t5),
          "TWRP base: twrp-recovery.tar.md5 puts TWRP back (the dd padding dropped, the image whole)")

    # Not boot images.
    junk = os.path.join(d, "junk.img")
    open(junk, "wb").write(b"\0" * 4096)
    check(run([junk, "--fmrre", fmrre])[0] == 1, "junk refused")
    vb = os.path.join(d, "vendor_boot.img")
    open(vb, "wb").write(b"VNDRBOOT" + b"\0" * 4096)
    rc, _ = run([vb, "--fmrre", fmrre])
    check(rc == 1, "vendor_boot pointed to make_recovery_img.sh")


def main():
    lz4_tests()
    for pure in (False, True):
        if pure:
            # Again with no lz4 module and no lz4 tool: everything in pure Python (what a bare Windows PC has).
            mk._lz4_module = lambda: None
            mk.shutil.which = lambda name: None
            print("-- pure Python (no lz4 module, no lz4 tool) --")
        with tempfile.TemporaryDirectory() as d:
            scenarios(d)
    print("FAILED: %d" % FAILS if FAILS else "All mkrecovery tests passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
