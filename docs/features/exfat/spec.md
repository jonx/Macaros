# exFAT handler — Phase 1 functional specification

> Cleanroom spec under [CLEANROOM.md](../CLEANROOM.md). Role A artefact: this document
> is the **only** description of behaviour Role B implements from, together with the
> approved public sources named here.
>
> Branch `exfat-handler` on `../aros-upstream`, off `dos64-packets`.
> Status: **revision 2, after review.** No handler code exists yet.

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
Mountlist entry. Out of scope for Phase 1: all writing, formatting, partition-table
probing and USB mass-storage auto-detection.

### 1.1 Revision policy

`[PUB]` `FileSystemRevision` is a minor byte at offset 104 and a major byte at 105.
**V1** The handler accepts **exactly major 1, minor 0**. `[DERIVED]` Any other value is
refused, including a higher minor. Justification: a later minor revision may attach
meaning to fields this specification treats as reserved, and a read-only mount that
ignores them presents a view of the volume its author did not intend. This is the same
refuse-when-unsure posture used for the boot checksum and TexFAT, and the three are now
consistent.

### 1.2 TexFAT is not supported

`[PUB]` TexFAT is signalled by `NumberOfFats == 2`. `[DERIVED]` The handler **rejects**
such a volume rather than mounting it as plain exFAT. Justification: the active FAT is
selected by the `ActiveFat` bit of `VolumeFlags`, and a reader that ignores it can walk
a stale chain and return data from a partially-committed transaction.

## 2. Sector domain invariants

`[OURS]` The `rom/filesys/fat` disk and cache layer this handler's scaffolding derives
from expresses sector numbers as `ULONG` while computing the byte offset as `UQUAD`. Its
bounds arithmetic is performed in 32 bits, so a wrap makes an out-of-range access pass
the guard and then issue a well-formed 64-bit offset into the wrong place.

- **S1** Every sector number, sector count and sector bound is `UQUAD` end to end:
  `AccessDisk()`, the cache block key, and every `FSSuper` sector field.
- **S2** Range checks are **overflow-safe by construction, not merely 64-bit**.
  Widening alone is insufficient: `first + total` can overflow `UQUAD` as easily as
  `ULONG`. Express every bound as a **subtraction against a known-valid limit**, never
  as an addition that is then compared:

  ```
  /* wrong, even in 64-bit */    if (num + count > end) ...
  /* required */                 if (num >= end || count > end - num) ...
  ```

  No comparison guarding an access may contain an addition or a multiplication whose
  operands are not already bounded.
- **S3** Cluster numbers remain `ULONG`. `[PUB]` exFAT FAT entries are 32-bit, so the
  cluster domain is 32-bit by definition. Only the sector domain widens.
- **S4** `SECTOR_FROM_CLUSTER` **must promote before the shift**, and **must only be
  called after the cluster has been validated** as `2 ..= ClusterCount + 1`.

  ```
  (((UQUAD)cluster - 2) << shift) + heap
  ```

  `[OURS]` T13a disproves a weaker claim made in revision 2 of this document, that
  moving the promotion ahead of the subtraction makes an invalid cluster safe. It does
  not. `(UQUAD)1 - 2` underflows exactly as `(ULONG)1 - 2` does, and the subsequent
  shift and add wrap the result back into a plausible-looking sector: with an 8-sector
  cluster and a heap at 4096, cluster 1 yields sector **4088** under either form.
  Promotion order changes which wrong answer is produced, not whether one is.

  The promotion is required because `cluster << shift` evaluated in `ULONG` overflows
  for a large cluster. The **only** protection against underflow is the range check, so
  it is a precondition of the macro and not an optimisation the caller may skip. The
  macro is not defensive and must not be treated as though it were.
- **S5** No `UQUAD` may reach `bug()` / `ErrorMessage()` through an unchecked `%lu`.
  Either one audited formatting helper, or every widened diagnostic call is
  compile-tested on m68k and AArch64. `[OURS]` This project has already shipped a
  64-bit vararg defect of exactly this shape.
- **S6** The cache hash shift is evaluated on the `UQUAD` value, not a truncated copy.

## 3. Boot region

### 3.1 Main Boot Sector

`[PUB]` At sector 0, all fields little-endian. Let `SS = 1 << BytesPerSectorShift`.

| Offset | Size | Field | Accept when |
|---|---|---|---|
| 0 | 3 | `JumpBoot` | `EB 76 90` |
| 3 | 8 | `FileSystemName` | exactly `"EXFAT   "` |
| 11 | 53 | `MustBeZero` | all bytes zero |
| 64 | 8 | `PartitionOffset` | informational, not validated |
| 72 | 8 | `VolumeLength` | `>= 2^(20 - BytesPerSectorShift)` |
| 80 | 4 | `FatOffset` | `>= 24` **and** `<= ClusterHeapOffset - FatLength * NumberOfFats` |
| 84 | 4 | `FatLength` | `>= ceil((ClusterCount + 2) * 4 / SS)` **and** `<= (ClusterHeapOffset - FatOffset) / NumberOfFats` |
| 88 | 4 | `ClusterHeapOffset` | `>= FatOffset + FatLength * NumberOfFats` **and** `<= VolumeLength` |
| 92 | 4 | `ClusterCount` | `== min((VolumeLength - ClusterHeapOffset) >> SectorsPerClusterShift, 0xFFFFFFF5)` |
| 96 | 4 | `FirstClusterOfRootDirectory` | `2 ..= ClusterCount + 1` |
| 104 | 2 | `FileSystemRevision` | exactly 1.00 (§1.1) |
| 106 | 2 | `VolumeFlags` | `ActiveFat` (bit 0) **must be 0** when `NumberOfFats == 1` |
| 108 | 1 | `BytesPerSectorShift` | `9 ..= 12` |
| 109 | 1 | `SectorsPerClusterShift` | `0 ..= (25 - BytesPerSectorShift)`, i.e. cluster ≤ 32 MiB |
| 110 | 1 | `NumberOfFats` | exactly 1 (§1.2) |
| 112 | 1 | `PercentInUse` | `0 ..= 100`, or `0xFF` meaning unknown |
| 510 | 2 | `BootSignature` | `0xAA55` |

**G1** Every bound above is evaluated in the S2 subtraction form. The geometry fields
are mutually constrained, so each must be validated in an order where its operands are
already known good: sector shift, then cluster shift, then `VolumeLength`, then
`FatOffset`, `FatLength`, `ClusterHeapOffset`, `ClusterCount`, then the root cluster.

**G2** `MustBeZero` is the region a legacy FAT driver reads as its BPB. `[DERIVED]` It
is validated rather than ignored: a non-zero value there is not a conforming exFAT
volume, and mounting it risks acting on a FAT BPB that is not one.

### 3.2 Boot region checksum

`[PUB]` Sectors 0 to 10 are checksummed into sector 11, which is filled with **repeated
copies** of the same 32-bit value, `SS / 4` of them. The checksum is a 32-bit
rotate-right-then-add over every byte, excluding bytes 106, 107 and 112 of sector 0
(`VolumeFlags` and `PercentInUse`, which mutate in normal use).

- **B1** The handler computes the checksum over sectors 0 to 10.
- **B2** `[PUB]` A mismatch refuses the mount. This is not a judgement call: the format
  requires the boot region to be validated before its contents are used.
- **B3** **Every** repeated word in sector 11 is verified, not only the first.
  `[DERIVED]` A sector whose copies disagree is self-inconsistent, which is evidence of
  damage even when the first copy happens to match.

### 3.3 Backup boot region

`[PUB]` A backup copy occupies sectors 12 to 23.

**B4** Phase 1 does **not** fall back to the backup region, and refuses the mount when
the primary fails, *whether or not the backup is valid*. `[DERIVED]` This is a
**deferred recovery policy**, not a claim that recovery is wrong. Choosing between two
disagreeing geometries is a decision a user should make explicitly, and Phase 1 has no
way to present that choice. Test T9b asserts the refusal holds when the backup is good.

## 4. Access rules

**A1** `[PUB]` Every on-disk field is little-endian and read through the AROS byte-order
macros. No structure is dereferenced directly for a multi-byte field.

**A2** `[DERIVED]` exFAT structures are packed with fields at unaligned offsets:
`VolumeLength` at 72 in the boot sector, and `DataLength` at **offset 24 within the
Stream Extension entry**, which is offset 56 within the entry set. A 68000 or 68010
takes an address error on an odd-address word access, so every multi-byte read is
byte-wise or through an accessor that is byte-wise on m68k. The AROS `amiga-m68k` target
includes 68000-class machines; this is not optional there.

**A3** Reserved and `MustBeZero` fields are never written and never interpreted beyond
§3.1.

## 5. Streams and cluster traversal

`[PUB]` The FAT holds 32-bit little-endian entries. Entry 0 is `0xFFFFFFF8`, entry 1 is
`0xFFFFFFFF`; both describe no cluster. `0xFFFFFFF7` marks a bad cluster, `0xFFFFFFFF`
terminates a chain. Data clusters are numbered from 2.

### 5.1 The stream descriptor

`[DERIVED]` `NoFatChain` makes "what is the next cluster" a **per-stream** property, and
a walker holding only `(volume, cluster)` cannot express it. A flags parameter is not
sufficient either, because a correct traversal also needs to know where the stream ends
and whether its length is known at all. Traversal therefore takes a descriptor:

| Field | Meaning |
|---|---|
| `first_cluster` | `ULONG`, validated `2 ..= ClusterCount + 1` |
| `data_length` | `UQUAD`, total length |
| `valid_data_length` | `UQUAD`, `<= data_length` |
| `contiguous` | `NoFatChain` set: clusters run consecutively, the FAT is not read |
| `length_known` | FALSE for the root directory (§5.3) |

**F1** A cluster outside `2 ..= ClusterCount + 1` encountered during a walk terminates
the walk with an I/O error. Never a wrap, never a read.

**F2 — NoFatChain.** `[PUB]` When bit 1 of `GeneralSecondaryFlags` is set, clusters run
consecutively from `first_cluster` and **the FAT must not be consulted**.

**F3** For a contiguous stream, the cluster count derives from `data_length` and the
cluster size, and a read beyond it is out of range regardless of the FAT's contents.

### 5.2 Root directory

`[PUB]` The root directory is a **FAT-chained stream** with no directory entry
describing it. **F4** Its descriptor is built with `first_cluster` from
`FirstClusterOfRootDirectory`, `contiguous` FALSE, `length_known` FALSE, and its length
obtained by walking the chain. A walk that revisits a cluster already seen terminates
with an I/O error, bounding a cyclic chain.

### 5.3 Allocation bitmap is authoritative

`[PUB]` The allocation bitmap, located by an Allocation Bitmap directory entry (type
`0x81`), is exFAT's authoritative record of which clusters are in use. One bit per
cluster, least-significant bit first, bit 0 describing cluster 2. A set bit means
allocated. This is a **separate** structure from FAT chain linkage, and the two answer
different questions.

- **M1** The bitmap is loaded at mount and used for `ACTION_INFO` free-space reporting.
- **M2** **Every cluster read as part of a stream's data must be marked allocated.**
  A cluster that is not fails that read with an I/O error. `[DERIVED]` The mount is not
  refused and other files remain readable: one bad stream is not evidence the volume's
  geometry is wrong.
- **M3** M2 matters most for a **contiguous** stream, where nothing else bounds the run.
  A corrupt or hostile `DataLength` on a `NoFatChain` file would otherwise expose
  unallocated residue, which may be another file's deleted contents. For a FAT-chained
  stream the chain provides a second constraint; for a contiguous one the bitmap is the
  only one.

## 6. Up-case table

`[PUB]` Located by an Up-case Table entry (type `0x82`). Maps UTF-16 code units to their
upper-case form, and may be compressed: a `0xFFFF` entry is followed by a count of code
points mapping to themselves.

- **U1** The table is read from the volume, never replaced by a built-in rule.
  `[DERIVED]` Name comparison and the name hash must agree byte-for-byte with the
  writing implementation, and only the on-disk table guarantees that.
- **U2** The table's checksum, recorded in its directory entry, is verified. A mismatch
  refuses the mount, by the same reasoning as B2.

## 7. Directory entry sets

`[PUB]` Directories are arrays of 32-byte entries. In `EntryType`: bit 7 is InUse, bit 6
is TypeCategory (0 primary, 1 secondary), bit 5 is TypeImportance (0 critical, 1
benign), bits 0-4 are the type code. A whole byte of `0x00` ends the directory. An entry
with InUse clear is deleted.

A file is an **entry set**:

| Type | Role | Fields Phase 1 reads |
|---|---|---|
| `0x85` | File | `SecondaryCount`, `SetChecksum`, `FileAttributes`, create and modify timestamps with 10 ms increments and UTC offsets, last-access timestamp with a UTC offset **but no 10 ms field** |
| `0xC0` | Stream Extension | `GeneralSecondaryFlags`, `NameLength`, `NameHash`, `FirstCluster`, `ValidDataLength`, `DataLength` |
| `0xC1` | File Name | 15 UTF-16 code units each |

Also recognised: `0x83` Volume Label, `0x81` Allocation Bitmap, `0x82` Up-case Table.

### 7.1 Structural validation before trusting the count

**D1** `SecondaryCount` is **untrusted input**. Before an entry set is skipped or
accepted, all of the following must hold:

- `SecondaryCount` is `2 ..= 18`, the minimum being one Stream Extension plus one File
  Name, the maximum being what 255 name code units require;
- the whole claimed set lies within the directory stream;
- entry 2 of the set is a Stream Extension (`0xC0`);
- entries 3 onward are File Name entries (`0xC1`);
- `NameLength` is `1 ..= 255` and consistent with the File Name entry count.

**D2** If any of D1 fails, the structure is malformed and the handler **terminates
enumeration of that directory with an I/O error**. `[DERIVED]` It does not skip
`SecondaryCount` entries and continue: that would be navigating by a field already known
to be untrustworthy, and can walk into the middle of another set or past the stream.

**D3** If D1 holds but `SetChecksum` fails, that **one file is skipped** as if deleted
and enumeration continues. `[DERIVED]` The set is structurally sound, so the reader
knows exactly how far to advance; only the contents are suspect. This is the difference
between "I cannot navigate" and "I can navigate but should not trust this record".
`SetChecksum` covers the whole set excluding bytes 2 and 3 of the first entry.

**D4** `NameHash` is verified against the reconstructed name using the up-case table.
A mismatch is treated as D3. The hash is a 16-bit reject filter only; a lookup that
matches on hash **must** still perform the full up-cased comparison (see T6b).

### 7.2 Unknown entry types

`[PUB]` The importance bit distinguishes entries a reader may ignore from those it may
not. `[DERIVED]`:

- **Unknown benign** (bit 5 set), primary or secondary: **skipped**, enumeration
  continues.
- **Unknown critical primary** (bits 5 and 6 clear): the directory contains a record
  this handler cannot interpret and whose meaning may alter the rest. **Terminate that
  directory with an I/O error.**
- **Unknown critical secondary** (bit 5 clear, bit 6 set): the enclosing set cannot be
  trusted. **Skip the whole set**, continue the directory.

### 7.3 Valid data length

**D5** `[PUB]` `ValidDataLength` may be less than `DataLength`, and the specification
requires reads beyond it to return **zeroes**. Phase 1 does so, and does not return the
on-disk residue, which may be another file's deleted contents.

## 8. Names and character set

**N1** `[PUB]` Names are UTF-16, up to 255 code units, case-insensitive and
case-preserving. `[PUB]` The format requires names within a directory to be **unique
after up-casing**, so a conforming volume never contains two names differing only in
case.

**N2** `[AROS]` Conversion to the local character set uses the existing
`from_unicode[65536]` / `to_unicode[256]` tables the FAT handler already maintains.

**N3** Lookup compares the requested name against the on-disk UTF-16, up-cased through
the volume's table. It never compares against the lossy local form.

**N4 — the Phase 1 limitation, stated honestly.** A code unit with no local
representation is presented as `_`, matching what the FAT handler already does for long
filenames. `[DERIVED]` The consequence is accepted rather than argued away:

- such a file **may not be openable by its displayed name**, because the displayed name
  is not the name on disk;
- two distinct on-disk names **may display identically**, and the display gives the user
  no way to tell them apart.

This is a real defect, not a design. It is retained for Phase 1 because a reversible
alias scheme is a compatibility feature with its own design questions, and inventing one
here would be scope creep. T5b exists specifically to document the limitation rather
than to assert it is acceptable.

**N5** Surrogate pairs pass through as two code units, subject to N4. Phase 1 does not
attempt astral-plane composition.

## 9. DOS action behaviour

**R1** Every mutating action returns `DOSFALSE` with `ERROR_DISK_WRITE_PROTECTED`:
`ACTION_WRITE`, `ACTION_DELETE_OBJECT`, `ACTION_RENAME_OBJECT`, `ACTION_CREATE_DIR`,
`ACTION_SET_PROTECT`, `ACTION_SET_DATE`, `ACTION_SET_COMMENT`, `ACTION_SET_FILE_SIZE`,
`ACTION_FORMAT`, `ACTION_RELABEL`.

**R2** `ACTION_FINDOUTPUT` and `ACTION_FINDUPDATE` are refused the same way.
`ACTION_FINDINPUT` succeeds.

**R3** The four 64-bit packet actions are handled explicitly, not left to a default:

| Action | Phase 1 behaviour |
|---|---|
| `ACTION_GET_FILE_POSITION64` (8002) | **supported**, returns the position |
| `ACTION_GET_FILE_SIZE64` (8004) | **supported**, returns `DataLength` |
| `ACTION_CHANGE_FILE_POSITION64` (8001) | **supported**, seeking does not mutate the volume |
| `ACTION_CHANGE_FILE_SIZE64` (8003) | `DOSFALSE` + `ERROR_DISK_WRITE_PROTECTED` |

**R4** `[AROS]` An action the handler does not implement replies `DOSFALSE` with
`ERROR_ACTION_NOT_KNOWN`. `[OURS]` This is not cosmetic: `dos64.library` distinguishes
"unsupported" from "answered zero" solely by the secondary result. Three upstream
regressions have been traced to handlers that replied `DOSFALSE` with no error, and one
was found in the NTFS handler during this project.

**R5** `RESULT1` is a **value**, never a pointer into handler state. `[OURS]` The NTFS
handler returned the address of its own size field for `ACTION_GET_FILE_SIZE64`; with
`dos64.library` present that address is read as the file size.

## 10. Expected mount errors

| Condition | Result |
|---|---|
| `FileSystemName` not `"EXFAT   "` | `ERROR_NOT_A_DOS_DISK` |
| `MustBeZero` non-zero | `ERROR_NOT_A_DOS_DISK` |
| `BootSignature` not `0xAA55` | `ERROR_NOT_A_DOS_DISK` |
| Boot checksum mismatch, or copies disagree | `ERROR_DISK_NOT_VALIDATED` |
| Up-case table checksum mismatch | `ERROR_DISK_NOT_VALIDATED` |
| `FileSystemRevision` not exactly 1.00 | `ERROR_OBJECT_WRONG_TYPE` |
| `NumberOfFats == 2` (TexFAT) | `ERROR_OBJECT_WRONG_TYPE` |
| `ActiveFat` set with `NumberOfFats == 1` | `ERROR_OBJECT_WRONG_TYPE` |
| Geometry out of range (§3.1) | `ERROR_BAD_NUMBER` |
| Device cannot address the volume | `ERROR_SEEK_ERROR` |

## 11. Acceptance tests

`[OURS]` Authored on macOS, verified against the originals. The hosted darwin build
mounts a disk image through `fdsk.device` and is driven headlessly by `aros-ctl`.

| # | Vector | Pass condition |
|---|---|---|
| T1 | Empty volume, 512 and 4096-byte sectors | Mounts; `Info` free space plausible |
| T2 | File of 4 GiB − 1, 4 GiB, 4 GiB + 1 | Exact size; full byte-compare |
| T3 | Fragmented file, ≥ 8 extents | Byte-compare |
| T4 | Contiguous file, `NoFatChain` set | Byte-compare, and the FAT is never read for it |
| T5 | 255-code-unit name, mixed case, non-ASCII | Listed and openable |
| T5b | Name with an unmappable code unit | Documents N4: displayed as `_`, **not** openable by the displayed form. Asserts the limitation, does not assert it is fine |
| T6 | Mixed-case name opened using different casing | Opens, proving up-cased comparison |
| T6b | Two distinct names contrived to share a 16-bit `NameHash` | Both resolve correctly, proving the full comparison runs and the hash is only a filter |
| T7 | Directory of 10,000 entries | Full enumeration, no truncation |
| T8 | `ValidDataLength < DataLength` | Tail reads as zeroes (D5) |
| T9 | Corrupted boot checksum | Refused, `ERROR_DISK_NOT_VALIDATED` |
| T9b | Primary boot region bad, **backup valid** | Still refused (B4) |
| T10 | `NumberOfFats = 2` | Refused, `ERROR_OBJECT_WRONG_TYPE` |
| T11 | One set with a bad `SetChecksum`, structurally valid | That file invisible, rest of directory intact (D3) |
| T11b | Set with `SecondaryCount` beyond the stream | Directory enumeration errors, no wild skip (D2) |
| T11c | Unknown critical primary entry | Directory errors. Unknown benign entry: skipped |
| T12 | Every mutating action from R1, plus `ACTION_CHANGE_FILE_SIZE64` | `ERROR_DISK_WRITE_PROTECTED` |
| T13a | **Unit test**: bounds and `SECTOR_FROM_CLUSTER` around `2^32` sectors against a fake backend | No wrap; S2 and S4 hold |
| T13b | **Integration**: device shim recording 64-bit offsets, exposing synthetic sectors | Offsets issued match those computed |
| T14 | Cache key distribution below and above `2^32` | Chain lengths comparable; equality on `UQUAD` keys is exact |

**T-neg** No test may pass by the handler declining to mount. T9, T9b and T10 assert
refusal; every other test asserts a successful mount first.

## 12. Test harness

`[AROS]` **`fdsk.device` cannot exercise T13.** Its supported-command table lacks
`TD_READ64` and `NSCMD_TD_READ64`, its read path takes `iotd_Req.io_Offset` straight to
a 32-bit `Seek()`, and its geometry uses a 32-bit `fib_Size`.

**H1** exFAT does **not** depend on fixing that. T13 splits into T13a, a pure arithmetic
unit test against a fake backend, and T13b, an integration test against a purpose-built
device shim that records the 64-bit offsets it is asked for and serves synthetic
sectors. Neither requires a real multi-terabyte image.

**H2** Extending `fdsk.device` with a 64-bit path, now cheap because `dos64.library`
provides `Seek64()`, is worthwhile on its own merits and is tracked separately. It is
not a Phase 1 dependency.

**H3** `[AROS]` The cache hash needs no change for a `UQUAD` key.
`(blockNum >> 5) & (hash_size - 1)` distributes sequential ranges identically above and
below `2^32`, since widening preserves the low bits it uses. T14 measures this; the hash
is replaced only if measurement shows problematic chains.

`[DERIVED]` Separately, and not a correctness issue: the FAT handler creates its cache
with 64 blocks, 32 KB at a 512-byte sector size. That is small for exFAT on a large
volume and is likely to show up as a throughput problem in Phase 2. Flagged so it is a
measured decision later rather than a surprise.
