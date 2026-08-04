#!/usr/bin/env python3
"""Independent structural oracle for images authored by ACTION_FORMAT."""

import struct
import sys


def u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def u64(data, off):
    return struct.unpack_from("<Q", data, off)[0]


def rotate32(value, byte):
    return ((value >> 1) | ((value & 1) << 31)) + byte & 0xFFFFFFFF


def fail(message):
    raise SystemExit("FAIL: " + message)


def main(path):
    with open(path, "rb") as image:
        boot = image.read(512)
        if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
            fail("identity")
        sector_size = 1 << boot[108]
        image.seek(0)
        regions = image.read(24 * sector_size)
        if len(regions) != 24 * sector_size:
            fail("short boot regions")
        if regions[:12 * sector_size] != regions[12 * sector_size:]:
            fail("backup boot region differs")
        checksum = 0
        for sector in range(11):
            data = regions[sector * sector_size:(sector + 1) * sector_size]
            for offset, byte in enumerate(data):
                if sector == 0 and offset in (106, 107, 112):
                    continue
                checksum = rotate32(checksum, byte)
        check_sector = regions[11 * sector_size:12 * sector_size]
        if any(u32(check_sector, off) != checksum
               for off in range(0, sector_size, 4)):
            fail("boot checksum sector")

        spc = 1 << boot[109]
        cluster_size = sector_size * spc
        volume_length = u64(boot, 72)
        fat_offset, fat_length = u32(boot, 80), u32(boot, 84)
        heap_offset, cluster_count = u32(boot, 88), u32(boot, 92)
        root_cluster = u32(boot, 96)
        if fat_offset < 24 or fat_offset + fat_length > heap_offset:
            fail("FAT geometry")
        if heap_offset + cluster_count * spc > volume_length:
            fail("heap geometry")

        def cluster_data(number):
            image.seek((heap_offset + (number - 2) * spc) * sector_size)
            data = image.read(cluster_size)
            if len(data) != cluster_size:
                fail("short cluster")
            return data

        root = cluster_data(root_cluster)
        bitmap = upcase = label = None
        for off in range(0, cluster_size, 32):
            kind = root[off]
            if kind == 0:
                break
            if kind == 0x81:
                bitmap = (u32(root, off + 20), u64(root, off + 24))
            elif kind == 0x82:
                upcase = (u32(root, off + 20), u64(root, off + 24),
                          u32(root, off + 4))
            elif kind == 0x83:
                length = root[off + 1]
                label = root[off + 2:off + 2 + length * 2].decode("utf-16le")
        if bitmap is None or upcase is None or label != "AROSFORMAT":
            fail("root system entries")
        bitmap_data = cluster_data(bitmap[0])
        upcase_data = cluster_data(upcase[0])[:upcase[1]]
        upcase_sum = 0
        for byte in upcase_data:
            upcase_sum = rotate32(upcase_sum, byte)
        if upcase_sum != upcase[2]:
            fail("up-case checksum")
        allocated = ((bitmap[1] + cluster_size - 1) // cluster_size
                     + (upcase[1] + cluster_size - 1) // cluster_size + 1)
        if any((bitmap_data[index >> 3] >> (index & 7)) & 1 == 0
               for index in range(allocated)):
            fail("system allocation bitmap")
        print("sector=%d cluster=%d clusters=%d label=%s checksum=%08x" %
              (sector_size, cluster_size, cluster_count, label, checksum))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} IMAGE")
    main(sys.argv[1])
