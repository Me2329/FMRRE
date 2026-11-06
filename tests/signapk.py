#!/usr/bin/env python3
"""Sign a zip the way AOSP's `signapk -w` does (a whole-file signature), with the openssl command line tool.

The zip gets an archive comment that ends with a PKCS#7 SignedData block - no signed attributes - over the
whole file except the comment and its 2-byte length, then "(signature start) FF FF (comment size)",
little-endian. That is what Android's recovery checks (bootable/recovery/install/verifier.cpp) before it
installs an OTA, and what FMRRE checks. Only for tests: the keys come from `make_key`.

    signapk.py make_key rsa:2048|ec "/CN=Name" out_prefix [sha1|sha256]   -> out_prefix.pem + out_prefix.key
    signapk.py sign cert.pem key.pem in.zip out.zip [sha1|sha256]
"""
import os
import struct
import subprocess
import sys
import tempfile

MESSAGE = b"signed by SignApk\x00"  # signapk puts this readable text first in the comment


def make_key(kind, subject, prefix, md="sha256"):
    cmd = ["openssl", "req", "-x509", "-nodes", "-days", "36500", "-subj", subject, "-" + md,
           "-keyout", prefix + ".key", "-out", prefix + ".pem"]
    if kind == "ec":
        cmd += ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1"]
    else:
        cmd += ["-newkey", kind]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return prefix + ".pem", prefix + ".key"


def cms(data, cert, key, md):
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, "data"), os.path.join(d, "sig")
        with open(src, "wb") as f:
            f.write(data)
        subprocess.run(["openssl", "cms", "-sign", "-binary", "-noattr", "-outform", "DER", "-md", md,
                        "-signer", cert, "-inkey", key, "-in", src, "-out", out], check=True)
        with open(out, "rb") as f:
            return f.read()


def sign_zip(zip_bytes, cert, key, md="sha256", block=None):
    """block: use this PKCS#7 block instead of signing (to build broken fixtures)."""
    if zip_bytes[-22:-18] != b"PK\x05\x06":
        raise ValueError("the zip already has a comment")
    signed = zip_bytes[:-2]  # everything but the (empty) comment's length
    sig = block if block is not None else cms(signed, cert, key, md)
    total = len(MESSAGE) + len(sig) + 6
    if total > 0xFFFF:
        raise ValueError("signature too big for a zip comment")
    sig_start = total - len(MESSAGE)  # from the end of the file to the signature
    comment = MESSAGE + sig + struct.pack("<HBBH", sig_start, 0xFF, 0xFF, total)
    if b"PK\x05\x06" in comment:
        raise ValueError("an end-of-zip marker inside the signature; sign again")
    return signed + struct.pack("<H", total) + comment


def main(argv):
    if len(argv) >= 4 and argv[1] == "make_key":
        make_key(argv[2], argv[3], argv[4], argv[5] if len(argv) > 5 else "sha256")
    elif len(argv) >= 6 and argv[1] == "sign":
        with open(argv[4], "rb") as f:
            data = f.read()
        with open(argv[5], "wb") as f:
            f.write(sign_zip(data, argv[2], argv[3], argv[6] if len(argv) > 6 else "sha256"))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
