# exFAT handover — 2026-08-04

## Current state

The `exfat-handler` branch in `../aros-upstream` contains a functional,
explicit-Mountlist, read-only `FATX` handler. On hosted AArch64 it mounts a
macOS-formatted image through `fdsk.device`, enumerates directories, resolves
names case-insensitively, reads FAT-chained and `NoFatChain` files, and refuses
writes. It is not yet an auto-discovered USB/filesystem feature and it is not
a writable filesystem.

The executable specification is [spec.md](spec.md); [README.md](README.md)
is the development and invocation guide. This file is the operational status
snapshot for the next person taking the work.

## Proven, target-side acceptance coverage

| Vector | Evidence |
|---|---|
| baseline/T1a | `graft/exfat-fixture-smoke`: macOS-authored image mounts, lists and byte-compares root and nested files |
| T3 | `graft/exfat-fragmentation-smoke`: raw oracle requires `NoFatChain` clear and at least 8 extents (last run: 11), then target byte-compare |
| T4 | `graft/exfat-fixture-smoke`: raw oracle confirms `NoFatChain` set before target byte-compare |
| T5 | `EXFAT_NAME_ONLY=1 graft/exfat-fixture-smoke`: two distinct 255-unit names open and read by full name through DOS |
| T5b | unmappable UTF-16 name displays as `_`; its displayed spelling does not resolve |
| T6 | mixed-case open is part of the baseline fixture |
| T6b | two distinct names sharing the actual on-disk 16-bit `NameHash` resolve to their distinct contents |
| T7 | `graft/exfat-enumeration-smoke`: exactly 10,000 fixture entries plus target `TOTAL:` completion line |
| T9 | paired clean control mounts; boot-checksum corruption refuses |
| T9b | paired clean control mounts; broken primary / intact backup refuses |
| T10 | paired clean control mounts; `NumberOfFats == 2` refuses as TexFAT |
| T13a/T14–T16 | host bounds/cache/geometry suites in `hosted/exfat-tests/build.sh` |
| T13c/T13d | `graft/exfat-fdsk64-smoke`: raw NSD and CRT paths proven around 4 GiB |

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
  completion.
- T3 and T7 must use different images. T7 directory allocation consumes the
  holes that T3 relies on, yielding a false contiguous-file pass.
- Fixture images use dedicated fdsk units and scripts refuse to overwrite an
  existing image or Mountlist. They clean up their own Unit file and driver.
- The corruption runner supports one case per process for constrained runners:
  `EXFAT_CORRUPT_CASE=texfat ./graft/exfat-corruption-smoke`. Its default is
  the full T9/T9b/T10 batch.

## Still to do

### Finish Phase 1 acceptance

1. Wire the existing `set-checksum` mutation into T11: clean sibling mounts;
   corrupt file is invisible while unrelated entries remain enumerable.
2. Add corruptor mutations and target checks for T11b (overlong
   `SecondaryCount`) and T11c (unknown critical primary plus benign control).
3. Build the sparse, sentinel-backed fixture for T2, T8 and T13b. For T2,
   `ValidDataLength` must equal `DataLength` and boundary clusters must contain
   distinct sentinels; zero tails are reserved for T8. Assert `du` remains far
   below logical `ls -l` size so a formatter cannot silently densify the image.
4. Add the remaining malformed boot/metadata cases from the acceptance table
   where a host-only unit test is insufficient.
5. Obtain a genuine m68k AROS toolchain and run a full exFAT source compile;
   the current m68k gate proves headers and byte-load code generation only.

### Phase 2 and later

- Writable files/metadata, with external `fsck_exfat` after every mutation and
  torn-write/power-loss fault injection.
- Formatter, then safe shared exFAT content probing for partition and USB
  mass-storage paths. Do not add probing only under `rom/partition`.
- Measure cache sizing and performance after correctness, not before.

## Useful commands

```sh
cd /Users/jkn/Source/aros-aarch64
hosted/exfat-tests/build.sh
./graft/exfat-fixture-smoke
EXFAT_NAME_ONLY=1 ./graft/exfat-fixture-smoke
./graft/exfat-fragmentation-smoke
./graft/exfat-enumeration-smoke
EXFAT_CORRUPT_CASE=boot-checksum ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=primary-bad-backup-good ./graft/exfat-corruption-smoke
EXFAT_CORRUPT_CASE=texfat ./graft/exfat-corruption-smoke
./graft/exfat-fdsk64-smoke
```

No changes have been pushed. Existing unrelated aarch64 worktree changes are
not part of this feature and must remain unstaged.
