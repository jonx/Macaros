# exFAT handler — Phase 1 functional specification

> Cleanroom spec under [CLEANROOM.md](../CLEANROOM.md). Role A artefact: this document
> is the **only** description of behaviour Role B implements from, together with the
> approved public sources named here.
>
> Branch `exfat-handler` on `../aros-upstream`, off `dos64-packets`.
> Status: **draft, awaiting approval.** No handler code exists yet.

## Provenance record

**No third-party implementation source has been read, searched or consulted in
producing this specification.** Not `fuse-exfat`, not the Linux `exfat` driver, not
`exfatprogs`, not the OS4 or Aminet handlers, not any emulator's filesystem code. Any
resemblance to an existing implementation is coincidental.

Approved sources used:

- **`[PUB]`** the published Microsoft exFAT file system specification, revision 1.00,
  placed under the Open Invention Network patent pledge in 2019.
- **`[AROS]`** in-tree AROS headers and the existing `rom/filesys/fat` handler
  (APL/LGPL, ours to use) for the AmigaDOS handler contract and scaffolding shape.
- **`[OURS]`** measurements taken in this project, cited inline.
- **`[DERIVED]`** requirements we reasoned out, each carrying its own justification.

## 1. Scope

Phase 1 delivers a **read-only** exFAT handler, mounted only from an explicit `FATX`
Mountlist entry. Out of scope for Phase 1, in order of later phases: all writing,
formatting, partition-table probing and USB mass-storage auto-detection.

### 1.1 Supported revision

`[PUB]` The handler accepts `FileSystemRevision` major version **1**, minor version 0.
A major version other than 1 is rejected.

### 1.2 TexFAT is not supported

`[PUB]` TexFAT (the transaction-safe extension) is signalled by `NumberOfFats == 2`.
`[DERIVED]` The handler **rejects** such a volume rather than mounting it as plain
exFAT. Justification: with two FATs the active one is selected by the `ActiveFat` bit
of `VolumeFlags`, and a reader that ignores that bit can walk a stale chain and return
data from a partially-committed transaction. Refusing is the only safe read-only
behaviour, since silently reading the wrong FAT is indistinguishable from corruption.

## 2. Sector domain invariants

`[OURS]` The `rom/filesys/fat` disk and cache layer this handler's scaffolding derives
from expresses sector numbers as `ULONG`, while computing the byte offset as `UQUAD`.
Its bounds arithmetic (`num + nblocks`, `first_device_sector + total_sectors`) is
performed in 32 bits, so a wrap makes an out-of-range access pass the guard and then
issue a well-formed 64-bit offset into the wrong place. Unreachable below 2 TB at
512-byte sectors; exFAT exists to address media where it is reachable.

Requirements:

- **S1** Every sector number, sector count and sector bound is `UQUAD` end to end:
  `AccessDisk()`, the cache block key, and every `FSSuper` sector field.
- **S2** Bounds arithmetic is performed after promotion to `UQUAD`. No 32-bit
  intermediate may exist in a comparison that guards an access.
- **S3** Cluster numbers remain `ULONG`. `[PUB]` exFAT FAT entries are 32-bit, so the
  cluster domain is 32-bit by definition. Only the sector domain widens.
- **S4** `SECTOR_FROM_CLUSTER` promotes before shifting:
  `((UQUAD)(cluster - 2) << sectors_per_cluster_shift) + cluster_heap_offset`.
- **S5** No `UQUAD` may reach `bug()` / `ErrorMessage()` through an unchecked `%lu`.
  Either one audited formatting helper, or every widened diagnostic call is
  compile-tested on m68k and AArch64. `[OURS]` This project has already shipped a
  64-bit vararg defect of exactly this shape.

## 3. Boot region

### 3.1 Main Boot Sector, accepted conditions

`[PUB]` At sector 0, all fields little-endian:

| Offset | Size | Field | Accept when |
|---|---|---|---|
| 0 | 3 | `JumpBoot` | `EB 76 90` |
| 3 | 8 | `FileSystemName` | exactly `"EXFAT   "` (three trailing spaces) |
| 11 | 53 | `MustBeZero` | all bytes zero |
| 64 | 8 | `PartitionOffset` | informational, not validated |
| 72 | 8 | `VolumeLength` | ≥ `2^(20 - BytesPerSectorShift)` |
| 80 | 4 | `FatOffset` | within the volume |
| 84 | 4 | `FatLength` | covers `ClusterCount + 2` entries |
| 88 | 4 | `ClusterHeapOffset` | ≥ `FatOffset + FatLength` |
| 92 | 4 | `ClusterCount` | ≤ `0xFFFFFFF5` |
| 96 | 4 | `FirstClusterOfRootDirectory` | in `2 ..= ClusterCount + 1` |
| 104 | 2 | `FileSystemRevision` | major version 1 (§1.1) |
| 106 | 2 | `VolumeFlags` | read, not validated |
| 108 | 1 | `BytesPerSectorShift` | `9 ..= 12` (512 to 4096 bytes) |
| 109 | 1 | `SectorsPerClusterShift` | cluster size ≤ 32 MiB |
| 110 | 1 | `NumberOfFats` | exactly 1 (§1.2) |
| 510 | 2 | `BootSignature` | `0xAA55` |

`MustBeZero` is the field a legacy FAT driver would read as its BPB. `[DERIVED]` It is
validated rather than ignored because a volume with a non-zero value there is not a
conforming exFAT volume, and mounting it risks acting on a FAT BPB that is not one.

### 3.2 Boot region checksum

`[PUB]` Sectors 0 to 10 are checksummed into every 4-byte slot of sector 11. The
checksum is a 32-bit rotate-right-then-add over every byte, **excluding bytes 106, 107
and 112 of sector 0** (`VolumeFlags` and `PercentInUse`, which mutate in normal use).

**B1** The handler computes this checksum and compares it against sector 11.
**B2** `[DERIVED]` A mismatch is a **mount refusal**, not a warning. Justification: the
checksum is the only integrity check the format offers over its own geometry, and every
later computation depends on that geometry being right. Continuing past a mismatch means
computing sector addresses from fields we have positive evidence are wrong.

### 3.3 Backup boot region

`[PUB]` A backup copy occupies sectors 12 to 23.
**B3** Phase 1 does **not** fall back to the backup region. `[DERIVED]` Falling back is
a repair action; a read-only mount that silently uses backup geometry can present a
different view of the volume from the one the primary describes. Deferred to a later
phase where it is explicit.

## 4. Access rules

**A1** `[PUB]` Every on-disk field is little-endian and must be read through the AROS
byte-order macros. No structure may be dereferenced for a multi-byte field directly.

**A2** `[DERIVED]` exFAT structures are packed with fields at unaligned offsets
(`VolumeLength` at 72, `DataLength` at 56 within a 32-byte entry). A 68000 or 68010
takes an address error on an odd-address word access. Every multi-byte read is therefore
byte-wise or through an accessor that is byte-wise on m68k. Justification: the AROS
`amiga-m68k` target includes 68000-class machines; this is not optional there.

**A3** Reserved and `MustBeZero` fields are never written and never interpreted beyond
the validation in §3.1.

## 5. FAT and cluster chains

`[PUB]` The FAT holds 32-bit little-endian entries. Entry 0 is `0xFFFFFFF8`, entry 1 is
`0xFFFFFFFF`; both are reserved and describe no cluster. `0xFFFFFFF7` marks a bad
cluster, `0xFFFFFFFF` terminates a chain. Data clusters are numbered from 2.

**F1** A cluster number outside `2 ..= ClusterCount + 1` encountered while walking a
chain terminates the walk with an I/O error, never a wrap or a read.

**F2 — NoFatChain.** `[PUB]` When bit 1 of the Stream Extension entry's
`GeneralSecondaryFlags` is set, the file's clusters are contiguous from `FirstCluster`
and **the FAT must not be consulted**. `[DERIVED]` This makes "next cluster" a
per-file property, not a per-volume one. The cluster walker therefore takes the
owning stream's flags as a parameter; a walker with only `(volume, cluster)` in scope
cannot implement this correctly. This is the single most structurally invasive
difference from FAT12/16/32 and must be reflected in the interface, not patched around.

**F3** For a `NoFatChain` file, the chain length is derived from `DataLength` and the
cluster size; a read beyond that is out of range regardless of what the FAT contains.

## 6. Allocation bitmap

`[PUB]` Located by an Allocation Bitmap directory entry (type `0x81`) in the root
directory. One bit per cluster, least-significant bit first within each byte, bit 0
describing cluster 2. A set bit means allocated.

**M1** Phase 1 reads the bitmap only to answer `ACTION_INFO` free-space queries.
**M2** `[DERIVED]` The bitmap is **not** consulted to validate a chain. A read-only
handler that refuses to read a cluster marked free would fail on volumes that are
merely stale, and the authoritative structure for file contents is the chain, not the
bitmap.

## 7. Upcase table

`[PUB]` Located by an Up-case Table directory entry (type `0x82`). It maps UTF-16 code
units to their upper-case form and may be stored compressed: a `0xFFFF` entry is
followed by a count of code points that map to themselves.

**U1** The table is read from the volume. It is **not** replaced by a built-in
case-folding rule. `[DERIVED]` Name comparison and the name hash must agree
byte-for-byte with what the writing implementation used, and only the on-disk table
guarantees that.

**U2** The table's own checksum, recorded in its directory entry, is verified. A
mismatch refuses the mount, by the same reasoning as B2.

## 8. Directory entry sets

`[PUB]` Directories are arrays of 32-byte entries. Bit 7 of `EntryType` is the InUse
bit; a value of `0x00` ends the directory. An entry whose InUse bit is clear is deleted
and skipped.

A file is described by an **entry set** of consecutive entries:

| Type | Role | Fields Phase 1 reads |
|---|---|---|
| `0x85` | File | `SecondaryCount`, `SetChecksum`, `FileAttributes`, create/modify/access timestamps with 10 ms increments and UTC offsets |
| `0xC0` | Stream Extension | `GeneralSecondaryFlags`, `NameLength`, `NameHash`, `FirstCluster`, `ValidDataLength`, `DataLength` |
| `0xC1` | File Name | 15 UTF-16 code units each, concatenated |

Also recognised: `0x83` Volume Label, `0x81` Allocation Bitmap, `0x82` Up-case Table.

**D1** The set is `1 + SecondaryCount` entries. A set that is truncated by the end of
the directory is skipped, not partially interpreted.

**D2** `SetChecksum` is verified over the whole set, excluding bytes 2 and 3 of the
first entry. `[DERIVED]` A set failing its checksum is **skipped as if deleted**, and
the mount continues. Justification: unlike the boot region, one bad entry set does not
invalidate the geometry of the volume, so refusing the whole mount would be
disproportionate. The file is simply not visible.

**D3** `NameHash` is verified against the reconstructed name using the upcase table, and
used as a cheap reject during lookup. A mismatch is treated as D2.

**D4** `ValidDataLength` may be less than `DataLength`. `[PUB]` Bytes between the two
have never been written. **Phase 1 returns zeroes for that range** rather than the
on-disk residue, which may contain another file's data.

## 9. Names and character set

**N1** `[PUB]` Names are UTF-16, up to 255 code units, and are case-insensitive but
case-preserving.

**N2** `[AROS]` Conversion to and from the local character set uses the existing
`from_unicode[65536]` / `to_unicode[256]` tables that the FAT handler already maintains
for long filenames.

**N3** `[DERIVED]` A code unit with no local representation maps to `_` on the way out.
Justification: this matches what the FAT handler already does for long filenames, so a
user sees one consistent behaviour across both. Such a name is **not** matchable on the
way in; lookup compares against the on-disk UTF-16, not the lossy local form, so two
different names that both degrade to the same local string remain distinct.

**N4** Surrogate pairs are passed through as two code units and are subject to N3.
Phase 1 does not attempt astral-plane composition.

## 10. Read-only behaviour

**R1** Every mutating DOS action returns `DOSFALSE` with `ERROR_DISK_WRITE_PROTECTED`:
`ACTION_WRITE`, `ACTION_DELETE_OBJECT`, `ACTION_RENAME_OBJECT`, `ACTION_CREATE_DIR`,
`ACTION_SET_PROTECT`, `ACTION_SET_DATE`, `ACTION_SET_COMMENT`, `ACTION_SET_FILE_SIZE`,
`ACTION_FORMAT`, `ACTION_RELABEL`.

**R2** `ACTION_FINDOUTPUT` and `ACTION_FINDUPDATE` are refused the same way.
`ACTION_FINDINPUT` succeeds.

**R3** `[AROS]` An action the handler does not implement replies `DOSFALSE` with
`ERROR_ACTION_NOT_KNOWN`. `[OURS]` This is not cosmetic: `dos64.library` distinguishes
"unsupported" from "answered zero" solely by the secondary result, and a handler that
replies `DOSFALSE` with no error causes callers to accept the zero as an answer. Three
separate upstream regressions have been traced to this, and one was found in the NTFS
handler during this project.

**R4** `RESULT1` is a **value**, never a pointer into handler state. `[OURS]` The NTFS
handler returned the address of its own size field for `ACTION_GET_FILE_SIZE64`; with
`dos64.library` present that address is read as the file size.

## 11. Expected mount errors

Each refusal reports a distinct secondary result so a failure is diagnosable without a
debug build:

| Condition | Result |
|---|---|
| `FileSystemName` is not `"EXFAT   "` | `ERROR_NOT_A_DOS_DISK` |
| `MustBeZero` non-zero | `ERROR_NOT_A_DOS_DISK` |
| `BootSignature` not `0xAA55` | `ERROR_NOT_A_DOS_DISK` |
| Boot region checksum mismatch | `ERROR_DISK_NOT_VALIDATED` |
| Up-case table checksum mismatch | `ERROR_DISK_NOT_VALIDATED` |
| `FileSystemRevision` major ≠ 1 | `ERROR_OBJECT_WRONG_TYPE` |
| `NumberOfFats == 2` (TexFAT) | `ERROR_OBJECT_WRONG_TYPE` |
| Geometry out of range (§3.1) | `ERROR_BAD_NUMBER` |
| Device cannot address the volume (no TD64/NSD past 4 GB) | `ERROR_SEEK_ERROR` |

## 12. Acceptance tests

Authored on macOS, verified against the originals. `[OURS]` The hosted darwin build
mounts a disk image through `fdsk.device`, following the `AFD0` Mountlist pattern
already in tree, and is driven headlessly by `aros-ctl`.

| # | Vector | Pass condition |
|---|---|---|
| T1 | Empty volume, 512-byte and 4096-byte sectors | Mounts; `Info` reports plausible free space |
| T2 | File of exactly 4 GiB − 1, 4 GiB, 4 GiB + 1 | Size reported exactly; full byte-compare |
| T3 | Fragmented file, ≥ 8 extents | Byte-compare |
| T4 | Contiguous file with `NoFatChain` set | Byte-compare, and the FAT is never read for it |
| T5 | 255-code-unit name, mixed case, non-ASCII | Listed, and openable by the listed name |
| T6 | Two names differing only in case | Both visible and distinct (N3) |
| T7 | Directory of 10,000 entries | Full enumeration, no truncation |
| T8 | `ValidDataLength < DataLength` | Tail reads as zeroes (D4) |
| T9 | Deliberately corrupted boot checksum | Mount refused, `ERROR_DISK_NOT_VALIDATED` |
| T10 | `NumberOfFats = 2` | Mount refused, `ERROR_OBJECT_WRONG_TYPE` |
| T11 | One entry set with a bad `SetChecksum` | That file invisible, rest of directory intact |
| T12 | Every mutating action from R1 | Each returns `ERROR_DISK_WRITE_PROTECTED` |
| T13 | Volume > 2^32 sectors, or a simulated equivalent | No wrap; §2 bounds hold |

**T-neg** No test may pass by the handler declining to mount. T9 and T10 assert refusal;
every other test asserts a successful mount first.

## 13. Open questions for Role B

1. Whether `fdsk.device` can present an image large enough for T13, or whether that
   test needs a sparse image plus a device shim.
2. Whether the FAT handler's cache hash distributes acceptably once keyed on `UQUAD`,
   or wants rehashing for large volumes.
