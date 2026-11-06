#!/usr/bin/env python3
"""End-to-end test of FMRRE's built-in Zstandard decoder against the real zstd tool, in the simulator.

Text-like, random, and mixed images (zeros, noise, repeats) are compressed with the zstd tool at many levels and
options - fast and max levels, --ultra -22, --long windows, no checksum, multithreaded frames, streamed from
stdin (no content size), concatenated files - and `fmrre unpack` must give the exact original back. Also:
flashing an .img.zst to a partition, a damaged and a cut-off file, and an image inside a zip.

    tests/zstd_test.py [path/to/fmrre]      (default: build-host/fmrre)
"""
import hashlib
import os
import random
import shutil
import subprocess
import sys
import tempfile
import zipfile

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build-host/fmrre')
failed = 0


def check(cond, what, out=''):
    global failed
    print(('ok    ' if cond else 'FAIL  ') + what)
    if not cond:
        failed += 1
        if out:
            print('      ' + out.strip().replace('\n', '\n      ')[:2000])


def cli(root, *args):
    r = subprocess.run([BIN, '--sim', '--sim-root', root, 'fmrre', *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    if not shutil.which('zstd'):
        print('the zstd tool is needed')
        return 2
    work = tempfile.mkdtemp(prefix='fmrre-zstd-')
    root = os.path.join(work, 'sim')
    try:
        cli(root, 'help')  # creates the sandbox
        dl = os.path.join(root, 'data/media/0/Download')
        r = random.Random(7)
        words = [b'boot', b'system', b'vendor', b'slot', b'super', b'image', b'\n', b'FMRRE', b'lib', b'apk', b'odex']
        text = b' '.join(r.choice(words) for _ in range(300000))
        mixed = bytearray()
        for k in range(48):
            mixed += (bytes(r.randrange(256) for _ in range(4000)) if k % 3 == 0 else
                      bytes(90000) if k % 3 == 1 else text[k * 20000:(k + 1) * 20000])
        images = {'text': text, 'random': os.urandom(600000), 'mixed': bytes(mixed), 'tiny': b'FMRRE'}
        for name, data in images.items():
            with open(os.path.join(dl, name + '.img'), 'wb') as f:
                f.write(data)
        options = ['-1', '-3', '-9', '-19', '--ultra -22', '--long=25 -12', '--no-check -5', '-T4 -6', '--fast=5']

        def roundtrip(name, data, zst):
            rc, out = cli(root, 'unpack', '/sdcard/Download/' + os.path.basename(zst), '/sdcard/Download/out.img')
            with open(os.path.join(dl, 'out.img'), 'rb') as f:
                got = f.read() if rc == 0 else b''
            return rc == 0 and hashlib.sha256(got).digest() == hashlib.sha256(data).digest(), out

        for name, data in images.items():
            src = os.path.join(dl, name + '.img')
            bad = []
            for opt in options:
                zst = os.path.join(dl, name + '.img.zst')
                subprocess.run(['zstd', '-q', '-f'] + opt.split() + ['-o', zst, src], check=True)
                ok, out = roundtrip(name, data, zst)
                if not ok:
                    bad.append(opt + ': ' + out.strip().split('\n')[-1])
            check(not bad, '%s (%d bytes) at %d levels / options' % (name, len(data), len(options)), '\n'.join(bad))
        # An image that decodes to nothing can't be flashed or unpacked: said so, in every format.
        with open(os.path.join(dl, 'empty.img'), 'wb'):
            pass
        subprocess.run(['zstd', '-q', '-f', '-o', os.path.join(dl, 'empty.img.zst'), os.path.join(dl, 'empty.img')], check=True)
        rc, out = cli(root, 'unpack', '/sdcard/Download/empty.img.zst', '/sdcard/Download/out.img')
        check(rc != 0 and 'empty file' in out, 'an empty image is refused', out)
        # Streamed (no content size in the header) and concatenated frames
        zst = os.path.join(dl, 'stream.img.zst')
        with open(os.path.join(dl, 'mixed.img'), 'rb') as f_in, open(zst, 'wb') as f_out:
            subprocess.run(['zstd', '-q', '-c', '-7'], stdin=f_in, stdout=f_out, check=True)
        ok, out = roundtrip('stream', images['mixed'], zst)
        check(ok, 'compressed from a pipe (no content size)', out)
        cat = os.path.join(dl, 'cat.img.zst')
        with open(cat, 'wb') as f:
            for name in ('text', 'random', 'tiny'):
                f.write(subprocess.run(['zstd', '-q', '-c', '-3', os.path.join(dl, name + '.img')], capture_output=True,
                                       check=True).stdout)
        ok, out = roundtrip('cat', images['text'] + images['random'] + images['tiny'], cat)
        check(ok, 'three frames one after another', out)
        # Flash to a partition (the simulated dtbo_b is 1 MiB), with read-back verify
        part = images['mixed'][:900000]
        with open(os.path.join(dl, 'dtbo.img'), 'wb') as f:
            f.write(part)
        subprocess.run(['zstd', '-q', '-f', '-19', '-o', os.path.join(dl, 'dtbo.img.zst'), os.path.join(dl, 'dtbo.img')], check=True)
        rc, out = cli(root, 'flash', '/sdcard/Download/dtbo.img.zst', 'dtbo_b')
        with open(os.path.join(root, 'dev/block/by-name/dtbo_b'), 'rb') as f:
            got = f.read(len(part))
        check(rc == 0 and got == part, 'flash dtbo.img.zst to dtbo_b (no zstd tool needed)', out)
        # Damaged and cut off
        with open(os.path.join(dl, 'dtbo.img.zst'), 'rb') as f:
            z = bytearray(f.read())
        z[len(z) // 2] ^= 0x40
        with open(os.path.join(dl, 'damaged.img.zst'), 'wb') as f:
            f.write(z)
        rc, out = cli(root, 'unpack', '/sdcard/Download/damaged.img.zst', '/sdcard/Download/out.img')
        check(rc != 0 and 'zstd:' in out and not os.path.exists(os.path.join(dl, 'out.img')), 'a damaged file is refused', out)
        with open(os.path.join(dl, 'cut.img.zst'), 'wb') as f:
            f.write(bytes(z[:len(z) // 3]))
        rc, out = cli(root, 'unpack', '/sdcard/Download/cut.img.zst', '/sdcard/Download/out.img')
        check(rc != 0 and 'cut off' in out, 'a cut-off download is refused', out)
        # The image inside a zip
        with zipfile.ZipFile(os.path.join(dl, 'images.zip'), 'w', zipfile.ZIP_DEFLATED) as zf:
            zf.writestr('readme.txt', 'not an image')
            zf.writestr('boot.img', images['text'][:200000])
        rc, out = cli(root, 'unpack', '/sdcard/Download/images.zip', '/sdcard/Download/out.img')
        with open(os.path.join(dl, 'out.img'), 'rb') as f:
            got = f.read() if rc == 0 else b''
        check(rc == 0 and got == images['text'][:200000], 'unpack the .img inside a zip', out)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('%s: %d failure(s)' % ('FAILED' if failed else 'passed', failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
