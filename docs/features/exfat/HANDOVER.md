# exFAT handover — 2026-08-04

## Current state

The `exfat-handler` branch in `../aros-upstream` now contains a writable,
portable `FATX` handler. It reads FAT-chained and `NoFatChain` streams, creates,
grows, truncates, renames and deletes files/directories, updates protection and
dates, relabels volumes, and formats blank 512- through 4096-byte-sector media.
Every allocation-changing path uses ordered VolumeDirty, data, FAT, bitmap and
directory stages. Deterministic sync failpoints prove that interrupted images
remain dirty, repairable, and contain only the old or new file state.

The handler installs `TD_ADDCHANGEINT`, handles inhibit/remove/reinsert, emits
the standard disk-inserted/disk-removed input events, keeps outstanding locks
as safe offline objects, and never flushes a detached cache onto replacement
media. A shared freestanding boot-sector recognizer routes
MBR type 0x07 and GPT Microsoft Basic Data partitions to `FATX` by content,
and both ROM and workbench USB mass-storage automounters know the handler.
exFAT has no native comment field, so `ACTION_SET_COMMENT` is explicitly
unsupported with `ERROR_ACTION_NOT_KNOWN`.

The executable specification is [spec.md](spec.md); [README.md](README.md)
is the development and invocation guide. This file is the operational status
snapshot for the next person taking the work.

## Proven, target-side acceptance coverage

| Vector | Evidence |
|---|---|
| T1 | `graft/exfat-geometry-smoke`: empty 512- and 4096-byte-sector volumes mount and report plausible `Info` geometry |
| baseline/T1a | `graft/exfat-fixture-smoke`: macOS-authored image mounts, lists and byte-compares root and nested files |
| T2 | `graft/exfat-sparse-smoke`: exact 4 GiB - 1/4 GiB/4 GiB + 1 sizes plus raw-verified first/last-cluster and boundary sentinels |
| T3 | `graft/exfat-fragmentation-smoke`: raw oracle requires `NoFatChain` clear and at least 8 extents (last run: 11), then target byte-compare |
| T4 | `graft/exfat-fixture-smoke`: raw oracle confirms `NoFatChain` set before target byte-compare |
| T5 | `EXFAT_NAME_ONLY=1 graft/exfat-fixture-smoke`: two distinct 255-unit names open and read by full name through DOS |
| T5b | unmappable UTF-16 name displays as `_`; its displayed spelling does not resolve |
| T6 | mixed-case open is part of the baseline fixture |
| T6b | two distinct names sharing the actual on-disk 16-bit `NameHash` resolve to their distinct contents |
| T7 | `graft/exfat-enumeration-smoke`: exactly 10,000 fixture entries plus target `TOTAL:` completion line |
| T8 | raw tail contains non-zero sentinels beyond `ValidDataLength`; target reads return zero, including a read crossing the boundary |
| T9 | paired clean control mounts; boot-checksum corruption refuses |
| T9b | paired clean control mounts; broken primary / intact backup refuses |
| T10 | paired clean control mounts; `NumberOfFats == 2` refuses as TexFAT |
| T11 | paired clean control exposes both files; corrupt `SetChecksum` hides exactly the named file while unrelated entries and recursive enumeration remain intact |
| T11b | subdirectory set with `SecondaryCount == 255` reports `List: bad number`; volume remains mounted and leading control entry enumerates |
| T11c | unknown critical primary reports the directory error; the same entry marked benign is skipped and enumeration completes |
| T12 | target probe sends 4 write/size paths through a read-only handle and requires `ERROR_DISK_WRITE_PROTECTED`; comments and an unknown packet require `ERROR_ACTION_NOT_KNOWN` |
| T13a/T14 | host bounds/cache/geometry suites in `hosted/exfat-tests/build.sh` |
| T13b | `graft/exfat-sparse-smoke`: handler reads distinct raw sentinels above 4/8/12 GiB; every high marker is proven different from its low-32-bit alias |
| T13c/T13d | `graft/exfat-fdsk64-smoke`: raw NSD and CRT paths proven around 4 GiB |
| T15a/T15b | `graft/exfat-geometry-smoke`: both logical/device-sector-size mismatches refuse while matching controls mount |
| T16a/T16b | `graft/exfat-geometry-smoke`: a partition shorter than `VolumeLength` refuses; a longer partition mounts and the handler retains the volume boundary |
| T17 | `hosted/exfat-tests/build.sh`: every production source compiles with the genuine AROS compiler; both AArch64 and m68k modules link, and the latter is verified as a big-endian `m68k:68000` AROS ELF |
| T18 | `graft/exfat-fixture-smoke`: file/directory create/delete, same/cross-directory rename, case-folded create/rename collision rejection, directory growth with forced FAT conversion, protection/date updates, `FINDOUTPUT` truncate, contiguous/FAT shrink, poisoned-gap zeroing, allocation-changing extension, raw reachability checks, exact remount comparisons and external `fsck_exfat -n` |
| timestamps | strict Gregorian packing, 10 ms fields and signed UTC-quarter-hour conversion pass host/ASan/UBSan tests; target round-trips `SetFileDate`, automatically touches modified/accessed times after content writes, and leaves raw-valid offsets |
| benign extensions | file sets support the complete `SecondaryCount` range through 255 with dynamic storage; a 64-extension fixture mounts and survives protection/date/rename byte-for-byte. Delete releases generic `AllocationPossible` secondary extents. Deleting an otherwise empty directory also validates, hides and frees unknown benign primary sets and their primary/secondary allocations; raw bitmap/entry oracles and external fsck pass |
| removable media | `graft/exfat-media-change-smoke`: target ejects fdsk media with live locks/file handles, replaces it with a same-label image, closes stale objects, reloads and remounts; external fsck accepts both removed and replacement filesystems |
| ordered-failure recovery | `graft/exfat-failpoint-smoke`: six post-sync interruption points all leave VolumeDirty set; repaired images pass a clean second fsck and hash to exactly the old or new payload |
| pre-existing VolumeDirty | `graft/exfat-dirty-smoke`: a host-authored file remains byte-exact and readable, `Info` reports `ID_WRITE_PROTECTED`, update/create return `ERROR_DISK_NOT_VALIDATED`, and a whole-image comparison proves the handler neither clears the inherited flag nor changes any other byte |
| full volume | `graft/exfat-full-smoke`: a raw oracle requires zero free clusters, then allocation-changing resize and directory creation return `ERROR_DISK_FULL` with original content, reachability and clean flags unchanged; zero-length create/delete succeeds and external fsck remains clean |
| directory ceiling | the production `exfat_directory_can_grow()` predicate gates both known-length and FAT-walked directory growth; host/ASan/UBSan tests cover exactly 256 MiB, one byte over, zero operands and maximal 32-bit operands without overflow |
| relabel | fixture probe relabels to `AROSRENAMED`; raw label oracle, external fsck and host remount agree |
| formatter | handler-authored 512-byte image passes target I/O, raw structural validation, macOS fsck and remount; the 4096-byte image passes target I/O and the raw structural oracle (macOS exposes the raw file as a 512-byte physical device and cannot fsck that pairing) |
| discovery | shared content-probe host/ASan/UBSan tests; AArch64 and m68k `partition.library`, both USB mass-storage modules, and both AArch64/m68k handlers link; normal hosted/native and platform package graphs include `kernel-fs-exfat`; `build-exfat-discovery.sh` proves both USB stacks embed the installed `exfat-handler` name rather than the nonexistent `exfat.handler`; MBR/GPT probe block geometry is validated before its 32-bit byte-size shift |
| physical USB capture/writeback | `graft/exfat-usb-image-smoke`: the immutable byte-exact 512 MiB capture of macOS-formatted SanDisk `disk9s1` (512-byte sectors, 32 KiB clusters) is copied to disposable `Unit17` and mounts through the target handler; three macOS-authored files compare exactly in AROS; AROS create/write/commit/readback, rename and delete are visible after a macOS remount. Two consecutive clean runs prove the gate is repeatable. The earlier verified `Unit14` result was written back to the physical partition; the interactive `verify-exfat-physical.sh` run matched all three SHA-256 values and `diskutil verifyVolume` completed with exit code 0 |
| mount/metadata hardening | target corruptions assert exact DOS errors for `FileSystemName`, `MustBeZero`, boot signature, revision, `ActiveFat`, boot/up-case checksums and TexFAT; bad `NameHash`, unknown critical secondary and allocation-bitmap read locality cover directory/stream policy |

`List` has a legacy display limit: `MAXFILENAMELENGTH` is 108 bytes, and its
BCPL length byte plus NUL leave 106 visible name units. This is deliberately
tested with two exact-but-visually-identical long names. It is a presentation
limitation; path resolution is exact. Character mapping is independently
lossy above the local character set (N4).

## Recent commits

### aros-aarch64

- `7c4db91` paired clean-control corruption gates for T9/T9b/T10.
- `2ec4483` isolated complete 10,000-entry T7 gate.
- `ca3ff97` isolated, raw-verified fragmented T3 gate.
- `9c490d3` long-name display collision test and documentation.
- `1abf28b` T4/T5/T5b/T6b recipe gates.
- `9394d71` reproducible baseline target fixture.
- `92b32ac` CRT large-file proof.
- `36de905` fdsk 64-bit transport gate.

### aros-upstream

- `1ee3aa9806` `fdsk.device` NSD 64-bit backing-image I/O.
- `881c00d9f7` hosted handler successful-seek reply fix.
- `9b125d7f15` CRT routing through `dos64.library`.

The upstream head also contains later exFAT handler work (`6a588892ab`,
`528a62dd3e`) that was already present when this handover was written.

## Harness rules that must not regress

- Every corrupt fixture has a byte-identical clean control that must mount.
  Refusal-only vectors must not be able to pass if the handler refuses every
  image.
- A gate must prove its precondition. T3 reads its raw chain and rejects a
  contiguous stream; T7 requires both all 10,000 names and enumeration
  completion. The sparse gate verifies its allocation-bitmap writes, raw
  sentinel bytes, low-32-bit aliases, exact logical size, and that its 16 GiB
  backing file remains sparse (last run: 7,392 KiB allocated).
- The sparse T2 gate does not stream all roughly 12 GiB through the emulated
  target. General full-stream reads are already covered by T3/T4; T2 targets
  32-bit truncation with exact sizes plus first/final/4 GiB-boundary samples
  whose low-32-bit raw-image aliases are deliberately different.
- T3 and T7 must use different images. T7 directory allocation consumes the
  holes that T3 relies on, yielding a false contiguous-file pass.
- Fixture images use dedicated fdsk units and scripts refuse to overwrite an
  existing image or Mountlist. They clean up their own Unit file and driver.
- A capture made by `sudo dd` is normally root-owned and therefore read-only
  to hosted AROS. Preserve it as `Unit14.physical-capture`:
  `graft/exfat-usb-image-smoke` copies that immutable source to disposable,
  user-writable `Unit17`, and cleans the copy afterward. Passing a root-owned
  capture directly to `fdsk.device` would open an invisible Retry/Cancel
  requester on its first write and make a headless test appear to hang.
- Target filesystem gates use the launcher's `minimal` Startup-Sequence mode;
  unrelated audio, clipboard and desktop initialization must not run before a
  filesystem oracle. The launcher repairs an incomplete bootstrap module list
  if a partial AROS build has removed its mandatory kernel entry.
- The corruption runner supports one case per process for constrained runners:
  `EXFAT_CORRUPT_CASE=texfat ./graft/exfat-corruption-smoke`. Its default is
  the complete paired boot/metadata/directory batch. Directory corruptors target a
  named entry set in a subdirectory so root metadata bootstrap cannot mask the
  enumeration policy. Assertions must match the exact `List` name because
  macOS may also create a separate `._Name` AppleDouble entry. A malformed
  enumeration may print a partial `TOTAL:` before `List: bad number`, so the
  explicit DOS error—not absence of `TOTAL:`—is the T11b/T11c oracle.

## Remaining verification and compatibility work

- The hosted runtime was restored by reconstructing its missing mandatory
  bootstrap modules. The complete fixture, failpoint, 512/4096 formatter,
  geometry, sparse/high-offset, fragmentation, 10,000-entry enumeration,
  corruption, fdsk64 and removable-media suites now pass on target.
- The macOS-formatted partition from an actual USB stick now passes a captured
  data round trip through the target handler and a verified physical writeback.
  The Raspberry Pi 4B port provides the direct-hardware path: native PCIe xHCI,
  Poseidon and USB keyboard/mouse already work there. Its AArch64 handler,
  `partition.library`, mass-storage class and board package now include the
  exFAT discovery changes, and a presentation-friendly read-only console test
  is staged. The remaining MBR gate is one physical Pi run; GPT-partitioned and
  unpartitioned media remain follow-up hardware variants. Both automounters
  share the installed handler name and `EXFATHotplugProbe` provides the exact
  target-side FATX/size/CRC oracle.
- Hosted pass-through was investigated on this Mac. `vusbhci.device` now finds
  libusb through `pkg-config`, tries Linux and Darwin library names, claims each
  interface even when Darwin cannot report a kernel driver, and presents USB
  3.x devices through its implemented USB 2 high-speed view; the isolated
  AArch64 module links. This cannot close the hardware gate on macOS 26.5.1:
  libusb opens the SanDisk 0781:55a3 device but
  `libusb_claim_interface(0)` returns `LIBUSB_ERROR_OTHER` (-99), with IOKit
  reporting `IOCreatePlugInInterfaceForService: out of resources`, unchanged
  after both `diskutil unmountDisk` and `diskutil eject`. The failure occurs
  before AROS or exFAT receives any transfer. Use real AROS hardware (or a host
  OS that permits libusb to detach its mass-storage driver) for the remaining
  discovery/hotplug gate; do not count the captured-image mount as that gate.
- Reinsert-with-live-lock currently waits for stale locks to close before the
  same-named replacement volume is mounted. Safe lock adoption for an exactly
  matching volume identity would improve usability but is not required for
  data safety.
- Local 8-bit filename presentation remains lossy for unmappable UTF-16.
  Timestamp conversion now applies valid signed UTC-quarter-hour offsets;
  clocks before exFAT's 1980 epoch are clamped for automatic timestamps and
  stored without `OffsetValid` because their UTC relationship is unknowable.
- The m68k module initially exposed a `dos64` ABI generator defect:
  `AROS_LC2QUAD1` pasted a `QUAD` return token into a missing call alias. The
  m68k call generator now maps integer-wide returns to its existing D0/D1
  implementation and emits the five-register variant. `kernel-dos64-linklib`
  and the linked `AROS/L/exfat-handler` prove the path.
- On 32-bit AROS the OS4-style position/size packets use `DosPacket64`, not
  native-width `DosPacket` argument/result slots. The handler now validates
  `DP64_INIT`, reads the split overlay arguments, preserves the marker, and
  returns the full `QUAD`; the m68k build no longer merely links code that
  would truncate those packets at runtime.
- Measure cache sizing and performance after correctness, not before.

## Useful commands

```sh
cd /Users/jkn/Source/Macaros
./graft/exfat-acceptance-smoke

# Or run individual gates:
hosted/exfat-tests/build.sh
hosted/exfat-tests/build-exfat-discovery.sh
hosted/exfat-tests/build-exfat-hotplug-probe.sh
make -C /Users/jkn/aros-m68k-build kernel-dos64-linklib
make -C /Users/jkn/aros-m68k-build/rom/filesys/exfat \
  -f mmakefile \
  TOP=/Users/jkn/aros-m68k-build \
  SRCDIR=/Users/jkn/Source/aros-upstream CURDIR=rom/filesys/exfat \
  TARGET=kernel-fs-exfat AROS_TARGET_ARCH=amiga AROS_TARGET_CPU=m68k \
  AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 kernel-fs-exfat
./graft/exfat-fixture-smoke
EXFAT_NAME_ONLY=1 ./graft/exfat-fixture-smoke
./graft/exfat-fragmentation-smoke
./graft/exfat-enumeration-smoke
./graft/exfat-sparse-smoke
./graft/exfat-geometry-smoke
./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=boot-checksum ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=wrong-fsname ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=must-be-zero ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=boot-signature ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=revision ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=active-fat ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=primary-bad-backup-good ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=texfat ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=set-checksum ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=secondary-count ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=unknown-critical-primary ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=unknown-benign-primary ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=upcase-checksum ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=name-hash ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=unknown-critical-secondary ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=unknown-benign-secondary ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=unknown-benign-secondary-large ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=unknown-benign-secondary-write ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=bitmap-free-cluster ./graft/exfat-corruption-smoke
./graft/exfat-fdsk64-smoke
./graft/exfat-media-change-smoke
./graft/exfat-failpoint-smoke
./graft/exfat-dirty-smoke
./graft/exfat-full-smoke
./graft/exfat-format-smoke
EXFAT_FORMAT_UNIT=12 ./graft/exfat-format-smoke
./graft/exfat-usb-image-smoke
./graft/exfat-hotplug-probe-smoke
```

The USB gate now uses the preserved, root-owned
`Unit14.physical-capture` as an immutable source and mutates disposable
`Unit17`; it leaves the previously verified, modified `Unit14` untouched.
On 2026-08-04 that verified `Unit14` was written back to the original 512 MiB
`disk9s1` partition with the following commands; `dd` reported all
536,870,912 bytes and macOS remounted it as writable exFAT:

```sh
diskutil unmount disk9s1
sudo dd if=$HOME/aros-build/bin/darwin-aarch64/AROS/DiskImages/Unit14 \
  of=/dev/rdisk9s1 bs=8m
sync
diskutil mount disk9s1
```

The Codex host process is denied access to `/Volumes/AROSEX` by macOS privacy
controls, so the physical-media acceptance was completed in the interactive
host terminal with:

```sh
hosted/exfat-tests/verify-exfat-physical.sh
```

The script performs these checks (shown separately for manual diagnosis):

```sh
find /Volumes/AROSEX/AROSUSB -maxdepth 2 -type f -not -name '._*' -print
shasum -a 256 /Volumes/AROSEX/AROSUSB/{Renamed.txt,Handler.bin,AROS-Written.txt}
diskutil verifyVolume disk9s1
```

Expected SHA-256 values are `03b7b484ab8deb50fbcdc87dd06e0b97ed9b530b9df6eadc3c310852d9a76d92`
for `Renamed.txt`, `0dbc16c01be5dca6c30799ef8cedb56e2b1f8774feae170321c313ba109f792b`
for `Handler.bin`, and `5d23dbb1dc0c320bb5a6f41c201f2ebb435318c3bf075730738cd463640decfa`
for `AROS-Written.txt`. `Handover.txt` and `Nested/Source.c` must be absent.
The 2026-08-04 interactive run matched all values and `diskutil verifyVolume`
reported that `AROSEX` appears to be OK with filesystem-check exit code 0.

For direct AROS hardware acceptance, build the read-only target probes, put
the appropriate binary in `C:EXFATHotplugProbe`, boot without an EXFAT
Mountlist entry, insert the stick, and run:

```text
EXFATHotplugProbe AROSEX:
```

The pass line proves the USB stack discovered and loaded a FATX handler and
read every byte of all three files with the expected CRC32; it also checks the
rename/delete results. `graft/exfat-hotplug-probe-smoke` runs the identical
oracle against Unit14 through an explicit hosted mount, isolating probe logic
from the hardware automounter under test.

For the native Raspberry Pi 4B run, the Pi source is
`/Users/jkn/Source/aros-upstream-raspi`, the build is
`/Users/jkn/aros-build-850`, and the deployment harness is
`/Users/jkn/Source/aros-raspi`. The build contains the AArch64 probe and the
updated board package. Insert the Pi SD card and run on macOS:

```sh
cd /Users/jkn/Source/aros-raspi
harness/stage-exfat-test.sh
harness/deploy-card.sh /Users/jkn/aros-build-850
```

Then boot the real Pi, insert the `AROSEX` stick, open an AROS Shell, and run:

```text
Execute S:ExFAT-Pi4-Demo
```

The six-section console output is designed for screenshots/video: it prints
the live OS/library versions, Shell location and resolved probe, Pi/AArch64
and xHCI identity, Poseidon devices, `Info AROSEX:`, the fixture listing and
the final exact oracle. The script performs no writes. The SanDisk is currently
connected to the Mac again as `disk9`; unplug it before moving it to the Pi.

No changes have been pushed. Existing unrelated aarch64 worktree changes are
not part of this feature and must remain unstaged.
