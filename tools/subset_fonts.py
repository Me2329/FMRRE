#!/usr/bin/env python3
"""Rebuild the font subsets in res/fonts from the full fonts (pip install fonttools).

The recovery embeds small subsets (tools/embed_res.py) to stay a single static binary. They cover
ASCII, Latin-1, Latin Extended-A, the Romanian comma-below letters (Ș ș Ț ț), typographic
punctuation and the arrows / check marks the UI uses.

    tools/subset_fonts.py [--inter DIR] [--dejavu DIR]   (defaults: Debian/Ubuntu font paths)
"""
import argparse, os, subprocess, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("--inter", default="/usr/share/fonts/opentype/inter")
ap.add_argument("--dejavu", default="/usr/share/fonts/truetype/dejavu")
a = ap.parse_args()

ranges = [
    "U+0020-007E",          # ASCII
    "U+00A0-00FF",          # Latin-1 Supplement
    "U+0100-017F",          # Latin Extended-A (ă Ă ş ţ ...)
    "U+0218-021B",          # Ș ș Ț ț (Romanian, comma below)
    "U+2013-2014,U+2018-201B,U+201C-201E,U+2022,U+2026",  # dashes, quotes, bullet, ellipsis
    "U+2190-2193,U+25B2,U+25BC,U+2713,U+2717",            # arrows, triangles, check / cross
]
jobs = [
    (os.path.join(a.inter, "Inter-Regular.otf"), "res/fonts/Inter-Regular.otf"),
    (os.path.join(a.inter, "Inter-SemiBold.otf"), "res/fonts/Inter-SemiBold.otf"),
    (os.path.join(a.inter, "Inter-Bold.otf"), "res/fonts/Inter-Bold.otf"),
    (os.path.join(a.dejavu, "DejaVuSansMono.ttf"), "res/fonts/DejaVuSansMono.ttf"),
]
for src, dst in jobs:
    if not os.path.exists(src):
        sys.exit(f"missing {src} (pass --inter / --dejavu)")
    subprocess.check_call([sys.executable, "-m", "fontTools.subset", src, "--unicodes=" + ",".join(ranges),
                           "--layout-features=", "--drop-tables+=GSUB,GPOS,GDEF", "--no-hinting",  # stb_truetype draws plain glyphs
                           "--output-file=" + os.path.join(root, dst)])
    print(f"{dst}: {os.path.getsize(os.path.join(root, dst))} bytes")
