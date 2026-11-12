@echo off
rem Windows twin of zig-cc: zig's clang for a static musl binary. ZIG = zig.exe (pc/fmrre_build.py sets it).
if not defined ZIG_TARGET set ZIG_TARGET=aarch64-linux-musl
if defined ZIG_CPU (
  "%ZIG%" cc -target %ZIG_TARGET% -mcpu=%ZIG_CPU% %*
) else (
  "%ZIG%" cc -target %ZIG_TARGET% %*
)
