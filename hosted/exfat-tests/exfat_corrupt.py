#!/usr/bin/env python3
"""Deterministic byte-level exFAT corruption tool for the target corpus.

The mutations are deliberately applied after macOS has made a valid control
image.  This keeps the formatter outside the assertion and lets each vector
compare one changed byte range against its clean sibling.
"""
import struct
import sys


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def put16(b, o, v): struct.pack_into("<H", b, o, v)


def boot(image):
    image.seek(0)
    b = bytearray(image.read(512))
    if len(b) != 512 or b[3:11] != b"EXFAT   ":
        raise SystemExit("FAIL: expected exFAT main boot sector")
    return b


def root_offset(b):
    sector_size = 1 << b[108]
    return (u32(b, 88) + (u32(b, 96) - 2) * (1 << b[109])) * sector_size


def main(path, mode):
    with open(path, "r+b") as image:
        b = boot(image)
        if mode == "boot-checksum":
            # Covered byte in sector zero; sector 11 remains unaltered.
            image.seek(120); image.write(b"\x01")
        elif mode == "primary-bad-backup-good":
            # Primary identity is broken while backup sector 12 remains intact.
            image.seek(3); image.write(b"X")
        elif mode == "texfat":
            # NumberOfFats == 2 is classified before geometry.
            image.seek(110); image.write(b"\x02")
        elif mode == "set-checksum":
            off = root_offset(b)
            image.seek(off)
            root = bytearray(image.read(4096))
            for i in range(0, len(root), 32):
                if root[i] == 0x85:
                    put16(root, i + 2, (root[i + 2] | (root[i + 3] << 8)) ^ 1)
                    image.seek(off); image.write(root)
                    break
            else:
                raise SystemExit("FAIL: no file entry set to corrupt")
        else:
            raise SystemExit("usage: exfat_corrupt.py IMAGE boot-checksum|primary-bad-backup-good|texfat|set-checksum")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: exfat_corrupt.py IMAGE MODE")
    main(sys.argv[1], sys.argv[2])
