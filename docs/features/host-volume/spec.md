# Implementation spec — Host volume (a macOS folder as an AROS volume)

> Status: drafting (Role A) · Target: aarch64-darwin hosted · Drafted 2026-06-24
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Clean-room banner

**Role B (implementer): do NOT read WinUAE, FS-UAE, Amiberry, E-UAE/Janus-UAE, any
other UAE-family source, or vAmiga.** The UAE "directory hard drive" (host-dir-as-Amiga-
volume, with portable sidecar metadata and filename escaping) is GPL-2.0 and was read
**only by Role A**. Implement solely from this spec + the approved sources cited by tag:
`[PUB]` Apple framework docs / POSIX / published standards (Unicode normalization forms),
`[AROS]` in-tree AROS headers and the `emul-handler` source (paths given; APL/LGPL —
ours), `[OURS]` this project's spikes (H-series, `graft/*`, `hosted/*`). `[REF-CONFIRM]`
items were sanity-checked against the UAE directory-HD by Role A but are **restated here
with an independent `[PUB]`/`[AROS]`/`[OURS]` justification** — implement from that, not
from any reference. No UAE identifier, file layout, escaping table, or sidecar field
order crosses the wall; only the *fact that a portable sidecar is the right shape* does.

## Scope

This feature is **in progress, not greenfield.** AROS already ships a host-filesystem
handler — `emul-handler` — that splits into a **host-independent portable core** and a
thin **per-host overlay**, and the **POSIX (`all-unix`) overlay already compiles for
darwin-aarch64** in this repo (commit `a68e4c5c`, native 64-bit inodes). So the bulk of
the filesystem logic is reused verbatim; this spec covers only the **Mac-specific new
code** plus the **bring-up wiring** that makes the existing code *run, mount, and round-
trip* on Apple Silicon.

### Already works — reuse verbatim, cite the file (do NOT reimplement)

- **DOS-packet dispatch + volume/lock machinery** — `handlePacket`, `new_volume`, the
  `EmulHandlerMain` process, the `WaitPkt → handlePacket → ReplyPkt` loop.
  `arch/all-hosted/filesys/emul_handler/emul_handler.c` `[AROS]`.
- **AROS-path → host-path string surgery** — `shrink`, `validate`, `append`,
  `nextpart`, `makefilename`. `…/emul_handler/filenames.c`, `emul_handler.c:93`
  `[AROS]`. Pure string code; host-agnostic.
- **The whole `Do*` host-call contract over libc** — `DoOpen/DoClose/DoRead/DoWrite/
  DoSeek/DoMkDir/DoDelete/DoChMod/DoRename/DoSetDate/DoSetSize/DoStatFS/DoExamineEntry/
  DoExamineNext/DoExamineAll/DoReadLink/DoSymLink/DoHardLink/DoRewindDir`, the dlsym'd
  `struct LibCInterface`, the `HostLib_Lock … AROS_HOST_BARRIER … HostLib_Unlock`
  discipline, `prot_u2a`/`prot_a2u`, `timestamp2datestamp`/`datestamp2timestamp`,
  `fixcase`. `arch/all-unix/filesys/emul_handler/{emul_host.c,emul_host_unix.c,
  emul_dir.c,emul_unix.h,unix_hints.h}` `[AROS]`. **This overlay is the template; it is
  Darwin-aware and links `libSystem.dylib`.**
- **The default boot-volume self-mount** — `new_volume(NULL)` falls back to
  `GetCurrentDir()` and names the volume `System:` (`emul_handler.c:559,617,655–663`)
  `[AROS]`. First observable win needs *no* Mountlist.

### New code to write (this spec) — the four hard parts + bring-up

1. **Unicode normalization at the bridge** — normalize BOTH the AROS-side name and every
   `readdir` result to one agreed form before any comparison. APFS is a *bag of bytes*;
   the handler must not rely on the filesystem normalizing. (§Normalization.)
2. **Case handling correctness** on the case-insensitive-default volume, and a guard for
   the case-sensitive-volume edge. (§Case.)
3. **AmigaOS metadata mapping** — file comment + the AmigaOS-only protection bits
   (Script/Archive/Pure/Hold) that POSIX `rwx` can't hold — via a **portable sidecar
   file** (decision + justification below). (§Metadata sidecar.)
4. **Mac path/charset glue** — Latin-1 filename bytes ↔ host UTF-8, layered on the
   existing pure-ASCII `filenames.c` surgery. (§Charset.)
5. **Bring-up wiring** — make mmake select the `all-unix` overlay for the darwin-aarch64
   target; bring up `unixio.hidd`; add the `Mac:` Mountlist entry. (§Build/mount.)

### Out (non-goals, this spec)

- FSEvents live auto-refresh for Wanderer (deferred nicety — re-`dir` re-scans pick up
  new files already; only auto-notify needs `FSEventStreamCreate`).
- Resource-fork / AppleDouble handling (a no-issue on native APFS/HFS+ — xattrs live
  inline; see design.md). The sidecar is text, never a fork.
- A from-scratch filesystem handler, the FSA_/IOFileSys model (the handler is classic
  DOS-packet), or any change to the portable core's packet dispatch.
- Enforcing exclusive locks against the host (upstream doesn't; we keep parity).

## Architecture

```
AROS side (aarch64, AROS crosstools)              Host side (Apple, via libSystem.dylib)
┌─────────────────────────────────────┐          ┌────────────────────────────────────┐
│ emul-handler  (DOS process)         │          │ libSystem.dylib  (dlsym'd by        │
│  portable core  emul_handler.c      │          │  hostlib.resource at host_startup)  │
│   · handlePacket(ACTION_*)          │          │  open/read/write/lseek/stat/...     │
│   · new_volume / locks / examine    │          │  opendir/readdir/seekdir/...        │
│  unix overlay   emul_host*.c        │  HostLib │  chmod/utime/rename/statfs/...       │
│   · Do* over struct LibCInterface ──┼──Lock──► │                                     │
│   · NEW: normalize() at bridge      │ barrier  │  NEW host helpers (same libSystem): │
│   · NEW: sidecar read/write         │ ◄────────┤   getxattr-free; plain file I/O for │
│   · NEW: latin1<->utf8 at edge      │          │   the ".<name>.amimeta" sidecar     │
└─────────────────────────────────────┘          └────────────────────────────────────┘
   AROS DOS packet  ──WaitPkt──►  handler task  ──Do*──►  libc syscall  ──►  macOS FS
```

- The **only new host primitive** is the sidecar's plain-file read/write — and that uses
  the libc functions **already in `struct LibCInterface`** (`open/read/write/close/
  unlink/rename`). No new dlsym symbol, no new dylib, no Cocoa. Normalization and
  charset translation are **pure CPU work inside the handler** (no host call), so they
  need no new interface either.
- Spike-phase: a bare `hosted/hostfs.c` exercises the `Do*` shape + the new bridge code
  in isolation (H-series style), before `dos.library` is up. At graft, the new bridge
  code lands **in the existing overlay files** (it's Mac-specific but POSIX-mechanism),
  guarded so non-Darwin hosts are unaffected.

## The portable AROS contracts this binds to (grounded, `[AROS]`)

Restated from the headers and the live source so Role B needs no GPL anything:

- **Packet loop.** `dp = WaitPkt(); handlePacket(emulbase, dp, DOSBase); ReplyPkt(dp,
  Res1, Res2)` — `emul_handler.c:1147,1215–1219`. `dp->dp_Type` switches on `ACTION_*`
  (`dos/dosextens.h`). This is the H10/H11 switched-task-does-the-host-syscall pattern
  one layer up `[OURS]`.
- **The bytes that flow.** `ACTION_FINDINPUT/FINDOUTPUT/FINDUPDATE` (open), `ACTION_READ/
  WRITE/SEEK/END`, `ACTION_LOCATE_OBJECT/FREE_LOCK/COPY_DIR/PARENT`, `ACTION_EXAMINE_
  OBJECT/EXAMINE_NEXT/EXAMINE_ALL`, `ACTION_CREATE_DIR/DELETE_OBJECT/RENAME_OBJECT`,
  `ACTION_SET_PROTECT/SET_DATE`, `ACTION_INFO/DISK_INFO`, `ACTION_SAME_LOCK`. Each
  carries a BSTR or C name and reaches a `Do*` after `makefilename()` splices it onto
  `fh->hostname`.
- **`struct FileInfoBlock`** (`dos/dos.h`): `fib_FileName` (BSTR len+chars,
  `MAXFILENAMELENGTH`), `fib_Protection`, `fib_Size`, `fib_Date` (`DateStamp`),
  `fib_Comment`, `fib_DirEntryType` (`ST_FILE=-3/ST_ROOT=1/ST_USERDIR=2/ST_SOFTLINK=3`,
  `dosextens.h`), `fib_OwnerUID/GID`. `DoExamineNext` packs these from `lstat`
  (`emul_host.c:1074`); note it currently sets `fib_Comment[0]='\0'` unconditionally
  (`emul_host.c:1119`) — the sidecar hook replaces exactly that line.
- **`struct ExAllData` + the `sizes[]`/`offsetof` levels** `ED_NAME=1 … ED_OWNER=7`
  (`emul_host.c:300`, `dos/exall.h`). `DoExamineEntry`'s fall-through switch fills
  comment at `ED_COMMENT` (currently empties it, `emul_host.c:1016–1018`) — second
  sidecar hook.
- **Volume node.** `MakeDosEntry(volname, DLT_VOLUME)` + `AddDosEntry`, `dol_Task = mp`,
  then `IECLASS_DISKINSERTED` to `input.device` so Wanderer notices
  (`emul_handler.c:655–663`). DOSType `0x454D5500` = `'E','M','U',0`
  (`emul_init.c:61`).
- **Mount string.** `FileSysStartupMsg.fssm_Device` is the BSTR `<VolumeName>:<hostpath>`
  (`emul_handler.c:1193–1197`); `~` resolves via `GetHomeDir` (`emul_host.c:1259`).

## The normalization + case + charset model (the load-bearing constraint)

These four interact and **must be solved as one ordered pipeline**, applied at exactly
the bridge between an AROS name and a host name. Get the order wrong and case-folding
fights normalization. Define a single canonicalisation used everywhere a name is
compared or constructed.

### Normalization — `[PUB]` Unicode, `[REF-CONFIRM]` UAE directory-HD

**The problem, from a public source (no reference needed to state it).** macOS APFS is
documented as a **bag of bytes**: it stores and returns from `readdir` *exactly* the
byte sequence that was written — NFC **or** NFD, whichever the writer used — and can
legitimately hold two directory entries that differ only by normalization form. macOS
layers a normalization-*insensitive* lookup on top, but its placement (kernel VFS vs
userspace) is **UNVERIFIED** and the handler must not depend on it. `[PUB]` Unicode
Annex #15 (Normalization Forms) defines NFC (canonical composition) and NFD (canonical
decomposition); they are deterministic, lossless transforms between which any host name
can be moved. (This contradicts the old "HFS+ always returns NFD" folklore; that was
HFS+, not APFS — see design.md.)

**Why AROS can't ignore it `[AROS]`.** The existing `fixcase` (`emul_host.c:224`)
re-scans the parent dir and compares with `Stricmp` (ASCII case-fold only). `Stricmp`
does **not** reconcile NFC↔NFD: an accented name typed in AROS (NFC) will not match the
NFD bytes `readdir` may return for the same file, so the file appears missing on
`lstat` and `fixcase` fails to find it. Pure-ASCII names are unaffected (NFC==NFD for
ASCII), which is why the overlay works today for ASCII and the bug is latent.

**Requirement R-NORM (`[REF-CONFIRM]`, restated `[PUB]`).** The handler **owns**
normalization. Define the project canonical form **NFC** for all in-handler name
comparison and host-name construction. At the bridge:

- **AROS → host (lookup/create).** Before `lstat`/`open`/`mkdir`/`rename`, transform the
  spliced host path component bytes (after Latin-1→UTF-8, §Charset) to **NFC**. This is
  the form the handler will *write* for new files, so the host dir converges on NFC.
- **host → AROS (enumerate).** Each `readdir` `d_name` is transformed to **NFC** before
  it is compared (`fixcase`'s `Stricmp`, `ACTION_SAME_LOCK`) and before it is handed
  back to AROS as `fib_FileName`/`ed_Name` (then UTF-8→Latin-1, §Charset).
- **Comparison rule.** `fixcase`'s match becomes "normalize `d_name` to NFC, then
  `Stricmp` against the (already-NFC) target". Equivalently a small `nocase_norm_cmp`
  helper. Implement the NFC transform as a **standalone in-handler function** over the
  Unicode decomposition + canonical-ordering + composition algorithm `[PUB]` — *not*
  via any Apple `CFStringNormalize`/`precompose` call (keeps it host-call-free, inside
  the `HostLib_Lock` region is unnecessary, and avoids a CoreFoundation dependency in
  the handler). A compact decomposition table is a data deliverable (UNVERIFIED size;
  the BMP Latin ranges suffice for the Latin-1 round-trip — see Charset). Role B may
  start with a table covering Latin-1-mappable code points and widen later.

*Independent justification:* the requirement stands entirely on `[PUB]` Unicode UAX #15
+ the documented APFS byte behaviour + the observed `[AROS]` `Stricmp`/`fixcase` gap.
The UAE directory-HD only **confirmed** that a host-dir bridge must normalize itself
rather than trust the FS; it contributes no algorithm, table, or code shape here.

**Spike status vs production contract (`[OURS]` — a production TODO, not a spike gap).**
The NFC decomposition/composition table in the spike (`hosted/hostvolume/hv_norm.c`)
covers only the **Latin range** — the Latin-1 Supplement (U+00C0..U+00FF) and the commonly
typed Latin Extended-A (U+0100..U+017F). That is **sufficient and correct for the spike**:
its purpose is the Latin-1 round-trip, and any code point absent from the table is treated
as its own decomposition (so ASCII and unhandled scripts pass through unchanged, which is
the right NFC result for them). **Production must not ship this partial table for arbitrary
host names**: it must **generate the full normalization tables (canonical decomposition,
combining classes, composition exclusions, and the Hangul algorithmic range) from the
Unicode Character Database (UCD)** — `UnicodeData.txt`, `CompositionExclusions.txt`,
`DerivedNormalizationProps.txt` — so a file named in Greek, Cyrillic, CJK, or with arbitrary
BMP/astral combining marks normalizes correctly. This is a flagged **production TODO**; the
spike's narrow table is an intentional, documented scope boundary, not a correctness gap.

### Case — `[AROS]` + `[PUB]`

- **Insensitive default (the common case).** The Unix overlay already compiles with
  `#define NO_CASE_SENSITIVITY` (`emul_host_unix.c:35`, `emul_host.c:46`), enabling
  `fixcase`. This is correct for the APFS default (case-insensitive, case-preserving)
  and for AmigaOS semantics (case-insensitive, case-preserving). **Keep it.** R-NORM
  folds the normalization step *into* the same `Stricmp` path, so case + form are
  reconciled together.
- **Sensitive-volume guard (R-CASE).** On a case-*sensitive* APFS volume, `fixcase`
  could match a different-case sibling (wrong), and `ACTION_SAME_LOCK` compares host
  paths with `strcasecmp` (`emul_handler.c:797`) — wrong when the host distinguishes
  case. **Requirement:** detect the mounted volume's case-sensitivity once at mount
  (`pathconf(path, _PC_CASE_SENSITIVE)` `[PUB]`, queried through a libc handle; **add
  `pathconf` to `libcSymbols`** if used) and, when sensitive, skip the `fixcase`
  re-scan and use a case-*sensitive* compare in `SAME_LOCK`. Default-volume behaviour is
  unchanged. **UNVERIFIED:** whether `_PC_CASE_SENSITIVE` is reliable across APFS
  variants; fallback is a mount option `CASE=SENSITIVE|INSENSITIVE` in the Mountlist.

### Charset — `[PUB]` + `[AROS]`

AmigaOS/AROS filenames are a byte string conventionally interpreted as **ISO-8859-1
(Latin-1)**; macOS paths are **UTF-8**. The existing `filenames.c` surgery is pure byte
work and is charset-agnostic for ASCII. New code:

- **Requirement R-CHARSET.** At the *outer* edge of the bridge (the same point R-NORM
  acts), translate **AROS Latin-1 bytes → UTF-8** on the way to the host and **UTF-8 →
  Latin-1** on the way back. Latin-1 → UTF-8 is total (every byte maps). UTF-8 → Latin-1
  is partial: a host name with a code point outside U+0000..U+00FF has no Latin-1 byte.
  For those, **escape**: emit a reversible ASCII escape for the un-mappable code point
  so the name still appears (and round-trips) in AROS.
- **Escape scheme (R-ESCAPE, `[REF-CONFIRM]`, restated `[PUB]`/`[OURS]`).** Use a single
  reserved ASCII marker byte followed by the hex of the offending code point (our own
  ASCII convention, e.g. `%uXXXX`), chosen so it is (a) reversible, (b) valid in both an
  AROS and a POSIX name, (c) collision-free by also escaping the marker byte itself, and
  (d) symmetric — an AROS name already containing the marker is escaped on the way out
  so a host round-trip reproduces it. *Independent justification:* this is the standard
  "reversible percent/hex escape of un-representable code points" `[PUB]`; design.md's
  web note records that the UAE family escapes host-illegal characters for portability —
  that only **confirms** an escape is needed; the marker byte, the hex format, and the
  self-escaping rule are **ours**, not copied. Role B picks the exact marker and writes
  the table; no UAE escaping table is used.
- **Reversibility (load-bearing — the escape MUST decode).** The escape is only useful if
  it genuinely round-trips: the AROS→host direction (`hv_latin1_to_utf8`) **decodes**
  `%uXXXX` back to the real code point (emitted as UTF-8), and decodes `%u0025`→`%`. A
  literal-only Latin-1→UTF-8 mapping (without the decode) would re-emit the escape text
  verbatim and **lose/duplicate the file** (`fœ.txt` → AROS `f%u0153.txt` → host
  `f%u0153.txt`, a different name). The decode is what closes the loop.
- **Fixed-width, self-delimiting escape (`[OURS]`).** The escape is **fixed width**:
  `%u` + **exactly 4** hex for a BMP code point (≤ U+FFFF), `%U` + **exactly 6** hex for an
  astral one. A variable-length hex run is **wrong** — a hex digit in the following text
  (the `d` in `%u0025done`) would bleed into the escape and corrupt the decode. Fixed width
  makes the escape boundary independent of what follows.
- **Ambiguity policy (`[OURS]`).** Encode (host→AROS) **always** escapes a literal `%` as
  `%u0025`, so any `%u`+4hex (or `%U`+6hex) in an AROS name is **unambiguously** one of our
  escapes and is decoded; a bare `%` **not** in that exact shape is a literal `%` and passes
  through unchanged. **Residual ambiguity (documented, accepted for the spike):** a user who
  literally types the exact sequence `%uXXXX`/`%UXXXXXX` in an AROS name will have it decoded
  to the corresponding code point on the way to the host — i.e. the marker convention
  reserves those two exact textual shapes. This is the single, narrow, documented exception;
  ordinary names (including stray `%`) are unaffected.
- **Ordering (load-bearing).** The pipeline is strictly:
  - AROS→host: `Latin1→UTF-8` → `NFC-normalize` → splice/`shrink` → host call.
  - host→AROS: `readdir` bytes → `NFC-normalize` → compare / `UTF-8→Latin1 (+escape)`
    → AROS. Normalize on the UTF-8 side (Unicode operates on code points, not Latin-1
    bytes). Never case-fold before normalizing.

## Metadata mapping — the decision: a portable sidecar (`[REF-CONFIRM]`, restated)

**What needs a home.** Two AROS metadata items have no faithful POSIX slot:
(1) the **file comment** (`fib_Comment`/`ed_Comment`) — `ACTION_SET_COMMENT` is in the
handler's explicit "not supported yet" FIXME (`emul_handler.c:1132–1133`) and Examine
hard-codes the comment empty (`emul_host.c:1119`, `:1016`); and (2) the **AmigaOS-only
protection bits** — `FIBF_SCRIPT` is already squeezed into `S_ISVTX` (`emul_host.c:121,
157`), but **Archive/Pure/Hold/Reserved** have no POSIX `rwx` equivalent and are lost
today. The date is 1-second-precision via `mtime` (Amiga ticks are 1/50 s — sub-second
is lost) — acceptable, document it.

**Decision: a portable sidecar file, not a macOS xattr.** Store the comment + the full
AROS protection word + (optionally) the high-precision date in a small **text sidecar**
`.<name>.amimeta` alongside each file, **written only when a value is non-default**
(plain files with empty comment and plain `rwx`-only protection get **no** sidecar — the
host dir stays clean). On Examine, if the sidecar exists, its values override the
`lstat`-derived comment/protection; the `rwx` bits still come from `st_mode` so POSIX
tools and the sidecar agree on read/write/execute.

**Why sidecar over xattr (the justification).**
- **Portability `[PUB]`.** A sidecar is an ordinary file: the host folder stays copyable
  to FAT/SMB/zip/another OS with metadata intact. macOS xattrs (`com.apple.*` /
  `kMDItemFinderComment`) are dropped by most non-APFS transports and by many archivers.
  The project thesis is "a real Mac folder you can drag files in and out of" — keeping
  it a plain, portable directory wins.
- **No new host dependency `[AROS]`.** The sidecar is read/written with the libc calls
  **already in `LibCInterface`** (`open/read/write/close/unlink/rename`). An xattr path
  would add `getxattr`/`setxattr`/`removexattr` to `libcSymbols` (Darwin-only symbols),
  a `pathconf(_PC_XATTR_SIZE_BITS)` capability check, and a new code path — more surface,
  Mac-locked.
- **Atomicity `[PUB]`.** Sidecar writes are write-temp-then-`rename` (POSIX atomic
  replace), reusing the overlay's `rename`.

*`[REF-CONFIRM]` and its wall:* design.md's web research records that the UAE family
solved this exact comment/extra-protection/precise-date problem with a portable sidecar
(FS-UAE `.uaem` text file; WinUAE `UAEFSDB` index) rather than host xattrs — that
**confirms the sidecar is the right shape and that xattrs are the trap to avoid.** It
contributes nothing else: our **filename** (`.<name>.amimeta`, dotfile-hidden, per-file,
not a `UAEFSDB`-style index), our **format** (see below), our **field set**, and our
**"omit when default"** rule are restated independently below and owe no expression to
any reference.

**Sidecar format (R-SIDECAR, ours).** A tiny line-oriented ASCII record, self-describing,
forward-compatible:
```
amimeta 1
prot 0x<hex of the full AROS fib_Protection word>
comment <UTF-8 bytes of the AROS comment, percent-escaped past printable ASCII>
```
- One file per data file, name `.<basename>.amimeta` in the same host dir (so it sorts
  next to its file and is hidden as a dotfile).
- **Excluded from enumeration:** `DoExamineNext`/`DoExamineAll`/`ReadDir` must skip names
  matching `.*.amimeta` (extend the existing `is_special_dir` skip in `emul_dir.c:18`
  with an `.amimeta`-suffix skip) so sidecars never appear as AROS files.
- **Comment hook:** replace `fib_Comment[0]='\0'` (`emul_host.c:1119`) and the
  `ED_COMMENT` empty-string (`emul_host.c:1016–1018`) with "read sidecar; if present and
  has `comment`, copy it; else empty".
- **Protection hook:** after `prot_u2a(st.st_mode)` sets `fib_Protection`/`ed_Prot`, if
  the sidecar has `prot`, OR-in the AmigaOS-only bits (Script/Archive/Pure/Hold) from the
  sidecar word while keeping `rwx` from `st_mode` (so chmod via POSIX still shows).
- **Write hooks:** wire `ACTION_SET_COMMENT` (remove it from the FIXME list, add a
  `DoSetComment` that writes/updates/removes the sidecar) and extend `DoChMod`/
  `ACTION_SET_PROTECT` to persist the non-`rwx` bits to the sidecar (and delete the
  sidecar when the comment is empty *and* only `rwx` bits remain — keeps dirs clean).
- **Rename/Delete coupling:** `DoRename` and `DoDelete` must rename/delete the matching
  sidecar alongside the data file (atomic enough for the spike; document the window).

## AROS-side binding & bring-up (`[AROS]`, grounded in design.md)

The new code above lives in the overlay; these are the wiring tasks that make it run.

- **Build wiring (gap #1).** Ensure the darwin-aarch64 mmake selects the `all-unix`
  overlay objects (`emul_host.c`, `emul_host_unix.c`, `emul_dir.c`) and **not** the dummy
  `arch/all-hosted/filesys/emul_handler/emul_host.c` (every `Do*` returns
  `ERROR_NOT_IMPLEMENTED`). Keyed off `$(FAMILY)`/`$(ARCH)` in
  `emul_handler/mmakefile.src` `FAMILY_INCLUDES`; darwin-aarch64 must map to
  `FAMILY=unix`. **UNVERIFIED until `emul.handler` links** (asserted by [V0]).
- **`unixio.hidd` (gap #2).** `host_startup` opens `unixio.hidd` v43 to obtain
  `uio_LibcHandle` (the `libSystem.dylib` handle for `HostLib_GetInterface`) and
  `uio_ErrnoPtr` (`emul_host_unix.c:125–150`). On Darwin the libc handle is
  `libSystem.dylib` (hostdisk precedent, `hostdisk_host.h:35`), and errno is `__error()`.
  Bring `unixio.hidd` up for darwin-aarch64. **UNVERIFIED whether it builds/runs.**
- **Off_t guard (R-OFFT).** `HOST_LONG_ALIGNED` (the split-lseek hack, `emul_unix.h:48–
  54`, `:97–98`) is `__arm__`/iOS-only and must stay **off** for darwin-aarch64 (macOS
  arm64 `off_t` is plain 64-bit). Verify the macro is not defined for this target.
- **Mount.** Default boot volume self-mounts as `System:` (no Mountlist) — first win.
  For a named Mac folder, add a Mountlist entry modelled on the in-tree `HOME:`
  (`workbench/devs/Mountlist:98–106`):
  ```
  MAC:
      FileSystem = emul-handler
      Device     = Mac:~/Amiga
      DOSType    = 0x454D5500
  ```
  `Device` is `<VolumeName>:<hostpath>`; `~` → `$HOME` via `GetHomeDir`. Equivalent
  programmatic path: `MakeDosNode`/`AddDosNode(ADNF_STARTPROC)` with the host path in
  `FileSysStartupMsg.fssm_Device`, as `emul_init.c` already does for the `EMU` node.
- **Prerequisite (gating).** The handler is a `.resource` started as a DOS process; it
  needs `dos.library` + `expansion.library` + the boot module set. The current kickstart
  halts at cold-start (WORKFLOW F1); `dos.library` + the F2 boot set are the prerequisite
  for any in-AROS spike ([V4]+). The bare-process spikes ([V1]–[V3]) do **not** need it.

## Unattended verification (no TCC — `[OURS]` H7/H11 discipline)

The loop is the project's existing one: `graft/build-darwin-aarch64.sh` builds; hosted
AROS runs headless via `~/aros-darwin/run.sh` / `graft/bootrun.sh`; the agent reads
serial markers from stdout (same channel as M/H milestones). **Two-sided assertion is
the rule** (proven in H11): the harness **creates the fixture on the macOS side**
(`/tmp/aros-hostvol-XXXX/…`) and asserts AROS sees it, **and** writes from AROS and
re-reads the host file independently. No screenshot, no Finder-automation, no screen-
recording — the handler's file I/O runs as the AROS *process itself* under the launching
terminal's permissions, so **no TCC prompt**. Each spike prints `[Vn] PASS …`/`[Vn] FAIL
…`; a hung mount is reaped by the existing bash watchdog. Markers are unique per spike.

- **[V0] Builds for the target.** `emul.handler` (Unix overlay) compiles **and links**
  for darwin-aarch64. PASS = link artifact present **and** the link map shows
  `emul_host_unix.o`/`emul_host.o` (overlay) and **not** the all-hosted dummy (grep the
  map). *(Compile half already passes — `a68e4c5c`.)*
- **[V1] Host glue lists a real dir (bare spike).** `hosted/hostfs.c` (H-series) drives
  the `Do*` shape — `opendir`/`readdir` over libc — on a fixed temp dir the harness
  pre-populates with N marker files. PASS = enumerates exactly those N names. Proves
  `DoExamineNext` mechanics + the `HostLib_Lock`/`AROS_HOST_BARRIER` discipline on a
  switched task (reuse H11's harness).
- **[V2] Host glue reads bytes.** Harness writes a known 256-byte pattern; spike
  `DoOpen`+`DoRead` checks byte-equality. PASS = bytes match.
- **[V3] Host glue writes; host sees it.** Spike `DoOpen(MODE_NEWFILE)`+`DoWrite` a
  pattern to a new host path; harness re-reads that path on the macOS side. PASS = the
  Mac file exists with the right bytes (mirrors H11's two-sided check).
- **[V4] Mount as a named volume in booted AROS** (needs `dos.library`). `MAC: Device =
  Mac:<tmpdir>`; boot, `dir MAC:`. PASS = AROS lists the marker files the harness placed.
  Marker `[V4] MAC: <n> entries`.
- **[V5] Round-trip through DOS.** `copy MAC:in.dat MAC:out.dat` (or `Open`/`Write` from
  a boot script); harness re-reads `<tmpdir>/out.dat` on the Mac side. PASS = bytes
  match. Exercises `ACTION_FINDINPUT/FINDOUTPUT/READ/WRITE/END` end-to-end.
- **[V6] Examine/metadata fidelity.** `dir MAC:` then assert size/date/protection for a
  file the harness made with known `chmod`/`mtime`. PASS = AROS reports matching size,
  date, and `rwx`→`FIBF` mapping (low-active R/W/E correct).
- **[VN] Normalization round-trip (the new hard part).** Harness creates **two** fixture
  files whose names are the *same* accented string written once in **NFC** and once in
  **NFD** bytes (e.g. `café` composed vs decomposed). (a) `dir MAC:` lists both, each
  rendered to a stable AROS name; (b) from AROS, open a file by the NFC-typed accented
  name and assert it resolves to the file the harness wrote in **NFD** (proves the
  handler's own normalization, not the FS's). PASS = both listed **and** the cross-form
  open hits the right file. Marker `[VN]`. **This is the spike that fails today** without
  R-NORM and passes with it.
- **[VM] Metadata sidecar round-trip.** From AROS set a comment + a Pure/Script
  protection bit on a file (`ACTION_SET_COMMENT`/`SET_PROTECT`); harness reads
  `.<name>.amimeta` on the Mac side and asserts its `comment`/`prot` contents; then
  re-`Examine` from AROS and assert the comment + AmigaOS-only bits come back. Also
  assert `dir MAC:` does **not** list the `.amimeta` file. PASS = sidecar present with
  correct fields **and** round-trips **and** is hidden. Marker `[VM]`.
- **[V7] (nicety) Live pickup.** Harness drops a new file into `<tmpdir>` after mount; a
  re-`dir MAC:` shows it (re-scan; FSEvents deferred).

## Build / integration

- No new dylib, no Cocoa, no AppKit/Metal. The new code is C inside the existing overlay
  files (`emul_host.c`/`emul_dir.c`) plus a small `emul_norm.c`/`emul_meta.c` pair,
  built by the **AROS crosstools** as part of `emul.handler` — **not** the host clang.
- No new dlsym symbol is *required* (sidecar uses existing `LibCInterface` entries);
  `pathconf` is the *only* candidate addition, gated behind R-CASE and optional.
- New code paths are Darwin-guarded (`HOST_OS_darwin`) where behaviour diverges
  (`_PC_CASE_SENSITIVE`); the normalization/charset/sidecar logic is host-portable POSIX
  and may compile for every Unix host (it only *matters* where the FS is a bag-of-bytes,
  but it is correct everywhere). Confirm it doesn't regress the Linux overlay [V0]-style.
- The handler must not pull CoreFoundation/CoreServices: normalization is a self-
  contained table-driven function, not `CFStringNormalize`.

## Open questions / UNVERIFIED

- `unixio.hidd` build/run status on darwin-aarch64 (mount fails at `host_startup`
  without it) — gap #2.
- That darwin-aarch64 mmake pulls the `all-unix` overlay (not the dummy) — asserted by
  [V0], **UNVERIFIED** until a link.
- `_PC_CASE_SENSITIVE` reliability across APFS variants; fallback is a `CASE=` Mountlist
  option (R-CASE).
- Normalization table coverage/size: the Latin-1-mappable subset suffices for the
  Latin-1 round-trip, but a host name with arbitrary BMP code points needs a wider
  decomposition table — start narrow, widen if [VN] is extended past Latin-1.
- Sidecar/data-file atomicity across `Rename`/`Delete` (two host ops, not one) — a small
  window where they can desync; document, revisit if it bites.
- Symlink resolution (`read_softlink`, `emul_handler.c:431`) AROS-relative vs POSIX-
  absolute — untested on Darwin; orthogonal to this spec, flagged.
- Whether the AmigaOS-only protection bits are worth persisting at all for the first
  cut, or whether comment-only sidecars suffice (R-SIDECAR supports both; default-omit
  means protection-only round-trips can ship later).

## Provenance summary

`[PUB]` POSIX (`open/read/write/lseek/stat/lstat/readdir/seekdir/chmod/utime/rename/
statfs/pathconf`, `_PC_CASE_SENSITIVE`); Unicode UAX #15 normalization forms (NFC/NFD);
ISO-8859-1; reversible hex escaping; APFS bag-of-bytes filename behaviour (Apple docs /
design.md web cites). ·
`[AROS]` `arch/all-hosted/filesys/emul_handler/{emul_handler.c,emul_init.c,filenames.c,
emul_intern.h}`; `arch/all-unix/filesys/emul_handler/{emul_host.c,emul_host_unix.c,
emul_dir.c,emul_unix.h,unix_hints.h}` (the `Do*` contract, `LibCInterface`, `prot_u2a/
a2u`, `timestamp2datestamp`, `fixcase`, `is_special_dir`, the `HostLib_Lock/
AROS_HOST_BARRIER` discipline); `compiler/include/dos/{dosextens.h,dos.h,exall.h,
filehandler.h}` (`DosPacket`, `ACTION_*`, `FileLock`, `FileInfoBlock`, `ExAllData`,
`ST_*`, `DE_*`); `workbench/devs/Mountlist` (`HOME:` template); `unixio.hidd`,
`hostlib.resource`; `arch/all-unix/devs/hostdisk/hostdisk_host.h` (`libSystem.dylib`). ·
`[OURS]` commit `a68e4c5c` (overlay compiles for darwin-aarch64); the H3 host-call
boundary, H10 message ports, H11 device-on-a-file; `graft/{build-darwin-aarch64.sh,
bootrun.sh,WORKFLOW.md}`; this project's two-sided unattended-loop discipline. ·
`[REF-CONFIRM]` UAE-family directory-HD (FS-UAE `.uaem`, WinUAE `UAEFSDB`) — confirmed
**only** that (a) a host-dir bridge must normalize names itself, (b) un-representable
characters need a reversible escape, and (c) AmigaOS comment + extra protection bits
belong in a **portable sidecar, not host xattrs**. Every algorithm, table, filename,
format, and field set here is restated from `[PUB]`/`[AROS]`/`[OURS]` and owes no
expression to any reference.
