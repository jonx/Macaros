#!/usr/bin/env python3
"""Report allocation-bitmap occupancy from a small exFAT test image."""

import struct
import sys


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def fail(message):
    raise SystemExit("FAIL: " + message)


def main(image_name):
    with open(image_name, "rb") as image:
        boot = image.read(512)
        if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
            fail("not an exFAT image")
        sector_size = 1 << boot[108]
        sectors_per_cluster = 1 << boot[109]
        cluster_size = sector_size * sectors_per_cluster
        heap_offset = u32(boot, 88)
        cluster_count = u32(boot, 92)
        root_cluster = u32(boot, 96)
        image.seek((heap_offset + (root_cluster - 2) * sectors_per_cluster)
                   * sector_size)
        root = image.read(cluster_size)
        bitmap_cluster = None
        bitmap_length = None
        for offset in range(0, len(root), 32):
            if root[offset] == 0:
                break
            if root[offset] == 0x81:
                bitmap_cluster = u32(root, offset + 20)
                bitmap_length = u64(root, offset + 24)
                break
        if bitmap_cluster is None or bitmap_length is None:
            fail("allocation bitmap entry missing")
        image.seek((heap_offset + (bitmap_cluster - 2) * sectors_per_cluster)
                   * sector_size)
        bitmap = image.read(bitmap_length)
        if len(bitmap) != bitmap_length:
            fail("short bitmap")
        used = sum((bitmap[bit // 8] >> (bit & 7)) & 1
                   for bit in range(cluster_count))
        print("clusters=%d used=%d free=%d percent=%d" %
              (cluster_count, used, cluster_count - used, boot[112]))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: exfat_bitmap_inspect.py IMAGE")
    main(sys.argv[1])
