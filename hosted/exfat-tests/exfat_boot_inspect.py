#!/usr/bin/env python3
"""Raw geometry oracle for target-side exFAT mount fixtures."""
import struct
import sys


def fail(message):
    raise SystemExit("FAIL: " + message)


def main(path, expected_sector):
    with open(path, "rb") as image:
        boot = image.read(512)
        image.seek(0, 2)
        image_bytes = image.tell()
    if len(boot) != 512 or boot[3:11] != b"EXFAT   ":
        fail("not an exFAT image: " + path)
    sector_size = 1 << boot[108]
    cluster_size = sector_size * (1 << boot[109])
    volume_sectors = struct.unpack_from("<Q", boot, 72)[0]
    if sector_size != expected_sector:
        fail("expected sector {}, got {}".format(expected_sector, sector_size))
    if volume_sectors * sector_size != image_bytes:
        fail("volume length does not match backing image")
    print("GEOMETRY sector={} cluster={} sectors={} bytes={}".format(
        sector_size, cluster_size, volume_sectors, image_bytes))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: exfat_boot_inspect.py IMAGE SECTOR_SIZE")
    main(sys.argv[1], int(sys.argv[2]))
