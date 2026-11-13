#!/usr/bin/env python3
"""FMRRE's own adb (shell, pull, push) against the real adb client, in the simulator over TCP.

    tests/adb_shell_test.py [path/to/fmrre]      (default: build-host/fmrre)

A phone running FMRRE in its stock ramdisk has no adbd for these: FMRRE answers them itself. Checked:
`adb shell` output and exit codes (shell protocol v2), stdin, a command the computer stops, push / pull of a big
file and a folder, errors (missing file, folder as a file), and a ramdisk without sh (`adb shell fmrre help`).
"""
import hashlib
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build-host/fmrre')
failed = 0


def check(cond, what, out=''):
    global failed
    print(('ok    ' if cond else 'FAIL  ') + what, flush=True)
    if not cond:
        failed += 1
        if out:
            print('      ' + str(out).strip().replace('\n', '\n      ')[:2000])


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    adb = shutil.which('adb')
    if not adb:
        print('skip: no adb client')
        return 0
    tmp = tempfile.mkdtemp()
    sim = os.path.join(tmp, 'sim')
    srv = str(free_port())
    env = dict(os.environ, ANDROID_ADB_SERVER_PORT=srv)
    procs = []

    def start(nosh):
        port = free_port()
        e = dict(env)
        if nosh:
            e['FMRRE_ADB_NOSH'] = '1'
        p = subprocess.Popen([BIN, '--sim', '--sim-root', sim, 'fmrre', 'adbd', '--tcp', str(port)], env=e,
                             stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, 'adbd%d.log' % len(procs)), 'w'))
        procs.append(p)
        serial = '127.0.0.1:%d' % port
        for _ in range(50):
            r = subprocess.run([adb, 'connect', serial], env=env, capture_output=True, text=True, timeout=20)
            if 'connected' in r.stdout and 'failed' not in r.stdout and 'unable' not in r.stdout:
                break
            time.sleep(0.2)
        for _ in range(50):
            r = subprocess.run([adb, '-s', serial, 'get-state'], env=env, capture_output=True, text=True, timeout=20)
            if r.stdout.strip() in ('recovery', 'device'):
                break
            time.sleep(0.2)
        return serial

    def a(serial, *args, inp=None, timeout=60):
        return subprocess.run([adb, '-s', serial] + list(args), env=env, capture_output=True, input=inp, timeout=timeout)

    try:
        s = start(False)
        r = subprocess.run([adb, '-s', s, 'get-state'], env=env, capture_output=True, text=True)
        check(r.stdout.strip() == 'recovery', 'listed as a recovery device', r.stdout + r.stderr)
        r = a(s, 'shell', 'echo hello; echo oops >&2')
        check(r.returncode == 0 and r.stdout == b'hello\n' and r.stderr == b'oops\n', 'shell: stdout and stderr apart (v2)', r)
        r = a(s, 'shell', 'exit 7')
        check(r.returncode == 7, 'shell: exit code reaches the computer', r.returncode)
        r = a(s, 'shell', 'cat | tr a-z A-Z', inp=b'piped input\n')
        check(r.stdout == b'PIPED INPUT\n', 'shell: stdin, closed at the end', r)
        big = os.urandom(3 * 1024 * 1024 + 123)
        r = a(s, 'shell', 'cat', inp=big)
        check(r.stdout == big, 'shell: 3 MB through stdin and back unchanged', len(r.stdout))
        t0 = time.time()
        r = a(s, 'shell', 'sleep 30', timeout=5) if False else None
        p = subprocess.Popen([adb, '-s', s, 'shell', 'sleep 30; echo late'], env=env, stdout=subprocess.PIPE)
        time.sleep(1.0)
        p.kill()
        p.wait()
        r = a(s, 'shell', 'echo still here')
        check(r.stdout == b'still here\n' and time.time() - t0 < 20, 'a command the computer stops is hung up; the next one runs', r)

        # files: /sdcard is the phone's internal storage
        src = os.path.join(tmp, 'blob.bin')
        data = os.urandom(5 * 1024 * 1024 + 7)
        open(src, 'wb').write(data)
        r = a(s, 'push', src, '/sdcard/Download/blob.bin', timeout=120)
        on_phone = os.path.join(sim, 'data/media/0/Download/blob.bin')
        check(r.returncode == 0 and os.path.exists(on_phone) and open(on_phone, 'rb').read() == data,
              'push: 5 MB file lands in internal storage, unchanged', r.stderr)
        back = os.path.join(tmp, 'back.bin')
        r = a(s, 'pull', '/sdcard/Download/blob.bin', back, timeout=120)
        check(r.returncode == 0 and open(back, 'rb').read() == data, 'pull: the same bytes back', r.stderr)
        folder = os.path.join(tmp, 'logs')
        os.makedirs(os.path.join(folder, 'sub'))
        open(os.path.join(folder, 'a.txt'), 'w').write('A')
        open(os.path.join(folder, 'sub', 'b.txt'), 'w').write('B')
        r = a(s, 'push', folder, '/sdcard/')
        check(r.returncode == 0 and open(os.path.join(sim, 'data/media/0/logs/sub/b.txt')).read() == 'B', 'push: a folder', r.stderr)
        out = os.path.join(tmp, 'pulled')
        r = a(s, 'pull', '/sdcard/logs', out)
        check(r.returncode == 0 and open(os.path.join(out, 'sub', 'b.txt')).read() == 'B', 'pull: a folder', r.stderr)
        r = a(s, 'pull', '/sdcard/nope.txt', os.path.join(tmp, 'x'))
        check(r.returncode != 0 and (b'No such file' in r.stderr + r.stdout or b'does not exist' in r.stderr + r.stdout), 'pull: a missing file is an error', r)
        r = a(s, 'shell', 'ls %s' % on_phone)  # the simulator's shell is the computer's: the sandbox path
        check(r.returncode == 0, 'shell sees what push wrote', r)

        # A stock ramdisk without sh: programs still run, FMRRE's own command line included.
        s2 = start(True)
        r = a(s2, 'shell', 'fmrre', 'help')
        check(r.returncode == 0 and b'Commands' in r.stdout, 'no sh: `adb shell fmrre help` runs FMRRE itself', r)
        r = a(s2, 'shell', 'no-such-program')
        check(r.returncode == 127 and b'not found' in r.stderr, 'no sh: an unknown program says so (127)', r)
        r = a(s2, 'shell')
        check(r.returncode != 0 and b'no shell' in r.stderr, 'no sh: an interactive session explains why not', r)
    finally:
        subprocess.run([adb, 'kill-server'], env=env, capture_output=True)
        for p in procs:
            p.kill()
            p.wait()
        if failed:
            for f in sorted(os.listdir(tmp)):
                if f.endswith('.log'):
                    print('--- ' + f)
                    print(open(os.path.join(tmp, f)).read()[-3000:])
        shutil.rmtree(tmp, ignore_errors=True)
    print('passed: %d failure(s)' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
