#!/usr/bin/env python3
"""Raw oracle for benign-secondary preservation and allocation cleanup."""
import struct
import sys


def u32(data, off): return struct.unpack_from("<I", data, off)[0]
def u64(data, off): return struct.unpack_from("<Q", data, off)[0]


def fail(message):
    raise SystemExit("FAIL: " + message)


def entry_name(directory, off, include_deleted=False):
    if off + 64 > len(directory):
        return None
    primary = directory[off] | (0x80 if include_deleted else 0)
    if primary != 0x85:
        return None
    count = directory[off + 1] + 1
    end = off + count * 32
    if count < 3 or end > len(directory):
        return None
    if (directory[off + 32] | (0x80 if include_deleted else 0)) != 0xC0:
        return None
    length = directory[off + 35]
    result = bytearray()
    need = (length + 14) // 15
    for index in range(need):
        entry = off + (index + 2) * 32
        if (directory[entry] | (0x80 if include_deleted else 0)) != 0xC1:
            return None
        result.extend(directory[entry + 2:entry + 32])
    try:
        return result[:length * 2].decode("utf-16le")
    except UnicodeDecodeError:
        return None


def find_set(directory, name, include_deleted=False):
    for off in range(0, len(directory), 32):
        if directory[off] == 0:
            break
        if entry_name(directory, off, include_deleted) == name:
            return off
    fail("entry not found: " + name)


def main(path):
    with open(path, "rb") as image:
        boot = image.read(512)
        if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
            fail("not an exFAT image")
        sector_size = 1 << boot[108]
        sectors_per_cluster = 1 << boot[109]
        cluster_size = sector_size * sectors_per_cluster
        fat_offset = u32(boot, 80)
        heap = u32(boot, 88)
        root_cluster = u32(boot, 96)

        def cluster_offset(cluster):
            return (heap + (cluster - 2) * sectors_per_cluster) * sector_size

        def read_stream(stream):
            first_cluster = u32(stream, 20)
            length = u64(stream, 24)
            clusters = (length + cluster_size - 1) // cluster_size
            contiguous = (stream[1] & 2) != 0
            result = bytearray()
            cluster = first_cluster
            for index in range(clusters):
                image.seek(cluster_offset(cluster))
                data = image.read(cluster_size)
                if len(data) != cluster_size:
                    fail("short directory cluster")
                result.extend(data)
                if index + 1 < clusters:
                    if contiguous:
                        cluster += 1
                    else:
                        image.seek(fat_offset * sector_size + cluster * 4)
                        value = image.read(4)
                        if len(value) != 4:
                            fail("short directory FAT entry")
                        cluster = struct.unpack("<I", value)[0]
                        if cluster < 2 or cluster >= 0xfffffff8:
                            fail("short directory FAT chain")
            return bytes(result[:length])

        image.seek(cluster_offset(root_cluster))
        root = image.read(cluster_size)
        bitmap_cluster = None
        for off in range(0, len(root), 32):
            if root[off] == 0:
                break
            if root[off] == 0x81:
                bitmap_cluster = u32(root, off + 20)
                break
        if bitmap_cluster is None:
            fail("allocation bitmap missing")
        directory_set = find_set(root, "CorruptDir")
        directory = read_stream(root[directory_set + 32:directory_set + 64])

        preserved = find_set(directory, "Extended.txt")
        preserved_count = directory[preserved + 1] + 1
        name_length = directory[preserved + 35]
        names = (name_length + 14) // 15
        preserved_extensions = directory[
            preserved + (2 + names) * 32:
            preserved + preserved_count * 32]
        expected = b"".join(
            bytes([0xE2, 0x00] + [value] * 30)
            for value in range(1, 65))
        if preserved_extensions != expected:
            fail("metadata update or rename changed opaque secondary bytes")

        deleted = find_set(directory, "DeleteMe.txt", include_deleted=True)
        deleted_count = directory[deleted + 1] + 1
        deleted_set = directory[deleted:deleted + deleted_count * 32]
        if any(deleted_set[index] & 0x80
               for index in range(0, len(deleted_set), 32)):
            fail("deleted entry set still contains an in-use entry")
        extension = deleted_set[-32:]
        if (extension[0] | 0x80) != 0xE2 or (extension[1] & 3) != 3:
            fail("deleted allocation secondary is malformed")
        owned_cluster = u32(extension, 20)
        if u64(extension, 24) != cluster_size:
            fail("deleted allocation secondary length changed")
        bit = owned_cluster - 2
        image.seek(cluster_offset(bitmap_cluster) + (bit >> 3))
        value = image.read(1)
        if len(value) != 1 or value[0] & (1 << (bit & 7)):
            fail("benign-secondary allocation was not released")

        benign_dir = find_set(root, "BenignDir", include_deleted=True)
        if root[benign_dir] & 0x80:
            fail("directory containing benign primary was not deleted")
        directory_cluster = u32(root, benign_dir + 32 + 20)
        hidden = read_stream(root[benign_dir + 32:benign_dir + 64])
        if len(hidden) < 64 or (hidden[0] | 0x80) != 0xA4 \
                or (hidden[32] | 0x80) != 0xE2:
            fail("deleted benign primary set is malformed")
        if hidden[0] & 0x80 or hidden[32] & 0x80:
            fail("benign primary set remains in use")
        hidden_clusters = [u32(hidden, 20), u32(hidden, 52), directory_cluster]
        for cluster in hidden_clusters:
            bit = cluster - 2
            image.seek(cluster_offset(bitmap_cluster) + (bit >> 3))
            value = image.read(1)
            if len(value) != 1 or value[0] & (1 << (bit & 7)):
                fail("benign-primary or directory allocation was not released")
        print("PASS: benign secondary preserved and owned cluster released")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: exfat_extension_inspect.py IMAGE")
    main(sys.argv[1])
