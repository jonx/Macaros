# exfat — an exFAT handler for AROS

> Status: **spec written, no code.** Branch `exfat-handler` on `../aros-upstream`,
> off `dos64-packets`.

exFAT is the format on every SD card and USB stick over 32 GB. AROS today can write to
no medium a modern Mac or PC would hand it except FAT32: [`rom/filesys/fat`](../../../../aros-upstream/rom/filesys/fat)
is FAT12/16/32 only, `workbench/fs/ntfs` is read-only by a hardcoded `#define`, and
there is no ext, exFAT or APFS support at all.

This is the highest value-per-line change available in the AROS storage stack, which is
why it comes before the alternatives.

## Read this first

**[spec.md](spec.md)** is the cleanroom functional specification for Phase 1. It is the
only description of behaviour the implementation is written from. It carries the
provenance record required by [CLEANROOM.md](../CLEANROOM.md), and states plainly that
no third-party exFAT implementation has been read.

## Why a separate handler, not a fourth FAT type

The obvious plan is to add exFAT as another format inside the existing FAT handler,
which already has FAT12/16/32 dispatch, and that was the original intent here. Reading
the code changed it.

The FAT handler's variance is abstracted through exactly **two** function pointers,
`func_get_fat_entry` and `func_set_fat_entry`, covering FAT table access only.
Meanwhile there are **146 direct accesses** to the on-disk FAT directory entry spread
across `ops.c`, `direntry.c`, `names.c`, `volume.c` and `lock.c`, each hardcoding the
32-byte short-entry layout. exFAT has no such entry: it has entry *sets* of a File
entry, a Stream Extension entry and one or more Filename entries, bound by a checksum.
None of those 146 sites translate.

exFAT is not FAT with extensions. It reuses cluster chains and an optional FAT table,
and differs in boot sector, allocation, directory format, naming and size domain. A
separate handler follows the precedent already in the tree, `workbench/fs/ntfs` being
visibly a copy of the FAT scaffolding, and keeps a filesystem the whole system depends
on out of the blast radius.

## Phases

| Phase | Scope | State |
|---|---|---|
| 0 | 64-bit DOS packet plumbing, see [dos64-packets](../dos64-packets/README.md) | **done** |
| 0.5 | Copy the FAT scaffolding, widen its sector domain to `UQUAD` | next |
| 1 | Read-only: explicit `FATX` Mountlist, VBR validation, bitmap, upcase, entry sets, list, read | spec written |
| 2 | Writable files and metadata, with fault injection and external `fsck_exfat` validation | not started |
| 3 | Formatter, then shared content probing for MBR, GPT **and** USB mass storage | not started |

Phase 3 covers USB mass storage explicitly because a probe living only in
`rom/partition` would miss the single most common exFAT medium.

## DosType

`FATX`, `0x46415458`, matching the existing OS4 and Aminet exFAT handlers so media is
interchangeable. Note that exFAT **cannot be identified by partition type**: it shares
MBR type `0x07` with NTFS and the Microsoft Basic Data GUID on GPT, so identification
requires probing the volume boot record for the `"EXFAT   "` signature at offset 3.
That is Phase 3's problem, not Phase 1's, which mounts only from an explicit Mountlist
entry.
