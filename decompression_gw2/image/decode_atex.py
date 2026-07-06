#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
decode_atex.py -- CLI: ArenaNet ATEX texture -> PNG

Usage:
    python decode_atex.py FILE [FILE ...]          # decode mip0 of each -> FILE.png
    python decode_atex.py --all-mips FILE          # also dump every mip level
    python decode_atex.py --info FILE              # just print the header/mip table
    python decode_atex.py --outdir DIR FILE ...
"""

import os
import sys
import argparse

from PIL import Image

import gw2_atex
import block_decoders


def decode_file(path, outdir=None, all_mips=False, info_only=False):
    with open(path, "rb") as f:
        data = f.read()
    tex = gw2_atex.parse_atex(bytearray(data))

    print("%-28s %s  fmt=%-6s %dx%d  mips=%d" % (
        os.path.basename(path), tex["magic"].decode("latin1"),
        tex["fmt_name"], tex["width"], tex["height"], len(tex["mips"])))
    for mip in tex["mips"]:
        print("    mip%d  %4dx%-4d  blocks=%dx%d  bpb=%2d  flags=0x%02x  %s"
              % (mip["level"], mip["width"], mip["height"], mip["block_w"],
                 mip["block_h"], mip["bpb"], mip["flags"],
                 "raw" if mip["raw"] else "compressed"))
    if info_only:
        return

    base = os.path.splitext(os.path.basename(path))[0]
    outdir = outdir or os.path.dirname(os.path.abspath(path))
    os.makedirs(outdir, exist_ok=True)

    targets = tex["mips"] if all_mips else tex["mips"][:1]
    for mip in targets:
        img = block_decoders.decode_surface(
            tex["fmt_enum"], mip["surface"], mip["width"], mip["height"])
        suffix = "" if mip["level"] == 0 else "_mip%d" % mip["level"]
        out = os.path.join(outdir, "%s%s.png" % (base, suffix))
        Image.fromarray(img, "RGBA").save(out)
        print("    -> %s" % out)


def main(argv):
    ap = argparse.ArgumentParser(description="Decode ArenaNet ATEX textures to PNG")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--all-mips", action="store_true")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--outdir", default=None)
    args = ap.parse_args(argv)

    for path in args.files:
        try:
            decode_file(path, args.outdir, args.all_mips, args.info)
        except Exception as exc:
            print("!! %s : %s" % (os.path.basename(path), exc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
