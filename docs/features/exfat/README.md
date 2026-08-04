# exFAT handler status and development guide

The `exfat-handler` branch in `../aros-upstream` now contains a writable,
format-capable `FATX` handler with removable-media change handling and shared
MBR/GPT/USB content discovery. It supports exclusive update handles,
create/delete, truncate, VDL growth and allocation-changing resize. It is implemented
from [the clean-room functional specification](spec.md), not from another
exFAT implementation.

exFAT fills a practical gap in AROS: the existing FAT handler supports only
FAT12/16/32, while large SD cards and removable media are commonly exFAT.

## Why this is a separate handler

The FAT handler abstracts FAT-table access through two function pointers, but
contains 146 direct assumptions about the legacy 32-byte directory entry.
exFAT instead uses checksummed entry sets (File, Stream Extension and one or
more File Name entries), an allocation bitmap, an on-disk up-case table and
per-stream `NoFatChain` semantics. Those differences cross the whole handler,
not one dispatch point, so a separate module keeps the existing boot-critical
FAT implementation outside the change's blast radius.

The DosType is `FATX` (`0x46415458`), matching the existing OS4 and Aminet
handlers. exFAT shares MBR type 0x07 and the GPT Microsoft Basic Data GUID
with other formats, so automounters use the shared on-disk signature probe
instead of claiming those partition identifiers unconditionally.

## Delivered

- Overflow-safe `UQUAD` sector addressing and cache keys, including refusal
  when a device cannot address the requested byte range.
- Main/extended boot-region validation and full repeated checksum validation.
- FAT-chained and `NoFatChain` stream reads with allocation-bitmap checks,
  zero-filled invalid-data tails, and cyclic-chain detection.
- Allocation bitmap and compressed up-case table loading and validation.
- Checked directory entry-set parsing, set checksum and name-hash validation,
  case-insensitive lookup, nested paths, and DOS volume registration.
- DOS packets for locks, parent/copy/same-lock, examine/enumerate,
  open/read/seek/close, info, current volume, and the 64-bit position/size
  actions. `FINDUPDATE`, `FINDOUTPUT`, writes and 32/64-bit size changes use
  exclusive object locking and ordered commits. Adjacent free clusters
  preserve `NoFatChain`; blocked growth converts the complete stream to a FAT
  chain. Shrink and delete unpublish reachability before releasing FAT/bitmap
  ownership. Directory create/growth/delete, same- and cross-parent rename,
  read-only/archive protection, and modification-date updates use the same
  ordered transaction foundation. Relabel and `ACTION_FORMAT` are supported.
  Creation, modification and access timestamps use strict Gregorian packing,
  10 ms fields, and signed 15-minute UTC-offset conversion through
  `locale.library`, with a defined pre-1980/unrepresentable-offset fallback.
  Unknown benign secondaries remain opaque across metadata updates and rename,
  including entry sets across the complete `SecondaryCount` range through 255;
  deletion releases allocations they own through the generic entry template.
  Empty-directory deletion also purges unknown benign primary sets and frees
  allocations declared by their generic primary and secondary templates.
  exFAT has no native comment field, so comments and unknown packets fail with
  `ERROR_ACTION_NOT_KNOWN`.
- Deterministic post-sync failpoints, old-or-new payload checks, dirty-bit
  inspection, repair fsck and clean second-fsck validation.
- Full-volume preflight rollback: failures before the first real mutation
  restore the exact original boot flags, while failures after mutation retain
  the conservative dirty-volume recovery contract. Case-folded create and
  rename collisions are rejected without changing either object.
- A single-FAT formatter for 512, 1024, 2048 and 4096-byte sectors, including
  main/backup boot regions, repeated checksum, FAT, bitmap, compressed up-case
  table, root metadata and volume label.
- `TD_ADDCHANGEINT` media lifecycle with standard disk-inserted/disk-removed
  input events, safe offline locks and detached-cache discard, plus shared
  MBR/GPT and both USB-stack discovery paths. All automounters take the
  installed module name from one `EXFAT_HANDLER_NAME` definition.
- A target fix for `fdsk.device`: its custom Open path now holds a temporary
  open reference so low-memory expunge cannot unload the device while its
  worker process is being created.
- `fdsk.device` now advertises and implements `NSCMD_TD_READ64` and
  `NSCMD_TD_WRITE64`. The hosted `emul-handler` accepts the corresponding
  dos64 seek and size packets, so a sparse backing image can be addressed
  past 4 GiB.

## Gates and reproduced target result

Run the host and cross-code-generation gates with:

```sh
cd hosted/exfat-tests
./build.sh
```

Run the entire reproducible host and target matrix from the harness root with:

```sh
./graft/exfat-acceptance-smoke
```

It includes the immutable physical-capture round trip when
`Unit14.physical-capture` is available. Direct USB-stack discovery remains a
separate real-hardware gate.

This runs the bounds, boot-region, metadata/up-case, ASan and UBSan suites,
the AArch64 AROS compile gate, the m68k byte-I/O code-generation check, and a
complete production-source compile with a genuine big-endian m68k AROS
toolchain when `$HOME/aros-m68k-build` is present. Its output objects are
verified as `m68k:68000`; `AROS_M68K_BUILD` can select another build tree.
The genuine m68k module also links as `AROS/L/exfat-handler`; this required a
portable D0/D1 return alias in the m68k library-call generator for the
`dos64` functions which combine a `QUAD` return with a split `QUAD` argument.
The handler packet loop also uses the required `DosPacket64` overlay on
32-bit targets, preserving `DP64_INIT` and full-width position/size results.
Build the AArch64 target module with:

```sh
TARGETS=kernel-fs-exfat ./graft/rebuild-aros.sh
```

The transport gate below builds native raw-device and CRT probes, uses a
disposable sparse `FDSK:Unit3` image, and proves that bytes at offset zero
cannot be confused with bytes at 4 GiB - 1, 4 GiB, or 4 GiB + 1. It also
writes and reads back a byte at 4 GiB + 2, then proves `fopen`/`fseek`/
`ftell`/`lseek`/`fstat` report the same file correctly past 4 GiB:

```sh
./graft/exfat-fdsk64-smoke
```

The handler-level sparse gate formats a 16 GiB logical image with 1 MiB
clusters while requiring its physical allocation to stay below 512 MiB. It
raw-authors three contiguous files sized 4 GiB - 1, 4 GiB and 4 GiB + 1 plus a
`ValidDataLength` fixture, then verifies sizes, sentinels and zero-tail reads
through DOS:

```sh
./graft/exfat-sparse-smoke
```

Every high-offset sentinel is checked against its low-32-bit raw-image alias,
so an offset truncation cannot accidentally return the expected byte. The
allocation bitmap, raw sentinels and sparse physical size are verified before
the target boots.

This is a bounded sparse oracle rather than a literal stream of every byte in
the roughly 12 GiB of large-file payload. It proves exact sizes and reads the
first/final bytes and 4 GiB boundary while rejecting every low-32-bit alias.
General full-stream comparison is already covered by T3/T4; T2 specifically
targets 32-bit size and offset truncation.

For a reproducible small-volume target fixture, rather than the retained
manual Unit0 image, run:

```sh
./graft/exfat-fixture-smoke
```

That gate runs both packet-contract probes. The 4 write/size paths exercised
through a read-only handle return `ERROR_DISK_WRITE_PROTECTED`; comments and
an unknown packet return `ERROR_ACTION_NOT_KNOWN`. A second probe takes an
exclusive update handle,
writes across a sector boundary, creates/deletes and `FINDOUTPUT`-truncates
files, creates/grows/deletes directories, moves files and non-empty directories
across parents, updates protection/date metadata, resizes contiguous and
chained streams, extends one contiguous file in place, and converts blocked
file and directory streams to FAT chains. The stopped image must then pass
`fsck_exfat -n` and exact comparisons after a host read-only remount.

Run the empty-volume and mount-geometry matrix with:

```sh
./graft/exfat-geometry-smoke
```

It mounts clean 512- and 4096-byte-sector controls, rejects both logical/device
sector mismatches, rejects a Mountlist partition shorter than `VolumeLength`,
and mounts a longer partition while proving that the handler keeps the on-disk
volume boundary.

Run its dedicated 255-code-unit target probe with:

```sh
EXFAT_NAME_ONLY=1 ./graft/exfat-fixture-smoke
```

Run the isolated T3 gate with:

```sh
./graft/exfat-fragmentation-smoke
```

It deliberately cannot share T7's image: T3 creates and frees cluster holes,
whereas T7's 10,000-entry directory consumes those same clusters for directory
data and can turn a supposedly fragmented test stream contiguous.

Run the isolated T7 enumeration gate with:

```sh
./graft/exfat-enumeration-smoke
```

Run the paired clean/corrupt directory and boot-region gates with:

```sh
./graft/exfat-corruption-smoke
```

The batch covers T9, T9b, T10, T11, T11b and T11c plus all remaining
mount-error-table identity/version cases, up-case checksum, name-hash,
critical-secondary and allocation-bitmap corruption. Refusal cases assert the
exact mapped DOS error, not just that the mount failed. A single case can be
selected with `EXFAT_CORRUPT_CASE=set-checksum`; the other directory cases are
`secondary-count`, `unknown-critical-primary` and
`unknown-benign-primary`. The additional selectors are `upcase-checksum`,
`name-hash`, `unknown-critical-secondary`, `unknown-benign-secondary`,
`unknown-benign-secondary-large`, `unknown-benign-secondary-write` and
`bitmap-free-cluster`. The large case adds 64 opaque extensions. The write case
proves opaque-byte preservation across protection/date/rename, then verifies
that deletion clears a benign secondary's allocated cluster and that deleting
an otherwise empty directory purges hidden benign-primary allocations. Boot and
version selectors include `boot-checksum`, `wrong-fsname`, `must-be-zero`,
`boot-signature`, `revision`, `active-fat`, `primary-bad-backup-good` and
`texfat`. Directory mutations
target a named subdirectory entry set so metadata bootstrap cannot mask the
DOS enumeration policy. They prove that bad checksums and unknown benign
entries skip only one set, while overlong counts and unknown critical entries
report a directory error without refusing the volume.

The normal gate currently discharges T4 (a verified `NoFatChain` stream),
T5b (an unmappable UTF-16 unit is displayed as `_` and that display spelling
does not open the file), and T6b (two distinct names with the same on-disk
16-bit name hash both resolve to their distinct contents). The dedicated T5
probe opens two distinct 255-code-unit names directly through DOS. They share
their first 106 units and therefore display as two identical `List` entries.
This is a second independent lossy presentation case alongside N4's `_`
mapping: AmigaDOS presentation is lossy in both character mapping and name
length, while path resolution remains exact in both cases.

The baseline gate formats a disposable raw 64 MiB image with macOS
`newfs_exfat`, mounts it through `fdsk.device` as `EXFAT4:`, and asserts
enumeration, case-folded open, and host byte comparisons. The isolated gates
above cover vectors whose fixture requirements conflict or need byte-level
mutation.

The August 4, 2026 target smoke test used a 64 MiB image formatted and populated
by macOS, mounted through `fdsk.device`. `List EXFAT0: ALL` enumerated the root,
the macOS metadata directory, and a nested directory. Files were copied from
the volume into `SYS:T` and checked on the host:

```text
Hello.txt             ea19efb17590b24c4080dbe807df42e52169ec85e85ab6b961de79896ac4d424
SubDir/Handler.bin    ece5e609bd1a9cd248d8154280be2bb4c72c5fa4f92ffa873e6ef4df2a721b4a
```

Opening `hello.TXT` proved case-insensitive lookup. The current fixture adds
transactional VDL, allocation-changing extension, directory mutation, rename,
and metadata-update coverage, then validates the image with the host checker
and read-only remount.

Exercise the removable-media lifecycle through the real target handler and
`fdsk.device` change interrupt path with:

```sh
./graft/exfat-media-change-smoke
```

The probe holds a root lock and file handle while ejecting the medium, replaces
the backing image with another volume carrying the same label, closes both
stale objects, reloads and waits for the replacement to remount. The host then
runs `fsck_exfat -n` against both the removed and replacement images, proving
that detached cache state was not flushed across the media boundary.

Exercise the completely full allocation-bitmap boundary with:

```sh
./graft/exfat-full-smoke
```

The raw fixture author consumes every free cluster. The target then proves
that allocation-changing resize and directory creation return
`ERROR_DISK_FULL` while content, namespace and clean flags remain unchanged;
zero-length create/delete still succeeds. A raw bitmap oracle and external
`fsck_exfat -n` validate the image before and after the target run.

Exercise the inherited dirty-volume safety boundary with:

```sh
./graft/exfat-dirty-smoke
```

This creates and validates a clean host fixture, sets only the checksum-exempt
`VolumeDirty` word, and snapshots the image. The target must still read the
known payload, report `ID_WRITE_PROTECTED`, and refuse both update and create.
A whole-image comparison then proves that it did not clear the inherited flag
or mutate any other byte. The host sanitizer suite separately drives the
production directory-growth predicate at the exact 256 MiB limit and its
overflow boundaries.

For the captured physical-stick round trip, use:

```sh
./graft/exfat-usb-image-smoke
```

`Unit14.physical-capture` is the immutable byte-exact 512 MiB capture of the
exFAT partition, not a synthetic formatter fixture. The gate copies it to a
disposable, user-writable `Unit17`, verifies macOS-authored files in AROS,
performs a staged AROS create/write/commit/readback plus rename and delete,
runs `fsck_exfat -n`, and verifies the mutations through a macOS read-only
remount. The disposable image is removed afterward, so the gate is repeatable
and never asks hosted `fdsk.device` to write the root-owned `sudo dd` capture.
After writing `Unit14` back to the partition, run
`hosted/exfat-tests/verify-exfat-physical.sh` from an interactive macOS
terminal. It checks the exact device size and filesystem, three expected
SHA-256 values, rename/delete visibility, and `diskutil verifyVolume`.

Build the read-only physical-hotplug acceptance probe for both AArch64 and
m68k with:

```sh
hosted/exfat-tests/build-exfat-hotplug-probe.sh
./graft/exfat-hotplug-probe-smoke
```

The smoke gate validates the probe's full-file CRC32 and FATX checks against
Unit14. To test discovery itself, copy the architecture-appropriate output to
an AROS system as `C:EXFATHotplugProbe`, boot without a manual exFAT Mountlist,
plug in the physical stick, and run `EXFATHotplugProbe AROSEX:`. A pass proves
that the active USB stack selected and loaded the handler and that all three
post-mutation payloads are exact.

Hosted libusb pass-through is not a substitute on this Mac. The virtual HCI was
made build-portable (`pkg-config` headers, Linux/Darwin runtime names), fixed to
claim interfaces on hosts where kernel-driver queries are unsupported, and made
to expose USB 3.x devices through its implemented USB 2 high-speed view. Its
AArch64 module links, but macOS 26.5.1 refuses ownership of the SanDisk mass-
storage interface: libusb can open 0781:55a3, then interface 0 fails with -99
and IOKit reports `IOCreatePlugInInterfaceForService: out of resources`, even
after whole-disk unmount and eject. Therefore direct discovery/hotplug remains
a real-AROS-hardware (or permissive libusb-host) acceptance item.

## Phase 1 compatibility limits

The Phase 1 matrix in `spec.md` is discharged. Every production source now
compiles with the genuine AROS m68k compiler and the full handler links as a
big-endian 68000 AROS module, in addition to the AArch64 module build and
target-side fixture corpus.

Phase 1 currently presents code units above the local 8-bit character set as
`_`, as specified by N4. Timestamp UTC offsets are applied, including the
signed seven-bit quarter-hour representation and safe fallback for values the
format cannot encode. The filename mapping remains a compatibility limit, not
a silent claim of full Unicode support.

## Roadmap

The ordered mutation and power-failure contract for Phase 2 is in
[write-spec.md](write-spec.md). File-tree mutation, allocation-changing resize,
full-volume rollback, collision handling, failpoint recovery,
formatter/relabel, media changes and discovery are implemented. Comments are
explicitly unsupported because exFAT defines no portable on-disk
representation for them.

| Phase | Scope | State |
|---|---|---|
| 0 | 64-bit DOS packet plumbing | delivered on the branch |
| 0.5 | 64-bit-safe disk/cache and mount geometry | delivered and gated |
| 1 | Explicit read-only `FATX` mount, list and read | delivered and gated |
| 2 | Writes, fault injection and external `fsck_exfat` validation | delivered and gated |
| 3 | Formatter, media changes and shared MBR/GPT/USB content probing | implemented; a captured physical partition passes cross-OS mutation, direct USB hotplug/discovery acceptance remains |

USB discovery is shared across `rom/partition`, the ROM Poseidon mass-storage
class, and the alternate workbench USB mass-storage implementation. Partition
content probes validate the external block geometry before converting its
longword count to bytes, avoiding a wrap or oversized allocation on 32-bit
targets. `hosted/exfat-tests/build-exfat-discovery.sh` links both USB modules
for AArch64 and m68k and verifies that every output selects the installed
`exfat-handler` module, not the historical dot-named handlers used by the
alternate stack. It also asserts that normal hosted, generic native, Raspberry
Pi, Sam440 and RISC-V build/package graphs include `kernel-fs-exfat`, so
recognition cannot ship without the handler itself.

## Implementation map

- `rom/filesys/exfat/volume.c`: ordered mount, boot region, metadata bootstrap,
  DOS volume lifecycle.
- `stream.c`: FAT/contiguous traversal, stream I/O and new-cluster zeroing.
- `directory.c`: bitmap, up-case and validated entry sets.
- `transaction.c`: main-boot dirty-bit and durable-stage boundaries used by
  data, FAT, bitmap and directory publication.
- `allocation.c`: ordered zero/FAT/bitmap allocation and stream growth.
- `packet.c`: AmigaDOS read surface plus exclusive extending update handles.
- `format.c`: portable single-FAT formatter.
- `compiler/include/dos/exfat.h`: freestanding content recognizer used by
  partition and USB discovery.
- `exfat_meta.h`: pure metadata helpers shared by hosted corruption tests.
- `hosted/exfat-tests/`: host sanitiser and target code-generation gates.
