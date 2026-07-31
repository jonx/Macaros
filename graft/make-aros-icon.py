#!/usr/bin/env python3
"""make-aros-icon.py -- turn a Mac PNG into an AROS Workbench icon (.info).

AROS icon.library reads OS4-style PNG icons: the .info file IS a PNG, with the
Amiga side of the icon carried in a private `icOn` chunk placed just before
IEND (workbench/libs/icon/diskobjPNGio.c). A PNG with no icOn chunk still
loads, but then do_Type is 0 and every Amiga attribute takes a default, so we
write the chunk and set the ones that matter.

  make-aros-icon.py src.png Zed.info --size 64 --stack 262144
  make-aros-icon.py src.png Docs.info --type drawer

Needs ImageMagick for the resize; the chunk surgery is done here.
"""
import argparse
import binascii
import struct
import subprocess
import sys
from pathlib import Path

ATTR_STACKSIZE = 0x80001009
ATTR_DEFAULTTOOL = 0x8000100A
ATTR_TOOLTYPE = 0x8000100B
ATTR_TYPE = 0x8000100F
ATTR_FRAMELESS = 0x80001010

WB_TYPE = {"disk": 1, "drawer": 2, "tool": 3, "project": 4, "garbage": 5}


def attr_long(attr, value):
    return struct.pack(">II", attr, value & 0xFFFFFFFF)


def attr_str(attr, value):
    return struct.pack(">I", attr) + value.encode("latin-1") + b"\0"


def png_chunk(kind, payload):
    crc = binascii.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


def insert_icon_chunk(png_bytes, payload):
    """Return the PNG with an icOn chunk spliced in ahead of IEND."""
    if png_bytes[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("not a PNG")
    out = bytearray(png_bytes[:8])
    pos = 8
    while pos < len(png_bytes):
        (size,) = struct.unpack(">I", png_bytes[pos : pos + 4])
        kind = png_bytes[pos + 4 : pos + 8]
        whole = png_bytes[pos : pos + 12 + size]
        if kind == b"icOn":
            pos += 12 + size          # drop any chunk we wrote before
            continue
        if kind == b"IEND":
            out += png_chunk(b"icOn", payload)
        out += whole
        pos += 12 + size
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("output")
    ap.add_argument("--size", type=int, default=64)
    ap.add_argument("--type", default="tool", choices=sorted(WB_TYPE))
    ap.add_argument("--stack", type=int, default=262144)
    ap.add_argument("--default-tool", default=None)
    ap.add_argument("--tooltype", action="append", default=[])
    ap.add_argument("--framed", action="store_true",
                    help="let Workbench draw a border box around the image")
    args = ap.parse_args()

    src, dst = Path(args.source), Path(args.output)
    if not src.is_file():
        sys.exit(f"no such file: {src}")

    # Resize on transparency, and strip the Mac colour profile: icon.library
    # decodes with plain libpng and has no colour management.
    subprocess.run(
        ["magick", str(src), "-strip", "-resize", f"{args.size}x{args.size}",
         "-background", "none", "-gravity", "center",
         "-extent", f"{args.size}x{args.size}", "PNG32:" + str(dst)],
        check=True,
    )

    payload = b"".join([
        attr_long(ATTR_FRAMELESS, 0 if args.framed else 1),
        attr_long(ATTR_STACKSIZE, args.stack),
        attr_long(ATTR_TYPE, WB_TYPE[args.type]),
    ])
    if args.default_tool:
        payload += attr_str(ATTR_DEFAULTTOOL, args.default_tool)
    for tt in args.tooltype:
        payload += attr_str(ATTR_TOOLTYPE, tt)

    dst.write_bytes(insert_icon_chunk(dst.read_bytes(), payload))
    print(f"[icon] {dst} ({args.size}x{args.size}, {args.type}, {dst.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
