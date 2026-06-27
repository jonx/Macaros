# Host volume — share a Mac folder with AROS

> Status: **working & verified on darwin-aarch64 (2026-06-26)**.
> This is the practical "what & how". For the design and the
> implementation spec, see [design.md](design.md) and [spec.md](spec.md).

## What it does

Mounts a real macOS folder as an AROS DOS volume. The folder shows up in AROS as
a normal volume (e.g. `MacRO:` / `MacRW:`), and the standard AmigaDOS commands
operate on the real Mac files:

```
Dir MacRW:                 list the folder
Type MacRW:notes.txt       read a file
Copy MacRW:a.txt MacRW:b   copy within it
Copy MacRW:a.txt RAM:      copy a file out
```

Drop a file into the folder in Finder → it appears in AROS on the next `Dir`.
Save from AROS into a writable mount → it appears in Finder. It is an ordinary,
portable directory the whole time — no disk image, no special container.

**Read-only is the default.** A mounted Mac folder is outside AROS's world, so it
is mounted **read-only** unless you *explicitly* ask for writes. A write to a
read-only volume is refused with `disk is write-protected`; the folder cannot be
changed from inside AROS. Writes are enabled only with our explicit **`;WRITE`
keyword** (see below) — and that keyword is ours, never handed to AROS's `Mount`.

## Quick start

```sh
~/Source/aros-aarch64/graft/run-window.sh            # shares ~/AROS/Shared
~/Source/aros-aarch64/graft/run-window.sh ~/Work     # share a different folder
```

This boots AROS in a Mac window and maps the folder as **two** volumes:

| Volume  | Access      | Notes |
|---------|-------------|-------|
| `MacRO:` | read-only  | listing/reading; writes refused |
| `MacRW:` | read/write | `Copy`/`MakeDir`/`Delete` here land in the Mac folder |

Both auto-mount at boot — no typing. Click the window, then at the `1>` prompt:

```
Dir MacRW:
Copy MacRW:in.dat MacRW:out.dat       (appears in the Mac folder)
MakeDir MacRO:nope                     (refused: disk is write-protected)
Info                                   (lists all volumes + their state)
```

## How to mount — three ways

There are three ways to mount a host folder. Pick by how much control you want.

### 1. The launcher env var (recommended — read-only OR read/write)

`emul-handler` reads the `AROS_HOST_VOLUME` host environment variable at boot and
mounts what it names. The value is one or more `<Volume>:<hostpath>[;WRITE]`
specs, **one per line**:

```sh
# read-only
AROS_HOST_VOLUME="Mac:~/Amiga" ./AROSBootstrap ...

# read/write (note the ;WRITE keyword)
AROS_HOST_VOLUME="Mac:~/Amiga;WRITE" ./AROSBootstrap ...

# several at once (this is exactly what run-window.sh does)
AROS_HOST_VOLUME="MacRO:~/AROS/Shared
MacRW:~/AROS/Shared;WRITE" ./AROSBootstrap ...
```

`~` expands to `$HOME`. The volume name is the part before the first `:`. This is
*our* mount path: we build the device string ourselves, so the `;WRITE` keyword
reaches the handler intact.

### 2. A Mountlist entry (read-only only)

A declarative `DEVS:Mountlist` entry, then `Mount` it from the shell or
Startup-Sequence. **Mountlist host folders are always read-only** — the write
keyword is deliberately not exposed to AROS's `Mount` command (its parser mangles
`;`), so a mountlist mount can never become writable. That is the safe default.

```
MAC:
	FileSystem = emul-handler
	Device     = Mac:~/Amiga
	DOSType    = 0x454D5500
	Activate   = 1
#
```

```
Mount MAC:
Dir MAC:
```

### 3. Programmatic

The same `MakeDosNode` / `AddDosNode(ADNF_STARTPROC)` path with
`FileSysStartupMsg.fssm_Device = "<Volume>:<hostpath>[;WRITE]"`. This is what the
launcher (way 1) and the boot's own `System:` volume use.

### The write keyword

Append one of these to the device string to enable writes (read-only otherwise):

| Keyword | Meaning |
|---------|---------|
| `;WRITE`, `;W`, `;RW` | enable writes |
| `;READONLY`, `;RO`    | explicit read-only (the default; for clarity) |

e.g. `Mac:~/Amiga;WRITE`. A volume that does not carry the keyword stays
read-only, no matter how it was mounted.

## Filenames — long, accented, and special characters

AmigaOS filenames are ISO-8859-1 (Latin-1); macOS is UTF-8 and APFS stores names
as a "bag of bytes" (NFC *or* NFD). The handler bridges all of that, so special
names round-trip in both directions:

- **Accented names work.** A Mac `café.txt` / `grün.txt` is reachable from AROS by
  its Latin-1 name, and a file created from AROS with Latin-1 accents lands as
  UTF-8 on disk.
- **Normalization is handled.** A file stored decomposed (NFD) on APFS is still
  found by its composed (NFC) AROS name — you don't have to care which form the
  Mac used.
- **Characters Latin-1 can't hold** (e.g. `œ`, `€`) appear in AROS as a reversible
  escape `%uXXXX` (and `%UXXXXXX` for astral code points), so they're still
  visible and round-trip back to the real character on the host.
- **Length:** capped at ~107 characters — the AmigaDOS `FileInfoBlock` limit, not
  a Mac one. A longer Mac name is truncated when AROS lists it.

(The bundled NFC tables cover the Latin ranges; other scripts pass through
unchanged. Full Unicode tables are a later upgrade — see spec.md.)

## AmigaOS metadata — comments & protection bits

AmigaOS has metadata POSIX can't hold: the **file comment** and the AmigaOS-only
**protection bits** (Archive/Pure/Script/Hold). These are preserved in a small,
portable **sidecar** file next to each data file:

```
Filenote MacRW:tool "needs RAD:"      sets a comment
Protect  MacRW:tool s ADD             sets the Script bit
List     MacRW:                       shows the flags + comment
```

- The sidecar is `.<name>.amimeta` (a hidden dotfile) — a tiny text file holding
  the comment and the full protection word.
- It is written **only when there is something non-default to keep**, so plain
  files never sprout sidecars and the folder stays clean.
- It is **hidden** from AROS: `Dir`/`List` never show `.amimeta` files.
- It is a **plain text file**, so the folder stays copyable to FAT/zip/another
  machine with the metadata intact (unlike macOS xattrs).
- The `rwx` bits still come from the file's real Unix permissions, so `chmod` on
  the Mac and `Protect` in AROS agree.

## How it works (no disk driver!)

There is **no disk driver and no emulated hardware.** `emul-handler` is an
AmigaDOS *filesystem handler* — the same kind of component as FFS or SFS — but
instead of translating DOS packets into block reads against a device driver, it
translates each `ACTION_*` packet straight into a **macOS libc call**:

```
  AROS:  Dir MacRW:   /   Copy MacRW:a MacRW:b
            │  (DOS packets: EXAMINE_NEXT, FINDOUTPUT, WRITE, …)
            ▼
  emul-handler  ──►  open() readdir() write() rename() …   (libSystem.dylib)
            │                                   via hostlib.resource
            ▼
        macOS  ──►  APFS  ──►  your SSD
```

macOS already owns the hardware and the drivers; AROS just reaches them through
the normal exec/DOS interfaces. "Mounting a folder as a drive" is simply: create
a DOS device node, point it at `emul-handler`, and hand it a host path. The
read-only policy, the `;WRITE` keyword, the charset/normalization bridge, and the
metadata sidecar are all layered on top of that, inside the handler.

## Where the code lives

In the AROS source tree (`aros-upstream`, branch `aarch64-darwin-graft`):

- `arch/all-hosted/filesys/emul_handler/` — portable core: packet dispatch,
  `new_volume` (device-string + `;WRITE` parse), the launcher (`mount_hostvol`),
  the write guard, `ACTION_SET_COMMENT`.
- `arch/all-unix/filesys/emul_handler/` — the POSIX overlay and our new modules:
  `emul_norm.c` (NFC), `emul_charset.c` (Latin-1/UTF-8 + escape),
  `emul_meta.c` (the `.amimeta` sidecar), plus the name/sidecar/case hooks.

Launcher script: [graft/run-window.sh](../../../graft/run-window.sh).

## Status

All of the above is implemented and verified two-sided on hosted AROS (the macOS
side seeds fixtures and re-reads them; AROS drives the operations). See the
"Implementation status" table in [spec.md](spec.md) for the per-requirement
verdicts and the intentional deviations from the original spec.
