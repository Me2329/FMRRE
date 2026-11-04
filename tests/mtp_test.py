#!/usr/bin/env python3
"""MTP tests: an independent MTP initiator (the part a computer plays) drives FMRRE's MTP responder over TCP.

  python3 tests/mtp_test.py out/sim/fmrre                      the simulator
  python3 tests/mtp_test.py "qemu-mips -cpu 24Kf out/mips/recovery"   any build, e.g. big-endian MIPS

The containers are exactly what goes over USB (length, type, code, transaction, params / data); only the
transport differs. Covers browsing, downloads (whole and partial), uploads, folders, rename / move / copy /
delete, the object property lists Windows uses, device properties, and names that try to leave the folder.
"""
import os
import random
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import time

CMD = shlex.split(sys.argv[1] if len(sys.argv) > 1 else "out/sim/fmrre")
FAILS = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        FAILS.append(name)
        if detail:
            print("     " + str(detail)[:2000])


def mstr(s):
    if not s:
        return b"\0"
    u = s.encode("utf-16-le") + b"\0\0"
    return bytes([len(u) // 2]) + u


def read_str(b, i):
    n = b[i]
    i += 1
    if n == 0:
        return "", i
    s = b[i:i + 2 * n].decode("utf-16-le").rstrip("\0")
    return s, i + 2 * n


def read_a16(b, i):
    (n,) = struct.unpack_from("<I", b, i)
    return list(struct.unpack_from("<%dH" % n, b, i + 4)), i + 4 + 2 * n


def read_a32(b, i=0):
    (n,) = struct.unpack_from("<I", b, i)
    return list(struct.unpack_from("<%dI" % n, b, i + 4)), i + 4 + 4 * n


def object_info(storage, fmt, size, parent, name):
    fixed = struct.pack("<IHHIHIIIIIIIHII", storage, fmt, 0, size, 0, 0, 0, 0, 0, 0, 0, parent, 1 if fmt == 0x3001 else 0, 0, 0)
    assert len(fixed) == 52
    return fixed + mstr(name) + mstr("20261104T120000") + mstr("20261104T120000") + mstr("")


def parse_object_info(b):
    storage, fmt, prot, size = struct.unpack_from("<IHHI", b, 0)
    (parent,) = struct.unpack_from("<I", b, 38)
    name, _ = read_str(b, 52)
    return {"storage": storage, "format": fmt, "size": size, "parent": parent, "name": name}


class Mtp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.tid = 0

    def _recv(self, n):
        out = bytearray()
        while len(out) < n:
            k = self.s.recv(n - len(out))
            if not k:
                raise EOFError("connection closed")
            out += k
        return bytes(out)

    def op(self, code, *params, data=None):
        self.tid += 1
        body = b"".join(struct.pack("<I", p) for p in params)
        self.s.sendall(struct.pack("<IHHI", 12 + len(body), 1, code, self.tid) + body)
        if data is not None:
            self.s.sendall(struct.pack("<IHHI", 12 + len(data), 2, code, self.tid) + data)
        got = None
        while True:
            length, typ, rc, tid = struct.unpack("<IHHI", self._recv(12))
            payload = self._recv(length - 12)
            assert tid == self.tid, "transaction id %d != %d" % (tid, self.tid)
            if typ == 2:
                got = payload
                continue
            if typ == 3:
                return rc, list(struct.unpack("<%dI" % (len(payload) // 4), payload)), got

    def children(self, storage, parent):
        rc, _, data = self.op(0x1007, storage, 0, parent)
        handles, _ = read_a32(data)
        out = {}
        for h in handles:
            rc2, _, info = self.op(0x1008, h)
            out[parse_object_info(info)["name"]] = h
        return out


def main():
    work = tempfile.mkdtemp(prefix="fmrre-mtp-")
    root = os.path.join(work, "sim")
    subprocess.run(CMD + ["--sim", "--sim-root", root, "fmrre", "help"], capture_output=True, timeout=120)  # makes the sandbox
    media = os.path.join(root, "data", "media", "0")
    port = 30000 + random.randint(0, 20000)
    srv = subprocess.Popen(CMD + ["--sim", "--sim-root", root, "fmrre", "mtp", "--tcp", str(port)], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
    try:
        m = None
        for _ in range(100):
            try:
                m = Mtp(port)
                break
            except OSError:
                time.sleep(0.2)
        check("the server accepts a computer", m is not None)
        if m is None:
            return 1

        rc, _, info = m.op(0x1001)
        ver, ext = struct.unpack_from("<HI", info, 0)
        desc, i = read_str(info, 8)
        i += 2  # functional mode
        ops, i = read_a16(info, i)
        _, i = read_a16(info, i)
        props, i = read_a16(info, i)
        _, i = read_a16(info, i)
        _, i = read_a16(info, i)
        maker, i = read_str(info, i)
        model, i = read_str(info, i)
        version, i = read_str(info, i)
        check("GetDeviceInfo before a session: MTP 1.0, Microsoft extension, the phone's name",
              rc == 0x2001 and ver == 100 and ext == 6 and "microsoft.com" in desc and 0x9805 in ops and 0xD402 in props
              and maker == "Google" and model == "Pixel 8 Pro" and version.startswith("FMRRE"), (hex(rc), desc, maker, model, version))
        check("no session yet: refused", m.op(0x1004)[0] == 0x2003)
        check("OpenSession", m.op(0x1002, 1)[0] == 0x2001)
        check("a second OpenSession: already open", m.op(0x1002, 1)[0] == 0x201E)

        rc, _, data = m.op(0x1004)
        ids, _ = read_a32(data)
        check("GetStorageIDs: internal storage first", rc == 0x2001 and ids and ids[0] == 0x00010001, ids)
        sid = ids[0]
        rc, _, si = m.op(0x1005, sid)
        cap, free = struct.unpack_from("<QQ", si, 6)
        sdesc, _ = read_str(si, 26)
        check("GetStorageInfo: capacity, free space, 'Internal storage'", rc == 0x2001 and cap > 0 and 0 < free <= cap and sdesc == "Internal storage", (cap, free, sdesc))
        check("unknown storage: refused", m.op(0x1005, 0x00990001)[0] == 0x2008)

        top = m.children(sid, 0xFFFFFFFF)
        check("the root lists the storage's folders", "Download" in top and "FMRRE" in top, sorted(top))
        dl = top["Download"]
        files = m.children(sid, dl)
        check("Download lists its files", "notes.txt" in files and "Magisk-v29.0.zip" in files, sorted(files))
        notes = open(os.path.join(media, "Download", "notes.txt"), "rb").read()
        rc, params, data = m.op(0x1009, files["notes.txt"])
        check("GetObject: the file's bytes", rc == 0x2001 and data == notes)
        rc, params, data = m.op(0x101B, files["notes.txt"], 2, 5)
        check("GetPartialObject", rc == 0x2001 and data == notes[2:7] and params == [5], (params, data))
        rc, params, data = m.op(0x95C1, files["notes.txt"], 3, 0, 1000)
        check("GetPartialObject64 (past the end: what is there)", rc == 0x2001 and data == notes[3:] and params == [len(notes) - 3])
        rc, _, info = m.op(0x1008, files["notes.txt"])
        oi = parse_object_info(info)
        check("GetObjectInfo: size, format, parent", oi["size"] == len(notes) and oi["format"] == 0x3004 and oi["parent"] == dl, oi)
        check("a folder can't be downloaded", m.op(0x1009, dl)[0] == 0x2009)

        # Upload 3 MB (more than one 1 MiB chunk) into Download
        blob = os.urandom(3 * 1024 * 1024 + 123)
        rc, params, _ = m.op(0x100C, sid, dl, data=object_info(sid, 0x3000, len(blob), dl, "from_pc.bin"))
        check("SendObjectInfo: storage, parent, new handle", rc == 0x2001 and params[:2] == [sid, dl] and params[2] > 0, (hex(rc), params))
        up = params[2]
        rc, _, _ = m.op(0x100D, data=blob)
        on_disk = os.path.join(media, "Download", "from_pc.bin")
        check("SendObject: the file arrives intact", rc == 0x2001 and os.path.isfile(on_disk) and open(on_disk, "rb").read() == blob)
        rc, _, data = m.op(0x1009, up)
        check("and comes back the same", rc == 0x2001 and data == blob)
        rc, params, _ = m.op(0x100C, sid, dl, data=object_info(sid, 0x3001, 0, dl, "New folder"))
        folder = params[2] if rc == 0x2001 else 0
        check("a new folder", rc == 0x2001 and os.path.isdir(os.path.join(media, "Download", "New folder")))
        check("SendObject without SendObjectInfo: refused (and the data is consumed)", m.op(0x100D, data=b"stray")[0] == 0x2015)

        for bad in ("../escape.txt", "a/b.txt", ".."):
            rc, _, _ = m.op(0x100C, sid, dl, data=object_info(sid, 0x3000, 4, dl, bad))
            check("name %r refused" % bad, rc == 0xA806)
        check("nothing written outside", not os.path.exists(os.path.join(media, "escape.txt")))

        rc, _, _ = m.op(0x9804, up, 0xDC07, data=mstr("renamed.bin"))
        renamed = os.path.join(media, "Download", "renamed.bin")
        check("rename (SetObjectPropValue file name)", rc == 0x2001 and os.path.isfile(renamed) and not os.path.exists(on_disk))
        rc, _, _ = m.op(0x9804, up, 0xDC07, data=mstr("../x"))
        check("rename out of the folder refused", rc == 0x201D)
        rc, params, _ = m.op(0x101A, up, sid, folder)
        check("CopyObject", rc == 0x2001 and open(os.path.join(media, "Download", "New folder", "renamed.bin"), "rb").read() == blob)
        copy = params[0] if params else 0
        rc, _, _ = m.op(0x1019, up, sid, 0xFFFFFFFF)
        check("MoveObject to the root", rc == 0x2001 and os.path.isfile(os.path.join(media, "renamed.bin")) and not os.path.exists(renamed))

        # The property list Windows Explorer asks for: every property of every child of Download
        rc, _, data = m.op(0x9805, dl, 0, 0xFFFFFFFF, 0, 1)
        (n,) = struct.unpack_from("<I", data, 0)
        i, names, sizes = 4, {}, {}
        for _ in range(n):
            h, prop, typ = struct.unpack_from("<IHH", data, i)
            i += 8
            if typ == 0xFFFF:
                v, i = read_str(data, i)
            elif typ == 0x0008:
                (v,) = struct.unpack_from("<Q", data, i)
                i += 8
            elif typ == 0x000A:
                v = data[i:i + 16]
                i += 16
            elif typ == 0x0006:
                (v,) = struct.unpack_from("<I", data, i)
                i += 4
            else:
                (v,) = struct.unpack_from("<H", data, i)
                i += 2
            if prop == 0xDC07:
                names[h] = v
            if prop == 0xDC04:
                sizes[h] = v
        nh = [h for h, v in names.items() if v == "notes.txt"]
        check("GetObjectPropList (children, all properties)", rc == 0x2001 and i == len(data) and nh and sizes.get(nh[0]) == len(notes),
              (hex(rc), n, len(names)))
        rc, _, data = m.op(0x9805, files["notes.txt"], 0, 0xDC04, 0, 0)
        check("GetObjectPropList (one object, one property)", rc == 0x2001 and struct.unpack_from("<IIHHQ", data, 0)[4] == len(notes))
        check("depth 2: refused", m.op(0x9805, dl, 0, 0xFFFFFFFF, 0, 2)[0] == 0xA808)
        rc, _, d = m.op(0x9802, 0xDC07, 0x3000)
        check("GetObjectPropDesc: file name is a string you can set", rc == 0x2001 and struct.unpack_from("<HHB", d, 0) == (0xDC07, 0xFFFF, 1))
        rc, _, d = m.op(0x9801, 0x3000)
        sup, _ = read_a16(d, 0)
        check("GetObjectPropsSupported", rc == 0x2001 and 0xDC07 in sup and 0xDC04 in sup)
        rc, _, d = m.op(0x1015, 0xD402)
        fname, _ = read_str(d, 0)
        check("device friendly name", rc == 0x2001 and "Pixel 8 Pro" in fname, fname)
        rc, _, d = m.op(0x1014, 0x5001)
        check("battery level property (0..100)", rc == 0x2001 and struct.unpack_from("<HHB", d, 0) == (0x5001, 0x0002, 0))
        rc, _, d = m.op(0x9810, files["notes.txt"])
        check("GetObjectReferences: none", rc == 0x2001 and read_a32(d)[0] == [])

        check("DeleteObject (file)", m.op(0x100B, copy)[0] == 0x2001 and not os.path.exists(os.path.join(media, "Download", "New folder", "renamed.bin")))
        check("DeleteObject (folder)", m.op(0x100B, folder)[0] == 0x2001 and not os.path.exists(os.path.join(media, "Download", "New folder")))
        check("a deleted handle is gone", m.op(0x1008, folder)[0] == 0x2009)
        check("unknown operation", m.op(0x1234)[0] == 0x2005)
        check("CloseSession", m.op(0x1003)[0] == 0x2001)
        check("closed: refused again", m.op(0x1004)[0] == 0x2003)
        m.s.close()
        m2 = Mtp(port)
        check("the next computer can connect", m2.op(0x1002, 7)[0] == 0x2001 and m2.op(0x1004)[0] == 0x2001)
        m2.s.close()
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=10)
        except subprocess.TimeoutExpired:
            srv.kill()
    print()
    if FAILS:
        print("%d MTP test(s) FAILED" % len(FAILS))
        return 1
    print("All MTP tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
