#!/usr/bin/env python3
"""End-to-end test of compressed backups, in the simulator.

FMRRE compresses on every core (pigz-style: 1 MiB pieces joined into one deflate stream). The archive must be a
single ordinary gzip member that zlib, Python's gzip module and `gzip -t` read, holding the files byte for byte.
On restore every gzip member is read (some tools write several back to back - FMRRE used to stop after the first
one) and each one's CRC-32 and length are checked, so a damaged archive is refused even when the stored digests
were made to match it; zero padding after the end is fine, other bytes after it are not.

    tests/backup_test.py [path/to/fmrre]      (default: build-host/fmrre)
"""
import glob
import gzip
import hashlib
import io
import os
import random
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zlib

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build-host/fmrre')
failed = 0


def check(cond, what, out=''):
    global failed
    print(('ok    ' if cond else 'FAIL  ') + what)
    if not cond:
        failed += 1
        if out:
            print('      ' + str(out).strip().replace('\n', '\n      ')[:3000])


def cli(root, *args):
    r = subprocess.run([BIN, '--sim', '--sim-root', root, 'fmrre', *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def fix_digest(path):
    """Make the backup's stored SHA-256 match the changed archive: only the gzip checks can catch damage then."""
    folder, name = os.path.split(path)
    sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    man = os.path.join(folder, 'fmrre.manifest')
    lines = []
    for line in open(man).read().split('\n'):
        if line.startswith('item='):
            f = line[5:].split('|')
            if f[1] == name:
                f[5] = sha
            line = 'item=' + '|'.join(f)
        lines.append(line)
    open(man, 'w').write('\n'.join(lines))
    if os.path.exists(path + '.sha2'):
        open(path + '.sha2', 'w').write(sha + '  ' + name + '\n')


def main():
    top = tempfile.mkdtemp(prefix='fmrre-backup-')
    sim = os.path.join(top, 'sim')
    cli(sim, 'help')
    data = os.path.join(sim, 'data', 'backuptest')
    os.makedirs(data)
    rnd = random.Random(7)
    files = {  # about 7 MB: text that compresses, random bytes that don't, zeros - several 1 MiB pieces
        'text.txt': ''.join('line %d: the quick brown fox %f\n' % (i, rnd.random()) for i in range(90000)).encode(),
        'random.bin': bytes(rnd.getrandbits(8) for _ in range(1500000)),
        'zeros.bin': bytes(2000000),
        'one': b'x',
        'empty': b'',
    }
    for n, b in files.items():
        open(os.path.join(data, n), 'wb').write(b)

    def restored():
        return all(os.path.exists(os.path.join(data, n)) and open(os.path.join(data, n), 'rb').read() == b
                   for n, b in files.items())

    rc, out = cli(sim, 'backup', 'DO', 'gz1')
    check(rc == 0 and 'gzip on' in out, 'a compressed backup of /data, on every core', out)
    arcs = glob.glob(os.path.join(sim, '**', 'gz1', 'data.*.win.gz'), recursive=True)
    check(len(arcs) == 1, 'one data archive', arcs)
    if not arcs:
        return
    arc = arcs[0]
    raw = open(arc, 'rb').read()
    z = zlib.decompressobj(31)
    plain = z.decompress(raw) + z.flush()
    check(z.eof and z.unused_data == b'', 'a single gzip member that zlib reads to its end (%d -> %d bytes)' % (len(plain), len(raw)))
    check(gzip.decompress(raw) == plain, "Python's gzip module reads it")
    t = tarfile.open(fileobj=io.BytesIO(plain))
    names = {m.name: m for m in t.getmembers()}
    same = all(('./backuptest/' + n) in names and (t.extractfile(names['./backuptest/' + n]).read() == b)
               for n, b in files.items())
    check(same, 'the archive holds the files, byte for byte')
    if shutil.which('gzip'):
        r = subprocess.run(['gzip', '-t', arc], capture_output=True, text=True)
        check(r.returncode == 0, 'gzip -t accepts it', r.stderr)

    shutil.rmtree(data)
    rc, out = cli(sim, 'restore', 'gz1', 'D')
    check(rc == 0 and restored(), 'restore', out)

    # Three members back to back, cut anywhere: everything comes back.
    a, b = len(plain) // 3 + 17, 2 * len(plain) // 3 + 5
    open(arc, 'wb').write(gzip.compress(plain[:a]) + gzip.compress(plain[a:b]) + gzip.compress(plain[b:]))
    fix_digest(arc)
    shutil.rmtree(data)
    rc, out = cli(sim, 'restore', 'gz1', 'D')
    check(rc == 0 and restored(), 'an archive of three gzip members restores completely', out)

    # Zero padding after the end is fine.
    open(arc, 'wb').write(raw + bytes(4096))
    fix_digest(arc)
    rc, out = cli(sim, 'restore', 'gz1', 'D')
    check(rc == 0 and restored(), 'zero padding after the end is allowed', out)

    for what, damaged, expect in (
            ('a wrong CRC in the trailer', raw[:-8] + bytes([raw[-8] ^ 0xFF]) + raw[-7:], 'CRC mismatch'),
            ('a wrong length in the trailer', raw[:-4] + bytes([raw[-4] ^ 0x01]) + raw[-3:], 'length mismatch'),
            ('a cut-off archive', raw[:len(raw) // 2], 'cut off'),
            ('bytes after the end', raw + b'junk', 'after the end'),
            ('a damaged deflate stream', raw[:30] + bytes([raw[30] ^ 0xFF]) + raw[31:], 'gzip:')):
        open(arc, 'wb').write(damaged)
        fix_digest(arc)
        rc, out = cli(sim, 'restore', 'gz1', 'D')
        check(rc != 0 and expect in out, '%s is refused (%s)' % (what, expect), out)

    shutil.rmtree(top, ignore_errors=True)


if __name__ == '__main__':
    main()
    print('passed' if not failed else '%d FAILED' % failed)
    sys.exit(1 if failed else 0)
