#!/usr/bin/env python3
"""Delete one simple root fixture entry and release its contiguous clusters."""
import struct
import sys


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def fail(message):
    raise SystemExit("FAIL: " + message)


def main(image_name, wanted):
    with open(image_name, "r+b") as image:
        boot = image.read(512)
        if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
            fail("not an exFAT image")
        sector_size = 1 << boot[108]
        sectors_per_cluster = 1 << boot[109]
        cluster_size = sector_size * sectors_per_cluster
        heap_offset = u32(boot, 88)
        root_cluster = u32(boot, 96)
        root_offset = (heap_offset
            + (root_cluster - 2) * sectors_per_cluster) * sector_size
        image.seek(root_offset)
        root = bytearray(image.read(cluster_size))
        bitmap_cluster = None
        target = None

        for offset in range(0, len(root), 32):
            if root[offset] == 0:
                break
            if root[offset] == 0x81:
                bitmap_cluster = u32(root, offset + 20)
            if root[offset] != 0x85:
                continue
            count = root[offset + 1] + 1
            end = offset + count * 32
            if (count < 3 or count > 19 or end > len(root)
                    or root[offset + 32] != 0xc0):
                fail("malformed file entry set")
            name_length = root[offset + 35]
            name_bytes = bytearray()
            for name_entry in range(offset + 64, end, 32):
                if root[name_entry] != 0xc1:
                    fail("non-name secondary in fixture set")
                name_bytes.extend(root[name_entry + 2:name_entry + 32])
            name = name_bytes[:name_length * 2].decode("utf-16le")
            if name == wanted:
                target = (offset, count)
                break

        if bitmap_cluster is None or target is None:
            fail("bitmap or target entry missing")
        offset, count = target
        stream = offset + 32
        first_cluster = u32(root, stream + 20)
        data_length = u64(root, stream + 24)
        clusters = (data_length + cluster_size - 1) // cluster_size
        if clusters == 0 or (root[stream + 1] & 2) == 0:
            fail("fixture deletion requires a nonempty contiguous stream")

        for entry in range(count):
            root[offset + entry * 32] &= 0x7f
        image.seek(root_offset + offset)
        image.write(root[offset:offset + count * 32])

        bitmap_offset = (heap_offset
            + (bitmap_cluster - 2) * sectors_per_cluster) * sector_size
        for cluster in range(first_cluster, first_cluster + clusters):
            bit = cluster - 2
            image.seek(bitmap_offset + bit // 8)
            value = image.read(1)
            if len(value) != 1 or (value[0] & (1 << (bit & 7))) == 0:
                fail("fixture cluster was not allocated")
            image.seek(bitmap_offset + bit // 8)
            image.write(bytes([value[0] & ~(1 << (bit & 7))]))
        print("deleted %s at cluster %d (%d clusters)" % (
            wanted, first_cluster, clusters))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: exfat_delete_fixture.py IMAGE NAME")
    main(sys.argv[1], sys.argv[2])
