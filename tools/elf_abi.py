#!/usr/bin/env python3
"""Which FMRRE build (out/<arch>/recovery) fits a phone?

  elf_abi.py FILE          FILE is an Android ELF binary (e.g. system/bin/recovery from the stock ramdisk)
  elf_abi.py --abi NAME    NAME is an Android ABI (adb shell getprop ro.product.cpu.abi)
  -v                       also print what was found

Prints one of: arm64 arm armv5 x86 x86_64 riscv64 mips   (exit 1 if unknown / not ELF)
"""
import struct
import sys

ABI_TO_ARCH = {
    "arm64-v8a": "arm64", "arm64": "arm64", "aarch64": "arm64",
    "armeabi-v7a": "arm", "armeabi": "armv5",
    "x86": "x86", "x86_64": "x86_64",
    "riscv64": "riscv64",
    "mips": "mips",
}

EM_386, EM_MIPS, EM_ARM, EM_X86_64, EM_AARCH64, EM_RISCV = 3, 8, 40, 62, 183, 243
SHT_ARM_ATTRIBUTES = 0x70000003
ARM_ARCH_NAMES = ["pre-v4", "v4", "v4T", "v5T", "v5TE", "v5TEJ", "v6", "v6KZ", "v6T2", "v6K", "v7", "v6-M", "v6S-M",
                  "v7E-M", "v8", "v8-R", "v8-M.base", "v8-M.main", "v8.1-M.main", "v9"]


def uleb(b, i):
    v = shift = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return v, i


def arm_cpu_arch(data, end):
    """Tag_CPU_arch from an .ARM.attributes section, or None."""
    if not data or data[0:1] != b"A":
        return None
    i = 1
    while i + 4 <= len(data):
        sub_len = struct.unpack_from(end + "I", data, i)[0]
        sub_end = i + sub_len
        j = data.index(b"\0", i + 4) + 1
        vendor = data[i + 4:j - 1]
        if vendor == b"aeabi":
            while j < sub_end:
                tag, k = uleb(data, j)
                size = struct.unpack_from(end + "I", data, k)[0]
                block_end = j + size
                k += 4
                if tag == 1:  # Tag_File: attributes for the whole file
                    while k < block_end:
                        at, k = uleb(data, k)
                        if at in (4, 5, 67):           # CPU_raw_name, CPU_name, conformance: strings
                            k = data.index(b"\0", k) + 1
                        elif at == 32:                 # Tag_compatibility: uleb + string
                            _, k = uleb(data, k)
                            k = data.index(b"\0", k) + 1
                        else:
                            val, k = uleb(data, k)
                            if at == 6:
                                return val
                j = block_end
        i = sub_end if sub_len else len(data)
    return None


def elf_arch(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF":
        return None, "not an ELF file"
    cls, data_enc = d[4], d[5]
    end = "<" if data_enc == 1 else ">"
    machine = struct.unpack_from(end + "H", d, 18)[0]
    if machine == EM_AARCH64:
        return "arm64", "AArch64"
    if machine == EM_X86_64:
        return "x86_64", "x86-64"
    if machine == EM_386:
        return "x86", "i386"
    if machine == EM_RISCV:
        return ("riscv64", "RISC-V 64") if cls == 2 else (None, "RISC-V 32 (not an Android ABI)")
    if machine == EM_MIPS:
        if cls == 2:
            return None, "MIPS64 (no FMRRE build)"
        if data_enc != 1:
            return None, "big-endian MIPS (not an Android ABI)"
        return "mips", "MIPS32 little-endian"
    if machine == EM_ARM:
        if data_enc != 1:
            return None, "big-endian ARM (not an Android ABI)"
        # ELF32 section headers: find SHT_ARM_ATTRIBUTES
        shoff = struct.unpack_from("<I", d, 0x20)[0]
        shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
        arch = None
        for n in range(shnum if shoff else 0):
            o = shoff + n * shentsize
            if o + 40 > len(d):
                break
            sh_type = struct.unpack_from("<I", d, o + 4)[0]
            if sh_type == SHT_ARM_ATTRIBUTES:
                off, size = struct.unpack_from("<II", d, o + 16)
                try:
                    arch = arm_cpu_arch(d[off:off + size], "<")
                except (ValueError, IndexError, struct.error):
                    arch = None
                break
        if arch is None:
            return "arm", "ARM 32-bit (no attributes: assuming ARMv7)"
        name = ARM_ARCH_NAMES[arch] if arch < len(ARM_ARCH_NAMES) else str(arch)
        return ("arm" if arch >= 10 else "armv5"), "ARM 32-bit, " + name
    return None, f"unsupported machine {machine}"


def main():
    args = [a for a in sys.argv[1:] if a != "-v"]
    verbose = "-v" in sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    if args[0] == "--abi":
        abi = args[1].strip() if len(args) > 1 else ""
        arch = ABI_TO_ARCH.get(abi)
        if verbose:
            print(f"{abi} -> {arch or 'no FMRRE build'}", file=sys.stderr)
    else:
        try:
            arch, what = elf_arch(args[0])
        except OSError as e:
            arch, what = None, str(e)
        if verbose:
            print(f"{args[0]}: {what}", file=sys.stderr)
    if not arch:
        return 1
    print(arch)
    return 0


if __name__ == "__main__":
    sys.exit(main())
