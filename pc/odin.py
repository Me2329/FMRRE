#!/usr/bin/env python3
"""Samsung Download mode ("Odin mode") flashing, in Python - no Odin, no Heimdall.

  python pc/odin.py detect                      is a phone in Download mode? (by its USB id - nothing is sent)
  python pc/odin.py pit [-o phone.pit]          the phone's partition table
  python pc/odin.py flash recovery.img [--partition RECOVERY] [--reboot]
  python pc/odin.py flash fmrre-recovery.tar.md5          every image in an Odin package (AP)

The protocol is the one Odin speaks and Heimdall documented (the bootloader calls itself "LOKE"): a handshake,
a session, the PIT (which partition each file goes to), then each file in sequences of fixed-size parts, each
part acknowledged by number.

How it reaches the phone:
  Windows   the COM port Samsung's USB driver creates for Download mode ("SAMSUNG Mobile USB Modem") - the same
            way Odin talks to it; nothing else to install (Samsung's driver: "Samsung Android USB Driver").
  Linux/mac pyusb (pip install pyusb; libusb) when present - the way Heimdall does it; otherwise the serial device
            the kernel makes (/dev/ttyACM0), which works on most phones.

Nothing is written before the phone has answered the handshake, given its PIT, and the PIT has a partition for
every file. Flashing anything but Samsung-signed files needs OEM unlocking on (and trips Knox).
"""
import argparse
import glob
import hashlib
import io
import os
import struct
import sys
import tarfile
import time

CONTROL_SIZE = 1024
T_SESSION, T_PIT, T_FILE, T_END = 0x64, 0x65, 0x66, 0x67
SESSION_BEGIN, SESSION_DEVICE_TYPE, SESSION_TOTAL_BYTES, SESSION_PART_SIZE = 0, 1, 2, 5
PIT_FLASH, PIT_DUMP, PIT_PART, PIT_END = 0, 1, 2, 3
FILE_FLASH, FILE_DUMP, FILE_PART, FILE_END = 0, 1, 2, 3
END_SESSION, END_REBOOT = 0, 1
DEST_PHONE, DEST_MODEM = 0, 1
PIT_MAGIC = 0x12349876
SAMSUNG_VID = 0x04E8
DOWNLOAD_PIDS = (0x6601, 0x685D, 0x68C3)


class OdinError(Exception):
    pass


def control(kind, request, *args):
    """A control message: always 1024 bytes, little-endian integers."""
    b = bytearray(CONTROL_SIZE)
    struct.pack_into("<II", b, 0, kind, request)
    for i, a in enumerate(args):
        struct.pack_into("<I", b, 8 + 4 * i, a & 0xFFFFFFFF)
    return bytes(b)


# ---- the PIT ---------------------------------------------------------------------------------------------------

class PitEntry:
    __slots__ = ("binary_type", "device_type", "identifier", "attributes", "update_attributes", "block_size",
                 "block_count", "file_offset", "file_size", "name", "flash_name", "fota_name")

    def __repr__(self):
        return "PitEntry(%s, id=%d, file=%s)" % (self.name, self.identifier, self.flash_name)


def parse_pit(data):
    if len(data) < 28 or struct.unpack_from("<I", data, 0)[0] != PIT_MAGIC:
        raise OdinError("the phone's PIT is not readable (no PIT header)")
    count = struct.unpack_from("<I", data, 4)[0]
    if count > 512 or 28 + count * 132 > len(data):
        raise OdinError("the phone's PIT is damaged (%d entries in %d bytes)" % (count, len(data)))
    out = []
    for i in range(count):
        o = 28 + i * 132
        e = PitEntry()
        (e.binary_type, e.device_type, e.identifier, e.attributes, e.update_attributes, e.block_size, e.block_count,
         e.file_offset, e.file_size) = struct.unpack_from("<9I", data, o)
        cstr = lambda b: b.split(b"\0", 1)[0].decode("ascii", "replace")
        e.name, e.flash_name, e.fota_name = cstr(data[o + 36:o + 68]), cstr(data[o + 68:o + 100]), cstr(data[o + 100:o + 132])
        out.append(e)
    return out


def pit_target(entries, filename=None, partition=None):
    """The PIT entry a file goes to: by partition name, else by the file name Odin would match (recovery.img)."""
    for e in entries:
        if partition and e.name.upper() == partition.upper():
            return e
    if filename:
        base = os.path.basename(filename)
        for e in entries:
            if e.flash_name and e.flash_name.lower() == base.lower():
                return e
        stem = base.lower().split(".")[0]
        for e in entries:
            if e.flash_name and e.flash_name.lower().split(".")[0] == stem:
                return e
    return None


# ---- the session -----------------------------------------------------------------------------------------------

class Odin:
    """One Download-mode session on a transport (read(n, timeout) -> bytes, write(bytes), zlp(), close())."""

    def __init__(self, transport, log=print):
        self.t = transport
        self.log = log
        self.part_size = 128 * 1024   # bytes per file part (old bootloaders)
        self.seq_len = 800            # parts per sequence
        self.seq_timeout = 30.0
        self.session = False

    # -- low level
    def _send(self, data, zlp=True):
        self.t.write(data)
        if zlp:
            self.t.zlp()  # a transfer that is a whole number of USB packets needs an empty one after it

    def _recv(self, n, timeout=6.0):
        got = bytearray()
        end = time.time() + timeout
        while len(got) < n:
            left = end - time.time()
            if left <= 0:
                raise OdinError("the phone didn't answer (timeout) - is it still in Download mode?")
            got += self.t.read(n - len(got), left)
        return bytes(got)

    def _response(self, kind, timeout=6.0):
        r = self._recv(8, timeout)
        t, value = struct.unpack("<Ii", r)
        if t != kind:
            raise OdinError("unexpected answer from the phone (0x%x where 0x%x was due)" % (t, kind))
        return value

    def handshake(self):
        self.t.write(b"ODIN")
        r = self._recv(4, 4.0)
        if r != b"LOKE":
            raise OdinError("this is not a Samsung in Download mode (answer %r)" % r)

    def begin(self):
        self.handshake()
        self._send(control(T_SESSION, SESSION_BEGIN))
        result = self._response(T_SESSION)
        self.session = True
        if result != 0:  # the bootloader can take bigger parts (Odin 3 era and later)
            self._send(control(T_SESSION, SESSION_PART_SIZE, 1024 * 1024))
            if self._response(T_SESSION) != 0:
                raise OdinError("the phone refused the 1 MiB part size")
            self.part_size, self.seq_len, self.seq_timeout = 1024 * 1024, 30, 120.0

    def total_bytes(self, n):
        """Odin tells the bootloader how much is coming; some bootloaders check it against the space."""
        self._send(control(T_SESSION, SESSION_TOTAL_BYTES, n))
        return self._response(T_SESSION) == 0

    def pit(self):
        self._send(control(T_PIT, PIT_DUMP))
        size = self._response(T_PIT)
        if size <= 0 or size > (1 << 20):
            raise OdinError("the phone gave no PIT (size %d)" % size)
        data = bytearray()
        for i in range((size + 499) // 500):
            self._send(control(T_PIT, PIT_PART, i))
            data += self._recv(min(500, size - len(data)))
        self._send(control(T_PIT, PIT_END, size))
        self._response(T_PIT)
        return bytes(data)

    def flash(self, data, entry, progress=None):
        """One file into one PIT partition."""
        dest = DEST_MODEM if entry.binary_type == 1 else DEST_PHONE
        self._send(control(T_FILE, FILE_FLASH))
        self._response(T_FILE)
        P, L, n = self.part_size, self.seq_len, len(data)
        per_seq = P * L
        seqs = max(1, (n + per_seq - 1) // per_seq)
        sent = 0
        for s in range(seqs):
            last = s == seqs - 1
            chunk = data[s * per_seq:(s + 1) * per_seq]
            parts = max(1, (len(chunk) + P - 1) // P)
            self._send(control(T_FILE, FILE_PART, parts * P))
            self._response(T_FILE)
            for i in range(parts):
                piece = chunk[i * P:(i + 1) * P]
                self._send(piece + bytes(P - len(piece)), zlp=False)  # parts are always whole; the end packet says how much counts
                got = self._response(0, self.seq_timeout)
                if got != i:
                    raise OdinError("the phone acknowledged part %d where %d was sent" % (got, i))
                sent += len(piece)
                if progress:
                    progress(sent, n)
            if dest == DEST_PHONE:
                end = control(T_FILE, FILE_END, DEST_PHONE, len(chunk), 0, entry.device_type, entry.identifier, 1 if last else 0)
            else:
                end = control(T_FILE, FILE_END, DEST_MODEM, len(chunk), 0, entry.device_type, 1 if last else 0)
            self._send(end)
            if self._response(T_FILE, self.seq_timeout) != 0:
                raise OdinError("the phone refused %s" % entry.name)

    def end(self, reboot=False):
        if not self.session:
            return
        self._send(control(T_END, END_SESSION))
        self._response(T_END)
        if reboot:
            self._send(control(T_END, END_REBOOT))
            try:
                self._response(T_END, 3.0)
            except OdinError:
                pass  # already restarting
        self.session = False


# ---- transports ------------------------------------------------------------------------------------------------

class WinSerial:
    """A COM port on Windows, through kernel32 (no pyserial needed)."""

    def __init__(self, port):
        import ctypes
        from ctypes import wintypes
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.ct, self.wt = ctypes, wintypes
        k = self.k
        k.CreateFileW.restype = wintypes.HANDLE
        k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                  wintypes.DWORD, wintypes.HANDLE]
        k.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        k.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_char_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        k.ReadFile.restype = k.WriteFile.restype = k.CloseHandle.restype = wintypes.BOOL
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        k.SetupComm.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD]
        k.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
        h = self.k.CreateFileW("\\\\.\\" + port, 0xC0000000, 0, None, 3, 0, None)  # GENERIC_READ|WRITE, OPEN_EXISTING
        if h is None or h == ctypes.c_void_p(-1).value:
            raise OdinError("can't open %s (Odin or another program may be using it)" % port)
        self.h = h
        self.k.SetupComm(h, 1 << 20, 1 << 20)
        k.EscapeCommFunction.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        k.EscapeCommFunction(h, 5)  # SETDTR: "a program is here" - some phones only talk once it is set
        k.EscapeCommFunction(h, 3)  # SETRTS
        k.PurgeComm.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        k.PurgeComm(h, 0x0F)        # nothing left over from an earlier program
        self.name = port

    def _timeouts(self, ms):
        class T(self.ct.Structure):
            _fields_ = [(n, self.wt.DWORD) for n in ("ri", "rm", "rc", "wm", "wc")]
        t = T(0xFFFFFFFF if ms == 0 else 0, 0, max(1, int(ms)) if ms else 0, 0, 20000)
        self.k.SetCommTimeouts(self.h, self.ct.byref(t))

    def read(self, n, timeout):
        self._timeouts(timeout * 1000)
        buf = self.ct.create_string_buffer(n)
        got = self.wt.DWORD(0)
        if not self.k.ReadFile(self.h, buf, n, self.ct.byref(got), None):
            raise OdinError("reading from %s failed (cable?)" % self.name)
        return buf.raw[:got.value]

    def write(self, data):
        self._timeouts(1)
        done = self.wt.DWORD(0)
        if not self.k.WriteFile(self.h, bytes(data), len(data), self.ct.byref(done), None) or done.value != len(data):
            raise OdinError("writing to %s failed (cable?)" % self.name)

    def zlp(self):
        pass  # Samsung's driver ends the transfer itself, as for Odin

    def close(self):
        self.k.CloseHandle(self.h)


class PosixSerial:
    def __init__(self, path):
        import termios
        import tty
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        a = termios.tcgetattr(self.fd)
        a[6][termios.VMIN], a[6][termios.VTIME] = 0, 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.name = path

    def read(self, n, timeout):
        import select
        r, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, n) if r else b""

    def write(self, data):
        view = memoryview(data)
        while view:
            k = os.write(self.fd, view)
            view = view[k:]

    def zlp(self):
        pass

    def close(self):
        os.close(self.fd)


class UsbTransport:
    """libusb through pyusb: the phone's CDC data interface, bulk in/out (what Heimdall uses)."""

    def __init__(self, dev):
        import usb.core
        import usb.util
        self.usb, self.dev = usb, dev
        cfg = dev.get_active_configuration()
        intf = next((i for i in cfg if i.bInterfaceClass == 0x0A), None)
        if intf is None:
            raise OdinError("the phone's USB data interface wasn't found")
        try:
            if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                dev.detach_kernel_driver(intf.bInterfaceNumber)
        except (NotImplementedError, usb.core.USBError):
            pass
        usb.util.claim_interface(dev, intf.bInterfaceNumber)
        try:
            dev.ctrl_transfer(0x21, 0x22, 0x3, 0, None, 1000)  # CDC SET_CONTROL_LINE_STATE: DTR + RTS
        except usb.core.USBError:
            pass
        self.intf = intf
        d = usb.util.ENDPOINT_IN
        self.ep_in = next(e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) == d)
        self.ep_out = next(e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) != d)
        self.name = "USB %04x:%04x" % (dev.idVendor, dev.idProduct)

    def read(self, n, timeout):
        try:
            return bytes(self.ep_in.read(max(n, 512), int(timeout * 1000)))[:n]
        except self.usb.core.USBTimeoutError:
            return b""

    def write(self, data):
        self.ep_out.write(data, 30000)

    def zlp(self):
        try:
            self.ep_out.write(b"", 1000)
        except self.usb.core.USBError:
            pass

    def close(self):
        self.usb.util.release_interface(self.dev, self.intf.bInterfaceNumber)


def _win_download_ports():
    """COM ports of USB devices with a Download-mode id (VID 04E8, PID 685D...), from the registry - found without
    sending anything. Only ports that exist now (SERIALCOMM lists the present ones)."""
    ports = []
    try:
        import winreg
        present = set()
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
        i = 0
        while True:
            try:
                present.add(winreg.EnumValue(k, i)[1].upper())
            except OSError:
                break
            i += 1
        usb_root = r"SYSTEM\CurrentControlSet\Enum\USB"
        names = []
        try:
            root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, usb_root)
            n = 0
            while True:
                try:
                    names.append(winreg.EnumKey(root, n))
                except OSError:
                    break
                n += 1
        except OSError:
            pass
        wanted = tuple("VID_%04X&PID_%04X" % (SAMSUNG_VID, pid) for pid in DOWNLOAD_PIDS)
        for name in names:  # the device itself, or its interface child (...&MI_01)
            if not name.upper().startswith(wanted):
                continue
            base = usb_root + "\\" + name
            try:
                dev = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base)
            except OSError:
                continue
            j = 0
            while True:
                try:
                    inst = winreg.EnumKey(dev, j)
                except OSError:
                    break
                j += 1
                for sub in ("Device Parameters",):
                    try:
                        dp = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base + "\\" + inst + "\\" + sub)
                        port = winreg.QueryValueEx(dp, "PortName")[0]
                        if port.upper() in present and port not in ports:
                            ports.append(port)
                    except OSError:
                        pass
    except (OSError, ImportError):
        pass
    return ports


def present():
    """Is a phone in Download mode connected? Looks at USB ids only: nothing is sent (the bootloader takes the ODIN
    handshake once per session - probing with it would break the flash that follows)."""
    if os.name == "nt":
        return bool(_win_download_ports())
    try:
        import usb.core
        return any(usb.core.find(idVendor=SAMSUNG_VID, idProduct=pid) is not None for pid in DOWNLOAD_PIDS)
    except Exception:
        pass
    for dev in glob.glob("/sys/bus/usb/devices/*"):
        try:
            vid = open(os.path.join(dev, "idVendor")).read().strip()
            pid = open(os.path.join(dev, "idProduct")).read().strip()
        except OSError:
            continue
        if vid == "%04x" % SAMSUNG_VID and int(pid, 16) in DOWNLOAD_PIDS:
            return True
    return False


def candidates():
    """Where a phone in Download mode can be: (label, opener)."""
    out = []
    if os.name == "nt":
        for port in _win_download_ports():
            out.append((port, lambda p=port: WinSerial(p)))
        if out:
            return out
        try:
            import winreg
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
            i = 0
            while True:
                try:
                    name, port, _ = winreg.EnumValue(k, i)
                except OSError:
                    break
                i += 1
                low = name.lower()
                if "ssud" in low or "usbser" in low or "samsung" in low or "acm" in low:  # Samsung's modem driver
                    out.append((port, lambda p=port: WinSerial(p)))
        except OSError:
            pass
        return out
    try:
        import usb.core
        for pid in DOWNLOAD_PIDS:
            for dev in usb.core.find(find_all=True, idVendor=SAMSUNG_VID, idProduct=pid) or []:
                out.append(("USB %04x:%04x" % (SAMSUNG_VID, pid), lambda d=dev: UsbTransport(d)))
    except Exception:
        pass  # no pyusb / libusb: the serial devices below
    for path in sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/cu.usbmodem*")):
        out.append((path, lambda p=path: PosixSerial(p)))
    return out


def connect(port=None, log=print):
    """Open the phone in Download mode: the port given, or the first one that answers the handshake."""
    tried = []
    cands = [(port, (lambda: WinSerial(port)) if os.name == "nt" else (lambda: PosixSerial(port)))] if port else candidates()
    for label, opener in cands:
        try:
            t = opener()
        except Exception as e:
            tried.append("%s (%s)" % (label, e))
            continue
        o = Odin(t, log)
        try:
            o.handshake()
            return o
        except OdinError as e:
            tried.append("%s (%s)" % (label, e))
            t.close()
    hint = ("Install Samsung's USB driver, close Odin / Smart Switch, and check the phone shows 'Downloading...'"
            if os.name == "nt" else "Is it in Download mode, and may you use USB devices (udev rule / sudo)?")
    raise OdinError("no phone in Download mode found%s. %s" % ((": tried " + ", ".join(tried)) if tried else "", hint))


# ---- files -------------------------------------------------------------------------------------------------------

def images_from(path):
    """[(name, bytes)]: an image, or every file in an Odin .tar / .tar.md5 (MD5 checked)."""
    with open(path, "rb") as f:
        head = f.read(512)
    if len(head) >= 262 and head[257:262] == b"ustar":
        data = open(path, "rb").read()
        body = data[:len(data) - len(data) % 512]
        line = data[len(body):].strip()
        if line and hashlib.md5(body).hexdigest() != line.split()[0].decode("ascii", "replace").lower():
            raise OdinError("%s is damaged: its MD5 doesn't match" % os.path.basename(path))
        with tarfile.open(fileobj=io.BytesIO(body)) as t:
            return [(m.name, t.extractfile(m).read()) for m in t.getmembers() if m.isfile()]
    with open(path, "rb") as f:
        return [(os.path.basename(path), f.read())]


def flash_files(files, partition=None, reboot=False, port=None, log=print, odin=None):
    """Flash [(name, bytes)]: the PIT decides where each goes (or `partition` for a single file). Nothing is
    written unless every file has a partition."""
    o = odin or connect(port, log)
    try:
        log("Phone in Download mode (%s)" % getattr(o.t, "name", "?"))
        o.begin()
        o.total_bytes(sum(len(d) for _, d in files))  # before the PIT, in Heimdall's order (known to work on the S8)
        entries = parse_pit(o.pit())
        plan = []
        for name, data in files:
            if name.endswith(".lz4"):
                raise OdinError("%s is LZ4-packed: unpack it first (pc/fmrre_mkrecovery.py does it for recoveries)" % name)
            e = pit_target(entries, name, partition if len(files) == 1 else None)
            if e is None:
                raise OdinError("the phone has no partition for %s - nothing was written" % name)
            if e.name.upper() in ("BOOTLOADER", "SBOOT", "PARAM", "UP_PARAM", "CM", "KEYSTORAGE", "PIT") and not os.environ.get("FMRRE_ODIN_ANY"):
                raise OdinError("%s goes to %s (bootloader area) - not flashed by this tool; use Odin with the full firmware" % (name, e.name))
            plan.append((name, data, e))
        for name, data, e in plan:
            log("Flashing %s -> %s (%.1f MB)" % (name, e.name, len(data) / 1048576.0))
            last = [0]

            def progress(done, total, last=last):
                pct = done * 100 // max(total, 1)
                if pct >= last[0] + 10 or done == total:
                    last[0] = pct
                    log("  %d%%" % pct)
            o.flash(data, e, progress)
        o.end(reboot)
        log("Done%s." % (" - the phone restarts" if reboot else ""))
        return True
    except BaseException:
        try:
            if o.session:
                o.end(False)  # leave the bootloader in a clean state; it shows the failure on screen
        except Exception:
            pass
        raise
    finally:
        o.t.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("detect", "pit", "flash"):
        p = sub.add_parser(c)
        p.add_argument("--port", help="COM5 / /dev/ttyACM0 (default: found automatically)")
        if c == "pit":
            p.add_argument("-o", "--out")
        if c == "flash":
            p.add_argument("file")
            p.add_argument("--partition", help="PIT partition name (default: from the file name, as Odin does)")
            p.add_argument("--reboot", action="store_true", help="restart the phone afterwards (default: stay, to boot recovery by hand)")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "detect":
            if not present():
                print("No phone in Download mode")
                return 1
            print("Phone in Download mode%s" % ("".join(" on " + p for p in _win_download_ports()[:1]) if os.name == "nt" else ""))
        elif a.cmd == "pit":
            o = connect(a.port)
            try:
                o.begin()
                data = o.pit()
                o.end(False)
            finally:
                o.t.close()
            for e in parse_pit(data):
                print("  %-16s id %-3d %-24s %s" % (e.name, e.identifier, e.flash_name or "-", "CP" if e.binary_type == 1 else "AP"))
            if a.out:
                open(a.out, "wb").write(data)
        else:
            flash_files(images_from(a.file), a.partition, a.reboot, a.port)
    except OdinError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
