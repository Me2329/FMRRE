#!/usr/bin/env python3
"""End-to-end test of FMRRE's built-in tar (what backups and restores use) against GNU tar, in the simulator.

A tree with regular files (empty, 512 / 513 bytes, 1 MB), nested and long names (a 120-character name, a path
over 300 characters), names with spaces and non-ASCII letters, symlinks, a hard link, a FIFO, special modes,
set times and extended attributes (user.*, and security.selinux where this machine lets it be set) goes:
  FMRRE -> FMRRE, FMRRE -> GNU tar, GNU tar (pax, --xattrs) -> FMRRE, GNU tar (gnu format, long names) -> FMRRE,
and must come out the same each time. Then archives that try to write outside the folder ("..", absolute names,
a path through a symlink, a hard link out) are refused, and a classic TWRP backup's absolute names ("/data/app/x")
land inside the partition.

    tests/tar_test.py [path/to/fmrre]      (default: build-host/fmrre)
"""
import hashlib
import io
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build-host/fmrre')
ROOT = os.geteuid() == 0
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


def can_xattr(d, key, value):
    p = os.path.join(d, '.probe')
    open(p, 'w').close()
    try:
        os.setxattr(p, key, value)
        return True
    except OSError:
        return False
    finally:
        os.remove(p)


def make_tree(d, xattr_keys):
    os.makedirs(d)
    w = lambda rel, data: open(os.path.join(d, rel), 'wb').write(data)
    os.makedirs(os.path.join(d, 'app/com.example.app/lib/arm64'))
    os.makedirs(os.path.join(d, 'misc/vold'))
    w('empty', b'')
    w('block512', b'x' * 512)
    w('block513', b'y' * 513)
    w('app/com.example.app/base.apk', os.urandom(1 << 20))
    w('app/com.example.app/lib/arm64/libfoo.so', b'\x7fELF' + os.urandom(3000))
    w('spaces and ünïcode ✓.txt', 'hello'.encode())
    long_dir = os.path.join(d, 'deep/' + '/'.join('folder%02d_with_a_long_name' % i for i in range(9)))
    os.makedirs(long_dir)
    open(os.path.join(long_dir, 'n' * 120), 'wb').write(b'a long path')
    os.symlink('base.apk', os.path.join(d, 'app/com.example.app/link-relative'))
    os.symlink('/system/bin/sh', os.path.join(d, 'misc/link-absolute'))
    os.link(os.path.join(d, 'block513'), os.path.join(d, 'misc/hardlink'))
    os.mkfifo(os.path.join(d, 'misc/fifo'))
    os.chmod(os.path.join(d, 'misc/vold'), 0o700)
    os.chmod(os.path.join(d, 'app/com.example.app/lib/arm64/libfoo.so'), 0o755)
    os.chmod(os.path.join(d, 'empty'), 0o600)
    os.makedirs(os.path.join(d, 'sticky'))
    os.chmod(os.path.join(d, 'sticky'), 0o1777)
    if ROOT:
        os.chown(os.path.join(d, 'app/com.example.app/base.apk'), 1000, 1000)
        os.chown(os.path.join(d, 'misc/vold'), 1013, 1013)
    for rel in ('app/com.example.app/base.apk', 'misc/vold', 'empty', 'deep'):
        for k in xattr_keys:
            v = b'u:object_r:apk_data_file:s0\0' if k == 'security.selinux' else b'value of ' + rel.encode()
            os.setxattr(os.path.join(d, rel), k, v)
    # times last (contents first, then the folders, deepest first)
    for dirpath, dirs, files in os.walk(d, topdown=False):
        for n in files + dirs:
            os.utime(os.path.join(dirpath, n), (1700000000, 1700000000 + len(n)), follow_symlinks=False)
    os.utime(d, (1700000000, 1700000000))


def snapshot(d, xattr_keys, times=True):
    out, inodes = {}, {}
    for dirpath, dirs, files in os.walk(d):
        for n in dirs + files:
            p = os.path.join(dirpath, n)
            rel = os.path.relpath(p, d)
            st = os.lstat(p)
            e = {'type': stat.S_IFMT(st.st_mode), 'mode': stat.S_IMODE(st.st_mode)}
            if times and not stat.S_ISLNK(st.st_mode):
                e['mtime'] = int(st.st_mtime)
            if ROOT:
                e['owner'] = (st.st_uid, st.st_gid)
            if stat.S_ISREG(st.st_mode):
                e['sha'] = hashlib.sha256(open(p, 'rb').read()).hexdigest()
                if st.st_nlink > 1:
                    inodes.setdefault(st.st_ino, []).append(rel)
            if stat.S_ISLNK(st.st_mode):
                e['link'] = os.readlink(p)
            else:
                xa = {}
                for k in xattr_keys:
                    try:
                        xa[k] = os.getxattr(p, k, follow_symlinks=False).rstrip(b'\0')
                    except OSError:
                        pass
                if xa:
                    e['xattrs'] = xa
            out[rel] = e
    out['<hard links>'] = sorted(sorted(v) for v in inodes.values())
    return out


def diff(a, b):
    lines = []
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            lines.append('%s: %s != %s' % (k, a.get(k), b.get(k)))
    return '\n'.join(lines[:20])


def main():
    top = tempfile.mkdtemp(prefix='fmrre-tar-')
    sim = os.path.join(top, 'sim')
    cli(sim, 'help')
    work = os.path.join(sim, 'data/media/0/tartest')  # the CLI sees it as /sdcard/tartest
    os.makedirs(work)
    real_cli = cli

    def cli2(root, *args):  # host paths under `work` -> the simulator's /sdcard/tartest/...
        return real_cli(root, *[('/sdcard/tartest/' + os.path.relpath(a, work)) if a.startswith(work) else a for a in args])

    try:
        keys = [k for k in ('user.fmrre', 'security.selinux') if can_xattr(work, k, b'probe')]
        print('extended attributes tested here: %s%s' % (', '.join(keys) or 'none', '' if ROOT else ' (not root: owners not compared)'))
        src = os.path.join(work, 'src')
        make_tree(src, keys)
        want = snapshot(src, keys)

        # FMRRE -> FMRRE
        a1, x1 = os.path.join(work, 'fmrre.tar'), os.path.join(work, 'x1')
        rc, out = cli2(sim, 'tar', 'c', src, a1)
        check(rc == 0 and ('%d SELinux labels' % (4 if 'security.selinux' in keys else 0)) in out, 'FMRRE archives the tree', out)
        os.makedirs(x1)
        rc, out = cli2(sim, 'tar', 'x', a1, x1)
        got = snapshot(x1, keys)
        check(rc == 0 and got == want, 'FMRRE -> FMRRE: the same files, modes, times, owners, links, attributes',
              out + diff(want, got))
        # GNU tar reads FMRRE's archive
        r = subprocess.run(['tar', '-tvf', a1], capture_output=True, text=True)
        check(r.returncode == 0 and 'n' * 120 in r.stdout, 'GNU tar lists FMRRE\'s archive (pax long names)', r.stderr)
        x2 = os.path.join(work, 'x2')
        os.makedirs(x2)
        r = subprocess.run(['tar', '--xattrs', '--xattrs-include=*', '-xpf', a1, '-C', x2], capture_output=True, text=True)
        got = snapshot(x2, keys)
        check(r.returncode == 0 and got == want, 'FMRRE -> GNU tar (--xattrs): the same', r.stderr + diff(want, got))
        # FMRRE reads GNU tar's archives
        for fmt in ('posix', 'gnu'):
            a = os.path.join(work, 'gnu-%s.tar' % fmt)
            r = subprocess.run(['tar', '--format=' + fmt, '--xattrs', '--xattrs-include=*', '-cf', a, '-C', src, '.'],
                               capture_output=True, text=True)
            x = os.path.join(work, 'x-' + fmt)
            os.makedirs(x)
            rc, out = cli2(sim, 'tar', 'x', a, x)
            got = snapshot(x, keys if fmt == 'posix' else [])
            exp = snapshot(src, keys if fmt == 'posix' else [])
            check(r.returncode == 0 and rc == 0 and got == exp, 'GNU tar (%s format) -> FMRRE: the same' % fmt,
                  r.stderr + out + diff(exp, got))

        # Archives that try to write outside the folder
        outside = os.path.join(work, 'outside')
        os.makedirs(outside)
        evil = os.path.join(work, 'evil.tar')
        with tarfile.open(evil, 'w', format=tarfile.PAX_FORMAT) as t:
            def add(name, data=b'pwned', kind=tarfile.REGTYPE, link=''):
                i = tarfile.TarInfo(name)
                i.type, i.linkname, i.size = kind, link, len(data) if kind == tarfile.REGTYPE else 0
                t.addfile(i, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
            add('../outside/dotdot.txt')
            add(os.path.join(outside, 'absolute.txt'))
            add('escape', kind=tarfile.SYMTYPE, link=outside)
            add('escape/through-link.txt')
            add('hard', kind=tarfile.LNKTYPE, link='../../../etc/passwd')
            add('fine.txt', b'this one is fine')
        xe = os.path.join(work, 'xe')
        os.makedirs(xe)
        rc, out = cli2(sim, 'tar', 'x', evil, xe)
        check(not os.listdir(outside) and open(os.path.join(xe, 'fine.txt')).read() == 'this one is fine' and
              '4 refused' in out, 'names outside the folder, through a symlink or a hard link out are refused', out)
        # A classic TWRP backup: absolute names of the partition
        twrp = os.path.join(work, 'twrp.tar')
        with tarfile.open(twrp, 'w', format=tarfile.GNU_FORMAT) as t:
            for name in ('/data/app/x/base.apk', '/data/system/packages.xml', '/system/bin/not-data'):
                i = tarfile.TarInfo(name)
                i.size = len(name)
                t.addfile(i, io.BytesIO(name.encode()))
        xt = os.path.join(work, 'xt')
        os.makedirs(xt)
        rc, out = cli2(sim, 'tar', 'x', twrp, xt, '--strip', '/data')
        check(rc == 0 and open(os.path.join(xt, 'app/x/base.apk')).read() == '/data/app/x/base.apk' and
              os.path.exists(os.path.join(xt, 'system/packages.xml')) and not os.path.exists(os.path.join(xt, 'bin')) and
              '1 refused' in out, 'a classic TWRP backup ("/data/app/...") lands inside the partition', out)
        # Cut off
        with open(a1, 'rb') as f:
            data = f.read()
        cut = os.path.join(work, 'cut.tar')
        with open(cut, 'wb') as f:
            f.write(data[:len(data) // 2 + 100])
        xc = os.path.join(work, 'xc')
        os.makedirs(xc)
        rc, out = cli2(sim, 'tar', 'x', cut, xc)
        check(rc != 0 and 'cut off' in out, 'a cut-off archive is reported', out)
    finally:
        shutil.rmtree(top, ignore_errors=True)
    print('%s: %d failure(s)' % ('FAILED' if failed else 'passed', failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
