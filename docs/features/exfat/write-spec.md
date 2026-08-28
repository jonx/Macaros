# exFAT writable-phase design

This is the mutation contract for Phase 2. It extends [spec.md](spec.md) and
follows the exFAT 1.00 [recommended write ordering](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification#81-recommended-write-ordering).
The supported format remains one FAT and one allocation bitmap; TexFAT is not
silently treated as ordinary exFAT.

## Safety boundary

- A volume mounted with `VolumeDirty` already set is readable but not writable.
  The handler has no repair engine and therefore is not allowed to clear a
  pre-existing dirty indication.
- A device that refuses writes is a separate condition from a dirty volume, and
  is asked about once per medium with `TD_PROTSTATUS`, so a swapped-in disk gets
  its own answer. Such a volume reads normally, reports `ID_WRITE_PROTECTED`,
  and refuses every mutation with `ERROR_DISK_WRITE_PROTECTED`: the medium is
  intact and complete, it simply may not be changed. That is a different
  statement from a dirty volume, whose own consistency is unproven and which
  keeps the stricter `ERROR_DISK_NOT_VALIDATED` contract under which even
  clearing the flag is refused. A device that cannot answer the command is
  treated as writable; a write that cannot be done still fails on its own.
- Before the first metadata change in an operation, clear `ClearToZero`, set
  `VolumeDirty` in the main boot sector, and force it to the device with
  `CMD_UPDATE`. These flag bytes are excluded from the boot checksum. The
  backup boot sector's flags are stale by definition and are not rewritten by
  ordinary metadata transactions.
- Clear `VolumeDirty` only after every ordered metadata write and a final
  device update succeeds. Any error leaves it set.
- If allocation planning or another preflight check fails after the durable
  dirty barrier but before any data, FAT, bitmap or directory state changes,
  restore and flush the exact original boot flags. Once a real mutation has
  begun, no rollback is claimed and any error leaves the volume dirty.
- All sector and byte positions remain `UQUAD`; on-disk little-endian fields
  are read and written byte-wise so 68000 alignment and host endianness are
  irrelevant.
- A cache write or flush error is returned to DOS. It must never be converted
  into a successful short mutation.
- Cache dirty state is tracked per sector. A 32-sector cache line must not
  collapse boot/FAT/bitmap/directory stages into one physical write.

## Allocation and publication order

Creating or extending an allocation uses this order:

1. set and flush `VolumeDirty`;
2. initialise new data without publishing it through `ValidDataLength`;
3. write the FAT chain when one is required;
4. set and flush allocation-bitmap bits;
5. publish the final directory entry set, including checksum, lengths and
   first cluster;
6. flush and clear `VolumeDirty`.

Deleting or shrinking reverses the ownership boundary:

1. set and flush `VolumeDirty`;
2. remove or shorten the directory entry set so freed clusters are no longer
   reachable;
3. update the FAT when one describes the allocation;
4. clear allocation-bitmap bits;
5. flush and clear `VolumeDirty`.

The bitmap is authoritative for allocation. New contiguous allocations use
`NoFatChain`. If a contiguous stream cannot grow in place, the handler writes
a complete FAT chain for its old and new clusters before clearing
`NoFatChain` in the published stream entry.

`ValidDataLength` advances only after the corresponding bytes have reached the
cache and their allocation is owned by the file. Gaps and newly exposed bytes
must read as zero; stale cluster contents must never become user-visible.

## Directory updates

- Names are validated and converted to UTF-16 before allocation. Lookup uses
  the volume up-case table and rejects a case-insensitive duplicate.
- A new set needs one primary, exactly one stream extension and enough name
  entries for all code units. It is assembled off-disk with its final name
  hash and set checksum.
- Secondaries are written before the in-use primary. Until that final primary
  write, interruption leaves no reachable new file.
- Deletion clears the primary's in-use bit before releasing any allocation.
- Unknown benign secondary entries are preserved on ordinary metadata updates.
  Deletion frees any allocation they own as required by the base format.
- Directory growth follows the same allocation ordering as file growth and is
  capped at the exFAT 256 MiB directory limit.

## DOS surface

The implementation order is deliberately incremental but each merged step is
internally consistent:

1. `FINDUPDATE`, in-range `WRITE`, flush-on-`END`;
2. create/`FINDOUTPUT`, append and 32/64-bit size changes;
3. delete, create directory and rename;
4. protection, date, comment policy and volume relabel.

Exclusive write locks must conflict with all other locks on the same canonical
object. Read-only attributes return the DOS write-protection error. `Info`
reports a validated disk only when the volume is clean and reports live free
space from the in-memory bitmap.

## Acceptance gates

- Host tests cover little-endian writers, bitmap transitions, checksums, name
  hashes, allocation planning and every overflow boundary under ASan/UBSan.
- Every target mutation is followed by host `fsck_exfat -n` and a byte-level
  comparison after detach/remount.
- The corpus covers 512- and 4096-byte sectors, contiguous and FAT-chained
  files, zero-length files, directory growth, full-disk rollback, 4 GiB
  boundaries and case-insensitive collisions.
- `graft/exfat-full-smoke` fills the allocation bitmap completely and proves
  that allocation-changing resize and directory creation return
  `ERROR_DISK_FULL` without changing content, reachability or clean flags;
  zero-length create/delete still works. The fixture mutation probe rejects
  case-folded create and rename collisions while preserving the source.
- `graft/exfat-writeprotect-smoke` mounts a volume whose device refuses writes
  and proves that reads and `Info` are unaffected while update handles, create,
  delete, rename, directory creation, protection changes and relabel are all
  refused as write-protected, leaving the image byte-exact.
- Deterministic failpoints stop after every ordered flush. Each resulting
  image must either contain the old state or the new state, or remain dirty
  and repairable without exposing freed or uninitialised data.
- The complete handler is compiled as an AROS module for AArch64 and m68k on
  every writable milestone.
