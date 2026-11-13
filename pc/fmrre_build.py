#!/usr/bin/env python3
"""Compile FMRRE from this project folder - on Windows, Linux or macOS, with nothing but Python.

  python pc/fmrre_build.py [arm64|arm|x86|x86_64|riscv64]      (default arm64: the Galaxy S8 and most phones)

The compiler is Zig (a clang that cross-compiles to a static musl binary - no Android NDK), driven by CMake and
Ninja. All three come from pip and are installed for this user when missing (asked first; --yes skips the
question). The result is out/<arch>/recovery, the same binary scripts/build.sh makes.
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ARCHS = {  # same table as scripts/build.sh
    "arm64": ("aarch64-linux-musl", "baseline", "arm64-v8a"),
    "arm": ("arm-linux-musleabihf", "cortex_a8", "armeabi-v7a"),
    "x86": ("x86-linux-musl", "core2", "x86"),
    "x86_64": ("x86_64-linux-musl", "x86_64", "x86_64"),
    "riscv64": ("riscv64-linux-musl", "baseline", "riscv64"),
}
ALIASES = {"aarch64": "arm64", "arm64-v8a": "arm64", "armeabi-v7a": "arm", "armv7": "arm", "amd64": "x86_64"}


class BuildError(Exception):
    pass


def tools(ask, log):
    """(zig, cmake, ninja) executables; the missing pip packages are installed after asking."""
    def zig():
        try:
            import ziglang
            d = os.path.dirname(ziglang.__file__)
            for n in ("zig.exe", "zig"):
                if os.path.isfile(os.path.join(d, n)):
                    return os.path.join(d, n)
        except ImportError:
            pass
        return shutil.which("zig")

    def cmake():
        try:
            import cmake as c
            p = os.path.join(c.CMAKE_BIN_DIR, "cmake.exe" if os.name == "nt" else "cmake")
            if os.path.isfile(p):
                return p
        except (ImportError, AttributeError):
            pass
        return shutil.which("cmake")

    def ninja():
        try:
            import ninja as n
            p = os.path.join(n.BIN_DIR, "ninja.exe" if os.name == "nt" else "ninja")
            if os.path.isfile(p):
                return p
        except (ImportError, AttributeError):
            pass
        return shutil.which("ninja")

    found = {"ziglang": zig(), "cmake": cmake(), "ninja": ninja()}
    missing = [k for k, v in found.items() if not v]
    if missing:
        if not ask("Install %s with pip (about 70 MB, once)?" % " + ".join(missing)):
            raise BuildError("needed to compile: %s (pip install %s)" % (", ".join(missing), " ".join(missing)))
        log("Installing %s ..." % ", ".join(missing))
        cmd = [sys.executable, "-m", "pip", "install", "--user", "--quiet"] + missing
        if subprocess.call(cmd) != 0 and subprocess.call(cmd[:4] + cmd[5:]) != 0:  # a venv refuses --user
            raise BuildError("pip couldn't install %s" % ", ".join(missing))
        import importlib
        import site
        importlib.invalidate_caches()
        user = site.getusersitepackages()
        if user not in sys.path:
            sys.path.append(user)
        found = {"ziglang": zig(), "cmake": cmake(), "ninja": ninja()}
        if not all(found.values()):
            raise BuildError("installed, but not found: open a new terminal and run this again")
    return found["ziglang"], found["cmake"], found["ninja"]


def build(arch="arm64", ask=lambda q: True, log=print, jobs=None):
    """Compile out/<arch>/recovery. Returns its path."""
    arch = ALIASES.get(arch, arch)
    if arch not in ARCHS:
        raise BuildError("unknown architecture %s (one of %s)" % (arch, ", ".join(ARCHS)))
    target, cpu, abi = ARCHS[arch]
    zig, cmake, ninja = tools(ask, log)
    bdir = os.path.join(ROOT, "build-pc-" + arch)  # its own folder: scripts/build.sh's use another generator
    env = dict(os.environ, ZIG=zig, ZIG_TARGET=target, ZIG_CPU=cpu)
    log("Compiling FMRRE for %s (%s) with %s ..." % (arch, abi, os.path.basename(zig)))
    cfg = [cmake, "-S", ROOT, "-B", bdir, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ROOT, "tools", "toolchain", "zig-android.cmake"),
           "-DZIG_TARGET=" + target, "-DZIG_CPU=" + cpu, "-DTW_SELFTEST=OFF"]
    stamp = os.path.join(bdir, ".zig-target")
    if os.path.isfile(stamp) and open(stamp).read().strip() != target + " " + cpu:
        shutil.rmtree(bdir, ignore_errors=True)  # objects for another CPU must not be reused
    if not os.path.isfile(os.path.join(bdir, "build.ninja")):
        r = subprocess.run(cfg, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            shutil.rmtree(bdir, ignore_errors=True)
            raise BuildError("configure failed:\n" + r.stdout.decode("utf-8", "replace")[-3000:])
        with open(stamp, "w") as f:
            f.write(target + " " + cpu)
    cmd = [cmake, "--build", bdir, "--target", "fmrre"] + (["-j", str(jobs)] if jobs else [])
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    tail = []
    for raw in p.stdout:
        line = raw.decode("utf-8", "replace").rstrip()
        tail = (tail + [line])[-60:]
        if line.startswith("[") and "/" in line.split("]")[0]:  # ninja's [12/96] progress
            n, total = line[1:].split("]")[0].split("/")
            if n == total or int(n) % 16 == 0:
                log("  %s/%s" % (n, total))
        elif "error" in line.lower():
            log(line)
    if p.wait() != 0:
        raise BuildError("compile failed:\n" + "\n".join(tail[-30:]))
    out = os.path.join(ROOT, "out", arch)
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(bdir, "fmrre")
    dst = os.path.join(out, "recovery")
    shutil.copyfile(exe, dst)
    os.chmod(dst, 0o755)
    with open(os.path.join(out, "ABI"), "w") as f:
        f.write(abi + "\n")
    with open(dst, "rb") as f:
        head = f.read(20)
    if head[:4] != b"\x7fELF":
        raise BuildError("the compiler made no ELF program")
    log("==> %s (%.1f MB)" % (dst, os.path.getsize(dst) / 1048576.0))
    return dst


def up_to_date(arch="arm64"):
    """out/<arch>/recovery newer than every source file?"""
    dst = os.path.join(ROOT, "out", ALIASES.get(arch, arch), "recovery")
    if not os.path.isfile(dst):
        return False
    t = os.path.getmtime(dst)
    for d in ("src", "third_party", "res"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):
            for n in files:
                if os.path.getmtime(os.path.join(dirpath, n)) > t:
                    return False
    return os.path.getmtime(os.path.join(ROOT, "CMakeLists.txt")) <= t


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("arch", nargs="?", default="arm64")
    ap.add_argument("-y", "--yes", action="store_true", help="install missing tools without asking")
    ap.add_argument("-j", "--jobs", type=int)
    a = ap.parse_args(argv)
    ask = (lambda q: True) if a.yes else (lambda q: input(q + " [y/N] ").strip().lower() in ("y", "yes", "d", "da"))
    try:
        build(a.arch, ask, print, a.jobs)
    except BuildError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
