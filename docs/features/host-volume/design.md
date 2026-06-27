# Host volume — a macOS folder as an AROS volume

> Status: **IMPLEMENTED & verified on darwin-aarch64 (2026-06-26)** · Drafted 2026-06-24
> The whole spec is grafted into the live `emul-handler` and verified two-sided:
> mount + read-only-default + `;WRITE` keyword + `AROS_HOST_VOLUME` launcher,
> NFC normalization, Latin-1↔UTF-8 + escape, the `.amimeta` metadata sidecar, and
> the case-sensitivity guard. See [spec.md](spec.md) "Implementation status".

## What & why

Mount a real macOS directory (e.g. `~/Amiga`) as an AROS DOS volume so it shows up
in Wanderer and AROS commands (`dir`, `copy`, `ed`, `type`) operate on real Mac
files. Drag a file in from Finder → it's there in AROS; opt into a writable mount and
writes from AROS → Finder sees them. This is the *filesystem* face of the project thesis
— "macOS owns the drivers; AROS reaches them via standard exec I/O" — realised as a DOS
filesystem handler whose backing store is the live macOS filesystem.

The whole thing is **not greenfield**: AROS already ships a host-filesystem handler,
`emul-handler` (the "emulation handler"), used by every hosted flavour (Linux,
Windows, Android, …). Almost all of its logic is host-independent. The job is to
bring it up for `darwin-aarch64`, supply the macOS-specific host-call glue, and add
Mac niceties. The foundation already landed in this repo (see below); this doc plans
the rest as loop-verifiable spikes.

## Does it already exist?

Yes — mostly. `emul-handler` splits cleanly into a **portable core** and a thin
**per-host overlay**, and only the overlay is host-specific.

**Portable core (host-independent, reused verbatim):**
`/Users/user/Source/aros-upstream/arch/all-hosted/filesys/emul_handler/`
- `emul_handler.c` — the DOS-packet dispatcher (`handlePacket`), the volume/lock
  machinery, `new_volume()`, `EmulHandlerMain` process. ~1240 lines, no syscalls.
- `emul_init.c` — `startup()`: creates the `EMU` BootNode via `expansion.library`,
  spawns `EmulHandlerMain`. **Currently being debugged in-tree** (uncommitted:
  `#define DEBUG 1`, `git diff HEAD`).
- `filenames.c` — AROS-path → host-path string surgery (`shrink`, `validate`,
  `append`, `nextpart`). Pure string code.
- `emul_intern.h` — declares the `Do*` host-call contract (the overlay's job).
- `emul_host.c` — a **dummy template** where every `Do*` returns
  `ERROR_NOT_IMPLEMENTED`. This is the fallback if no overlay exists.

**Per-host overlays that exist** (each implements the `Do*` contract):
- `arch/all-unix/filesys/emul_handler/` — the POSIX overlay. **This is our template.**
  `emul_host.c` (the real `Do*` over libc), `emul_host_unix.c` (host startup +
  the dlsym'd `LibCInterface` table), `emul_dir.c`, `emul_unix.h`, `emul_host.h`,
  `unix_hints.h`.
- `arch/all-mingw32/filesys/emul_handler/` — Win32 (`CreateFile`/`ReadFile`/…).
- `arch/all-android/filesys/emul_handler/` — Unix overlay minus `seekdir`/`telldir`.

**Darwin status — what's present vs. absent:**
- `arch/all-darwin/filesys/` **does not exist** (verified: `ls` → "No such file or
  directory"). There is **no darwin-specific emul_handler overlay**, and there does
  not need to be a full one: macOS is POSIX, so it builds against the *Unix* overlay.
- The Unix overlay is **already Darwin-aware**, and this repo already extended it:
  - `unix_hints.h` has a `HOST_OS_darwin` branch (commit `a68e4c5c` in
    aros-upstream, *this project's* work) that drops the ~2010 `_DARWIN_NO_64_BIT_INODE`
    assumption — modern macOS / Apple Silicon is 64-bit-inode-only
    (`__DARWIN_ONLY_64_BIT_INO_T` is a hard compile error otherwise). `INODE64_SUFFIX`
    is left empty so the plain `stat`/`readdir` symbols (already the 64-bit versions)
    are dlsym'd. Commit message: *"emul-handler builds on modern macOS … now builds
    for darwin-aarch64."*
  - So `emul-handler` **compiles for darwin-aarch64 today.**
- The libc-symbol path is grounded: the overlay dlsym's `"stat" INODE64_SUFFIX`,
  `"lstat" …`, `"open" UNIX2003_SUFFIX`, etc. from the host libc handle
  (`emul_host_unix.c:50–94, 133`). On Darwin that handle is `libSystem.dylib` (the
  hostdisk precedent hardcodes exactly this: `hostdisk_host.h:35` `#define LIBC_NAME
  "libSystem.dylib"`).

**What darwin-aarch64 still needs (the gap):**
1. The build must actually *select* the Unix overlay for the darwin target (mmake
   `FAMILY`/`ARCH` wiring → see `mmakefile.src` `FAMILY_INCLUDES`). Confirm the
   darwin-aarch64 target maps to `all-unix` (it's `all-darwin` + presumably
   `FAMILY=unix`); **UNVERIFIED** until a build links `emul.handler`.
2. `unixio.hidd` must come up on darwin-aarch64 — the overlay opens it
   (`emul_host_unix.c:125`) to obtain the libc handle (`uio_LibcHandle`) and errno
   pointer (`uio_ErrnoPtr`). **UNVERIFIED** whether `unixio.hidd` is built/working
   for this target.
3. The runtime path: `emul-handler` is a `.resource` started as a DOS process; it
   needs `dos.library` + `expansion.library` + a mount entry. The current kickstart
   is a minimal 3-module set (exec/kernel/hostlib) that halts at cold-start
   (`graft/WORKFLOW.md` F1), so **the handler cannot run yet** — `dos.library` and
   the boot module set (WORKFLOW F2) are the prerequisite.
4. The macOS niceties (FSEvents, comment/resource-fork mapping, normalization) — all
   new.

Net: the *code* largely exists and builds; the *bring-up* (does it run, does it
mount, does it list/read/write) is what's unproven and is what the spikes below
nail down.

**External prior art / status (web-grounded, *not* in the AROS tree):**
- **Upstream AROS hosted Darwin is Intel/PPC-only and X11-bound** — the official
  ports page lists only `darwin-i386`, `darwin-x86_64`, `darwin-ppc`, all needing an
  X11 server; **no arm64/Apple-Silicon Darwin port exists upstream**
  (https://aros.sourceforge.io/introduction/ports.html). So this project is genuinely
  new ground for `darwin-aarch64`, but the *handler design* is unchanged from the
  proven Intel-Darwin path. Corroborates the in-tree picture; nothing external
  changes the emul.handler approach.
- **Even the Intel-Darwin hosted build had open boot crashes** — AROS issue #408
  (macOS 11, x86_64) reports a cold-boot crash in `tlsf_freevec`/`lddemon.resource`
  during library init, *before* any filesystem work
  (https://github.com/aros-development-team/AROS/issues/408). It doesn't touch
  emul.handler, but it confirms the prerequisite framing here (handler can't run until
  the boot set is up, WORKFLOW F2) is the real-world gating, not a local quirk.
- **A host-directory-as-volume bridge (independently derived)** solves the exact
  metadata/charset problems this doc flags via *sidecar* files rather than xattrs:
  - A per-file text sidecar (e.g. `<name>.<ext>`) can store Amiga metadata
    (protection bits, 1/50s-precision date, comment), written *only* when the value is
    non-default, and URL-escape host-illegal characters (e.g. Amiga `Foo\Bar` →
    `Foo%5cBar`) for cross-OS portability. This is a concrete, portable alternative
    to the doc's "map AROS comment → xattr" nicety, and a precedent for the
    protection-bit / date-precision mapping the overlay already does inline.
  - A hidden index file is an alternative shape for the same extra-attribute storage.
    Neither uses host xattrs — they keep the
    host dir a plain, portable directory, which is the opposite of the resource-fork
    approach and worth weighing.
- **Catch / correction (Mac normalization):** the doc's "APFS returns NFD
  (decomposed)" is **HFS+ behaviour, not APFS**. HFS+ *normalized* every name to NFD
  on disk; **APFS is a "bag of bytes" — it preserves whatever form was written (NFC
  *or* NFD) and `readdir` returns those exact bytes**
  (https://mjtsai.com/blog/2017/03/24/apfss-bag-of-bytes-filenames/,
  https://eclecticlight.co/2021/05/08/explainer-unicode-normalization-and-apfs/). macOS
  layers a *normalization-insensitive* lookup on top so two forms usually resolve to
  one file, but the layer is under-documented (kernel vs userspace **UNVERIFIED**) and
  APFS can legitimately hold two files differing only by normalization. Net for the
  bridge: don't assume "always NFD" — the safe move is to normalize the *AROS-side*
  name and the *readdir result* to a single agreed form (NFC) before `Stricmp`, rather
  than relying on the filesystem. (Risks updated.)
- **AppleDouble `._` files are a non-issue on a normal Mac disk** — `._<name>` sidecars
  only appear when macOS writes Mac metadata to a volume that *can't* hold xattrs (FAT,
  NFSv3, zip); on native APFS/HFS+ the resource fork / Finder info live *inline* as
  `com.apple.*` xattrs, so a host folder on the Mac's own disk won't sprout `._` files
  for the handler to trip over (https://en.wikipedia.org/wiki/AppleSingle_and_AppleDouble_formats).
  Only relevant if the mounted host path is itself on a FAT/network volume.

## Background: the AROS filesystem-handler contract (grounded)

A filesystem handler is a DOS process listening on a `MsgPort`; DOS sends it
**DOS packets**, it acts and replies. `emul-handler` speaks the **classic DOS-packet
interface**, not the FSA_/IOFileSys model (confirmed: `handlePacket` switches on
`dp->dp_Type` against `ACTION_*`; there is no `struct IOFileSys`/`FSA_*` in the
public headers).

**The packet** —
`/Users/user/Source/aros-upstream/compiler/include/dos/dosextens.h`:
```c
struct DosPacket {
    struct Message *dp_Link;   struct MsgPort *dp_Port;
    LONG  dp_Type;             /* one of ACTION_* */
    SIPTR dp_Res1, dp_Res2;    /* primary result, IoErr() */
    SIPTR dp_Arg1 .. dp_Arg7;
};
struct StandardPacket { struct Message sp_Msg; struct DosPacket sp_Pkt; };
```
The handler loop is literally `dp = WaitPkt(); handlePacket(...); ReplyPkt(dp,Res1,Res2)`
(`emul_handler.c:1215–1219, 1147`). This is the **exact shape proven hosted in H10
(message ports) and H11 (a device on the exec I/O path)** — an IORequest/packet sent
to a port, the server task acts, replies; the client blocks on the reply. H11's
`hosted/device.c` runs `BeginIO`→`PutMsg`→device task→`pread/pwrite`→`ReplyMsg`→
`WaitIO`; `emul-handler` is the same switched-task-does-the-host-syscall pattern, one
layer up (DOS packet instead of raw IORequest).

**ACTION_* codes handled** (`emul_handler.c`, defs in `dosextens.h`):
`ACTION_FINDINPUT/FINDOUTPUT/FINDUPDATE` (open file → `dp_Arg1`=FileHandle,
`dp_Arg2`=lock, `dp_Arg3`=BSTR name), `ACTION_READ`/`ACTION_WRITE` (`dp_Arg1`=fh,
`dp_Arg2`=buffer, `dp_Arg3`=length), `ACTION_SEEK`, `ACTION_END` (close),
`ACTION_LOCATE_OBJECT` (make a lock), `ACTION_FREE_LOCK`, `ACTION_COPY_DIR`
(DupLock), `ACTION_PARENT`, `ACTION_EXAMINE_OBJECT`/`EXAMINE_NEXT`/`EXAMINE_ALL`,
`ACTION_CREATE_DIR`, `ACTION_DELETE_OBJECT`, `ACTION_RENAME_OBJECT`,
`ACTION_SET_PROTECT`/`SET_DATE`, `ACTION_MAKE_LINK`/`READ_LINK`, `ACTION_INFO`/
`DISK_INFO`, `ACTION_IS_FILESYSTEM`, `ACTION_SET_FILE_SIZE`, `ACTION_SAME_LOCK`.

**FileLock** — `dosextens.h`:
```c
struct FileLock { BPTR fl_Link; IPTR fl_Key; LONG fl_Access;
                  struct MsgPort *fl_Task; BPTR fl_Volume; };
```
`fl_Key` holds the handler's private `struct filehandle *` (`emul_handler.c:861,
FH_FROM_LOCK`); `fl_Access` is `SHARED_LOCK(-2)`/`EXCLUSIVE_LOCK(-1)`
(`dos/dos.h`, also `ACCESS_READ`/`ACCESS_WRITE`); `fl_Volume = MKBADDR(fh->dl)`.

**Examine** — `dos/dos.h` `struct FileInfoBlock` (`fib_DirEntryType`, `fib_FileName`
[BSTR-style len+chars], `fib_Protection`, `fib_Size`, `fib_Date`, `fib_Comment`,
`fib_OwnerUID/GID`); `dos/exall.h` `struct ExAllData`/`ExAllControl` with `ED_*`
levels (`ED_NAME=1 … ED_OWNER=7`, `dosextens.h`). Directory-entry types:
`ST_FILE=-3`, `ST_ROOT=1`, `ST_USERDIR=2`, `ST_SOFTLINK=3` (`dosextens.h`). The
handler's `examine()` (`emul_handler.c:309`) calls `DoExamineEntry` then packs the
`ExAllData` into the `FileInfoBlock`.

**Mount / DOSList / volume** —
- `struct DeviceNode`/`DosList` and `struct DosEnvec` with `DE_*` indices
  (`dos/filehandler.h`, `dosextens.h`): `DE_DOSTYPE`, `DE_TABLESIZE`, `DE_MASK`, …
- A volume node is made at runtime by `MakeDosEntry(volname, DLT_VOLUME)` +
  `AddDosEntry(doslist)` (`emul_handler.c:655–661`; `rom/dos/makedosentry.c`,
  `adddosentry.c`), with `doslist->dol_Task = mp` (the handler's port). The handler
  also fires `IECLASS_DISKINSERTED` to `input.device` so Wanderer notices
  (`emul_handler.c:663`).
- The handler is associated with a device via `dn_Handler = "emul-handler"` +
  `dn_SegList = CreateSegList(EmulHandlerMain)` and started with `ADNF_STARTPROC`
  (`emul_init.c:66–71`). The startup packet carries the `FileSysStartupMsg`
  (`fssm_Device` = the BSTR `<volname>:<hostpath>`, `emul_handler.c:1193–1197`).
- Declarative mounts live in `workbench/devs/Mountlist` — the existing `HOME:`/`USR:`
  entries are the template:
  ```
  HOME:  FileSystem = emul-handler   Device = Home:~      DOSType = 0x454D5500
  USR:   FileSystem = emul-handler   Device = USR:/usr    DOSType = 0x454D5500
  ```
  `DOSType 0x454D5500` = `'E','M','U',0` = `AROS_MAKE_ID('E','M','U',0)` (matches
  `emul_init.c:61`). The `Device` string is `<VolumeName>:<hostpath>`; `~` means
  "home dir" (resolved via `GetHomeDir`).

## Design

### Host side (macOS POSIX fs glue)

Reuse the Unix overlay unchanged — it already implements the full `Do*` contract over
the dlsym'd `LibCInterface` and is Darwin-aware. Concretely
(`arch/all-unix/filesys/emul_handler/`):

- **The interface table** (`emul_unix.h` `struct LibCInterface`): function pointers
  for `open, close, read, write, lseek, ftruncate, opendir, readdir, rewinddir,
  closedir, seekdir, telldir, mkdir, rmdir, unlink, link, symlink, readlink, rename,
  chmod, statfs, utime, stat, lstat, fstat, localtime, mktime, getcwd, getenv, …`.
  Resolved at `host_startup()` via `HostLib_GetInterface(uio_LibcHandle, libcSymbols,
  &unresolved)` (`emul_host_unix.c:133`); any unresolved symbol fails the mount.
- **The Do* implementations** (`emul_host.c`): real syscalls. e.g. `DoOpen` does
  `lstat` then `open`; `DoRead`/`DoWrite` call `read`/`write`; `DoExamineEntry`
  (line 981) does `lstat`/`stat`, then maps `st_mode`→`ed_Type`
  (`S_ISDIR`→`ST_USERDIR`/`ST_ROOT`, `S_ISLNK`→`ST_SOFTLINK`, else `ST_FILE`),
  `st_mtime`→`ed_Days/Mins/Ticks` via `timestamp2datestamp` (line 165), and
  `st_mode`→`ed_Prot` via `prot_u2a` (line 130). `DoExamineNext` iterates `readdir`.
- **The host-call discipline** (load-bearing on this port): every syscall is bracketed
  `HostLib_Lock(); ...; AROS_HOST_BARRIER; HostLib_Unlock();` (e.g. `emul_host.c:170–175`).
  `HostLib_Lock` serialises all host calls (semaphore); `AROS_HOST_BARRIER` is the
  compiler/CPU fence after returning from host code. This is **the same boundary H3
  de-risked** — AROS-built code (generic AAPCS64) calling macOS libc (Apple arm64 ABI).
  Non-variadic libc calls (open/read/stat) need no shim; only variadic host calls
  would (none of the `Do*` paths use them — `open` is called with a fixed arg count).
- **errno**: read through `emulbase->pdata.errnoPtr` (= `uio_ErrnoPtr`), which on
  Darwin is `__error()`'s return (the hostdisk precedent dlsym's `__error`,
  `hostdisk_host.c`). The Unix overlay caches it at startup (`emul_host_unix.c:150`).

### AROS side (emul_handler for darwin-aarch64 + mount/volume)

- Build: ensure mmake selects `all-unix` overlay files for darwin-aarch64
  (`emul_handler/mmakefile.src` keys off `$(ARCH)`/`$(FAMILY)`). Verify
  `emul.handler` actually links for the target (gap item #1).
- Bring up `unixio.hidd` for darwin-aarch64 (gap item #2) — needed for the libc
  handle + errno pointer.
- Add a `~/Amiga`-style mount two ways (both implemented):
  - **Declarative (always read-only):** a Mountlist entry modelled on `HOME:`. Read-only
    is our policy and a mountlist host folder cannot opt out of it — the write keyword is
    ours and is deliberately never handed to the `Mount` command:
    ```
    MAC:  FileSystem = emul-handler   Device = Mac:~/Amiga   DOSType = 0x454D5500
    ```
  - **Our launcher (read-only or read/write):** set the `AROS_HOST_VOLUME` host env var;
    `emul_init.c`'s `mount_hostvol()` does the `MakeDosNode`/`AddDosNode(ADNF_STARTPROC)`
    with the value as `fssm_Device`, so our `;WRITE` keyword reaches `new_volume` intact:
    ```
    AROS_HOST_VOLUME="Mac:~/Amiga"        # read-only
    AROS_HOST_VOLUME="Mac:~/Amiga;WRITE"  # read/write
    ```
  `new_volume` parses the trailing `;WRITE|;W|;RW|;READONLY|;RO` keyword off `fssm_Device`
  and strips it before resolving `~`/the host path. We do NOT route the keyword through
  `Mount`: its mountlist parser (`preparefile()`) turns a bare `;` into whitespace (keyword
  lost → safely read-only) and errors on a quoted `";…;WRITE"`. The launcher is the seed of
  the `run-window.sh --host-volume` developer UX.
- The default boot volume already self-mounts: `new_volume(NULL)` falls back to
  `GetCurrentDir()` and names the volume `System:` (`emul_handler.c:559, 617, 627`).
  So the *first* observable win is "AROS has a `System:` volume backed by the launch
  directory" — no Mountlist needed.

### The bridge (path & charset translation, metadata mapping)

- **Path syntax**: AROS uses `Volume:dir/sub/file` and `/` to go up a level; POSIX
  uses `/dir/sub/file`. `filenames.c` + `makefilename()` (`emul_handler.c:93`) splice
  the user's AROS path onto the volume's host root (`fh->hostname`) and `shrink()` it
  to a clean POSIX path. Device prefix (`Mac:`) is stripped (`strrchr(filename,':')`,
  `emul_handler.c:105`). This is pure string code, host-agnostic — reused as-is.
- **Case + Unicode (the real Mac risk)**: AmigaOS filenames are case-*insensitive*,
  case-*preserving*, Latin-1-ish; macOS paths are UTF-8, and APFS should be treated as
  preserving whatever Unicode normalization form was written rather than as an
  NFD-normalizing filesystem. The Unix overlay already compiles with
  `#define NO_CASE_SENSITIVITY` (`emul_host_unix.c:35`), which enables `fixcase()`
  (`emul_host.c:224`): on a failed `lstat`, it re-scans the parent dir with `Stricmp`
  to find a case-folded match. That covers ASCII case; it does **not** handle NFC↔NFD
  (a name with accents typed in AROS as NFC may not `Stricmp`-match decomposed bytes
  returned by `readdir`). The executable spec requires a bridge-level NFC pass and does
  not rely on the filesystem to normalize.
- **Protection bits**: `prot_u2a`/`prot_a2u` (`emul_host.c:95–161`) map POSIX `rwx`
  (user/group/other) ↔ AROS `FIBF_*`. Note the AmigaOS inversion: R/W/E are
  *low-active* in `fib_Protection` (set = denied), handled correctly already.
- **Dates**: `st_mtime` ↔ AROS `DateStamp` via `Date2Amiga`/`Amiga2Date` and
  host `localtime`/`mktime` (`emul_host.c:165, 195`).
- **Comments / metadata**: AROS file comments and AmigaOS-only protection bits have no
  faithful POSIX slot. The spec chooses a portable sidecar, not xattrs: one
  `.<basename>.amimeta` file beside the data file, omitted when metadata is default and
  hidden from AROS enumeration. This keeps the mounted directory copyable across hosts
  and avoids Finder/private-xattr semantics. The cost is an explicit reserved host
  namespace: files matching `.*.amimeta` are metadata inside the mounted volume, not
  ordinary AROS-visible files.
- **Access mode** (implemented): default is read-only; the `;WRITE`/`;W`/`;RW` keyword on
  `fssm_Device` allows AROS-originated mutations. The keyword is **ours** — delivered via
  the `AROS_HOST_VOLUME` launcher, never through `Mount` (see above) — so a mountlist host
  folder is always read-only. Without write, the packet handler enforces protection before
  any host syscall or sidecar update: reads, directory scans, examine, locks, and copy-out
  work; mutating packets (`FINDOUTPUT`, write-capable `FINDUPDATE`, `WRITE`, create/delete/
  rename, set date/protect/size/comment, link creation) fail with
  `ERROR_DISK_WRITE_PROTECTED`, and `ACTION_INFO`/`DISK_INFO` report `ID_WRITE_PROTECTED`.
  `READONLY`/`RO` remain accepted aliases, redundant because read-only is the default.

## Plan — spikes in the loop

The handler can only *run* once `dos.library` + the boot set exist (WORKFLOW F2), so
the early spikes prove the host glue in isolation (the H-series style, bare process)
and the later ones prove it inside booted AROS. Each spike emits a unique marker the
agent greps; the agent creates/asserts files on **both** sides so there is no manual
(TCC-free) step.

- **[V0] Builds for the target.** `emul.handler` (Unix overlay) compiles *and links*
  for darwin-aarch64 in the AROS mmake. PASS = link artifact present + no
  `ERROR_NOT_IMPLEMENTED` dummy pulled in (grep the map for `emul_host_unix.o`).
  *(The compile half already passes — commit `a68e4c5c`.)*
- **[V1] Host glue lists a real dir (bare spike).** A standalone `hosted/hostfs.c`
  (H-series style) that exercises the overlay's `Do*` shape — `opendir`/`readdir`
  over libc on a fixed temp dir the agent pre-populates with N marker files. PASS =
  it enumerates exactly those N names. This proves `DoExamineNext` mechanics + the
  `HostLib_Lock`/barrier discipline on a switched task (reuse H11's harness).
- **[V2] Host glue reads a file's bytes.** Same spike: agent writes a known 256-byte
  pattern to a host file; the spike `DoOpen`+`DoRead`s it and checks byte-equality.
  PASS = bytes match.
- **[V3] Host glue writes; the host sees it.** Spike `DoOpen(MODE_NEWFILE)`+`DoWrite`s
  a pattern to a new host path; *the agent then reads that path back on the macOS
  side* and asserts the bytes. PASS = the Mac file exists with the right content.
  (Mirrors H11's two-sided verification exactly.)
- **[V4] Read-only mountlist mount — DONE (2026-06-26).** Mountlist `MAC: Device =
  Mac:<tmpdir>`; boot windowed, drive from `S/Startup-Sequence`. PASS (verified): `Dir
  MAC:` lists the markers, `Type MAC:in.dat` returns the bytes; `MakeDir`/`Copy`-into/
  `Delete` on `MAC:` fail "disk is write-protected"; the agent re-reads `<tmpdir>` and
  nothing changed. (Drive from the Startup-Sequence file, not stdin — no input race.)
- **[V5] Writable launcher mount — DONE (2026-06-26).** Boot with
  `AROS_HOST_VOLUME="MacW:<tmpdir>;WRITE"`; `Copy MacW:in.dat MacW:out.dat` + `MakeDir
  MacW:newdir`. PASS (verified): the agent re-reads `<tmpdir>/out.dat` (bytes match) and
  `<tmpdir>/newdir` on the Mac side — the `;WRITE` keyword honoured via our path.
- **[V5-ro] (covered by [V4]).** Read-only *is* the mountlist behaviour, so the plain
  `MAC:` entry is the read-only sweep; the agent confirms the host directory and sidecars
  did not change.
- **[V6] Examine/metadata fidelity.** `dir MAC:` then assert sizes/dates/protection
  for a file the agent created with known `chmod`/`mtime`. PASS = AROS reports the
  matching size, date, and `rwx`→`FIBF` mapping.
- **[V7] (nicety) Live pickup.** Agent drops a new file into `<tmpdir>` *after* mount;
  a re-`dir MAC:` shows it (re-scan picks it up; FSEvents only needed for Wanderer
  auto-refresh — deferred).

### How we verify it unattended

The loop is the project's existing one: `graft/build-darwin-aarch64.sh` builds, the
hosted AROS runs headless via `~/aros-darwin/run.sh` / `graft/bootrun.sh`, and the
agent reads serial markers from stdout — same channel as the M/H milestones. No
screenshot, no TCC, no manual approval (the home-folder/`~/Amiga` access the handler
uses is ordinary file I/O by the AROS *process itself*, which inherits the launching
terminal's permissions; it is **not** a Finder-automation or screen-recording
permission, so no TCC prompt).

Two-sided assertion is the rule (proven in H11): the agent **creates the fixture on the
macOS side** (`/tmp/aros-hostvol-XXXX/…`) and asserts AROS sees it; write-enabled spikes
also write from AROS and assert the bytes landed on the Mac side by re-reading the host
file independently. Each spike prints `[Vn] PASS …` / `[Vn] FAIL …`; a hung mount is
reaped by the existing bash watchdog. Markers are unique per spike so a regression
localises (the M/H marker discipline). For [V0] the assertion is a grep over the mmake
link map (overlay object linked, dummy not).

## Risks & open questions

- **Unicode normalization (NFC vs NFD)** — the sharpest Mac-specific risk, but the
  mechanism is subtler than "APFS returns NFD" (that was *HFS+*). **APFS is a
  bag-of-bytes** that preserves and returns whatever form was written — NFC *or* NFD —
  and can even hold two names differing only by form
  (https://mjtsai.com/blog/2017/03/24/apfss-bag-of-bytes-filenames/). macOS adds a
  normalization-*insensitive* lookup layer, but its placement (kernel VFS vs userspace)
  is **UNVERIFIED**, so the handler can't rely on it. AROS/Amiga tooling assumes one
  byte per char; `fixcase()` (`emul_host.c:224`) only ASCII-case-folds via `Stricmp`
  and won't reconcile NFC↔NFD, so an accented name typed in AROS (NFC) may not match
  the bytes `readdir` hands back. Fix is a normalization pass at the bridge: normalize
  both the AROS-side name and each `readdir` result to a single agreed form (NFC)
  before comparison — new code, do not depend on the filesystem normalizing. Open.
- **Case sensitivity** — handled for the *insensitive* default (`NO_CASE_SENSITIVITY`
  + `fixcase`), but a case-*sensitive* APFS volume would make `fixcase` find duplicates;
  and `ACTION_SAME_LOCK` compares with `strcasecmp` (`emul_handler.c:797`), which is
  wrong on a case-sensitive volume. Open.
- **Protection-bit mapping** is lossy: AROS has 4 owner-ish flags + `FIBF_SCRIPT`
  (→ `S_ISVTX`) + Archive/Pure/Hold bits with no POSIX equivalent; only `rwx`
  round-trips. Acceptable, document it.
- **off_t / size width** — `emul_intern.h` explicitly warns Darwin's `off_t` is 64-bit
  even on 32-bit hosts; on aarch64 everything is 64-bit so the seek/size paths should
  be clean, but the iOS `HOST_LONG_ALIGNED` split-lseek hack (`emul_unix.h:48–54`) must
  **not** be enabled for darwin-aarch64 (it's `__arm__`/iOS only). Verify the macro
  stays off.
- **Locking semantics** — `emul-handler` tracks share counts but does not enforce
  exclusive locks against the host (two AROS exclusive locks on one host file aren't
  blocked at the POSIX layer). Faithful to upstream; note it.
- **Access-option parsing** — `WRITE`/`W`/`RW` and `READONLY`/`RO` reserve semicolon as
  the keyword separator in `fssm_Device`; a host path may not contain a literal `;` (a
  later quoting rule could lift this). Because the keyword is delivered via our launcher
  (`AROS_HOST_VOLUME`) and not the `Mount` command, it never meets `Mount`'s mountlist
  parser — which would mangle a bare `;` to whitespace and error on a quoted one.
- **Symlinks** — `DoSymLink`/`DoReadLink`/`ACTION_MAKE_LINK`/`READ_LINK` exist in the
  overlay; AROS-relative vs POSIX-absolute link resolution (`read_softlink`,
  `emul_handler.c:431`) is subtle and untested on Darwin. Open.
- **`unixio.hidd` availability** — the overlay hard-depends on it for the libc handle
  and errno; if it isn't up for darwin-aarch64, the mount fails at `host_startup`.
  Gating unknown (#2). **UNVERIFIED.**
- **Build wiring** — that the darwin-aarch64 mmake actually pulls the `all-unix`
  overlay (not the dummy `all-hosted/emul_host.c`) is asserted by [V0] but
  **UNVERIFIED** until a link.
- **FSEvents / Finder drag** — live change pickup for Wanderer auto-refresh wants
  `FSEventStreamCreate` (CoreServices) feeding `IECLASS_DISKINSERTED`-style notifies;
  pure nicety, deferred. Drag-from-Finder needs nothing special — it's just a file
  appearing in the host dir, picked up on the next `dir`/re-scan.

## References

- Portable handler core:
  `/Users/user/Source/aros-upstream/arch/all-hosted/filesys/emul_handler/{emul_handler.c,emul_init.c,filenames.c,emul_intern.h,emul.conf,mmakefile.src}`
- POSIX host overlay (the template):
  `/Users/user/Source/aros-upstream/arch/all-unix/filesys/emul_handler/{emul_host.c,emul_host_unix.c,emul_unix.h,emul_host.h,unix_hints.h,emul_dir.c}`
- Darwin precedent (host file/disk over `libSystem.dylib`):
  `/Users/user/Source/aros-upstream/arch/all-darwin/hostdisk/{geometry.c}`,
  `/Users/user/Source/aros-upstream/arch/all-unix/devs/hostdisk/{hostdisk_host.c,hostdisk_host.h}`
- DOS contract:
  `/Users/user/Source/aros-upstream/compiler/include/dos/{dosextens.h,dos.h,exall.h,filehandler.h}`
  (`DosPacket`, `ACTION_*`, `FileLock`, `FileInfoBlock`, `ExAllData`, `DosList`,
  `DeviceNode`, `DosEnvec`, `DE_*`, `ST_*`)
- DOSList ops: `/Users/user/Source/aros-upstream/rom/dos/{makedosentry.c,adddosentry.c,runhandler.c}`
- Mount template: `/Users/user/Source/aros-upstream/workbench/devs/Mountlist` (`HOME:`, `USR:`)
- Host-call mechanism: `/Users/user/Source/aros-upstream/arch/all-hosted/hostlib/`
  (`HostLib_Open/GetInterface/Lock/Unlock`); bootstrap `dlopen/dlsym` in
  `/Users/user/Source/aros-upstream/arch/all-unix/bootstrap/`
- This project's grounding: `NOTES.md` (H10 message ports, H11 device-on-a-file —
  the switched-task host-syscall pattern), `graft/WORKFLOW.md` (boot status, F1/F2),
  `hosted/device.c` (the IORequest→task→`pread/pwrite`→reply spike),
  upstream commit `a68e4c5c` (emul-handler builds on modern macOS).
- External / web (prior art + macOS gotchas):
  - AROS ports list (Darwin = i386/x86_64/ppc, X11, no arm64):
    https://aros.sourceforge.io/introduction/ports.html
  - AROS issue #408 — Intel-Darwin hosted cold-boot crash in `tlsf_freevec`/`lddemon`:
    https://github.com/aros-development-team/AROS/issues/408
  - Host-directory-as-volume design note (independently derived): a per-file text
    sidecar for Amiga metadata + filename escaping, or a hidden index file, are the
    two portable shapes for the same problem; neither uses host xattrs.
  - APFS "bag of bytes" filenames (preserves NFC/NFD, ≠ HFS+ NFD normalization):
    https://mjtsai.com/blog/2017/03/24/apfss-bag-of-bytes-filenames/ ;
    https://eclecticlight.co/2021/05/08/explainer-unicode-normalization-and-apfs/
  - AppleDouble `._` files (only on non-xattr volumes; native APFS/HFS+ keeps xattrs inline):
    https://en.wikipedia.org/wiki/AppleSingle_and_AppleDouble_formats
