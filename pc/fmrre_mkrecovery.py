#!/usr/bin/env python3
"""Build an FMRRE recovery image from your phone's stock recovery - on Windows, Linux or macOS.

Only Python 3.8+ is needed: no magiskboot, no WSL. The lz4 tool or the python lz4 module make big Samsung images
faster when they are installed; neither is required.

  python pc/fmrre_mkrecovery.py STOCK [--device NAME | --fstab FILE] [--fmrre BINARY] [-o fmrre-recovery.img]

STOCK is your phone's own stock recovery: recovery.img, recovery.img.lz4, or a whole Samsung AP_....tar.md5 (the
recovery image is taken out of it once its MD5 is checked). FMRRE replaces the stock recovery program inside the
stock ramdisk; the phone's own kernel, device tree, boot header and security patch level stay as they were.

Writes fmrre-recovery.img (for fastboot), and for a Samsung also:
  fmrre-recovery.tar.md5   Odin, AP slot: installs FMRRE
  stock-recovery.tar.md5   Odin, AP slot: puts the stock recovery back
A disabled vbmeta.img goes into the Odin package only when the stock recovery carries an AVB footer (phones that
launched with Android 9 or later). Older Samsungs such as the Galaxy S8 have no vbmeta partition: Odin would fail.

Handles boot image header v0-v4, Samsung's extra DT blob, the SEANDROIDENFORCE tail and AVB footers, and ramdisks
compressed with gzip, LZ4 (legacy or frame), xz, lzma, bzip2 or not at all. Not vendor_boot images: use
scripts/make_recovery_img.sh (magiskboot) for those.
"""
import argparse
import bz2
import gzip
import hashlib
import io
import lzma
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ACTIONS = "https://github.com/Me2329/FMRRE/actions"


class Fail(Exception):
    """A problem the user can act on: printed without a traceback."""


def say(msg=""):
    print(msg, flush=True)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def mb(n):
    return "%.1f MB" % (n / 1048576.0)


def pad(n, page):
    return (n + page - 1) // page * page


# ---- LZ4 -----------------------------------------------------------------------------------------------------------
# Samsung packs firmware images as LZ4 frames; kernels unpack LZ4 ramdisks in the "legacy" format. Pure-Python
# coders below; the python lz4 module or the lz4 tool are used instead when present (much faster on big images).

LZ4_FRAME = 0x184D2204
LZ4_LEGACY = 0x184C2102
LEGACY_BLOCK = 8 << 20  # the kernel's legacy block size
_P1, _P2, _P3, _P4, _P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
_M = 0xFFFFFFFF


def xxh32(data, seed=0):
    """xxHash32 - the LZ4 frame checksums."""
    data = memoryview(data)
    n, i = len(data), 0
    if n >= 16:
        v1, v2, v3, v4 = (seed + _P1 + _P2) & _M, (seed + _P2) & _M, seed & _M, (seed - _P1) & _M
        for a, b, c, d in struct.iter_unpack("<4I", data[: n - n % 16]):
            v1 = (v1 + a * _P2) & _M
            v1 = ((v1 << 13) | (v1 >> 19)) * _P1 & _M
            v2 = (v2 + b * _P2) & _M
            v2 = ((v2 << 13) | (v2 >> 19)) * _P1 & _M
            v3 = (v3 + c * _P2) & _M
            v3 = ((v3 << 13) | (v3 >> 19)) * _P1 & _M
            v4 = (v4 + d * _P2) & _M
            v4 = ((v4 << 13) | (v4 >> 19)) * _P1 & _M
        i = n - n % 16
        h = (((v1 << 1) | (v1 >> 31)) + ((v2 << 7) | (v2 >> 25)) + ((v3 << 12) | (v3 >> 20)) +
             ((v4 << 18) | (v4 >> 14))) & _M
    else:
        h = (seed + _P5) & _M
    h = (h + n) & _M
    while i + 4 <= n:
        h = (h + u32(data, i) * _P3) & _M
        h = ((h << 17) | (h >> 15)) * _P4 & _M
        i += 4
    while i < n:
        h = (h + data[i] * _P5) & _M
        h = ((h << 11) | (h >> 21)) * _P1 & _M
        i += 1
    h ^= h >> 15
    h = h * _P2 & _M
    h ^= h >> 13
    h = h * _P3 & _M
    return h ^ (h >> 16)


def _lz4_module():
    try:
        import lz4.block
        import lz4.frame
        return lz4
    except ImportError:
        return None


def lz4_block_decode(src, out):
    """Decode one LZ4 block onto the end of `out` (a bytearray; what is already in it is the match history)."""
    i, n = 0, len(src)
    if n == 0:
        return
    try:
        while True:
            tok = src[i]
            i += 1
            lit = tok >> 4
            if lit == 15:
                b = 255
                while b == 255:
                    b = src[i]
                    i += 1
                    lit += b
            if lit:
                if i + lit > n:
                    raise Fail("lz4: the literals run past the end of the block")
                out += src[i:i + lit]
                i += lit
            if i >= n:
                return
            off = src[i] | src[i + 1] << 8
            i += 2
            ml = tok & 15
            if ml == 15:
                b = 255
                while b == 255:
                    b = src[i]
                    i += 1
                    ml += b
            ml += 4
            start = len(out) - off
            if off == 0 or start < 0:
                raise Fail("lz4: a match points before the start of the data")
            if ml <= off:
                out += out[start:start + ml]
            else:  # overlapping copy: the last `off` bytes repeat
                pat = out[start:]
                q, r = divmod(ml, off)
                out += pat * q + pat[:r]
    except IndexError:
        raise Fail("lz4: the block is cut short")


def _lz4_run(out, n):
    if n >= 15:
        q, r = divmod(n - 15, 255)
        out += b"\xff" * q
        out.append(r)


def lz4_block_encode(src):
    """A greedy LZ4 block compressor. Follows the format's end rules (last match starts 12+ bytes before the end,
    last 5 bytes are literals), so the kernel's decoder accepts it."""
    src = bytes(src)
    n = len(src)
    out = bytearray()
    anchor = i = 0
    limit, match_end = n - 12, n - 5
    table = {}
    miss = 0
    while i < limit:
        key = src[i:i + 4]
        ref = table.get(key)
        table[key] = i
        if ref is None or i - ref > 65535:
            miss += 1
            i += 1 + (miss >> 6)  # move faster through data that doesn't compress
            continue
        miss = 0
        ml, maxml = 4, match_end - i
        while ml + 64 <= maxml and src[ref + ml:ref + ml + 64] == src[i + ml:i + ml + 64]:
            ml += 64
        while ml < maxml and src[ref + ml] == src[i + ml]:
            ml += 1
        lit = i - anchor
        out.append((min(lit, 15) << 4) | min(ml - 4, 15))
        _lz4_run(out, lit)
        out += src[anchor:i]
        out += struct.pack("<H", i - ref)
        _lz4_run(out, ml - 4)
        i += ml
        anchor = i
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    _lz4_run(out, lit)
    out += src[anchor:]
    return bytes(out)


def _block_encode(chunk):
    m = _lz4_module()
    if m:
        return m.block.compress(chunk, mode="high_compression", compression=9, store_size=False)
    return lz4_block_encode(chunk)


def _legacy_decode(data, pos, out, info):
    n = len(data)
    m = _lz4_module()
    while pos + 4 <= n:
        size = u32(data, pos)
        if size == LZ4_LEGACY:  # a second legacy stream follows
            pos += 4
            continue
        if size == LZ4_FRAME or 0x184D2A50 <= size <= 0x184D2A5F:
            return pos
        if pos + 4 + size > n:
            if n - pos == 4:  # the kernel build's "size_append": the uncompressed size ends the stream
                info["size_appended"] = True
                return n
            raise Fail("lz4: a legacy block runs past the end of the data")
        blk = data[pos + 4:pos + 4 + size]
        pos += 4 + size
        if m:
            try:
                out += m.block.decompress(bytes(blk), uncompressed_size=LEGACY_BLOCK)
            except Exception as e:  # lz4.block.LZ4BlockError
                raise Fail("lz4: %s" % e)
        else:
            lz4_block_decode(blk, out)
    return pos


def _frame_decode(data, pos, out):
    flg = data[pos + 4]
    if flg >> 6 != 1:
        raise Fail("lz4: unknown frame version")
    if flg & 1:
        raise Fail("lz4: frames with a dictionary are not supported")
    p = pos + 6 + (8 if flg & 8 else 0)
    if (xxh32(data[pos + 4:p]) >> 8) & 0xFF != data[p]:
        raise Fail("lz4: damaged frame header (checksum)")
    p += 1
    first = len(out)
    while True:
        size = u32(data, p)
        p += 4
        if size == 0:
            break
        raw, size = size >> 31, size & 0x7FFFFFFF
        blk = data[p:p + size]
        if len(blk) != size:
            raise Fail("lz4: the file is cut short")
        p += size + (4 if flg & 0x10 else 0)  # a block checksum is not checked: the content checksum covers it
        if raw:
            out += blk
        else:
            lz4_block_decode(blk, out)
    if flg & 4:
        if xxh32(memoryview(out)[first:]) != u32(data, p):
            raise Fail("lz4: the content checksum doesn't match - the file is damaged")
        p += 4
    return p


def lz4_decode_py(data, info=None):
    """LZ4 frame(s) and/or legacy streams -> bytes, in pure Python."""
    info = {} if info is None else info
    data = memoryview(data)
    out = bytearray()
    pos, n = 0, len(data)
    try:
        while pos + 4 <= n:
            magic = u32(data, pos)
            if magic == LZ4_LEGACY:
                info["format"] = info.get("format", "legacy")
                pos = _legacy_decode(data, pos + 4, out, info)
            elif magic == LZ4_FRAME:
                info["format"] = info.get("format", "frame")
                pos = _frame_decode(data, pos, out)
            elif 0x184D2A50 <= magic <= 0x184D2A5F:  # skippable frame
                pos += 8 + u32(data, pos + 4)
            elif not bytes(data[pos:]).strip(b"\0"):
                break  # zero padding
            else:
                raise Fail("lz4: unexpected data at byte %d" % pos)
    except (IndexError, struct.error):
        raise Fail("lz4: the data is cut short")
    return bytes(out)


def lz4_decode(data, what):
    """Decompress a (possibly big) LZ4 file: the python lz4 module, else the lz4 tool, else pure Python."""
    m = _lz4_module()
    if m and u32(data, 0) == LZ4_FRAME:
        try:
            return m.frame.decompress(data)
        except Exception as e:
            raise Fail("%s: %s" % (what, e))
    tool = shutil.which("lz4")
    if tool and len(data) > (2 << 20):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "in.lz4"), os.path.join(d, "out.bin")
            with open(src, "wb") as f:
                f.write(data)
            r = subprocess.run([tool, "-d", "-f", "-q", src, dst], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if r.returncode != 0:
                raise Fail("%s: the lz4 tool failed: %s" % (what, r.stdout.decode("utf-8", "replace").strip()))
            with open(dst, "rb") as f:
                return f.read()
    if len(data) > (4 << 20):
        say("  (no lz4 tool or python lz4 module: decoding in Python, this takes a little while)")
    try:
        return lz4_decode_py(data)
    except Fail as e:
        raise Fail("%s: %s" % (what, e))


def lz4_legacy_encode(data, size_appended):
    out = bytearray(struct.pack("<I", LZ4_LEGACY))
    for off in range(0, len(data), LEGACY_BLOCK):
        blk = _block_encode(data[off:off + LEGACY_BLOCK])
        out += struct.pack("<I", len(blk)) + blk
    if size_appended:
        out += struct.pack("<I", len(data) & _M)
    return bytes(out)


def lz4_frame_encode(data):
    m = _lz4_module()
    if m:
        return m.frame.compress(data, block_size=m.frame.BLOCKSIZE_MAX4MB, content_checksum=True,
                                compression_level=9)
    desc = bytes([0x64, 0x70])  # version 1, independent blocks, content checksum; 4 MB blocks
    out = bytearray(struct.pack("<I", LZ4_FRAME) + desc + bytes([(xxh32(desc) >> 8) & 0xFF]))
    for off in range(0, len(data), 4 << 20):
        chunk = data[off:off + (4 << 20)]
        blk = lz4_block_encode(chunk)
        if len(blk) >= len(chunk):
            out += struct.pack("<I", len(chunk) | 0x80000000) + chunk
        else:
            out += struct.pack("<I", len(blk)) + blk
    out += struct.pack("<II", 0, xxh32(data))
    return bytes(out)


# ---- ramdisk compression -------------------------------------------------------------------------------------------

def sniff(b):
    if b[:2] == b"\x1f\x8b":
        return "gzip"
    if len(b) >= 4 and u32(b, 0) == LZ4_LEGACY:
        return "lz4_legacy"
    if len(b) >= 4 and u32(b, 0) == LZ4_FRAME:
        return "lz4"
    if b[:6] == b"\xfd7zXZ\x00":
        return "xz"
    if b[:3] == b"\x5d\x00\x00":
        return "lzma"
    if b[:3] == b"BZh":
        return "bzip2"
    if b[:6] in (b"070701", b"070702"):
        return "cpio"
    return None


def ramdisk_unpack(rd):
    """-> (cpio bytes, how): `how` lets ramdisk_pack() compress the new one the same way."""
    kind = sniff(rd)
    how = {"kind": kind}
    try:
        if kind == "gzip":
            return gzip.decompress(rd), how
        if kind in ("lz4_legacy", "lz4"):
            info = {}
            out = lz4_decode_py(rd, info)
            how["size_appended"] = info.get("size_appended", False)
            return out, how
        if kind == "xz":
            how["check"] = rd[7] & 0x0F  # the stream's check type (the kernel wants CRC32)
            return lzma.decompress(rd, format=lzma.FORMAT_XZ), how
        if kind == "lzma":
            return lzma.decompress(rd, format=lzma.FORMAT_ALONE), how
        if kind == "bzip2":
            return bz2.decompress(rd), how
        if kind == "cpio":
            return bytes(rd), how
    except (OSError, EOFError, lzma.LZMAError, ValueError) as e:
        raise Fail("the stock ramdisk (%s) is damaged: %s" % (kind, e))
    raise Fail("the ramdisk is packed in a way this tool doesn't know (it starts with %s) - use "
               "scripts/make_recovery_img.sh (magiskboot)" % bytes(rd[:4]).hex())


def ramdisk_pack(cpio, how):
    kind = how["kind"]
    if kind == "gzip":
        return gzip.compress(cpio, compresslevel=9, mtime=0)
    if kind == "lz4_legacy":
        return lz4_legacy_encode(cpio, how.get("size_appended", False))
    if kind == "lz4":
        return lz4_frame_encode(cpio)
    if kind == "xz":
        check = {1: lzma.CHECK_CRC32, 4: lzma.CHECK_CRC64, 10: lzma.CHECK_SHA256, 0: lzma.CHECK_NONE}.get(
            how.get("check"), lzma.CHECK_CRC32)
        return lzma.compress(cpio, format=lzma.FORMAT_XZ, check=check,
                             filters=[{"id": lzma.FILTER_LZMA2, "preset": 9, "dict_size": 32 << 20}])
    if kind == "lzma":
        return lzma.compress(cpio, format=lzma.FORMAT_ALONE, preset=9)
    if kind == "bzip2":
        return bz2.compress(cpio, 9)
    return cpio


# ---- cpio (newc) ---------------------------------------------------------------------------------------------------

S_IFMT, S_IFDIR, S_IFREG, S_IFLNK = 0o170000, 0o040000, 0o100000, 0o120000


class Entry:
    __slots__ = ("name", "mode", "uid", "gid", "nlink", "mtime", "data", "ino", "dev", "rdev")

    def __init__(self, name, mode, data=b"", ino=0, uid=0, gid=0, nlink=1, mtime=0, dev=(0, 0), rdev=(0, 0)):
        self.name, self.mode, self.data, self.ino = name, mode, data, ino
        self.uid, self.gid, self.nlink, self.mtime, self.dev, self.rdev = uid, gid, nlink, mtime, dev, rdev

    def kind(self):
        return self.mode & S_IFMT


def norm(name):
    while name.startswith("./"):
        name = name[2:]
    return name.strip("/")


def cpio_read(buf):
    entries = []
    i, n = 0, len(buf)
    while i + 110 <= n:
        magic = buf[i:i + 6]
        if magic not in (b"070701", b"070702"):
            if not buf[i:].strip(b"\0"):
                break  # padding after the last archive
            raise Fail("the ramdisk is not a cpio archive (bad header at byte %d)" % i)
        try:
            f = [int(buf[i + 6 + 8 * k:i + 14 + 8 * k], 16) for k in range(13)]
        except ValueError:
            raise Fail("the ramdisk's cpio archive is damaged (byte %d)" % i)
        ino, mode, uid, gid, nlink, mtime, size, dmaj, dmin, rmaj, rmin, nsize, _chk = f
        name = buf[i + 110:i + 110 + nsize - 1].decode("utf-8", "surrogateescape")
        i = pad(i + 110 + nsize, 4)
        data = bytes(buf[i:i + size])
        if len(data) != size:
            raise Fail("the ramdisk's cpio archive is cut short (%s)" % name)
        i = pad(i + size, 4)
        if name == "TRAILER!!!":
            continue  # archives can be concatenated: keep reading
        entries.append(Entry(name, mode, data, ino, uid, gid, nlink, mtime, (dmaj, dmin), (rmaj, rmin)))
    return entries


def cpio_write(entries):
    out = bytearray()
    for e in entries + [Entry("TRAILER!!!", 0)]:
        name = e.name.encode("utf-8", "surrogateescape") + b"\0"
        fields = (e.ino, e.mode, e.uid, e.gid, e.nlink, e.mtime, len(e.data), e.dev[0], e.dev[1], e.rdev[0],
                  e.rdev[1], len(name), 0)
        out += b"070701" + b"".join(b"%08x" % v for v in fields) + name
        out += b"\0" * (-len(out) % 4)
        out += e.data
        out += b"\0" * (-len(out) % 4)
    return bytes(out)


class Ramdisk:
    def __init__(self, entries):
        self.entries = entries
        self.next_ino = max([e.ino for e in entries] + [0]) + 1

    def get(self, name):
        name = norm(name)
        for e in reversed(self.entries):  # in concatenated archives the last copy wins, as in the kernel
            if norm(e.name) == name:
                return e
        return None

    def remove(self, name):
        name = norm(name)
        self.entries = [e for e in self.entries if norm(e.name) != name]

    def mkdirs(self, path):
        parts = norm(path).split("/")
        for k in range(1, len(parts) + 1):
            d = "/".join(parts[:k])
            e = self.get(d)
            if e is None:
                self.add(d, S_IFDIR | 0o755, nlink=2)
            elif e.kind() != S_IFDIR:
                raise Fail("can't create %s in the ramdisk: %s is not a folder there" % (path, d))

    def add(self, name, mode, data=b"", nlink=1):
        name = norm(name)
        if "/" in name:
            self.mkdirs(name.rsplit("/", 1)[0])
        self.remove(name)
        self.entries.append(Entry(name, mode, data, self.next_ino, nlink=nlink))
        self.next_ino += 1


# ---- boot image ----------------------------------------------------------------------------------------------------

class BootImage:
    """An Android boot/recovery image (header v0-v4) cut into its sections, plus what follows them."""

    def __init__(self, data):
        if data[:8] != b"ANDROID!":
            if data[:8] == b"VNDRBOOT":
                raise Fail("this is a vendor_boot image: build it with scripts/make_recovery_img.sh (magiskboot)")
            raise Fail("not an Android boot/recovery image (no ANDROID! header)")
        self.data = data
        hv = u32(data, 40)
        self.dt_blob = False
        if hv in (3, 4):
            self.version, self.page, self.header_len = hv, 4096, 4096
            names, sizes = ["kernel", "ramdisk"], [u32(data, 8), u32(data, 12)]
            if hv == 4:
                names.append("signature")
                sizes.append(u32(data, 1580))
        else:
            self.page = u32(data, 36)
            if self.page not in (2048, 4096, 8192, 16384):
                raise Fail("this boot image has an unusual page size (%d) - use scripts/make_recovery_img.sh" % self.page)
            self.header_len = self.page
            names, sizes = ["kernel", "ramdisk", "second"], [u32(data, 8), u32(data, 16), u32(data, 24)]
            if hv > 4:  # Samsung/Qualcomm images keep the size of an extra device tree blob in this field
                self.version, self.dt_blob = 0, True
                names.append("dt")
                sizes.append(hv)
            else:
                self.version = hv
                if hv >= 1:
                    names.append("recovery_dtbo")
                    sizes.append(u32(data, 1632))
                if hv >= 2:
                    names.append("dtb")
                    sizes.append(u32(data, 1648))
        self.order = names
        self.sections = {}
        off = self.header_len
        for name, size in zip(names, sizes):
            if off + size > len(data):
                raise Fail("the image is cut short (its %s needs %d bytes at %d, the file has %d)" %
                           (name, size, off, len(data)))
            self.sections[name] = data[off:off + size]
            off += pad(size, self.page)
        self.end = off
        self.seandroid = data[off:off + 16] == b"SEANDROIDENFORCE"
        self.avb = None
        if len(data) >= 64 and data[-64:-60] == b"AVBf":
            major, minor, orig, voff, vsize = struct.unpack_from(">IIQQQ", data, len(data) - 60)
            if orig <= voff and voff + vsize <= len(data) - 64:
                self.avb = {"major": major, "minor": minor, "vbmeta": data[voff:voff + vsize], "total": len(data)}

    def image_id(self, sections, sha256):
        h = hashlib.sha256() if sha256 else hashlib.sha1()
        for name in self.order:
            blob = sections[name]
            h.update(blob)
            h.update(struct.pack("<I", len(blob)))
        return h.digest().ljust(32, b"\0")

    def id_matches_stock(self):
        if self.version >= 3:
            return None
        stock = bytes(self.data[576:608])
        return stock == self.image_id(self.sections, any(stock[20:]))

    def rebuild(self, ramdisk):
        secs = dict(self.sections)
        secs["ramdisk"] = ramdisk
        hdr = bytearray(self.data[:self.header_len])
        out = bytearray(self.header_len)
        offsets = {}
        for name in self.order:
            offsets[name] = len(out)
            out += secs[name]
            out += b"\0" * (-len(out) % self.page)
        if self.version >= 3:
            struct.pack_into("<I", hdr, 12, len(ramdisk))
        else:
            struct.pack_into("<I", hdr, 16, len(ramdisk))
            if self.version >= 1 and secs["recovery_dtbo"]:
                struct.pack_into("<Q", hdr, 1636, offsets["recovery_dtbo"])
            stock_id = bytes(self.data[576:608])
            hdr[576:608] = self.image_id(secs, any(stock_id[20:]))
        out[:self.header_len] = hdr
        if self.seandroid:
            out += b"SEANDROIDENFORCE"
        if self.avb:  # keep the AVB footer (moved after the new contents), like magiskboot
            body = len(out)
            voff = pad(body, 4096)
            vb = self.avb["vbmeta"]
            total = self.avb["total"]
            if voff + len(vb) + 64 > total:
                raise Fail("FMRRE doesn't fit: the image would need %s but the recovery partition holds %s" %
                           (mb(voff + len(vb) + 64), mb(total)))
            out += b"\0" * (voff - body) + vb
            out += b"\0" * (total - 64 - len(out))
            out += b"AVBf" + struct.pack(">IIQQQ", self.avb["major"], self.avb["minor"], body, voff, len(vb))
            out += b"\0" * 28
        return bytes(out)

    def describe(self):
        bits = ["header v%d" % self.version, "page %d" % self.page]
        if self.dt_blob:
            bits.append("Samsung DT blob")
        if self.seandroid:
            bits.append("SEANDROIDENFORCE")
        if self.avb:
            bits.append("AVB footer")
        return ", ".join(bits)


# ---- CPU architecture ----------------------------------------------------------------------------------------------

EM_ARCH = {183: "arm64", 40: "arm", 62: "x86_64", 3: "x86", 243: "riscv64", 8: "mips"}


def elf_arch(blob):
    """-> (arch, static) for an ELF binary, (None, None) for anything else."""
    if len(blob) < 64 or blob[:4] != b"\x7fELF":
        return None, None
    fmt = "<" if blob[5] == 1 else ">"
    machine = struct.unpack_from(fmt + "H", blob, 18)[0]
    if blob[4] == 2:
        phoff = struct.unpack_from(fmt + "Q", blob, 32)[0]
        phentsize, phnum = struct.unpack_from(fmt + "HH", blob, 54)
    else:
        phoff = struct.unpack_from(fmt + "I", blob, 28)[0]
        phentsize, phnum = struct.unpack_from(fmt + "HH", blob, 42)
    static = True
    for k in range(phnum):
        o = phoff + k * phentsize
        if o + 4 <= len(blob) and struct.unpack_from(fmt + "I", blob, o)[0] == 3:  # PT_INTERP: needs a loader
            static = False
    arch = EM_ARCH.get(machine, "machine-%d" % machine)
    if arch == "riscv64" and blob[4] != 2:
        arch = "riscv32"
    return arch, static


def ramdisk_arch(rd):
    for c in ("system/bin/recovery", "sbin/recovery", "system/bin/init", "init", "system/bin/linker64",
              "system/bin/linker", "sbin/adbd"):
        e = rd.get(c)
        if e is not None and e.kind() == S_IFREG:
            arch, _ = elf_arch(e.data)
            if arch:
                return arch, c
    return None, None


def find_fmrre(arch):
    cands = [os.path.join(ROOT, "out", arch, "recovery"), os.path.join(os.getcwd(), "out", arch, "recovery"),
             os.path.join(os.getcwd(), "fmrre-" + arch, "recovery"), os.path.join(ROOT, "fmrre-" + arch, "recovery")]
    for c in cands:
        if os.path.isfile(c):
            return c
    raise Fail("no FMRRE binary for %s. Download the 'fmrre-installer-kit' artifact of the latest green run at\n"
               "  %s\n"
               "unzip it and run this tool from inside it (it holds out/%s/recovery), or pass --fmrre path/to/recovery"
               % (arch, ACTIONS, arch))


# ---- the FMRRE ramdisk ---------------------------------------------------------------------------------------------

def install_fmrre(rd, binary, fstab):
    """Put FMRRE in place of the stock recovery program. Returns the path it went to."""
    target = None
    for c in ("system/bin/recovery", "sbin/recovery"):
        e = rd.get(c)
        if e is not None and e.kind() == S_IFREG:
            target = c
            break
    notes = []
    if target is None:
        target = "system/bin/recovery"
        notes.append("no stock recovery program in the ramdisk: FMRRE added as /%s" % target)
        mode = S_IFREG | 0o755
    else:
        stock = rd.get(target)
        mode = S_IFREG | (stock.mode & 0o7777 or 0o755) | 0o500
        if rd.get(target + ".stock") is None:
            rd.add(target + ".stock", stock.mode, stock.data)
        else:
            notes.append("/%s.stock is already there (an FMRRE image?): only the FMRRE binary is replaced" % target)
    rd.add(target, mode, binary)
    folder = target.rsplit("/", 1)[0]
    for n in ("fmrre", "twrp"):  # `adb shell fmrre ...`, and classic TWRP scripts
        rd.add(folder + "/" + n, S_IFLNK | 0o777, ("/" + target).encode())
    if fstab is not None:
        # FMRRE reads /etc/twrp.fstab or /system/etc/twrp.fstab: follow /etc if it is a link (newer ramdisks).
        etc = rd.get("etc")
        if etc is not None and etc.kind() == S_IFLNK:
            where = norm(etc.data.decode("utf-8", "replace")) + "/twrp.fstab"
        elif etc is not None and etc.kind() == S_IFDIR:
            where = "etc/twrp.fstab"
        else:
            where = "system/etc/twrp.fstab"
        rd.add(where, S_IFREG | 0o644, fstab)
        notes.append("fstab: /%s" % where)
    return target, notes


# ---- inputs and outputs --------------------------------------------------------------------------------------------

def read_ap(path, check_md5=True):
    """A Samsung AP .tar(.md5): checks the MD5 line, returns (entry name, recovery image bytes)."""
    size = os.path.getsize(path)
    tail = size % 512  # a tar is whole 512-byte blocks; the "<md5>  <name>.tar" line comes after it
    with open(path, "rb") as f:
        if tail and check_md5:
            f.seek(size - tail)
            want = f.read().split(b" ")[0].strip().decode("ascii", "replace").lower()
            say("Checking the AP's MD5 (%s) ..." % mb(size))
            h = hashlib.md5()
            f.seek(0)
            left = size - tail
            while left:
                chunk = f.read(min(left, 8 << 20))
                if not chunk:
                    break
                h.update(chunk)
                left -= len(chunk)
            if h.hexdigest() != want:
                raise Fail("the AP's MD5 doesn't match: the download is damaged. Download the firmware again.")
            say("  MD5 OK")
        f.seek(0)
        try:
            with tarfile.open(fileobj=f, mode="r:") as t:
                members = [m for m in t if m.isfile()]
                for want in ("recovery.img.lz4", "recovery.img"):
                    for m in members:
                        if m.name == want:
                            return m.name, t.extractfile(m).read()
        except tarfile.TarError as e:
            raise Fail("%s is not a readable tar: %s" % (path, e))
    raise Fail("there is no recovery image in this AP (it holds: %s)" % ", ".join(m.name for m in members[:12]))


def vbmeta_disabled(size=4096):
    """Unsigned vbmeta, no descriptors, flags=3 (same bytes as tools/odin_pack.py vbmeta-disabled)."""
    h = bytearray(size)
    h[0:4] = b"AVB0"
    struct.pack_into(">II", h, 4, 1, 0)
    struct.pack_into(">I", h, 120, 3)
    rel = b"avbtool 1.3.0 (fmrre)"
    h[128:128 + len(rel)] = rel
    return bytes(h)


def odin_tar(path, files):
    """Odin's AP format: a plain ustar, then '<md5>  <name>.tar' on a line of its own."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, data in files:
            ti = tarfile.TarInfo(name)
            ti.size, ti.mode, ti.mtime = len(data), 0o644, int(time.time())
            ti.uname = ti.gname = "dpi"  # what Samsung's own firmware tars use
            t.addfile(ti, io.BytesIO(data))
    body = buf.getvalue()
    name = os.path.basename(path)
    name = name[:-4] if name.endswith(".md5") else name
    with open(path, "wb") as f:
        f.write(body)
        f.write(("%s  %s\n" % (hashlib.md5(body).hexdigest(), name)).encode())


def build(a):
    if not os.path.isfile(a.stock):
        raise Fail("%s: no such file" % a.stock)
    with open(a.stock, "rb") as f:
        head = f.read(512)
    from_ap = len(head) >= 262 and head[257:262] == b"ustar"
    if from_ap:
        name, raw = read_ap(a.stock, not a.no_md5)
        say("Samsung AP: using %s" % name)
    else:
        with open(a.stock, "rb") as f:
            raw = f.read()
    if len(raw) >= 4 and u32(raw, 0) in (LZ4_FRAME, LZ4_LEGACY):
        say("Decompressing LZ4 (%s) ..." % mb(len(raw)))
        raw = lz4_decode(raw, "recovery image")

    img = BootImage(raw)
    say("Base image: %s (%s)" % (mb(len(raw)), img.describe()))
    if img.id_matches_stock() is False:
        say("  note: its ID field doesn't match its contents (made by another tool) - a correct one is written")
    rd_bytes = img.sections["ramdisk"]
    if not rd_bytes:
        raise Fail("this image has no ramdisk: on this phone the recovery lives in vendor_boot - use "
                   "scripts/make_recovery_img.sh (magiskboot)")
    cpio, how = ramdisk_unpack(rd_bytes)
    rd = Ramdisk(cpio_read(cpio))
    say("Ramdisk: %s, %d entries, %s unpacked" % (how["kind"], len(rd.entries), mb(len(cpio))))
    # What it is now: TWRP (its fstab and shell stay and FMRRE uses them), an earlier FMRRE build, or stock.
    base = "stock"
    if rd.get("system/bin/recovery.stock") or rd.get("sbin/recovery.stock"):
        base = "fmrre"
    elif rd.get("etc/twrp.fstab") or rd.get("system/etc/twrp.fstab") or rd.get("twres") or rd.get("sbin/twrp"):
        base = "twrp"
    say({"twrp": "This is TWRP: FMRRE takes its place and uses TWRP's fstab and shell.",
         "fmrre": "This is already an FMRRE image: only the FMRRE program is replaced.",
         "stock": "This is a stock recovery."}[base])

    phone_arch, seen_in = ramdisk_arch(rd)
    if phone_arch:
        say("Phone CPU: %s (from /%s)" % (phone_arch, seen_in))
    fmrre = a.fmrre or find_fmrre(phone_arch or "arm64")
    with open(fmrre, "rb") as f:
        binary = f.read()
    arch, static = elf_arch(binary)
    if arch is None:
        raise Fail("%s is not an FMRRE binary (not an ELF program)" % fmrre)
    if phone_arch and arch != phone_arch and not a.force:
        raise Fail("%s is built for %s but this phone is %s - it would never start.\n"
                   "  Use out/%s/recovery (or --force if you are sure)" % (fmrre, arch, phone_arch, phone_arch))
    if not static:
        say("warning: %s is dynamically linked; a recovery ramdisk usually lacks its libraries - use the static "
            "build (scripts/build.sh or the CI artifacts)" % fmrre)
    say("FMRRE: %s (%s, %s)" % (fmrre, arch, mb(len(binary))))

    fstab = None
    if a.device:
        path = os.path.join(ROOT, "device", a.device, "twrp.fstab")
        if not os.path.isfile(path):
            have = sorted(d for d in os.listdir(os.path.join(ROOT, "device")) if d != "example")
            raise Fail("no device/%s/twrp.fstab (known: %s)" % (a.device, ", ".join(have) or "none"))
        a.fstab = path
    if a.fstab:
        with open(a.fstab, "rb") as f:
            fstab = f.read().replace(b"\r\n", b"\n")
    target, notes = install_fmrre(rd, binary, fstab)
    say("FMRRE goes to /%s (stock program kept as /%s.stock)" % (target, target))
    for n in notes:
        say("  " + n)
    if fstab is None:
        say("  fstab: the one already in the image (pass --device or --fstab to add a twrp.fstab)")

    new_cpio = cpio_write(rd.entries)
    new_rd = ramdisk_pack(new_cpio, how)
    out = img.rebuild(new_rd)
    # Read back what was written before anyone flashes it.
    chk = BootImage(out)
    back, _ = ramdisk_unpack(chk.sections["ramdisk"])
    if back != new_cpio or chk.sections["kernel"] != img.sections["kernel"] or chk.id_matches_stock() is False:
        raise Fail("internal check failed: the new image doesn't read back correctly (please report this)")
    with open(a.out, "wb") as f:
        f.write(out)
    say()
    say("==> %s  (%s, was %s)" % (a.out, mb(len(out)), mb(len(raw))))

    samsung = a.odin == "yes" or (a.odin == "auto" and (from_ap or img.seandroid))
    if samsung:
        files = [("recovery.img", out)]
        if a.vbmeta == "disabled" or (a.vbmeta == "auto" and img.avb):
            files.append(("vbmeta.img", vbmeta_disabled()))
            why = "with a disabled vbmeta (this phone verifies recovery with AVB)"
        elif a.vbmeta not in ("auto", "none", "disabled"):
            with open(a.vbmeta, "rb") as f:
                vb = bytearray(f.read())
            if vb[:4] != b"AVB0":
                raise Fail("%s is not a vbmeta image" % a.vbmeta)
            struct.pack_into(">I", vb, 120, struct.unpack_from(">I", vb, 120)[0] | 3)
            files.append(("vbmeta.img", bytes(vb)))
            why = "with your vbmeta, verification disabled"
        else:
            why = "recovery only: no AVB footer, so no vbmeta partition to disable" if a.vbmeta == "auto" else \
                "recovery only"
        tar = os.path.splitext(a.out)[0] + ".tar.md5"
        odin_tar(tar, files)
        say("==> %s  Odin, AP slot (%s)" % (tar, why))
        name = {"twrp": "twrp", "fmrre": "previous"}.get(base, "stock")
        back_tar = os.path.join(os.path.dirname(os.path.abspath(a.out)), name + "-recovery.tar.md5")
        # A whole-partition dump (dd) ends in zeros up to the partition's size: keep the image itself (sections,
        # and the SEANDROIDENFORCE mark), and an AVB image whole (its footer sits at the very end).
        # Samsung's signature block follows SEANDROIDENFORCE and must stay (the stock image would look custom without
        # it): only a long run of zeros at the very end - the rest of the partition - is dropped.
        keep = raw
        if not img.avb:
            body = len(raw.rstrip(b"\0"))
            if len(raw) - body >= (64 << 10):
                keep = raw[:min(len(raw), max(img.end, pad(body, 4096)))]
        odin_tar(back_tar, [("recovery.img", keep)])
        say("==> %s  Odin, AP slot: puts the %s recovery back" % (back_tar, {"twrp": "TWRP", "fmrre": "previous"}.get(base, "stock")))
        say()
        say("Flash with Odin: phone in Download mode, AP = %s, Options: UNTICK Auto Reboot, Start." %
            os.path.basename(tar))
        say("When it says PASS, boot STRAIGHT into recovery (Android would put the stock one back):")
        say("  hold Volume Down + Power until the screen goes off, then at once Volume Up + Bixby + Power")
        say("  (Volume Up + Power on phones without Bixby) until FMRRE appears.")
    else:
        say()
        say("Test it without flashing:  fastboot boot %s" % os.path.basename(a.out))
        say("Then flash it:              scripts\\install.ps1 -Image %s -Permanent   (or scripts/install.sh)" %
            os.path.basename(a.out))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock", help="stock recovery.img, recovery.img.lz4 or a Samsung AP_....tar.md5")
    ap.add_argument("-o", "--out", default="fmrre-recovery.img")
    ap.add_argument("--fmrre", help="FMRRE binary for the phone's CPU (default: out/<arch>/recovery of the kit)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--device", help="add device/<NAME>/twrp.fstab, e.g. dreamlte for the Galaxy S8 (SM-G950F)")
    g.add_argument("--fstab", help="add this twrp.fstab")
    ap.add_argument("--odin", choices=("auto", "yes", "no"), default="auto",
                    help="write Odin packages (auto: for Samsung images)")
    ap.add_argument("--vbmeta", default="auto",
                    help="Odin package: auto (only if the stock recovery has an AVB footer), none, disabled, "
                         "or a stock vbmeta.img to disable")
    ap.add_argument("--force", action="store_true", help="build even if FMRRE's CPU doesn't match the phone's")
    ap.add_argument("--no-md5", action="store_true", help="don't check the AP's MD5 (faster)")
    a = ap.parse_args(argv)
    try:
        build(a)
    except Fail as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    except OSError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
