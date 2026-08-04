#!/usr/bin/env python3
"""Inspect one ordinary exFAT file stream in a raw image.

This is a test oracle written from the public exFAT 1.00 layout: it does not
use an exFAT implementation.  It is intentionally read-only and reports only
the stream facts the recipe corpus needs (name hash, NoFatChain and extents).
"""
import struct
import sys

ENTRY_FILE = 0x85
ENTRY_STREAM = 0xc0
ENTRY_NAME = 0xc1
EOC = 0xfffffff8


def u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def u64(data, off):
    return struct.unpack_from("<Q", data, off)[0]


def fail(message):
    raise SystemExit("FAIL: " + message)


def main(image_name, wanted):
    with open(image_name, "rb") as image:
        boot = image.read(512)
        if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
            fail("not an exFAT image")
        sector_size = 1 << boot[108]
        sectors_per_cluster = 1 << boot[109]
        cluster_size = sector_size * sectors_per_cluster
        fat_offset = u32(boot, 80)
        heap_offset = u32(boot, 88)
        root_cluster = u32(boot, 96)
        if root_cluster < 2:
            fail("bad root cluster")
        image.seek((heap_offset + (root_cluster - 2) * sectors_per_cluster)
                   * sector_size)
        directory = image.read(cluster_size)

        for off in range(0, len(directory), 32):
            if directory[off] == 0:
                break
            if directory[off] != ENTRY_FILE:
                continue
            count = directory[off + 1]
            end = off + (count + 1) * 32
            if count < 2 or end > len(directory) or directory[off + 32] != ENTRY_STREAM:
                fail("malformed file entry set")
            stream = directory[off + 32:off + 64]
            name_length = stream[3]
            name_bytes = bytearray()
            for entry_off in range(off + 64, end, 32):
                if directory[entry_off] != ENTRY_NAME:
                    fail("malformed name entry")
                name_bytes.extend(directory[entry_off + 2:entry_off + 32])
            name = name_bytes[:name_length * 2].decode("utf-16le")
            if name != wanted:
                continue
            first_cluster = u32(stream, 20)
            length = u64(stream, 24)
            clusters = (length + cluster_size - 1) // cluster_size
            nofat = (stream[1] & 2) != 0
            extents = 1 if clusters else 0
            if not nofat and clusters:
                image.seek(fat_offset * sector_size + first_cluster * 4)
                current = first_cluster
                for _ in range(1, clusters):
                    image.seek(fat_offset * sector_size + current * 4)
                    following = struct.unpack("<I", image.read(4))[0]
                    if following >= EOC or following < 2:
                        fail("short FAT chain for " + wanted)
                    if following != current + 1:
                        extents += 1
                    current = following
            print("name=%s hash=%04x nofat=%d clusters=%d extents=%d" % (
                wanted, u16(stream, 4), int(nofat), clusters, extents))
            return
    fail("entry not found: " + wanted)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: exfat_stream_inspect.py IMAGE NAME")
    main(sys.argv[1], sys.argv[2])
