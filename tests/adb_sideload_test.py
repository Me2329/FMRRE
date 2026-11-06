#!/usr/bin/env python3
"""End-to-end test of FMRRE's own adb sideload (no minadbd) with the real adb client, in the simulator over TCP.

Both ways the package can reach the installer: read from the computer while it installs, through FUSE (nothing
stored - where this machine lets the test mount FUSE, i.e. as root), and stored first (FMRRE_SIDELOAD_STORE=1, what
happens where the kernel has no FUSE):
  - `adb sideload` of a package: listed as a sideload device, served block by block, installed, and adb exits 0
  - a package that fails to install: adb exits non-zero (FAILFAIL)
  - a transfer the computer stops halfway
  - FUSE: the installer reads the zip's end first (its directory), blocks are read as needed
Then with a small adb host of our own: the old `sideload:SIZE` protocol (the file pushed), a service FMRRE doesn't
offer (refused), and `adb reboot bootloader`.

    tests/adb_sideload_test.py [path/to/fmrre]      (default: build-host/fmrre)
"""
import io
import os
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build-host/fmrre')
failed = 0


def check(cond, what, out=''):
    global failed
    print(('ok    ' if cond else 'FAIL  ') + what)
    if not cond:
        failed += 1
        if out:
            print('      ' + out.strip().replace('\n', '\n      ')[:3000])


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def package(text, size=0, broken=False):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('META-INF/com/google/android/update-binary', '#!/sbin/sh\n')
        z.writestr('META-INF/com/google/android/updater-script', 'ui_print("%s");\n' % text)
        z.writestr('system.new.dat', os.urandom(size))
    data = bytearray(buf.getvalue())
    if broken:  # a changed byte in the stored data: the CRC check refuses it
        data[200] ^= 0xFF
    return bytes(data)


class Device:
    """`fmrre sideload --tcp PORT` in the simulator."""

    def __init__(self, root, store=False):
        self.port = free_port()
        env = dict(os.environ)
        if store:
            env['FMRRE_SIDELOAD_STORE'] = '1'
        self.p = subprocess.Popen([BIN, '--sim', '--sim-root', root, 'fmrre', 'sideload', '--tcp', str(self.port)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        for _ in range(100):  # until it listens
            try:
                socket.create_connection(('127.0.0.1', self.port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        # that probe connection was a "computer" that left at once: the device waits for the next one

    def finish(self, timeout=60):
        try:
            out, _ = self.p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            out, _ = self.p.communicate()
        return self.p.returncode, '\n'.join(l for l in out.split('\n') if not l.startswith('['))


def adb(*args, timeout=60):
    r = subprocess.run(['adb'] + list(args), capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


# ---- a minimal adb host (the computer's side) for what the real client doesn't do any more ----
A_CNXN, A_OPEN, A_OKAY, A_CLSE, A_WRTE = 0x4e584e43, 0x4e45504f, 0x59414b4f, 0x45534c43, 0x45545257


class Host:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
        self.send(A_CNXN, 0x01000001, 256 * 1024, b'host::\0')
        cmd, a0, a1, data = self.recv()
        assert cmd == A_CNXN, hex(cmd)
        self.banner = data

    def send(self, cmd, a0, a1, data=b''):
        self.s.sendall(struct.pack('<6I', cmd, a0, a1, len(data), sum(data) & 0xFFFFFFFF, cmd ^ 0xFFFFFFFF) + data)

    def recv(self):
        h = b''
        while len(h) < 24:
            c = self.s.recv(24 - len(h))
            if not c:
                raise EOFError
            h += c
        cmd, a0, a1, n, _, magic = struct.unpack('<6I', h)
        assert magic == cmd ^ 0xFFFFFFFF
        data = b''
        while len(data) < n:
            data += self.s.recv(n - len(data))
        return cmd, a0, a1, data


def ab_ota(path):
    """A small A/B OTA zip (payload.bin: boot, and a dynamic system with a verity hash tree), from tests/payload_test.py."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import payload_test as pt
    rng = random.Random(11)
    def blocks(n):
        out = bytearray()
        for i in range(n):
            out += bytes(rng.choice(b'abcdefgh\x00\xff') for _ in range(64)) * 64 if rng.random() < 0.5 else bytes(pt.BS)
        return bytes(out)
    parts = [pt.Part('boot', b'ANDROID!' + blocks(48)[8:], [(pt.REPLACE_XZ, 0, 24, None), (pt.REPLACE, 24, 24, None)]),
             pt.Part('system', blocks(256), [(pt.REPLACE_BZ, 0, 128, None), (pt.REPLACE_XZ, 128, 128, None)], verity=True, dynamic=True)]
    pt.write_zip(path, pt.build_payload(parts))
    return [(p.name, p.image, p.dynamic) for p in parts]


def can_fuse(work):
    """Whether FUSE can be mounted here (the device side needs that; the test runs it as this user)."""
    import ctypes
    if not os.path.exists('/dev/fuse'):
        return False
    d = os.path.join(work, 'fuse-probe')
    os.makedirs(d, exist_ok=True)
    libc = ctypes.CDLL(None, use_errno=True)
    try:
        fd = os.open('/dev/fuse', os.O_RDWR)
    except OSError:
        return False
    try:
        opts = ('fd=%d,rootmode=40000,user_id=%d,group_id=%d' % (fd, os.getuid(), os.getgid())).encode()
        if libc.mount(b'probe', d.encode(), b'fuse', ctypes.c_ulong(6), opts) != 0:
            return False
        libc.umount2(d.encode(), 2)
        return True
    finally:
        os.close(fd)


def main():
    if not shutil.which('adb'):
        print('the adb tool is needed')
        return 2
    work = tempfile.mkdtemp(prefix='fmrre-adb-')
    root = os.path.join(work, 'sim')
    subprocess.run([BIN, '--sim', '--sim-root', root, 'fmrre', 'help'], capture_output=True)
    adb('start-server')
    modes = ['store']
    if can_fuse(work):
        modes.insert(0, 'fuse')
    else:
        print('(FUSE: this machine doesn\'t let the test mount it - only the stored way is tested)')
    try:
        for mode in modes:
            store = mode == 'store'
            tag = '[%s] ' % ('stored first' if store else 'FUSE, nothing stored')
            # 1. a package that installs
            good = os.path.join(work, 'good.zip')
            with open(good, 'wb') as f:
                f.write(package('hello from adb sideload', 3 << 20))
            d = Device(root, store)
            rc, out = adb('connect', '127.0.0.1:%d' % d.port)
            rc2, devs = adb('devices', '-l')
            check(('127.0.0.1:%d' % d.port) in devs and 'sideload' in devs, tag + 'adb lists the recovery as a sideload device', devs)
            rc, out = adb('-s', '127.0.0.1:%d' % d.port, 'sideload', good)
            drc, dout = d.finish()
            check(rc == 0 and 'Total xfer' in out, tag + 'adb sideload of 3 MB: served block by block, adb exits 0', out + dout)
            how = 'Received 3.00 MB' if store else 'Reading the 3.00 MB package from the computer as it installs'
            check(drc == 0 and how in dout and 'hello from adb sideload' in dout and 'Zip verified OK' in dout,
                  tag + 'the recovery got it whole and installed it', dout)
            if not store:
                check('/sideload/package.zip' in dout and not os.path.ismount(os.path.join(root, 'sideload')),
                      tag + 'installed from /sideload/package.zip, unmounted afterwards', dout)
            adb('disconnect', '127.0.0.1:%d' % d.port)
            # 1b. an A/B OTA (payload.bin): installed to the other slot, the payload read as it is written
            ota = os.path.join(work, 'ota.zip')
            parts = ab_ota(ota)
            d = Device(root, store)
            adb('connect', '127.0.0.1:%d' % d.port)
            rc, out = adb('-s', '127.0.0.1:%d' % d.port, 'sideload', ota)
            drc, dout = d.finish(timeout=120)
            m = re.search(r'OTA installed to slot ([AB])', dout)
            same = bool(m)
            for name, image, dynamic in parts:
                if m:
                    dev = os.path.join(root, 'dev/block/mapper' if dynamic else 'dev/block/by-name', '%s_%s' % (name, m.group(1).lower()))
                    same = same and os.path.exists(dev) and open(dev, 'rb').read()[:len(image)] == image
            check(rc == 0 and drc == 0 and same, tag + 'an A/B OTA (payload.bin) installs to the other slot, byte for byte', out + dout[-1500:])
            adb('disconnect', '127.0.0.1:%d' % d.port)
            # 2. a package that fails to install: the computer is told
            bad = os.path.join(work, 'bad.zip')
            with open(bad, 'wb') as f:
                f.write(package('never shown', 100000, broken=True))
            d = Device(root, store)
            adb('connect', '127.0.0.1:%d' % d.port)
            rc, out = adb('-s', '127.0.0.1:%d' % d.port, 'sideload', bad)
            drc, dout = d.finish()
            check(rc != 0 and drc != 0 and 'CORRUPT' in dout, tag + 'a package that fails: adb exits non-zero (FAILFAIL)', out + dout)
            adb('disconnect', '127.0.0.1:%d' % d.port)
            # 3. the computer stops halfway (our own host: serves two blocks of sideload-host, then closes the stream)
            d = Device(root, store)
            h = Host(d.port)
            size, bs = 10 * 65536, 65536
            h.send(A_OPEN, 5, 0, b'sideload-host:%d:%d\0' % (size, bs))
            cmd, local, _, _ = h.recv()
            served = []
            while cmd == A_OKAY and len(served) < 2:
                cmd, _, _, data = h.recv()
                if cmd != A_WRTE:
                    break
                h.send(A_OKAY, 5, local)
                served.append(int(data))
                h.send(A_WRTE, 5, local, bytes(bs))
                cmd, _, _, _ = h.recv()  # its OKAY
            h.send(A_CLSE, 5, local)
            drc, dout = d.finish(timeout=30)
            if store:
                check(served == [0, 1] and drc != 0 and 'transfer stopped after 128 KB of 640 KB' in dout,
                      tag + 'the computer stops halfway: blocks asked for in order, nothing installed', str(served) + dout)
            else:
                check(len(served) == 2 and served[0] == 9 and drc != 0 and 'Installing zip' in dout,
                      tag + 'the computer stops halfway: the zip\'s end read first, nothing installed', str(served) + dout)
        # 4. the old protocol (the whole file pushed in WRTEs)
        d = Device(root)
        h = Host(d.port)
        check(h.banner.startswith(b'sideload::') and b'ro.product.model=' in h.banner, 'banner: sideload state, product info',
              repr(h.banner))
        h.send(A_OPEN, 7, 0, b'shell:id\0')  # not offered in sideload mode
        cmd, a0, a1, _ = h.recv()
        check(cmd == A_CLSE and a1 == 7, 'other services are refused', hex(cmd))
        data = package('pushed the old way', 70000)
        h.send(A_OPEN, 8, 0, b'sideload:%d\0' % len(data))
        cmd, local, a1, _ = h.recv()
        ok = cmd == A_OKAY and a1 == 8
        for i in range(0, len(data), 4096):
            h.send(A_WRTE, 8, local, data[i:i + 4096])
            cmd, _, _, _ = h.recv()
            ok = ok and cmd == A_OKAY
        drc, dout = d.finish()
        check(ok and drc == 0 and 'pushed the old way' in dout, 'the old sideload protocol (the file pushed)', dout)
        # 5. adb reboot bootloader while waiting
        d = Device(root)
        h = Host(d.port)
        h.send(A_OPEN, 9, 0, b'reboot:bootloader\0')
        cmd, _, _, _ = h.recv()
        drc, dout = d.finish()
        check(cmd == A_OKAY and 'reboot to bootloader' in dout, 'adb reboot bootloader', dout)
    finally:
        adb('kill-server')
        shutil.rmtree(work, ignore_errors=True)
    print('%s: %d failure(s)' % ('FAILED' if failed else 'passed', failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
