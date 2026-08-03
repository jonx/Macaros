# dos64-packets — the 64-bit DOS packet ABI in AROS

> Status: **Phase 0, in progress.** Branch `dos64-packets` on `../aros-upstream`,
> off `aarch64-darwin-graft`. Contains no exFAT code or assumptions.
>
> `dos64.library` has been taken from master (`7cb141bdfd`) and its 32-bit
> `DosPacket64` field widths corrected against the ABI (`d17e88f4b3`). The full
> upstream merge is deferred: it produces 26 conflicted files, two of which are
> design divergences over the console input handler and the emul-handler
> notification scheme, and needs its own session with a boot verification.

Files above 4 GB are unreachable through `dos.library` on every AROS filesystem. This
is the prerequisite for [exfat](../exfat/README.md), and it independently unblocks
SFS's 4 GB file ceiling.

Scope is deliberately narrow: **verify and publish the `DosPacket64` layout, add safe
allocation/send/reply plumbing, and bring existing consumers onto the published
ABI.** No new packet IDs. No new LVOs.

## What is and is not already present

The four action constants are **already public** in
[`compiler/include/dos/dosextens.h:610`](../../../../aros-upstream/compiler/include/dos/dosextens.h),
under the comment `/* AmigaOS 4 (tm) compatable Extension(s) */`, present on
`aarch64-darwin-graft` and attributed to the repository's initial import. Publishing
them is **not** part of this work, and the `#if defined(ACTION_…)` guards in the NTFS
handler and in `List` are therefore already satisfied: that code compiles and runs
today.

What is missing:

- A **verified `struct DosPacket64` layout**. Nothing in the tree defines one.
- **Plumbing.** `rom/dos/` contains zero references to any of the four actions, so
  nothing in `dos.library` allocates, sends, waits on or replies to these packets.
- **Correct semantics in the existing consumers.** See below.

## The published ABI

Provenance: **`[PUB]`**, `dos.library` packet autodocs, `ACTION_*64` sections.
Source: <https://wiki.amigaos.net/amiga/autodocs/dos.dospackets.doc.txt>

| | 8001 CHANGE_FILE_POSITION64 | 8003 CHANGE_FILE_SIZE64 | 8002 GET_FILE_POSITION64 | 8004 GET_FILE_SIZE64 |
|---|---|---|---|---|
| `dp_Arg1` | `(BPTR)` `fh_Arg1` | `(BPTR)` `fh_Arg1` | `(BPTR)` `fh_Arg1` | `(BPTR)` `fh_Arg1` |
| `dp_Arg2` | `(int64)` position | `(int64)` size | `(int64)` unused, 0 | `(int64)` unused, 0 |
| `dp_Arg3` | `(int32)` mode | `(int32)` mode | **`FileHandle *`** | **`FileHandle *`** |
| `dp_Arg4` | `FileHandle *` | `FileHandle *` | **`(int32)` 0, sentinel** | **`(int32)` 0, sentinel** |
| `dp_Arg5` | `(int64)` 0, sentinel | `(int64)` 0, sentinel | unused | unused |
| `RESULT1` | `(int64)` `-1LL` ok, `0LL` fail | `(int64)` `-1LL` ok, `0LL` fail | `(int64)` position, `-1LL` fail | `(int64)` size, `-1LL` fail |
| `RESULT2` | `(int32)` code if `0LL` | `(int32)` code if `0LL` | `(int32)` code if `-1LL` | `(int32)` code if `-1LL` |

`mode` is `OFFSET_BEGINNING` / `OFFSET_CURRENT` / `OFFSET_END`.

### Three traps in that table

**1. The argument layout is not uniform across the four actions.** The GET pair has no
`mode` argument, so the file handle and its validation sentinel shift down one slot:
handle in `dp_Arg3` and sentinel in `dp_Arg4`, against `dp_Arg4` / `dp_Arg5` for the
CHANGE pair. An implementation that assumes one layout for all four validates the
wrong field on half of them.

**2. `RESULT1 == -1LL` means the opposite thing in each pair.** For the CHANGE pair it
is success (`DOSTRUE` widened). For the GET pair it is failure. A shared error check
is exactly backwards on half the actions.

**3. `RESULT1` is a value, not a pointer.** See the correction below; the tree
currently assumes otherwise.

### The sentinel

The zero in the sentinel slot is what licenses the handler to trust the `FileHandle`
pointer. Per the autodoc, if it is non-zero the caller is treated as legacy and only
`dp_Arg1` is used. AROS must honour this both when sending and when receiving.

## Correction required: the existing AROS convention is incompatible

There is a live, self-consistent, **AROS-private** protocol in the tree today, using
the OS4 action numbers with different semantics.

The NTFS handler answers `ACTION_GET_FILE_SIZE64` by returning **the address of its own
size field** (`workbench/fs/ntfs/packet.c`):

```c
if ((fl->entry) && (fl->gl))
    res = (IPTR)&fl->gl->size;
```

and `List` consumes it as a pointer (`workbench/c/List.c`):

```c
UQUAD *size_ptr = (UQUAD *)DoPkt(..., ACTION_GET_FILE_SIZE64, (IPTR)flock, 0, 0, 0, 0);
if (size_ptr)
    size = *size_ptr;
```

Three problems, independent of each other:

1. **It contradicts the published ABI.** `RESULT1` is specified as the `int64` size
   itself, `-1LL` on failure. A conforming handler returning a real size would have
   `List` dereference that number as an address.
2. **It leaks a raw interior pointer across a process boundary**, into the handler's
   own lock structure, with no lifetime guarantee. This only appears to work because
   AROS has no memory protection.
3. **`List` passes a lock where the ABI specifies a file handle's `fh_Arg1`**, and
   supplies neither the `FileHandle` in `dp_Arg3` nor the sentinel in `dp_Arg4`.

Keeping this would cement an AROS-only protocol under OS4's identifiers, which is
precisely what byte-level compatibility is meant to prevent. Both sides are therefore
corrected to the published ABI as part of Phase 0. This is a **deliberate behaviour
change to existing working code**, not a review of dead paths, and it is gated on the
regression pass.

## Why the pfs3 definitions cannot be adopted

pfs3's private `struct DosPacket64OS4` is disqualified as a reference:

- Its own comment states it is untested and that the logic was copied from a
  third-party GPL emulator. Under [CLEANROOM.md](../CLEANROOM.md) that is not a source
  we build on, independently of correctness.
- A second comment concedes the struct is "not real", an approximation.
- Its layout contradicts the published autodoc: `dp_Arg1` is declared as a 64-bit
  quantity where the autodoc specifies a `BPTR`.
- `dp_Link` and `dp_Port` are `ULONG`, so it is 32-bit only and wrong on `x86_64` and
  `aarch64`.
- It never checks the sentinel, so it never validates the `FileHandle` path.

pfs3 is corrected against the new public definition later, as a separate reviewed
consumer.

## Layout strategy

**32-bit targets: byte-level OS4 compatibility.** Packet actions are a wire ABI
between independently built callers and handlers. Labelling actions `8001`–`8004`
OS4-compatible while inventing a different layout would create an AROS-only protocol
under OS4's identifiers. The official SDK header is public provenance, so the offsets
are recorded rather than guessed.

**64-bit targets: an explicit AROS-native form**, pinned with static assertions. It may
turn out to alias or collapse to `struct DosPacket`, but that is to be verified against
allocation, `SendPkt`, `ReplyPkt` and common-prefix requirements, not assumed.

## Complete producer/consumer inventory

Every site in the tree touching any of the four action IDs, by name or by raw number.
Searched with a UTF-8-agnostic scan (see the tooling note at the end; a plain tree grep
misses a fifth of this repository).

| Site | Role |
|---|---|
| `compiler/include/dos/dosextens.h:611-614` | the public definitions |
| `rom/filesys/pfs3/fs/struct.h:1020-1025` | redundant private redefinition behind `#ifndef`; delete once verified |
| `rom/filesys/pfs3/fs/dostohandlerinterface.c:307-317, 448-451` | handler, behind `EXTENDED_PACKETS_OS4` |
| `workbench/fs/ntfs/packet.c:369-456` | handler, all four actions, pointer-return convention |
| `workbench/c/List.c:573-581` | sole consumer, pointer-dereference convention |
| `rom/dos/` | **nothing.** No allocation, send, wait or reply plumbing exists |

All other matches on `8001`-`8004` in the tree are coincidental (library pragma
offsets, `xadmaster` client IDs, an ADFlib constant, a compiler warning number, an SVG
path). Nothing else produces or consumes these packets.

That is the entire Phase 0 blast radius: two handlers, one consumer, one definition
site, one redundant redefinition.

## RESOLVED: the confirmed layout, and upstream already has a dos64 module

**The oracle is no longer needed.** The official SDK header was located vendored in a
public toolchain repository, and three independent sources agree exactly.

`[PUB]`, official OS4 SDK `dos/dosextens.h`. Its own comments state *"Only dp_Type
packets between 8000-8999 range use this structure"* and *"#pragma pack() used here to
obtain default alignment padding"*, i.e. natural alignment:

| field | type | offset | size |
|---|---|---|---|
| `dp_Link` | `struct Message *` | 0 | 4 |
| `dp_Port` | `struct MsgPort *` | 4 | 4 |
| `dp_Type` | `int32` | 8 | 4 |
| `dp_Res0` | `int32` | 12 | 4 |
| `dp_Res2` | `int32` | 16 | 4 |
| `dp_Res1` | `int64` | 24 | 8 |
| `dp_Arg1` | **`int32`** | 32 | **4** |
| `dp_Arg2` | `int64` | 40 | 8 |
| `dp_Arg3` | `int32` | 48 | 4 |
| `dp_Arg4` | `int32` | 52 | 4 |
| `dp_Arg5` | **`int64`** | 56 | **8** |
| | | **total** | **64** |

Corroborated independently by a STABS debug string in `adtools/db101`
(`DosPacket64:T(32,8)=s64…`), which is the *compiler's own emission* of the real SDK
struct and reproduces every offset and width above, and by the Free Pascal
transcription's documented 64-byte total. The earlier `{$PACKRECORDS 4}` arithmetic
that yielded 56 is confirmed as the transcription error.

### Upstream AROS added `rom/dos64/` on 2026-07-19

Two days after this branch's merge-base (2026-07-17), which is why it was not visible
here. It is much larger than this Phase 0's scope: `read64`, `write64`, `seek64`,
`examine64`, `exnext64`, `examinefh64`, `info64`, `lockrecord64`, `setfilesize64`,
`allocdosobject64`, plus `posixc` and `stdc` integration.

**Phase 0 is therefore no longer a design task. It is: merge upstream, then correct
two width errors in `compiler/include/dos/dos64.h`.**

| field | official OS4 | upstream AROS | consequence |
|---|---|---|---|
| `dp_Arg1` | `int32`, 4 bytes | `QUAD`, 8 bytes | On **big-endian 32-bit** (m68k) the meaningful value lands at bytes 36-39; a conforming reader takes `int32` at 32-35 and gets the high half, i.e. zero |
| `dp_Arg5` | `int64`, 8 bytes | `ULONG`, 4 bytes | `dp_Arg5` is the **CHANGE-pair sentinel**. Writing 4 bytes leaves 60-63 uninitialised, so a handler testing a 64-bit zero can see garbage and refuse to validate the `FileHandle` in `dp_Arg4`, silently falling back to the legacy path |

Field *offsets* coincide by luck of alignment, so the errors are invisible to a
`sizeof` check and to any offset-only assertion. Only the widths differ. Note the
`dp_Arg1` error is the same one pfs3's disqualified struct made.

Upstream also requires the originator to additionally store `fh_Arg1` in the *standard*
packet's `dp_Arg1` at offset 20, which in `DosPacket64` is padding. That is additive
rather than corrupting, but it is an AROS-private side channel an OS4 handler will
never read.

## Earlier corroboration: the Free Pascal transcription

**Superseded by the SDK header above. Retained for the record.**

Free Pascal's `os4units` transcribes `TDosPacket64`
([source](https://gitlab.com/freepascal.org/fpc/source/-/blob/main/packages/os4units/src/amigados.pas#L1016))
with `dp_Res1`, `dp_Arg2` and `dp_Arg5` 64-bit; `dp_Res0`, `dp_Res2`, `dp_Arg1`,
`dp_Arg3` and `dp_Arg4` 32-bit; `DP64_INIT = -3`; and a comment asserting the structure
is **64 bytes**.

The unit applies `{$PACKRECORDS 4}`, which caps field alignment at four bytes. That
contradicts its own comment, and the arithmetic shows why:

| | `{$PACKRECORDS 4}` | natural 8-byte alignment |
|---|---|---|
| `dp_Link` | 0 | 0 |
| `dp_Port` | 4 | 4 |
| `dp_Type` | 8 | 8 |
| `dp_Res0` | 12 | 12 |
| `dp_Res2` | 16 | 16 |
| `dp_Res1` (8) | 20 | **24** (4 pad) |
| `dp_Arg1` | 28 | 32 |
| `dp_Arg2` (8) | 32 | **40** (4 pad) |
| `dp_Arg3` | 40 | 48 |
| `dp_Arg4` | 44 | 52 |
| `dp_Arg5` (8) | 48 | 56 |
| **total** | **56** | **64** |

The natural-alignment column reproduces the documented 64 bytes exactly. PowerPC 32-bit
aligns 64-bit scalars to 8, so the likeliest reading is that **the layout is 64 bytes
and the `{$PACKRECORDS 4}` directive is the transcription error**, not the comment.

This is a prediction for the oracle to confirm or refute. It is not evidence, and it is
not implemented against. Note also that the field *order* above is assumed; only the
widths were transcribed, so the order needs confirming too.

If it holds, the common prefix with `struct DosPacket` runs through `dp_Res2`
(offsets 0-19), with `dp_Res0` occupying the slot where `DosPacket` has `dp_Res1`.

### `DP64_INIT`: CONFIRMED

That prefix suggests a purpose for `DP64_INIT = -3`. A **legacy** handler replies by
writing offset 12, which it believes is `dp_Res1`, and in a `DosPacket64` that is
`dp_Res0`. A **64-bit-aware** handler should leave offset 12 untouched at `-3` and
return through the real 64-bit `dp_Res1`. A caller that pre-seeds `-3` could then read
offset 12 afterwards to tell the two apart.

**Confirmed** by upstream's own implementation in `rom/dos64/dos64_packet.c`, which
states it directly: *"The overlay's dp_Res0 (holding the DP64_INIT marker) shares its
offset with the standard packet's dp_Res1. A handler that understands the 64-bit packet
preserves the marker and answers through the overlay fields; a plain 32-bit handler
answers through the standard fields, overwriting the marker. This is what tells the two
reply forms apart."* An assertion now pins the `dp_Res0` to `dp_Res1` aliasing this
depends on.

## Sequence

1. **BLOCKED, narrowed to exact offsets.** Obtain an ABI oracle rather than the header
   itself: [`abi-oracle.c`](abi-oracle.c) is a self-contained program that anyone with
   the OS4 SDK can compile and run to report field order, sizes, offsets, alignment,
   total size, `sizeof(struct Message)`, the `StandardPacket64` numbers and
   `DP64_INIT`. Only its **output** is needed, so the SDK header is never redistributed.
   The layout is not obtainable remotely: the packet autodocs name `struct DosPacket64`
   in all four action headings but never define it, the Vector-Port wiki page does not
   mention it, and the SDK is served only from a portal requiring registration.
   Deriving the offsets from semantics is out of scope: that is what produced the
   disqualified pfs3 struct.
2. Compile-time offset/size assertions for the 32-bit ABI.
3. Specify the 64-bit AROS-native layout separately, with its own assertions.
4. Prove `AllocDosObject`, message length, `SendPkt`, `WaitPkt`, `ReplyPkt` and freeing
   all use the correct packet size.
5. All four round-trip tests, including both sentinel positions and the opposite
   `-1LL` meanings.

## What proceeds without the oracle

The missing 32-bit offsets do not block the rest. Running in parallel:

- the 64-bit AROS-native semantic implementation;
- the NTFS + `List` protocol correction, as one atomic commit covering both ends;
- Phase 0.5, widening the copied disk/cache layer to a 64-bit sector domain;
- the cleanroom exFAT specification.

**The 32-bit `DosPacket64` is explicitly unavailable until the oracle lands.** Use of it
fails to build rather than silently falling back to a guessed layout.

That unavailability is scoped: it applies only where `DosPacket64` support is
*selected or exercised*. **An ordinary m68k or i386 AROS build must continue to build
and boot exactly as before.** The absence is a compile error for code that opts into
the 64-bit packet path, not a tree-wide breakage.

The existing pointer-return protocol is not retained or extended under the name of
64-bit support. It is an AROS implementation accident and is being removed.

**pfs3 is part of the same atomic cleanup**, not a follow-up. Its private redefinition
in `struct.h` goes, and its extended-packet path is either converted alongside NTFS and
`List` or disabled pending the oracle. What must not survive the commit is a
contradictory third implementation sitting behind `EXTENDED_PACKETS_OS4`.

## Phase 0 gate

**Layout**

- [ ] OS4 layout recorded as `[PUB]` from the oracle, with offset/size assertions (32-bit)
- [ ] 64-bit native layout specified, with its own assertions
- [ ] `DP64_INIT` semantics confirmed, not inferred, before any legacy-detection path
      depends on them
- [ ] Allocation, send, wait, reply and free proven to use the correct packet size

**Semantics**

- [ ] GET results correct **around `2^32`**, and `-1LL` on failure
- [ ] CHANGE returns `-1LL` on **success** and `0LL` on failure
- [ ] GET versus CHANGE argument positions correct: handle and sentinel in
      `dp_Arg3`/`dp_Arg4` for GET, `dp_Arg4`/`dp_Arg5` for CHANGE
- [ ] Sentinel validation exercised, both honoured and violated
- [ ] `List` consumes `RESULT1` as a **value**, and never dereferences it

**Hygiene**

- [ ] 32/64-bit compile matrix
- [ ] Ordinary m68k and i386 builds unaffected: build and boot as before
- [ ] FAT, NTFS and boot regression
- [ ] No residual private action definitions anywhere in the tree
- [ ] No residual pointer-return producers or consumers
- [ ] No new LVOs, no new packet IDs

## Tooling note: a fifth of this tree is invisible to a plain grep

`compiler/include/dos/dosextens.h` is ISO-8859, not UTF-8. The shell `grep` in this
environment is a wrapper around `ugrep -I --ignore-files`, and `-I` classifies any file
containing a non-UTF-8 byte as binary and **skips it silently**, with no warning and a
clean exit status.

A scan of the tree found **4,404 of 21,123 files (21%) are not valid UTF-8**. Those
files are invisible to every wrapped grep, including negative results used to conclude
that something is absent.

This produced a real, consequential error in this work: the four action constants were
twice reported as "not defined in AROS headers" when they have been public since the
initial import.

**For any exhaustive or negative search in this repository, use `command grep` (which
bypasses the wrapper) or a Python scan that decodes with `errors="replace"`.** Treat a
wrapped-grep "no matches" as unproven.
