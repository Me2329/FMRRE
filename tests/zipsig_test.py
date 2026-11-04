#!/usr/bin/env python3
"""End-to-end test of zip signature checks against the simulator, with keys made by openssl.

Zips are signed like AOSP's signapk -w (tests/signapk.py); the simulated phone's system partition gets an
otacerts.zip with "its" key, like a real ROM has. Checks that `fmrre verify-zip`:
  - names the key: the installed system's (RSA-4096), another ROM's (RSA-2048), an EC P-256 key, the public
    AOSP test key (SHA-1), a key the user added to FMRRE/keys, a key in /res/keys (older recoveries)
  - refuses a changed byte, a changed signature, an extra end-of-zip record, signed attributes
  - does what Android's recovery does when the certificate in the block isn't the signer's: its own keys count
and that installing prints the verdict, warns about a ROM signed with another key than the installed system
(and writes that to the flash history), and refuses untrusted zips when zip_sig_require is set.

    tests/zipsig_test.py [path/to/fmrre]      (default: build-host/fmrre)
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import signapk  # noqa: E402

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build-host/fmrre')
failed = 0


def check(cond, what, out=''):
    global failed
    print(('ok    ' if cond else 'FAIL  ') + what)
    if not cond:
        failed += 1
        if out:
            print('      ' + out.strip().replace('\n', '\n      '))


def cli(root, *args):
    r = subprocess.run([BIN, '--sim', '--sim-root', root, 'fmrre', *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def make_zip(name, rom):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('META-INF/com/google/android/update-binary', '#!/sbin/sh\necho "%s"\n' % name)
        z.writestr('META-INF/com/google/android/updater-script', 'ui_print("Installing %s");\n' % name)
        if rom:
            z.writestr('system.transfer.list', '4\n0\n0\n0\n')
            z.writestr('system.new.dat', bytes(range(256)) * 64)
        else:
            z.writestr('system/addon.d/70-test.sh', '#!/sbin/sh\n')
    return buf.getvalue()


def cms(data, args):
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, 'data'), os.path.join(d, 'sig')
        with open(src, 'wb') as f:
            f.write(data)
        subprocess.run(['openssl', 'cms', '-sign', '-binary', '-outform', 'DER', '-in', src, '-out', out] + args,
                       check=True)
        with open(out, 'rb') as f:
            return f.read()


def mincrypt_v4(cert):
    """The key of a certificate as a /res/keys entry (mincrypt v4: e=65537, SHA-256), like AOSP's dumpkey."""
    out = subprocess.run(['openssl', 'x509', '-noout', '-modulus', '-in', cert], capture_output=True, text=True,
                         check=True).stdout
    n = int(out.strip().split('=')[1], 16)
    words = (n.bit_length() + 31) // 32
    n0inv = (-pow(n, -1, 1 << 32)) % (1 << 32)
    rr = pow(2, 64 * words, n)
    w = lambda x: ','.join(str((x >> (32 * i)) & 0xFFFFFFFF) for i in range(words))
    return 'v4 {%d,0x%x,{%s},{%s}}' % (words, n0inv, w(n), w(rr))


def main():
    if not os.path.exists(BIN):
        print('missing', BIN)
        return 2
    root = tempfile.mkdtemp(prefix='fmrre-zipsig-')
    keys = tempfile.mkdtemp(prefix='fmrre-zipsig-keys-')
    try:
        cli(root, 'help')  # creates the sandbox
        dl = os.path.join(root, 'data/media/0/Download')
        os.makedirs(dl, exist_ok=True)
        k = lambda n: os.path.join(keys, n)
        sys_c, sys_k = signapk.make_key('rsa:4096', '/O=Example ROM/CN=Example ROM release key', k('sys'))
        oth_c, oth_k = signapk.make_key('rsa:2048', '/O=Other ROM/CN=Other ROM release key', k('oth'))
        ec_c, ec_k = signapk.make_key('ec', '/O=EC ROM/CN=EC release key', k('ec'))
        tst_c, tst_k = signapk.make_key('rsa:2048', '/C=US/ST=California/L=Mountain View/O=Android/OU=Android/'
                                        'CN=Android/emailAddress=android@android.com', k('tst'), 'sha1')
        usr_c, usr_k = signapk.make_key('rsa:2048', '/CN=My own key', k('usr'))
        # The installed system trusts updates signed with its release key, like every ROM.
        sec = os.path.join(root, 'system_root/system/etc/security')
        os.makedirs(sec, exist_ok=True)
        with zipfile.ZipFile(os.path.join(sec, 'otacerts.zip'), 'w') as z:
            z.write(sys_c, 'releasekey.x509.pem')
        os.makedirs(os.path.join(root, 'data/media/0/FMRRE/keys'), exist_ok=True)
        shutil.copy(usr_c, os.path.join(root, 'data/media/0/FMRRE/keys/mine.x509.pem'))

        rom, addon = make_zip('ROM', True), make_zip('addon', False)

        def put(name, data):
            with open(os.path.join(dl, name), 'wb') as f:
                f.write(data)
            return '/sdcard/Download/' + name

        z_sys = put('rom-update.zip', signapk.sign_zip(rom, sys_c, sys_k))
        z_oth = put('rom-other.zip', signapk.sign_zip(rom, oth_c, oth_k))
        z_ec = put('rom-ec.zip', signapk.sign_zip(rom, ec_c, ec_k))
        z_tst = put('rom-testkeys.zip', signapk.sign_zip(rom, tst_c, tst_k, 'sha1'))
        z_usr = put('addon-mine.zip', signapk.sign_zip(addon, usr_c, usr_k))
        z_add = put('addon-other.zip', signapk.sign_zip(addon, oth_c, oth_k))
        z_uns = put('unsigned.zip', rom)
        good = signapk.sign_zip(rom, oth_c, oth_k)
        t = bytearray(good)
        t[100] ^= 1
        z_t1 = put('changed.zip', bytes(t))
        t = bytearray(good)
        t[-12] ^= 1
        z_t2 = put('changed-sig.zip', bytes(t))
        clen = good[-2] | good[-1] << 8
        t = bytearray(good)
        t[len(good) - clen:len(good) - clen + 4] = b'PK\x05\x06'  # a second end-of-zip record in the comment
        z_t3 = put('extra-eocd.zip', bytes(t))
        z_att = put('signed-attrs.zip', signapk.sign_zip(rom, None, None, block=cms(rom[:-2], [
            '-md', 'sha256', '-signer', oth_c, '-inkey', oth_k])))  # without -noattr
        # The certificate in the block is another one than the signer's: Android tries its own keys anyway.
        z_mix = put('cert-mismatch.zip', signapk.sign_zip(rom, None, None, block=cms(rom[:-2], [
            '-noattr', '-md', 'sha256', '-signer', sys_c, '-inkey', sys_k, '-nocerts', '-certfile', ec_c])))
        z_mix2 = put('cert-mismatch-untrusted.zip', signapk.sign_zip(rom, None, None, block=cms(rom[:-2], [
            '-noattr', '-md', 'sha256', '-signer', oth_c, '-inkey', oth_k, '-nocerts', '-certfile', ec_c])))

        rc, out = cli(root, 'verify-zip', z_sys)
        check(rc == 0 and "installed system's key" in out and 'RSA-4096, SHA-256' in out and 'trusted by system' in out,
              'an update of the installed ROM: its own key (RSA-4096)', out)
        rc, out = cli(root, 'verify-zip', z_oth)
        check(rc == 0 and "doesn't know" in out and 'Other ROM release key' in out and 'another ROM' in out,
              'another ROM: valid, an unknown key, not the installed system\'s', out)
        rc, out = cli(root, 'verify-zip', z_ec)
        check(rc == 0 and 'EC P-256, SHA-256' in out, 'an EC P-256 signature', out)
        rc, out = cli(root, 'verify-zip', z_tst)
        check(rc == 0 and 'public AOSP test key' in out and 'SHA-1' in out and 'anyone can sign' in out,
              'the public AOSP test key, SHA-1', out)
        rc, out = cli(root, 'verify-zip', z_usr)
        check(rc == 0 and 'a key you trusted' in out and 'FMRRE/keys' in out, 'a key in FMRRE/keys', out)
        rc, out = cli(root, 'verify-zip', z_uns)
        check(rc != 0 and 'Not signed' in out, 'unsigned', out)
        rc, out = cli(root, 'verify-zip', z_t1)
        check(rc != 0 and "doesn't match" in out and 'changed after it was signed' in out, 'one changed byte', out)
        rc, out = cli(root, 'verify-zip', z_t2)
        check(rc != 0 and "doesn't match" in out, 'a changed signature', out)
        rc, out = cli(root, 'verify-zip', z_t3)
        check(rc != 0 and 'added after' in out, 'a second end-of-zip record', out)
        rc, out = cli(root, 'verify-zip', z_att)
        check(rc != 0 and 'signed attributes' in out, 'signed attributes (Android refuses them)', out)
        rc, out = cli(root, 'verify-zip', z_mix)
        check(rc == 0 and "installed system's key" in out, 'certificate mismatch, signed with a trusted key', out)
        rc, out = cli(root, 'verify-zip', z_mix2)
        check(rc != 0, 'certificate mismatch, untrusted key', out)
        rc, out = cli(root, 'verify-zip', '/sdcard/Download/nope.zip')
        check(rc != 0 and 'not found' in out, 'a missing file', out)
        # A recovery from before Android 10 keeps its keys in /res/keys (mincrypt format).
        os.makedirs(os.path.join(root, 'res'), exist_ok=True)
        with open(os.path.join(root, 'res/keys'), 'w') as f:
            f.write(mincrypt_v4(oth_c) + '\n')
        rc, out = cli(root, 'verify-zip', z_oth)
        check(rc == 0 and 'a key this recovery trusts' in out and '/res/keys' in out, 'a key from /res/keys', out)
        os.remove(os.path.join(root, 'res/keys'))

        # Installing: the verdict, the ROM-switch warning, the history.
        rc, out = cli(root, 'install', z_oth)
        check(rc == 0 and 'Signature: Signed with a key' in out and 'wipe data after installing it' in out,
              'install a ROM signed with another key: warned to wipe data', out)
        rc, out = cli(root, 'install', z_sys)
        check(rc == 0 and "installed system's key" in out and 'wipe data' not in out, 'install an update: no warning', out)
        rc, out = cli(root, 'install', z_add)
        check(rc == 0 and 'wipe data' not in out, 'an add-on signed by someone else is not a ROM switch', out)
        rc, out = cli(root, 'history', '5')
        check('rom-other.zip - signed: Other ROM release key' in out and 'a different ROM than the one installed' in out
              and "rom-update.zip - signed: Example ROM release key (the installed system's key)" in out,
              'the flash history names the signer and the ROM switch', out)

        # Settings: only zips signed with a trusted key.
        with open(os.path.join(root, 'data/media/0/FMRRE/.fmrre.conf'), 'w') as f:
            f.write('zip_sig_require=1\n')
        rc, out = cli(root, 'install', z_oth)
        check(rc != 0 and 'only zips signed with a trusted key' in out, 'required: another ROM\'s key is refused', out)
        rc, out = cli(root, 'install', z_uns)
        check(rc != 0, 'required: an unsigned zip is refused', out)
        rc, out = cli(root, 'install', z_sys)
        check(rc == 0, 'required: the installed system\'s key is accepted', out)
        rc, out = cli(root, 'install', z_usr)
        check(rc == 0, 'required: a key in FMRRE/keys is accepted', out)
    finally:
        shutil.rmtree(root, ignore_errors=True)
        shutil.rmtree(keys, ignore_errors=True)
    print('%s: %d failure(s)' % ('FAILED' if failed else 'passed', failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
