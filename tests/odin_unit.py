#!/usr/bin/env python3
"""Tests for pc/odin.py (Download-mode flashing) against a fake Samsung bootloader that checks every message.

Standard library only (runs on Windows too). The fake plays the phone's side: the ODIN/LOKE handshake, the
session (old bootloaders with 128 KiB parts and new ones with 1 MiB), the PIT, files sent in sequences of parts
acknowledged by number, end-of-sequence messages naming the partition, end of session and reboot - and it puts
together what was written to each partition, to compare with what was sent.
"""
import hashlib
import io
import os
import socket
import struct
import sys
import tarfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "pc"))
import odin  # noqa: E402

FAILS = 0


def check(cond, what):
    global FAILS
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS += 1


def make_pit(entries):
    """entries: (binary_type, device_type, identifier, name, flash_name)"""
    b = bytearray(28 + 132 * len(entries))
    struct.pack_into("<II", b, 0, odin.PIT_MAGIC, len(entries))
    for i, (bt, dt, ident, name, fname) in enumerate(entries):
        o = 28 + 132 * i
        struct.pack_into("<9I", b, o, bt, dt, ident, 0, 0, 0, 1024, 0, 0)
        b[o + 36:o + 36 + len(name)] = name.encode()
        b[o + 68:o + 68 + len(fname)] = fname.encode()
    return bytes(b)


PIT = make_pit([(0, 2, 1, "BOOTLOADER", "sboot.bin"), (0, 8, 10, "BOOT", "boot.img"), (0, 8, 11, "RECOVERY", "recovery.img"),
                (1, 8, 12, "RADIO", "modem.bin"), (0, 8, 22, "SYSTEM", "system.img")])


class SockTransport:
    def __init__(self, s):
        self.s, self.name = s, "fake"

    def read(self, n, timeout):
        self.s.settimeout(max(timeout, 0.01))
        try:
            return self.s.recv(n)
        except socket.timeout:
            return b""

    def write(self, data):
        self.s.sendall(data)

    def zlp(self):
        pass

    def close(self):
        self.s.close()


class FakePhone(threading.Thread):
    def __init__(self, sock, new_protocol=True, refuse=None, die_after_parts=None):
        super().__init__(daemon=True)
        self.s, self.new, self.refuse, self.die_after = sock, new_protocol, refuse, die_after_parts
        self.part_size = 128 * 1024
        self.written = {}      # partition id -> bytes
        self.errors = []
        self.log = []
        self.rebooted = False
        self.total = None

    def recv(self, n):
        out = bytearray()
        while len(out) < n:
            c = self.s.recv(n - len(out))
            if not c:
                raise EOFError
            out += c
        return bytes(out)

    def ctl(self):
        b = self.recv(1024)
        return struct.unpack_from("<8I", b, 0)

    def answer(self, kind, value=0):
        self.s.sendall(struct.pack("<Ii", kind, value))

    def run(self):
        try:
            self._run()
        except EOFError:
            pass
        except Exception as e:  # a protocol mistake by the tool
            self.errors.append(repr(e))

    def _run(self):
        if self.recv(4) != b"ODIN":
            self.errors.append("no handshake")
            return
        self.s.sendall(b"LOKE")
        parts = 0
        while True:
            kind, req, a0, a1, a2, a3, a4, a5 = self.ctl()
            self.log.append((kind, req))
            if kind == odin.T_SESSION and req == odin.SESSION_BEGIN:
                self.answer(kind, 0x20000 if self.new else 0)
            elif kind == odin.T_SESSION and req == odin.SESSION_PART_SIZE:
                if not self.new:
                    self.errors.append("part size asked of an old bootloader")
                self.part_size = a0
                self.answer(kind, 0)
            elif kind == odin.T_SESSION and req == odin.SESSION_TOTAL_BYTES:
                self.total = a0
                self.answer(kind, 0)
            elif kind == odin.T_PIT and req == odin.PIT_DUMP:
                self.answer(kind, len(PIT))
            elif kind == odin.T_PIT and req == odin.PIT_PART:
                self.s.sendall(PIT[a0 * 500:(a0 + 1) * 500])
            elif kind == odin.T_PIT and req == odin.PIT_END:
                if a0 != len(PIT):
                    self.errors.append("PIT end with the wrong size")
                self.answer(kind, 0)
            elif kind == odin.T_FILE and req == odin.FILE_FLASH:
                self.answer(kind, 0)
                data = bytearray()
                while True:  # sequences
                    k, r, seq_bytes = self.ctl()[:3]
                    if (k, r) != (odin.T_FILE, odin.FILE_PART) or seq_bytes % self.part_size:
                        self.errors.append("expected a sequence start, got %x/%d/%d" % (k, r, seq_bytes))
                        return
                    self.answer(odin.T_FILE, 0)
                    seq = bytearray()
                    for i in range(seq_bytes // self.part_size):
                        seq += self.recv(self.part_size)
                        parts += 1
                        if self.die_after and parts >= self.die_after:
                            self.s.close()
                            return
                        self.answer(0, i)
                    k, r, dest, eff, _unk, dtype, ident, last = self.ctl()
                    if (k, r) != (odin.T_FILE, odin.FILE_END) or eff > seq_bytes or dest != 0:
                        self.errors.append("bad end of sequence")
                        return
                    data += seq[:eff]
                    entry = [e for e in odin.parse_pit(PIT) if e.identifier == ident]
                    if not entry or entry[0].device_type != dtype:
                        self.errors.append("end of sequence names no PIT partition (%d)" % ident)
                    self.answer(odin.T_FILE, 1 if self.refuse == ident else 0)
                    if last:
                        self.written[ident] = bytes(data)
                        break
            elif kind == odin.T_END and req == odin.END_SESSION:
                self.answer(kind, 0)
            elif kind == odin.T_END and req == odin.END_REBOOT:
                self.rebooted = True
                self.answer(kind, 0)
                return
            else:
                self.errors.append("unknown message %x/%d" % (kind, req))
                return


def session(**kw):
    a, b = socket.socketpair()
    phone = FakePhone(b, **kw)
    phone.start()
    return odin.Odin(SockTransport(a), log=lambda s: None), phone


def odin_tar(files):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for n, d in files:
            ti = tarfile.TarInfo(n)
            ti.size = len(d)
            t.addfile(ti, io.BytesIO(d))
    body = buf.getvalue()
    return body + ("%s  x.tar\n" % hashlib.md5(body).hexdigest()).encode()


def flow_test():
    """pc/fmrre_rescue.py install-samsung end to end: a phone in TWRP (fake adb), the built-in Odin (fake bootloader)."""
    import lzma
    import tempfile
    import fmrre_mkrecovery as mk
    import fmrre_rescue as fr

    def elf():
        b = bytearray(256)
        b[0:4], b[4], b[5] = b"\x7fELF", 2, 1
        struct.pack_into("<HH", b, 16, 2, 183)
        return bytes(b)

    ents = [mk.Entry("sbin", 0o40755), mk.Entry("sbin/recovery", 0o100755, elf() + b"TWRP"),
            mk.Entry("etc", 0o40755), mk.Entry("etc/twrp.fstab", 0o100644, b"/data ext4 /dev/block/USERDATA\n")]
    rd = lzma.compress(mk.cpio_write(ents), format=lzma.FORMAT_ALONE)
    k = os.urandom(100000)
    h = bytearray(2048)
    h[0:8] = b"ANDROID!"
    struct.pack_into("<10I", h, 8, len(k), 0, len(rd), 0, 0, 0, 0, 2048, 0, 0)
    twrp = bytes(h) + k + bytes(-len(k) % 2048) + rd + bytes(-len(rd) % 2048) + b"SEANDROIDENFORCE"
    twrp += bytes((4 << 20) - len(twrp))  # the whole partition, as dd / adb pull gives it

    with tempfile.TemporaryDirectory() as d:
        binary = os.path.join(d, "recovery")
        open(binary, "wb").write(elf() + b"FMRRE" * 1000)
        calls = []
        phone_tmp = {}

        class FakeRunner:
            dry = False

            def __init__(self):
                self.log = lambda t: None
                self.last_output = ""

            def have(self, tool):
                return True

            def call(self, tool, *args, timeout=30):
                calls.append((tool,) + args)
                if args[:2] == ("shell", "getprop ro.product.cpu.abi"):
                    return 0, "arm64-v8a\n"
                part = "/dev/block/platform/11120000.ufs/by-name/RECOVERY"
                if args[0] == "shell" and args[1].startswith("for p in"):
                    return 0, part + "\n"
                if args[0] == "shell" and args[1].startswith("dd if=%s of=" % part):
                    phone_tmp[args[1].split(" of=")[1].split()[0]] = twrp
                    return 0, "44+1 records in\n44+1 records out\nFMRRE_DD_OK\n"
                if args[0] == "shell" and args[1].startswith("dd "):
                    return 1, "dd: can't open: No such file or directory"
                if args[0] == "shell":
                    return 0, ""
                if args[0] == "pull" and args[1].startswith("/dev/block/"):
                    return 1, "adb: error: remote object '%s' not a file or directory" % args[1]  # what real adb does
                if args[0] == "pull" and args[1] in phone_tmp:
                    open(args[2], "wb").write(phone_tmp[args[1]])
                    return 0, "1 file pulled"
                return 1, "error"

            def call_to_file(self, tool, path, *args, timeout=300):
                open(path, "wb").close()
                return 1

            def write(self, tool, *args):
                calls.append((tool,) + args)
                return True

        opts = fr.Options(yes=True, workdir=d)  # the tool's own options (a stand-in hid a missing attribute once)
        rs = fr.Rescue(FakeRunner(), opts)
        rs.phone.mode = "recovery"
        sessions = []

        def fake_connect(port=None, log=print):
            o, phone = session(new_protocol=True)
            sessions.append(phone)
            return o
        real = odin.connect
        odin.connect = fake_connect
        odin._fake = True
        try:
            ok = rs.install_samsung(fmrre_bin=binary)
        finally:
            odin.connect = real
            odin._fake = False
        phone = sessions[0]
        phone.join(5)
        built = open(os.path.join(d, "fmrre-recovery.img"), "rb").read()
        check(ok and ("adb", "reboot", "download") in calls, "install-samsung: the phone's TWRP read with adb pull, phone sent to Download mode")
        check(not phone.errors and phone.written.get(11) == built and not phone.rebooted,
              "install-samsung: the FMRRE image built on TWRP is what lands in RECOVERY (no reboot, recovery by hand)")
        im = mk.BootImage(built)
        files = mk.Ramdisk(mk.cpio_read(mk.ramdisk_unpack(im.sections["ramdisk"])[0]))
        check(files.get("sbin/recovery").data == open(binary, "rb").read() and files.get("etc/twrp.fstab") is not None,
              "install-samsung: FMRRE in TWRP's place, TWRP's fstab kept")
        check(os.path.isfile(os.path.join(d, "twrp-recovery.tar.md5")), "install-samsung: TWRP kept as an Odin package to go back")

        # The real command line, as typed in PowerShell (no phone, --dry-run): no unexpected error anywhere on the way.
        import contextlib
        os.environ["FMRRE_NO_ODIN_PROBE"] = "1"
        base = os.path.join(d, "twrp.img")
        open(base, "wb").write(twrp)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = fr.main(["install-samsung", "--dry-run", "--yes", "--base", base, "--fmrre", binary, "--workdir",
                          os.path.join(d, "cli"), "--lang", "en"])
        text = out.getvalue()
        check(rc == 0 and "Unexpected error" not in text and "Traceback" not in text and "dry run" in text,
              "install-samsung from the command line (--dry-run) runs through" + ("" if rc == 0 else ": " + text[-800:]))


def main():
    rec = os.urandom(3 * 1024 * 1024 + 12345)  # several 1 MiB parts and a partial one
    big = os.urandom(31 * 1024 * 1024 + 5)     # more than one 30 MiB sequence
    quiet = lambda s: None

    o, phone = session(new_protocol=True)
    odin.flash_files([("recovery.img", rec)], log=quiet, odin=o)
    phone.join(5)
    check(not phone.errors and phone.written.get(11) == rec, "new bootloader: recovery.img lands in RECOVERY whole (1 MiB parts)")
    check(phone.total == len(rec), "the total size is announced first, as Odin does")
    check(not phone.rebooted, "no reboot unless asked (boot recovery by hand)")

    o, phone = session(new_protocol=False)
    odin.flash_files([("whatever.bin", rec)], partition="recovery", reboot=True, log=quiet, odin=o)
    phone.join(5)
    check(not phone.errors and phone.written.get(11) == rec and phone.rebooted,
          "old bootloader: 128 KiB parts, partition named explicitly, reboot at the end")

    o, phone = session(new_protocol=True)
    odin.flash_files([("system.img", big)], log=quiet, odin=o)
    phone.join(10)
    check(not phone.errors and phone.written.get(22) == big, "a file of several sequences (31 MiB) arrives whole")

    o, phone = session()
    try:
        odin.flash_files([("recovery.img", rec), ("vbmeta.img", b"x" * 4096)], log=quiet, odin=o)
        check(False, "a file without a partition is refused before anything is written")
    except odin.OdinError as e:
        phone.join(5)
        check("vbmeta.img" in str(e) and not phone.written, "a file without a partition (S8: vbmeta) - nothing written at all")

    o, phone = session()
    try:
        odin.flash_files([("sboot.bin", b"x" * 1000)], log=quiet, odin=o)
        check(False, "the bootloader partition is refused")
    except odin.OdinError as e:
        phone.join(5)
        check("bootloader area" in str(e) and not phone.written, "bootloader partitions are never flashed by this tool")

    o, phone = session(refuse=11)
    try:
        odin.flash_files([("recovery.img", rec)], log=quiet, odin=o)
        check(False, "a refusal by the phone is an error")
    except odin.OdinError as e:
        check("refused" in str(e), "the phone refusing the image is reported")

    o, phone = session(die_after_parts=2)
    try:
        odin.flash_files([("recovery.img", rec)], log=quiet, odin=o)
        check(False, "a cable pulled half way is an error")
    except (odin.OdinError, OSError) as e:
        check(True, "a cable pulled half way is an error (%s)" % type(e).__name__)

    a, b = socket.socketpair()
    threading.Thread(target=lambda: (b.recv(4), b.sendall(b"AT\r\n")), daemon=True).start()
    try:
        odin.Odin(SockTransport(a), quiet).handshake()
        check(False, "a device that isn't in Download mode is not talked to")
    except odin.OdinError:
        check(True, "a device that isn't in Download mode is refused at the handshake")

    # The real detection code (registry on Windows, sysfs / libusb elsewhere) runs on this computer without a phone.
    try:
        found, cands = odin.present(), odin.candidates()
        check(found in (True, False) and isinstance(cands, list), "Download-mode detection runs here (no phone: %s, %d port(s))" % (found, len(cands)))
    except Exception as e:
        check(False, "Download-mode detection raised %r" % e)

    entries = odin.parse_pit(PIT)
    check([e.name for e in entries] == ["BOOTLOADER", "BOOT", "RECOVERY", "RADIO", "SYSTEM"] and entries[3].binary_type == 1, "PIT parsed")
    check(odin.pit_target(entries, "recovery.img").identifier == 11 and odin.pit_target(entries, "boot.img.lz4").identifier == 10,
          "files matched to partitions by name, as Odin does")

    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "fmrre-recovery.tar.md5")
        open(p, "wb").write(odin_tar([("recovery.img", rec)]))
        check(odin.images_from(p) == [("recovery.img", rec)], "Odin packages (.tar.md5) are read, MD5 checked")
        bad = bytearray(open(p, "rb").read())
        bad[1000] ^= 1
        open(p, "wb").write(bad)
        try:
            odin.images_from(p)
            check(False, "a damaged package is refused")
        except odin.OdinError:
            check(True, "a damaged package is refused")

    flow_test()
    print("FAILED: %d" % FAILS if FAILS else "All Odin tests passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
