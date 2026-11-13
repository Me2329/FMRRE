#!/usr/bin/env python3
"""End-to-end test of A/B OTA (payload.bin) support against the simulator.

Builds OTA zips the way update_engine's generator does (REPLACE / REPLACE_BZ / REPLACE_XZ / ZERO /
DISCARD operations, hash tree + FEC left for the device), with an independent Python implementation
of the protobuf encoding, avbtool's hash tree and libfec's Reed-Solomon FEC. Then checks that
`fmrre payload extract` and `fmrre payload install` produce byte-identical partitions, and that
corrupt, incremental, compressed and foreign packages are refused.

    tests/payload_test.py [path/to/fmrre]      (default: build-host/fmrre)
"""
import bz2, hashlib, lzma, os, random, shutil, struct, subprocess, sys, tempfile, zipfile

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build-host/fmrre')
BS = 4096
REPLACE, REPLACE_BZ, SOURCE_COPY, ZERO, DISCARD, REPLACE_XZ = 0, 1, 4, 6, 7, 8
fails = []


def check(cond, what):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        fails.append(what)


# ---- protobuf -----------------------------------------------------------------------
def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7f) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def fld(num, wire, payload):
    return varint(num << 3 | wire) + (varint(payload) if wire == 0 else varint(len(payload)) + payload)


def extent(start, count):
    return fld(1, 0, start) + fld(2, 0, count)


# ---- verity (avbtool generate_hash_tree, libfec RS + update_engine EncodeFEC) ------------
def rtm(x, b):
    return (x + b - 1) // b * b


def hash_tree(data, salt):
    sizes, size = [], len(data)
    while size > BS:
        size = rtm(((size + BS - 1) // BS) * 32, BS)
        sizes.append(size)
    offs = [sum(sizes[n + 1:]) for n in range(len(sizes))]
    tree = bytearray(sum(sizes))
    src, lvl = data, 0
    while len(src) > BS:
        out = b''.join(hashlib.sha256(salt + src[i:i + BS]).digest() for i in range(0, len(src), BS))
        out += b'\0' * (rtm(len(out), BS) - len(out))
        tree[offs[lvl]:offs[lvl] + len(out)] = out
        src, lvl = out, lvl + 1
    return bytes(tree)


class RS:
    def __init__(self, nroots):
        self.nroots, NN = nroots, 255
        self.alpha, self.index = [0] * 256, [0] * 256
        self.index[0] = NN
        sr = 1
        for i in range(NN):
            self.index[sr], self.alpha[i] = i, sr
            sr <<= 1
            if sr & 256:
                sr ^= 0x11d
            sr &= NN
        g = [1] + [0] * nroots
        for i in range(nroots):
            g[i + 1] = 1
            for j in range(i, 0, -1):
                g[j] = g[j - 1] ^ self.alpha[(self.index[g[j]] + i) % NN] if g[j] else g[j - 1]
            g[0] = self.alpha[(self.index[g[0]] + i) % NN]
        self.gen = [self.index[x] for x in g]

    def encode(self, msg):
        nr, bb = self.nroots, [0] * self.nroots
        for d in msg:
            fb = self.index[d ^ bb[0]]
            if fb != 255:
                for j in range(1, nr):
                    bb[j] ^= self.alpha[(fb + self.gen[nr - j]) % 255]
            bb = bb[1:] + [self.alpha[(fb + self.gen[0]) % 255] if fb != 255 else 0]
        return bytes(bb)


def fec(data, roots=2):
    rs, rsn = RS(roots), 255 - roots
    rounds = -(-(len(data) // BS) // rsn)
    out = bytearray()
    for i in range(rounds):
        blocks = []
        for j in range(rsn):
            off = i * BS + j * rounds * 4096
            blocks.append(data[off:off + BS] if off < len(data) else bytes(BS))
        cols = list(zip(*blocks))
        for k in range(BS):
            out += rs.encode(cols[k])
    return bytes(out)


# ---- payload -------------------------------------------------------------------------
class Part:
    def __init__(self, name, data, ops, verity=False, dynamic=False):
        """ops: list of (type, first_block, count, codec) covering the data; codec in raw/bz/xz/xznone."""
        self.name, self.dynamic, self.ops_spec = name, dynamic, ops
        assert len(data) % BS == 0
        data = bytearray(data)
        for (typ, start, count, _) in ops:  # the generator only uses ZERO / DISCARD for blocks that are zero
            if typ in (ZERO, DISCARD):
                data[start * BS:(start + count) * BS] = bytes(count * BS)
        data = bytes(data)
        self.data_blocks = len(data) // BS
        self.image = data
        self.tree = self.fec = 0
        if verity:
            self.salt = os.urandom(32)
            t = hash_tree(data, self.salt)
            f = fec(data + t)
            self.tree, self.fec = len(t) // BS, len(f) // BS
            self.image = data + t + f + bytes(BS)
            self.ops_spec = ops + [(ZERO, len(self.image) // BS - 1, 1, None)]


def sig_blob(data):
    """update_engine's Signatures message with one signature (data, unpadded_signature_size)."""
    return fld(1, 2, fld(2, 2, data) + bytes([3 << 3 | 5]) + struct.pack('<I', len(data)))


def sign_digest(digest, key):
    """RSASSA-PKCS1-v1_5 over a SHA-256 the caller computed - what update_engine's payload signer makes."""
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, 'h'), os.path.join(d, 's')
        with open(src, 'wb') as f:
            f.write(digest)
        subprocess.run(['openssl', 'pkeyutl', '-sign', '-inkey', key, '-pkeyopt', 'digest:sha256', '-in', src,
                        '-out', out], check=True)
        with open(out, 'rb') as f:
            return f.read()


def build_payload(parts, group='qti_dynamic_partitions', group_size=8 << 30, delta=False, sign_key=None):
    blobs = bytearray()
    man = fld(3, 0, BS) + fld(12, 0, 7 if delta else 0)
    for p in parts:
        pu = fld(1, 2, p.name.encode())
        pu += fld(7, 2, fld(1, 0, len(p.image)) + fld(2, 2, hashlib.sha256(p.image).digest()))
        for (typ, start, count, codec) in p.ops_spec:
            chunk = p.image[start * BS:(start + count) * BS]
            op = fld(1, 0, typ)
            blob = b''
            if typ == REPLACE:
                blob = chunk
            elif typ == REPLACE_BZ:
                blob = bz2.compress(chunk, 9)
            elif typ == REPLACE_XZ:
                blob = lzma.compress(chunk, format=lzma.FORMAT_XZ, check=lzma.CHECK_NONE if codec == 'xznone' else lzma.CHECK_CRC64)
            elif typ == SOURCE_COPY:
                op += fld(4, 2, extent(start, count))
            if blob:
                op += fld(2, 0, len(blobs)) + fld(3, 0, len(blob)) + fld(8, 2, hashlib.sha256(blob).digest())
                blobs += blob
            op += fld(6, 2, extent(start, count))
            pu += fld(8, 2, op)
        if p.tree:
            pu += fld(10, 2, extent(0, p.data_blocks)) + fld(11, 2, extent(p.data_blocks, p.tree))
            pu += fld(12, 2, b'sha256') + fld(13, 2, p.salt)
            pu += fld(14, 2, extent(0, p.data_blocks + p.tree)) + fld(15, 2, extent(p.data_blocks + p.tree, p.fec)) + fld(16, 0, 2)
        man += fld(13, 2, pu)
    dyn = [p.name for p in parts if p.dynamic]
    if dyn:
        g = fld(1, 2, group.encode()) + fld(2, 0, group_size) + b''.join(fld(3, 2, n.encode()) for n in dyn)
        man += fld(15, 2, fld(1, 2, g) + fld(2, 0, 1))
    man += fld(18, 2, b'2026-11-05')
    if sign_key:  # like ota_from_target_files: the signature sizes are fixed before anything is hashed
        size = len(sig_blob(bytes(256)))  # RSA-2048
        man += fld(4, 0, len(blobs)) + fld(5, 0, size)
        header = b'CrAU' + struct.pack('>QQI', 2, len(man), size)
        body = header + man + sig_blob(sign_digest(hashlib.sha256(header + man).digest(), sign_key)) + bytes(blobs)
        return body + sig_blob(sign_digest(hashlib.sha256(body).digest(), sign_key))
    return b'CrAU' + struct.pack('>QQI', 2, len(man), 0) + man + bytes(blobs)


def write_zip(path, payload, device='husky', stored=True):
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('META-INF/com/android/metadata', f'ota-type=AB\npre-device={device}\npost-build=test/{device}/{device}:15/TEST/1:user/release-keys\n')
        z.writestr('payload_properties.txt', f'FILE_SIZE={len(payload)}\n')
        z.writestr(zipfile.ZipInfo('payload.bin'), payload, compress_type=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED)


def cli(root, *args):
    r = subprocess.run([BIN, '--sim', '--sim-root', root, 'fmrre', *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    rng = random.Random(5)
    def blocks(n, density=0.5):
        out = bytearray()
        for i in range(n):
            if rng.random() < density:
                out += bytes(rng.choice(b'abcdefgh\x00\xff') for _ in range(64)) * 64
            else:
                out += bytes(BS)
        return bytes(out)

    work = tempfile.mkdtemp(prefix='fmrre-payload-')
    root = os.path.join(work, 'sim')
    try:
        cli(root, 'help')  # creates the sandbox
        dl = os.path.join(root, 'data/media/0/Download')
        boot = b'ANDROID!' + blocks(64)[8:]
        parts = [
            Part('boot', boot, [(REPLACE_BZ, 0, 32, None), (REPLACE_XZ, 32, 32, None)]),
            Part('vendor_boot', blocks(96), [(REPLACE_XZ, 0, 40, 'xznone'), (REPLACE, 40, 16, None), (ZERO, 56, 8, None), (REPLACE_BZ, 64, 32, None)]),
            Part('system', blocks(512, 0.3), [(REPLACE_XZ, 0, 200, None), (REPLACE_BZ, 200, 200, None), (REPLACE, 400, 112, None)], verity=True, dynamic=True),
            Part('product', blocks(128, 0.2), [(REPLACE_XZ, 0, 128, None)], dynamic=True),
            Part('vendor', blocks(64, 0.0), [(DISCARD, 0, 64, None)], dynamic=True),
        ]
        # DISCARD/ZERO blocks are written as zeros: make the expected image say so.
        zip_ok = os.path.join(dl, 'rom-ok.zip')
        write_zip(zip_ok, build_payload(parts))

        print('payload info / extract')
        rc, out = cli(root, 'payload', 'info', '/sdcard/Download/rom-ok.zip')
        check(rc == 0 and 'full OTA' in out and '+hashtree' in out and 'dynamic' in out, 'info lists partitions, verity and dynamic flags')
        ext = os.path.join(root, 'data/media/0/FMRRE/payload-test')  # simulator paths live in the sandbox
        rc, out = cli(root, 'payload', 'extract', '/sdcard/Download/rom-ok.zip', '--out', '/sdcard/FMRRE/payload-test')
        check(rc == 0, 'extract all partitions' + ('' if rc == 0 else ': ' + out[-300:]))
        for p in parts:
            f = os.path.join(ext, p.name + '.img')
            check(os.path.exists(f) and open(f, 'rb').read() == p.image, f'{p.name}.img is byte-identical (bz2/xz/zero/verity)')

        print('payload install')
        rc, out = cli(root, 'payload', 'install', '/sdcard/Download/rom-ok.zip')
        check(rc == 0 and 'OTA installed to slot B' in out, 'install to the inactive slot' + ('' if rc == 0 else ': ' + out[-400:]))
        for p in parts:
            dev = os.path.join(root, 'dev/block/mapper' if p.dynamic else 'dev/block/by-name', p.name + '_b')
            got = open(dev, 'rb').read()[:len(p.image)] if os.path.exists(dev) else b''
            check(got == p.image, f'{p.name}_b matches the image')
        rc, out = cli(root, 'lp', 'list')
        check(rc == 0 and 'system_b' in out and 'product_b' in out, 'super lists the new system_b and product_b')

        print('refusals')
        bad = bytearray(build_payload(parts))
        bad[-5000] ^= 0x40  # inside the last blobs
        write_zip(os.path.join(dl, 'rom-corrupt.zip'), bytes(bad))
        rc, out = cli(root, 'payload', 'install', '/sdcard/Download/rom-corrupt.zip')
        check(rc != 0 and 'corrupt' in out, 'corrupt data is detected (per-operation SHA-256)')
        check('was not made active' in out, 'a failed install does not switch slots')
        delta = [Part('boot', boot, [(SOURCE_COPY, 0, 64, None)])]
        write_zip(os.path.join(dl, 'ota-delta.zip'), build_payload(delta, delta=True))
        rc, out = cli(root, 'payload', 'install', '/sdcard/Download/ota-delta.zip')
        check(rc != 0 and 'Incremental' in out, 'incremental OTAs are refused')
        write_zip(os.path.join(dl, 'rom-other.zip'), build_payload(parts[:1]), device='shiba')
        rc, out = cli(root, 'payload', 'install', '/sdcard/Download/rom-other.zip')
        check(rc != 0 and "'shiba'" in out, 'an OTA for another device is refused')
        write_zip(os.path.join(dl, 'rom-deflated.zip'), build_payload(parts[:1]), stored=False)
        rc, out = cli(root, 'payload', 'info', '/sdcard/Download/rom-deflated.zip')
        check(rc != 0 and 'compressed inside this zip' in out, 'a deflated payload.bin is explained')
        trunc = os.path.join(dl, 'rom-truncated.zip')
        shutil.copy(zip_ok, trunc)
        with open(trunc, 'r+b') as f:
            f.truncate(os.path.getsize(trunc) // 2)
        rc, out = cli(root, 'payload', 'info', '/sdcard/Download/rom-truncated.zip')
        check(rc != 0, 'a truncated download is refused')

        # update_engine's signatures inside payload.bin, made with an openssl key the "installed system" trusts.
        key, cert = os.path.join(work, 'ota.key'), os.path.join(work, 'ota.pem')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-days', '3650', '-subj',
                        '/CN=OTA test key', '-keyout', key, '-out', cert], check=True, capture_output=True)
        sec = os.path.join(root, 'system_root/system/etc/security')
        os.makedirs(sec, exist_ok=True)
        with zipfile.ZipFile(os.path.join(sec, 'otacerts.zip'), 'w') as z:
            z.write(cert, 'releasekey.x509.pem')
        signed = build_payload(parts, sign_key=key)
        write_zip(os.path.join(dl, 'rom-signed.zip'), signed)
        rc, out = cli(root, 'payload', 'info', '/sdcard/Download/rom-signed.zip')
        check(rc == 0 and "payload metadata signed with the installed system's key" in out, 'payload info: metadata signature')
        rc, out = cli(root, 'payload', 'verify', '/sdcard/Download/rom-signed.zip')
        check(rc == 0 and 'payload signature valid' in out, 'payload verify: metadata and payload signatures')
        rc, out = cli(root, 'payload', 'verify', '/sdcard/Download/rom-ok.zip')
        check(rc != 0 and 'no metadata signature' in out, 'payload verify: an unsigned payload')
        t = bytearray(signed)
        t[-600] ^= 1  # in the last data blob: the manifest (and its signature) still match, the payload signature not
        write_zip(os.path.join(dl, 'rom-signed-changed.zip'), bytes(t))
        rc, out = cli(root, 'payload', 'verify', '/sdcard/Download/rom-signed-changed.zip')
        check(rc != 0 and "DOESN'T match" in out, 'payload verify: a changed data byte breaks the payload signature')
        i = signed.find(b'2026-11-05')
        t = bytearray(signed)
        t[i + 9] ^= 1  # inside the manifest
        write_zip(os.path.join(dl, 'rom-signed-manifest.zip'), bytes(t))
        rc, out = cli(root, 'payload', 'verify', '/sdcard/Download/rom-signed-manifest.zip')
        check(rc != 0 and "key FMRRE doesn't know" in out, 'payload verify: a changed manifest fails the metadata signature')
        # "Only zips signed with a trusted key": an A/B OTA counts when its payload is (update_engine's own rule).
        with open(os.path.join(root, 'data/media/0/FMRRE/.fmrre.conf'), 'w') as f:
            f.write('zip_sig_require=1\n')
        rc, out = cli(root, 'install', '/sdcard/Download/rom-ok.zip')
        check(rc != 0 and 'only zips signed with a trusted key' in out, 'required: an unsigned OTA is refused')
        rc, out = cli(root, 'payload', 'install', '/sdcard/Download/rom-signed.zip')
        check(rc == 0 and "payload metadata signed with the installed system's key" in out,
              'required: an OTA whose payload is signed with the system\'s key installs')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f'{"All payload tests passed" if not fails else str(len(fails)) + " payload test(s) FAILED"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
