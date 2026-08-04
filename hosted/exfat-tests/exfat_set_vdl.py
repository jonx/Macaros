#!/usr/bin/env python3
"""Set one root file's ValidDataLength and repair its entry-set checksum."""
import struct
import sys


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def checksum(entry_set):
    value = 0
    for offset, byte in enumerate(entry_set):
        if offset not in (2, 3):
            value = ((0x8000 if value & 1 else 0) + (value >> 1) + byte) & 0xffff
    return value


def fail(message):
    raise SystemExit("FAIL: " + message)


def main(image_name, wanted, valid_length):
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
        directory = bytearray(image.read(cluster_size))

        for offset in range(0, len(directory), 32):
            if directory[offset] == 0:
                break
            if directory[offset] != 0x85:
                continue
            count = directory[offset + 1] + 1
            end = offset + count * 32
            if count < 3 or count > 19 or end > len(directory):
                fail("malformed root entry set")
            stream = offset + 32
            if directory[stream] != 0xc0:
                fail("file set has no stream extension")
            name_length = directory[stream + 3]
            name_bytes = bytearray()
            for name_entry in range(offset + 64, end, 32):
                if directory[name_entry] != 0xc1:
                    fail("file set has a non-name secondary")
                name_bytes.extend(directory[name_entry + 2:name_entry + 32])
            name = name_bytes[:name_length * 2].decode("utf-16le")
            if name != wanted:
                continue
            data_length = struct.unpack_from("<Q", directory, stream + 24)[0]
            first_cluster = u32(directory, stream + 20)
            if valid_length < 0 or valid_length > data_length:
                fail("ValidDataLength is outside DataLength")
            if first_cluster < 2 or data_length > cluster_size:
                fail("test poisoner requires a single-cluster file")
            struct.pack_into("<Q", directory, stream + 8, valid_length)
            entry_set = directory[offset:end]
            struct.pack_into("<H", directory, offset + 2, checksum(entry_set))
            image.seek(root_offset + offset)
            image.write(directory[offset:end])
            data_offset = (heap_offset
                + (first_cluster - 2) * sectors_per_cluster) * sector_size
            image.seek(data_offset + valid_length)
            image.write(b"\xa5" * (data_length - valid_length))
            print("set %s ValidDataLength=%d DataLength=%d; poisoned invalid tail" % (
                wanted, valid_length, data_length))
            return
    fail("entry not found: " + wanted)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: exfat_set_vdl.py IMAGE NAME VALID_LENGTH")
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]))
