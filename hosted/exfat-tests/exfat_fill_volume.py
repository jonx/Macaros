#!/usr/bin/env python3
"""Extend one contiguous fixture file over every free exFAT cluster.

This is a raw test-fixture author, not a filesystem implementation.  It uses
only the public exFAT entry, FAT and allocation-bitmap layouts and leaves the
image in a state which an independent fsck must accept.
"""

import struct
import sys


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def put16(data, offset, value):
    struct.pack_into("<H", data, offset, value)


def put32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def put64(data, offset, value):
    struct.pack_into("<Q", data, offset, value)


def fail(message):
    raise SystemExit("FAIL: " + message)


def checksum(entry_set):
    value = 0
    for offset, byte in enumerate(entry_set):
        if offset in (2, 3):
            continue
        value = (((value << 15) | (value >> 1)) + byte) & 0xffff
    return value


def cluster_offset(heap_offset, sectors_per_cluster, sector_size, cluster):
    return (heap_offset + (cluster - 2) * sectors_per_cluster) * sector_size


def main(image_name, wanted):
    with open(image_name, "r+b") as image:
        boot = bytearray(image.read(512))
        if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
            fail("not an exFAT image")
        sector_size = 1 << boot[108]
        sectors_per_cluster = 1 << boot[109]
        cluster_size = sector_size * sectors_per_cluster
        fat_offset = u32(boot, 80) * sector_size
        heap_offset = u32(boot, 88)
        cluster_count = u32(boot, 92)
        root_cluster = u32(boot, 96)
        if root_cluster < 2 or cluster_count == 0:
            fail("invalid geometry")

        root_offset = cluster_offset(heap_offset, sectors_per_cluster,
                                     sector_size, root_cluster)
        image.seek(root_offset)
        root = bytearray(image.read(cluster_size))
        bitmap_cluster = None
        bitmap_length = None
        target = None

        for offset in range(0, len(root), 32):
            if root[offset] == 0:
                break
            if root[offset] == 0x81:
                bitmap_cluster = u32(root, offset + 20)
                bitmap_length = u64(root, offset + 24)
                continue
            if root[offset] != 0x85:
                continue
            count = root[offset + 1] + 1
            end = offset + count * 32
            if count < 3 or end > len(root) or root[offset + 32] != 0xc0:
                fail("malformed root file entry set")
            name_length = root[offset + 35]
            name_bytes = bytearray()
            for name_offset in range(offset + 64, end, 32):
                if root[name_offset] != 0xc1:
                    fail("fixture file has a non-name extension")
                name_bytes.extend(root[name_offset + 2:name_offset + 32])
            name = name_bytes[:name_length * 2].decode("utf-16le")
            if name == wanted:
                target = (offset, count)

        if bitmap_cluster is None or bitmap_length is None or target is None:
            fail("bitmap or filler entry missing")
        if bitmap_length < (cluster_count + 7) // 8:
            fail("short allocation bitmap")

        bitmap_offset = cluster_offset(heap_offset, sectors_per_cluster,
                                       sector_size, bitmap_cluster)
        image.seek(bitmap_offset)
        bitmap = bytearray(image.read(bitmap_length))
        if len(bitmap) != bitmap_length:
            fail("short bitmap read")

        entry_offset, entry_count = target
        stream_offset = entry_offset + 32
        first_cluster = u32(root, stream_offset + 20)
        old_length = u64(root, stream_offset + 24)
        old_clusters = (old_length + cluster_size - 1) // cluster_size
        if (root[stream_offset + 1] & 2) == 0 or old_clusters == 0:
            fail("filler must begin as a nonempty contiguous stream")
        if first_cluster < 2 or first_cluster + old_clusters > cluster_count + 2:
            fail("filler allocation is outside the heap")

        existing = list(range(first_cluster, first_cluster + old_clusters))
        for cluster in existing:
            bit = cluster - 2
            if (bitmap[bit // 8] & (1 << (bit & 7))) == 0:
                fail("filler cluster is not allocated")

        free = []
        for bit in range(cluster_count):
            if (bitmap[bit // 8] & (1 << (bit & 7))) == 0:
                free.append(bit + 2)
        if not free:
            fail("fixture is already full")

        zero = bytes(cluster_size)
        for cluster in free:
            image.seek(cluster_offset(heap_offset, sectors_per_cluster,
                                      sector_size, cluster))
            image.write(zero)
            bit = cluster - 2
            bitmap[bit // 8] |= 1 << (bit & 7)

        chain = existing + free
        for index, cluster in enumerate(chain):
            following = chain[index + 1] if index + 1 < len(chain) else 0xffffffff
            image.seek(fat_offset + cluster * 4)
            image.write(struct.pack("<I", following))

        new_length = len(chain) * cluster_size
        root[stream_offset + 1] &= ~2
        put64(root, stream_offset + 8, new_length)
        put64(root, stream_offset + 24, new_length)
        entry_end = entry_offset + entry_count * 32
        entry_set = root[entry_offset:entry_end]
        put16(entry_set, 2, checksum(entry_set))
        root[entry_offset:entry_end] = entry_set

        image.seek(bitmap_offset)
        image.write(bitmap)
        image.seek(root_offset + entry_offset)
        image.write(root[entry_offset:entry_end])
        boot[112] = 100
        image.seek(0)
        image.write(boot)
        image.flush()

    print("FULL clusters=%d filler_clusters=%d added=%d" %
          (cluster_count, len(chain), len(free)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: exfat_fill_volume.py IMAGE FILLER")
    main(sys.argv[1], sys.argv[2])
