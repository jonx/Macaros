#!/usr/bin/env python3
"""Deterministic byte-level exFAT corruption tool for the target corpus.

The mutations are deliberately applied after macOS has made a valid control
image.  This keeps the formatter outside the assertion and lets each vector
compare one changed byte range against its clean sibling.
"""
import struct
import sys


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]
def put16(b, o, v): struct.pack_into("<H", b, o, v)
def put32(b, o, v): struct.pack_into("<I", b, o, v)


def set_checksum(entry_set):
    value = 0
    for offset, byte in enumerate(entry_set):
        if offset in (2, 3):
            continue
        value = ((value << 15) | (value >> 1)) & 0xffff
        value = (value + byte) & 0xffff
    return value


def entry_set_name(root, off):
    """Return a structurally ordinary file set's UTF-16 name, or None."""
    if off + 64 > len(root) or root[off] != 0x85:
        return None
    count = root[off + 1] + 1
    end = off + count * 32
    if count < 3 or end > len(root) or root[off + 32] != 0xC0:
        return None
    name_length = root[off + 35]
    name = bytearray()
    for entry in range(2, count):
        entry_off = off + entry * 32
        if root[entry_off] != 0xC1:
            return None
        name.extend(root[entry_off + 2:entry_off + 32])
    try:
        return name[:name_length * 2].decode("utf-16le")
    except UnicodeDecodeError:
        return None


def find_entry_set(root, target):
    for off in range(0, len(root), 32):
        if root[off] == 0:
            break
        if root[off] == 0x85 and (target is None
                                 or entry_set_name(root, off) == target):
            return off
    detail = "" if target is None else " named {!r}".format(target)
    raise SystemExit("FAIL: no file entry set{} to corrupt".format(detail))


def boot(image):
    image.seek(0)
    b = bytearray(image.read(512))
    if len(b) != 512 or b[3:11] != b"EXFAT   ":
        raise SystemExit("FAIL: expected exFAT main boot sector")
    return b


def root_offset(b):
    return cluster_offset(b, u32(b, 96))


def cluster_offset(b, cluster):
    sector_size = 1 << b[108]
    return (u32(b, 88) + (cluster - 2) * (1 << b[109])) * sector_size


def cluster_size(b):
    return (1 << b[108]) * (1 << b[109])


def directory_offset(image, b, name):
    off = root_offset(b)
    image.seek(off)
    root = bytearray(image.read(cluster_size(b)))
    entry = find_entry_set(root, name)
    attributes = root[entry + 4] | (root[entry + 5] << 8)
    if (attributes & 0x10) == 0:
        raise SystemExit("FAIL: target {!r} is not a directory".format(name))
    first_cluster = u32(root, entry + 32 + 20)
    if first_cluster < 2:
        raise SystemExit("FAIL: target directory has no data cluster")
    return cluster_offset(b, first_cluster), u64(root, entry + 32 + 24)


def clear_bitmap_cluster(image, b, cluster):
    off = root_offset(b)
    image.seek(off)
    root = bytearray(image.read(cluster_size(b)))
    for entry in range(0, len(root), 32):
        if root[entry] == 0:
            break
        if root[entry] != 0x81:
            continue
        bitmap_cluster = u32(root, entry + 20)
        bitmap_length = u64(root, entry + 24)
        bit = cluster - 2
        if bit < 0 or bit // 8 >= bitmap_length:
            raise SystemExit("FAIL: target cluster outside allocation bitmap")
        byte_offset = cluster_offset(b, bitmap_cluster) + bit // 8
        image.seek(byte_offset)
        value = image.read(1)
        if len(value) != 1 or (value[0] & (1 << (bit & 7))) == 0:
            raise SystemExit("FAIL: target cluster was not allocated")
        image.seek(byte_offset)
        image.write(bytes([value[0] & ~(1 << (bit & 7))]))
        return
    raise SystemExit("FAIL: no allocation bitmap entry")


def allocate_bitmap_cluster(image, b):
    """Allocate and zero one previously free cluster, returning its number."""
    off = root_offset(b)
    image.seek(off)
    root = bytearray(image.read(cluster_size(b)))
    cluster_count = u32(b, 92)
    for entry in range(0, len(root), 32):
        if root[entry] == 0:
            break
        if root[entry] != 0x81:
            continue
        bitmap_cluster = u32(root, entry + 20)
        bitmap_length = u64(root, entry + 24)
        bitmap_offset = cluster_offset(b, bitmap_cluster)
        image.seek(bitmap_offset)
        bitmap = bytearray(image.read(bitmap_length))
        if len(bitmap) != bitmap_length:
            raise SystemExit("FAIL: short allocation bitmap")
        for bit in range(cluster_count):
            if (bitmap[bit >> 3] & (1 << (bit & 7))) == 0:
                bitmap[bit >> 3] |= 1 << (bit & 7)
                image.seek(bitmap_offset)
                image.write(bitmap)
                cluster = bit + 2
                image.seek(cluster_offset(b, cluster))
                image.write(bytes(cluster_size(b)))
                return cluster
        raise SystemExit("FAIL: no free cluster for benign secondary")
    raise SystemExit("FAIL: no allocation bitmap entry")


def append_benign_secondaries(root, entry, extensions):
    """Append secondaries while retaining following directory entries."""
    slots = len(extensions)
    old_count = root[entry + 1] + 1
    if slots == 0 or old_count + slots > 256:
        raise SystemExit("FAIL: invalid benign-secondary count")
    insertion = entry + old_count * 32
    end = insertion
    while end < len(root) and root[end] != 0:
        end += 32
    if end + (slots + 1) * 32 > len(root):
        raise SystemExit("FAIL: no room for benign secondary")
    root[insertion + slots * 32:end + (slots + 1) * 32] = \
        root[insertion:end + 32]
    root[insertion:insertion + slots * 32] = b"".join(extensions)
    root[entry + 1] += slots
    new_count = old_count + slots
    put16(root, entry + 2,
          set_checksum(root[entry:entry + new_count * 32]))


def main(path, mode, target=None, directory=None):
    with open(path, "r+b") as image:
        b = boot(image)
        if mode == "boot-checksum":
            # Covered byte in sector zero; sector 11 remains unaltered.
            image.seek(120); image.write(b"\x01")
        elif mode == "wrong-fsname":
            image.seek(3); image.write(b"NTFS    ")
        elif mode == "must-be-zero":
            image.seek(11); image.write(b"\x01")
        elif mode == "boot-signature":
            put16(b, 510, 0)
            image.seek(0); image.write(b)
        elif mode == "revision":
            put16(b, 104, 0x0101)
            image.seek(0); image.write(b)
        elif mode == "active-fat":
            put16(b, 106, 1)
            image.seek(0); image.write(b)
        elif mode == "primary-bad-backup-good":
            # Primary identity is broken while backup sector 12 remains intact.
            image.seek(3); image.write(b"X")
        elif mode == "texfat":
            # NumberOfFats == 2 is classified before geometry.
            image.seek(110); image.write(b"\x02")
        elif mode == "upcase-checksum":
            off = root_offset(b)
            image.seek(off)
            root = bytearray(image.read(cluster_size(b)))
            for entry in range(0, len(root), 32):
                if root[entry] == 0:
                    raise SystemExit("FAIL: no up-case table entry")
                if root[entry] == 0x82:
                    put32(root, entry + 4, u32(root, entry + 4) ^ 1)
                    image.seek(off)
                    image.write(root)
                    break
        elif mode in ("set-checksum", "name-hash", "secondary-count",
                      "unknown-critical-primary", "unknown-benign-primary",
                      "unknown-critical-secondary", "unknown-benign-secondary",
                      "unknown-benign-secondary-large",
                      "unknown-benign-secondary-allocation",
                      "unknown-benign-primary-allocation",
                      "bitmap-free-cluster"):
            directory_length = None
            if directory is None:
                off = root_offset(b)
            else:
                off, directory_length = directory_offset(image, b, directory)
            image.seek(off)
            root = bytearray(image.read(cluster_size(b)))
            if mode == "unknown-benign-primary-allocation":
                # A hidden benign primary set owns one cluster through the
                # primary template and one through a generic secondary.
                insertion = 0
                while insertion < len(root) and root[insertion] != 0:
                    insertion += 32
                if insertion + 96 > len(root):
                    raise SystemExit("FAIL: no room for benign primary set")
                primary_cluster = allocate_bitmap_cluster(image, b)
                secondary_cluster = allocate_bitmap_cluster(image, b)
                entry_set = bytearray(64)
                entry_set[0] = 0xA4
                entry_set[1] = 1
                entry_set[4] = 3
                put32(entry_set, 20, primary_cluster)
                struct.pack_into("<Q", entry_set, 24, cluster_size(b))
                entry_set[32] = 0xE2
                entry_set[33] = 3
                put32(entry_set, 52, secondary_cluster)
                struct.pack_into("<Q", entry_set, 56, cluster_size(b))
                put16(entry_set, 2, set_checksum(entry_set))
                root[insertion:insertion + 64] = entry_set
                root[insertion + 64:insertion + 96] = bytes(32)
                image.seek(off)
                image.write(root)
                return
            entry = find_entry_set(root, target)
            if mode == "set-checksum":
                put16(root, entry + 2,
                      (root[entry + 2] | (root[entry + 3] << 8)) ^ 1)
            elif mode == "name-hash":
                put16(root, entry + 32 + 4,
                      (root[entry + 32 + 4]
                       | (root[entry + 32 + 5] << 8)) ^ 1)
            elif mode == "secondary-count":
                # A count of 255 is valid in general but this mutation proves
                # that the claimed set extends beyond this directory stream.
                if (directory_length is not None
                        and entry + 256 * 32 <= directory_length):
                    raise SystemExit(
                        "FAIL: SecondaryCount mutation stays within directory")
                root[entry + 1] = 0xFF
            elif mode == "unknown-critical-primary":
                # InUse, primary, critical, otherwise unknown type code 4.
                root[entry] = 0x84
            elif mode == "unknown-benign-primary":
                # The same unknown primary with TypeImportance = benign.
                root[entry] = 0xA4
            elif mode == "unknown-critical-secondary":
                # InUse, secondary, critical, unknown type code 2.
                root[entry + 2 * 32] = 0xC2
            elif mode == "unknown-benign-secondary":
                # Append an opaque benign secondary after the mandatory name
                # entries. Move everything through the end marker by one
                # slot, expand SecondaryCount, then repair SetChecksum.
                extension = bytearray([0xE2, 0x00] + [0x5A] * 30)
                append_benign_secondaries(root, entry, [extension])
            elif mode == "unknown-benign-secondary-large":
                # Valid entry sets are not capped at the 18 secondaries needed
                # by a maximum-length file name.
                extensions = [
                    bytearray([0xE2, 0x00] + [value] * 30)
                    for value in range(1, 65)
                ]
                append_benign_secondaries(root, entry, extensions)
            elif mode == "unknown-benign-secondary-allocation":
                # AllocationPossible and NoFatChain make the generic
                # FirstCluster/DataLength fields owned by this extension.
                cluster = allocate_bitmap_cluster(image, b)
                extension = bytearray([0xE2, 0x03] + [0xA5] * 30)
                put32(extension, 20, cluster)
                struct.pack_into("<Q", extension, 24, cluster_size(b))
                append_benign_secondaries(root, entry, [extension])
            else:
                clear_bitmap_cluster(image, b, u32(root, entry + 32 + 20))
            if mode != "bitmap-free-cluster":
                image.seek(off)
                image.write(root)
        else:
            raise SystemExit("FAIL: unknown corruption mode {!r}".format(mode))


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4, 5):
        raise SystemExit(
            "usage: exfat_corrupt.py IMAGE MODE [TARGET_NAME [DIRECTORY]]")
    main(sys.argv[1], sys.argv[2],
         sys.argv[3] if len(sys.argv) >= 4 else None,
         sys.argv[4] if len(sys.argv) == 5 else None)
