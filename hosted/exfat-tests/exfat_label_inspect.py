#!/usr/bin/env python3
"""Print the active exFAT volume-label entry from a raw image."""

import struct
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} IMAGE", file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as image:
        boot = image.read(512)
        if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
            raise SystemExit("FAIL: not an exFAT image")
        sector_size = 1 << boot[108]
        sectors_per_cluster = 1 << boot[109]
        heap_offset = struct.unpack_from("<I", boot, 88)[0]
        root_cluster = struct.unpack_from("<I", boot, 96)[0]
        image.seek((heap_offset + (root_cluster - 2) * sectors_per_cluster)
                   * sector_size)
        directory = image.read(sector_size * sectors_per_cluster)
    for offset in range(0, len(directory), 32):
        entry = directory[offset:offset + 32]
        if not entry or entry[0] == 0:
            break
        if entry[0] == 0x83:
            length = entry[1]
            if length > 15:
                raise SystemExit("FAIL: invalid exFAT label length")
            print(entry[2:2 + length * 2].decode("utf-16le"))
            return 0
    raise SystemExit("FAIL: active exFAT label not found")


if __name__ == "__main__":
    raise SystemExit(main())
