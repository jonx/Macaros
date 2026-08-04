#!/usr/bin/env python3
"""Inspect or replace the main exFAT boot-sector VolumeFlags word."""

import struct
import sys


def main() -> int:
    if len(sys.argv) not in (2, 4) or (len(sys.argv) == 4 and sys.argv[1] != "--set"):
        print(f"usage: {sys.argv[0]} [--set FLAGS] IMAGE", file=sys.stderr)
        return 2
    path = sys.argv[-1]
    if len(sys.argv) == 4:
        try:
            flags = int(sys.argv[2], 0)
        except ValueError:
            print(f"invalid VolumeFlags value: {sys.argv[2]}", file=sys.stderr)
            return 2
        if not 0 <= flags <= 0xFFFF:
            print(f"VolumeFlags out of range: {sys.argv[2]}", file=sys.stderr)
            return 2
        with open(path, "r+b") as image:
            image.seek(0x6A)
            image.write(struct.pack("<H", flags))
            image.flush()
        return 0
    with open(path, "rb") as image:
        image.seek(0x6A)
        raw = image.read(2)
    if len(raw) != 2:
        print("short boot sector", file=sys.stderr)
        return 1
    print(f"0x{struct.unpack('<H', raw)[0]:04x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
