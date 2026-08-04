#!/usr/bin/env python3
"""Add sparse large-file acceptance streams to a freshly formatted exFAT image.

Only public exFAT 1.00 structures are used.  File data stays sparse: allocation
bitmap bits, four root entry sets, and a handful of sentinel bytes are written.
"""
import struct
import sys


IMAGE_SIZE = 16 * 1024 * 1024 * 1024
CLUSTER_SIZE = 1024 * 1024
ENTRY_SIZE = 32


def fail(message):
    raise SystemExit("FAIL: " + message)


def u16(data, off): return struct.unpack_from("<H", data, off)[0]
def u32(data, off): return struct.unpack_from("<I", data, off)[0]
def u64(data, off): return struct.unpack_from("<Q", data, off)[0]
def put16(data, off, value): struct.pack_into("<H", data, off, value)
def put32(data, off, value): struct.pack_into("<I", data, off, value)
def put64(data, off, value): struct.pack_into("<Q", data, off, value)


def rotate16(value, byte):
    return (((value & 1) << 15) + (value >> 1) + byte) & 0xFFFF


def name_hash(name):
    value = 0
    for char in name.upper().encode("utf-16le"):
        value = rotate16(value, char)
    return value


def entry_set(name, first_cluster, valid_length, data_length):
    encoded = name.encode("utf-16le")
    if len(encoded) > 30:
        fail("fixture name needs more than one File Name entry: " + name)

    result = bytearray(3 * ENTRY_SIZE)
    result[0] = 0x85
    result[1] = 2
    put16(result, 4, 0x20)             # Archive

    stream = ENTRY_SIZE
    result[stream] = 0xC0
    result[stream + 1] = 0x03          # AllocationPossible | NoFatChain
    result[stream + 3] = len(encoded) // 2
    put16(result, stream + 4, name_hash(name))
    put64(result, stream + 8, valid_length)
    put32(result, stream + 20, first_cluster)
    put64(result, stream + 24, data_length)

    filename = 2 * ENTRY_SIZE
    result[filename] = 0xC1
    result[filename + 2:filename + 2 + len(encoded)] = encoded

    checksum = 0
    for off, byte in enumerate(result):
        if off not in (2, 3):
            checksum = rotate16(checksum, byte)
    put16(result, 2, checksum)
    return result


class Geometry:
    def __init__(self, boot, image_length):
        if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
            fail("not an exFAT image")
        self.sector_size = 1 << boot[108]
        self.sectors_per_cluster = 1 << boot[109]
        self.cluster_size = self.sector_size * self.sectors_per_cluster
        self.heap_sector = u32(boot, 88)
        self.cluster_count = u32(boot, 92)
        self.root_cluster = u32(boot, 96)
        self.volume_length = u64(boot, 72)
        if self.sector_size != 512 or self.cluster_size != CLUSTER_SIZE:
            fail("fixture requires 512-byte sectors and 1 MiB clusters")
        if self.volume_length * self.sector_size != IMAGE_SIZE:
            fail("formatted volume does not span the 16 GiB image")
        if image_length != IMAGE_SIZE:
            fail("backing image logical size changed during formatting")

    def cluster_offset(self, cluster):
        if cluster < 2 or cluster > self.cluster_count + 1:
            fail("cluster outside formatted heap: {}".format(cluster))
        return ((self.heap_sector
                 + (cluster - 2) * self.sectors_per_cluster)
                * self.sector_size)


def find_free_run(bitmap, cluster_count, needed):
    start = None
    length = 0
    for bit in range(cluster_count):
        allocated = (bitmap[bit >> 3] & (1 << (bit & 7))) != 0
        if allocated:
            start = None
            length = 0
            continue
        if start is None:
            start = bit + 2
        length += 1
        if length == needed:
            return start
    fail("no contiguous free run for sparse fixture")


def set_allocated(bitmap, first_cluster, count):
    for cluster in range(first_cluster, first_cluster + count):
        bit = cluster - 2
        if bitmap[bit >> 3] & (1 << (bit & 7)):
            fail("selected fixture cluster was already allocated")
        bitmap[bit >> 3] |= 1 << (bit & 7)


def main(path):
    with open(path, "r+b", buffering=0) as image:
        image.seek(0, 2)
        image_length = image.tell()
        image.seek(0)
        geo = Geometry(image.read(512), image_length)

        root_offset = geo.cluster_offset(geo.root_cluster)
        image.seek(root_offset)
        root = bytearray(image.read(geo.cluster_size))
        if len(root) != geo.cluster_size:
            fail("short root-directory cluster")

        bitmap_cluster = None
        bitmap_length = None
        insert = None
        for off in range(0, len(root), ENTRY_SIZE):
            entry_type = root[off]
            if entry_type == 0:
                insert = off
                break
            if entry_type == 0x81:
                if bitmap_cluster is not None:
                    fail("multiple allocation bitmaps")
                bitmap_cluster = u32(root, off + 20)
                bitmap_length = u64(root, off + 24)
        if insert is None or bitmap_cluster is None or bitmap_length is None:
            fail("missing root terminator or allocation bitmap")
        bitmap_needed = (geo.cluster_count + 7) // 8
        if bitmap_length < bitmap_needed or bitmap_length > geo.cluster_size:
            fail("fixture requires a one-cluster allocation bitmap")

        bitmap_offset = geo.cluster_offset(bitmap_cluster)
        image.seek(bitmap_offset)
        bitmap = bytearray(image.read(bitmap_needed))
        if len(bitmap) != bitmap_needed:
            fail("short allocation bitmap")

        definitions = [
            ["Minus1.bin", 0xFFFFFFFF, 0xFFFFFFFF,
             {0: b"M", 0xFFFFFFFE: b"m"}],
            ["Exact4G.bin", 0x100000000, 0x100000000,
             {0: b"E", 0xFFFFFFFF: b"e"}],
            ["Plus1.bin", 0x100000001, 0x100000001,
             {0: b"P", 0xFFFFFFFF: b"B", 0x100000000: b"p"}],
            ["ZeroTail.bin", 2 * CLUSTER_SIZE, CLUSTER_SIZE + 3,
             {0: b"Z", CLUSTER_SIZE + 2: b"V",
              CLUSTER_SIZE + 3: b"R", 2 * CLUSTER_SIZE - 1: b"T"}],
        ]
        total_clusters = sum((item[1] + CLUSTER_SIZE - 1) // CLUSTER_SIZE
                             for item in definitions)
        next_cluster = find_free_run(bitmap, geo.cluster_count, total_clusters)

        sets = bytearray()
        manifest = []
        for name, data_length, valid_length, markers in definitions:
            clusters = (data_length + CLUSTER_SIZE - 1) // CLUSTER_SIZE
            first_cluster = next_cluster
            set_allocated(bitmap, first_cluster, clusters)
            sets.extend(entry_set(name, first_cluster, valid_length,
                                  data_length))
            manifest.append((name, data_length, valid_length, first_cluster,
                             clusters, markers))
            next_cluster += clusters

        if insert + len(sets) + ENTRY_SIZE > len(root):
            fail("fixture entry sets do not fit in the first root cluster")
        root[insert:insert + len(sets)] = sets
        root[insert + len(sets):insert + len(sets) + ENTRY_SIZE] = bytes(ENTRY_SIZE)
        image.seek(root_offset)
        image.write(root)
        image.seek(bitmap_offset)
        image.write(bitmap)

        physical_markers = []
        for name, data_length, valid_length, first, clusters, markers in manifest:
            for logical, marker in markers.items():
                if logical >= data_length:
                    fail("marker beyond {} data length".format(name))
                physical = (geo.cluster_offset(first + logical // CLUSTER_SIZE)
                            + logical % CLUSTER_SIZE)
                image.seek(physical)
                image.write(marker)
                physical_markers.append((name, logical, physical, marker))

        for name, logical, physical, marker in physical_markers:
            image.seek(physical)
            if image.read(1) != marker:
                fail("sentinel read-back failed for {}".format(name))
            if physical >= 0x100000000:
                image.seek(physical & 0xFFFFFFFF)
                if image.read(1) == marker:
                    fail("{} sentinel equals its truncated-offset alias".format(name))

        image.seek(bitmap_offset)
        check_bitmap = image.read(bitmap_needed)
        for _, _, _, first, clusters, _ in manifest:
            for cluster in range(first, first + clusters):
                bit = cluster - 2
                if not (check_bitmap[bit >> 3] & (1 << (bit & 7))):
                    fail("allocation bitmap write did not persist")

        print("SPARSE-EXFAT sector={} cluster={} heap={} root={}".format(
            geo.sector_size, geo.cluster_size, geo.heap_sector,
            geo.root_cluster))
        for name, data_length, valid_length, first, clusters, markers in manifest:
            print("FILE name={} size={} valid={} first={} clusters={} markers={}".format(
                name, data_length, valid_length, first, clusters,
                ",".join("{}:{}".format(pos, value.decode("ascii"))
                         for pos, value in sorted(markers.items()))))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: exfat_sparse_fixture.py IMAGE")
    main(sys.argv[1])
