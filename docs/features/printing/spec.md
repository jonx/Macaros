# Implementation spec — CUPS-backed AROS printing (printer.device → macOS)

> Status: started (partial) - print-to-PDF host engine built and green ([PRPDF]); AROS printer.device driver blocked at [PR0] · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent,
driver, or otherwise — was read, searched, or consulted in producing it, and any
resemblance to existing implementations is coincidental.** Implement only from this
spec + the approved sources cited by tag: `[PUB]` Apple/CUPS docs / POSIX / published
standards (the CUPS C API, the PostScript DSC and PDF formats), `[AROS]` in-tree AROS
headers and drivers (paths given; APL/LGPL — ours), `[OURS]` this project's spikes
(the H-series, `hosted/*`, `graft/*`). `[DERIVED]` items are independently-derived
requirements flagged for extra verification; each stands solely on its cited
`[PUB]`/`[AROS]`/`[OURS]` justification — implement from that justification, never from
any reference. No identifier name, call sequence, file layout, or algorithm in this
spec derives from any third-party implementation; the `CUPS_*` ABI and the device/
gadget layout are our own.

## Scope

This feature is **not greenfield on the AROS side.** AROS already ships the whole
generic printing stack — `printer.device` + the per-unit driver task, the `PRT:` text
spooler, the graphics-dump engine, the `PrinterExtendedData` plugin model, a working
**PostScript** driver, and the prefs model — and the host-volume mount already lands
AROS files in a real Mac folder. The bulk of the printing logic is **reused
unchanged**; this spec covers only the **macOS-specific new code** (a CUPS host shim, a
spool-sink device, an optional driver, a status gadget) plus the **one aarch64
bring-up fix** that gates all of it.

**In.**
1. The aarch64 `AROS_PRINTER_MAGIC` branch (without it no driver compiles). `[AROS]`.
2. A **`libcups_shim.dylib`** + flat `CUPS_*` C ABI: deliver a spool (drop to host
   folder or submit via libcups), enumerate printers, read job/printer status. `[OURS]`/
   `[PUB]`.
3. A small **`cups.device`** spool-sink (cloned from `printtofile`/`printtotool`) that
   the printer driver's spool targets via the existing `pu_DeviceName` redirection. It
   owns the shim handle. `[AROS]`/`[OURS]`.
4. **MVP delivery with no new driver**: select the in-tree PostScript driver, spool to
   the host volume; assert a valid DSC PostScript file. `[AROS]`.
5. The proper **`DEVS:Printers/CUPS`** driver (cloned from `skeleton`) producing **PDF**
   (to dodge the macOS PS→PDF removal). `[AROS]`/`[PUB]`.
6. A **printer-STATUS gadget** (`Tools/PrinterStatus`, a Zune/MUI app) over
   `cupsGetDests`/`cupsGetJobs`. `[AROS]`/`[PUB]`.

**Decision (the redirection seam).** Hook the **spool device**, not the `PRT:`
handler. `printer.device`'s spool leaves as `CMD_WRITE`s to a configurable backing
device (`pd_PWrite`→`pd_ior0/1`→`<PrtDevName>.device`, `driver.c:218–242,406–421`,
`pd_SyncPrefs` :695–705) `[AROS]`, and the backing device is user-set via
`PrinterUnitPrefs.pu_DeviceName` (`printertxt.h:83`, prefs gadget `editor.c:305`)
`[AROS]`. Setting `pu_DeviceName = "cups"` routes the whole spool through our device
with **zero change to `printer.device` or the driver**. This is the in-tree precedent:
`printtotool`/`printtofile` are exactly such spool-sink devices.

**Decision (format — the Sonoma constraint).** macOS 14 **removed** system PostScript→
PDF conversion (`CGPSConverter` errors; `pstopdf` gone) `[PUB]`. Therefore: the **MVP
delivers and verifies PostScript directly** (no host conversion), and the **proper
driver produces PDF on our side** (a `CGPDFContext` in the shim, or AROS-side PDF) so
libcups gets `application/pdf` and we never invoke a removed PS→PDF step. We do **not**
depend on `cupsfilter` (deprecated, and its PS→PDF leg is the removed one) `[PUB]`.

**Out (non-goals, this spec).** A from-scratch spooler or any change to
`printer.device`'s dispatch; turboprint/hardware-mixing extensions; full PostScript-
RIP render fidelity verification (we assert structure, not pixels); colour management /
ICC profiles (`DRPA_ICCProfile`); live AirPrint discovery beyond what `cupsGetDests`
returns; a human looking at paper (we verify by file-on-disk + numeric assert); the
`PRT:`/`port-handler` rewrite (it is a Layer-1 client, untouched).

## Architecture

Four pieces. The AROS-side modules and the CUPS host shim are joined by a **flat
hand-written C ABI** (ours, ASCII, independently authored); the modules couple to the
spooler through the in-tree printer contracts.

```
AROS side (aarch64, AROS crosstools)                 Host side (Apple toolchain)
┌──────────────────────────────────────┐             ┌────────────────────────────┐
│ printer.device + per-unit driver task │             │ libcups_shim.dylib  [OURS] │
│   (reused unchanged)            [AROS]│             │  · -lcups: enumerate,      │
│        │  pd_PWrite → CMD_WRITE       │  hostlib +  │    submit, jobs, state     │
│        ▼   to <pu_DeviceName>.device  │  H3 host-   │  · CGPDFContext (PDF gen)  │
│  PostScript driver (MVP)        [AROS]│  call ABI   │  · drop-to-host-folder     │
│  OR DEVS:Printers/CUPS (proper) [AROS]│ ──────────► │    (host-volume fallback)  │
│        │   spool bytes               │  CUPS_* ABI  └────────────────────────────┘
│        ▼                              │ ◄──────────         ▲
│  cups.device  (spool sink)      [OURS]│  job id / status    │ owns the hostlib handle
│   · CMD_WRITE → host file/submit ─────┼─────────────────────┘
│  Tools/PrinterStatus (MUI)      [OURS]│──── cups_list_printers/jobs ──► shim
└──────────────────────────────────────┘
```

- **Host shim** `[OURS]` — native arm64 C (`.c`), host clang (NOT AROS crosstools),
  peer of `hosted/coreaudio/coreaudio_shim.c`. Owns every CUPS object + (for PDF) the
  CGPDFContext. Exposes the `CUPS_*` ABI; pulls **no** AROS headers. Reached via
  `hostlib.resource` (`OpenResource("hostlib.resource")` → `HostLib_Open` of the dylib
  → `HostLib_GetPointer` per symbol) `[AROS]`, exactly as
  `hosted/cocoametal/abi_test.c:13–21` models and the coreaudio/clipboard/bsdsocket
  shims do `[OURS]`.
- **`cups.device`** `[OURS]`/`[AROS]` — the single owner of the shim handle; the only
  AROS module that reaches host code. Cloned from `printtofile`/`printtotool`.
- **Driver(s)** `[AROS]` — the in-tree PostScript driver (MVP, unchanged) or a cloned
  `CUPS` driver (proper). They emit a format; they do not touch host code.
- **Status gadget** `[OURS]` — a Zune/MUI app reading the shim. (It may open the shim
  via the same `cups.device` or a thin direct hostlib open — decision below.)
- Spike-phase paths: shim in `hosted/cups/`; at graft, `cups.device` lands in
  `workbench/devs/cups/`, the driver in `workbench/printers/cups/`, the gadget in
  `workbench/tools/PrinterStatus/`.

## The aarch64 bring-up fix (R-MAGIC) — `[AROS]`

`compiler/include/aros/printertag.h:16–30` defines `AROS_PRINTER_MAGIC` for
`__mc68000`/`__i386__`/`__x86_64__`/`__arm__`/`__ppc__`/`__riscv`, each a real
"return" opcode, then `#else #error`. **There is no `__aarch64__` branch**, so the
`AROS_PRINTER_TAG` macro (`printertag.h:32–43`) fails to compile for this target, and
`driver.c:757` (`ps_runAlert == AROS_PRINTER_MAGIC`) has nothing to match `[AROS]`.

- **Requirement R-MAGIC.** Add `#elif defined(__aarch64__)` with the AArch64 `ret`
  encoding `0xd65f03c0` `[PUB]` (A64 `RET` = `1101 0110 0101 1111 0000 0011 1100 0000`).
  The value is stored into `ps_runAlert` (`ULONG`, `prtbase.h:235`) and is also the
  leading word of the `.tag.printer` section, so it must be a harmless real instruction
  if executed (the other arches' values all are) `[AROS]`. **UNVERIFIED:** the byte/
  endianness layout — confirm the 32-bit `ps_runAlert` compare at `driver.c:757` reads
  back exactly `0xd65f03c0` on little-endian AArch64, and that placing a `ret` as the
  section's first instruction is benign (it is never called; it is a sentinel). `[DERIVED]`
  the choice of `ret` follows the established per-arch pattern in the same header;
  implement from that pattern + the published A64 `RET` encoding, not from any reference.

This is **[PR0]** and is the prerequisite for every later marker (the PostScript driver,
skeleton, and our modules all `#include <aros/printertag.h>`).

## The portable AROS contracts this binds to (grounded, `[AROS]`)

Restated from the headers/source so the implementer needs no third-party source:

- **The spool leaves as `CMD_WRITE` to a backing device.** A driver's output goes
  through `pd->pd_PWrite(data,len)` (`driver.c:218`), which `SendIO`s `CMD_WRITE` to
  `pd_ior0`/`pd_ior1` (`driver.c:226–238`), opened in `pd_Init` against
  `<PrtDevName>.device` (`driver.c:406–421`). `PrtDevName` is chosen by `pd_SyncPrefs`
  from `pp_Unit.pu_DeviceName` (or parallel/serial/printtofile defaults)
  (`driver.c:695–705`) `[AROS]`. **A device named in `pu_DeviceName` is the spool
  target.**
- **The driver plugin contract.** `PrinterSegment {ps_NextSegment, ps_runAlert(=magic),
  ps_Version, ps_Revision, ps_PED}` (`prtbase.h:232`); `PrinterExtendedData` hooks
  `ped_Init/Expunge/Open/Close` + `ped_Render(ct,x,y,status)` (the `PRS_*` raster state
  machine) + `ped_DoSpecial/ConvFunc/DoPreferences/CallErrHook` + the
  `ped_Commands[aRIS..aRAW]` table + geometry fields (`prtbase.h:166–200`) `[AROS]`.
  Declared with `AROS_PRINTER_TAG(PED, version, revision, …)` (`printertag.h:32–43`).
  `PRS_*` states (`printertag.h:45–120`): `PRS_PREINIT` (set DPI/page from prefs),
  `PRS_INIT` (alloc buffer), `PRS_TRANSFER` (one colour row), `PRS_FLUSH`, `PRS_CLEAR`,
  `PRS_NEXTCOLOR`, `PRS_CLOSE`.
- **The spool-sink device shape.** A minimal exec device: `Open` sets `io_Unit`, `Init`
  `OpenLibrary`s deps, `BeginIO` switches on `io_Command` (`CMD_WRITE` →
  consume `io_Data`/`io_Length`; `CMD_FLUSH`/`CMD_UPDATE` no-op; else `IOERR_NOCMD`),
  `Close` finalises the unit. The two in-tree templates are byte-for-byte this shape:
  `printtofile.c` (`CMD_WRITE` → `Write(file)` :103) and `printtotool.c` (`CMD_WRITE` →
  `Write(pipe)` :136, with `OpenTool` doing `PIPE:*` + `SystemTags` :24–49) `[AROS]`.
- **The prefs field + gadget.** `PrinterUnitPrefs.pu_DeviceName[DEVICENAMESIZE=32]`
  (`printertxt.h:83`), surfaced by the prefs editor's String gadget (`editor.c:305`),
  whose `DEVS:Printers` Dirlist (`editor.c:275`) picks `pp_Txt.pt_Driver` `[AROS]`.
- **The MUI app idiom.** The prefs editor is a `MUIC_PrefsEditor` subclass
  (`editor.c:211–213`, `CycleObject`/`StringObject`/`DirlistObject`); SysMon is a
  `muimaster` app with a refresh `Hook` and the `MUIM_Application_NewInput` loop
  (`SysMon/main.c`) `[AROS]`. The status gadget follows the SysMon idiom (dynamic,
  refresh-driven), **not** the PrefsEditor idiom (static save/use/cancel).

## The C ABI (`cups_shim.h`) — `[OURS]` shape, `[PUB]` underneath

Hand-authored, neutral. Verbs mirror the *role* of the coreaudio shim's opaque-handle
API (`ca_open`/`ca_*`/`ca_close`) `[OURS]`; CUPS objects under the hood are `[PUB]`. The
shim is the only contact surface (no AROS headers in the shim; no CUPS headers on the
AROS side).

```c
/* opaque host-side job handle */
typedef struct CupsJob CupsJob;

/* status records the shim fills (flat, fixed-size, ABI-stable) */
typedef struct {
    char  name[128];      /* cups_dest_t.name                         [PUB] */
    int   isDefault;      /* cups_dest_t.is_default                   [PUB] */
    int   state;          /* printer-state numeric (3 idle/4 proc/5 stop) [PUB] */
    char  reasons[256];   /* printer-state-reasons                    [PUB] */
} CupsPrinterInfo;

typedef struct {
    int   id;             /* cups_job_t.id                            [PUB] */
    char  title[128];     /* cups_job_t.title                         [PUB] */
    int   state;          /* ipp_jstate_t (3 pending .. 9 completed)  [PUB] */
} CupsJobInfo;

/* DELIVERY — owned/called only by cups.device (single caller).
   open a streaming job. dest may be NULL/"" → default printer (cupsGetNamedDest);
   format is one of "application/pdf" / "application/postscript" (CUPS_FORMAT_*).
   In drop-to-folder mode dest is ignored and bytes accumulate to a host temp file. */
CupsJob *cups_open_doc(const char *dest, const char *title, const char *format);
/* append spool bytes; non-blocking from AROS's view (request/reply under HostLib_Lock).
   returns bytes written, <0 on error. */
int      cups_write(CupsJob *, const void *buf, int len);
/* finish: submit (cupsFinishDestDocument / cupsPrintFile) and return the CUPS job id,
   or in drop mode move the temp file into destDir and return 0. <0 on error. */
int      cups_finish(CupsJob *);
void     cups_abort(CupsJob *);   /* cancel/cleanup without submitting */

/* drop-to-folder configuration (the headless, queue-free oracle path) */
int      cups_set_drop_dir(const char *hostDir); /* NULL ⇒ submit mode (default) */

/* ENUMERATION + STATUS — called by the status gadget (and tests). non-mutating. */
int      cups_list_printers(CupsPrinterInfo *out, int max); /* count, <0 on error */
int      cups_list_jobs(const char *dest, CupsJobInfo *out, int max);

/* OFFLINE PDF generation for verification / the PDF driver path.
   draw nothing here from CUPS — generate a PDF on disk via CGPDFContext from a
   provided raster or from already-PDF source; returns 0 on success. [PUB] CoreGraphics.
   (NOT a PS->PDF converter — macOS removed that; see Scope.) */
int      cups_raster_to_pdf(const char *pdfPath, const void *rgba, int w, int h);

/* abi/version handshake, like ca_* / cm_abi_version */
unsigned cups_abi_version(void);
```

The header is shared source, hand-written, independent work. Submission uses the
streaming quartet `cupsCreateJob`/`cupsStartDestDocument`/`cupsWriteRequestData`/
`cupsFinishDestDocument` or `cupsPrintFile` `[PUB]`; enumeration uses
`cupsGetDests`/`cupsGetNamedDest` + `cupsGetOption("printer-state"/"printer-state-
reasons", …)` `[PUB]`; status uses `cupsGetJobs(&jobs, dest, 0, CUPS_WHICHJOBS_ALL)`
over `cups_job_t`/`ipp_jstate_t` `[PUB]`. **No varargs in the hot path** (any logging
goes through the H3 shim `[OURS]`).

## Concurrency model — the easy direction (`[OURS]`, contrast with audio)

Unlike CoreAudio (a host RT callback that *calls AROS*), **every CUPS call here is
pull/request-reply**: the AROS task calls the shim and blocks for the reply, serialised
under `HostLib_Lock` — the exact H11 device pattern (`hosted/device.c`: `BeginIO` →
device task → host syscall → reply) `[OURS]`. There is **no foreign-thread / RT-callback
hazard**, no SPSC ring, no signal-mask guard needed for a CUPS thread (libcups does its
IPP I/O synchronously on the calling thread). This is why printing is a softer host
target than audio.

- **R-LOCK.** Bracket each shim host call from AROS with the standard
  `HostLib_Lock(); …; AROS_HOST_BARRIER; HostLib_Unlock();` discipline (the
  emul-handler/coreaudio precedent) `[AROS]`/`[OURS]`. One `cups.device` process
  serialises all printing host calls; the status gadget's reads are likewise lock-
  bracketed.
- **R-NOBLOCK-UI.** `cups_list_*` may take time (network IPP); the status gadget calls
  them on its own task (refresh hook / button), never from the input handler, so the UI
  does not wedge — the SysMon refresh-hook pattern `[AROS]`.

## `cups.device` — the spool sink (`[OURS]`/`[AROS]`)

A new device `workbench/devs/cups/cups.c`, cloned from `printtofile.c`/`printtotool.c`
`[AROS]`:

- **`Init`** — `OpenResource("hostlib.resource")`, `HostLib_Open("libcups_shim.dylib")`,
  fill a `CUPS_*` function-pointer table via `HostLib_GetPointer` (mirror the
  coreaudio/clipboard bridge load) `[OURS]`; `OpenLibrary("dos.library")`. Read the
  target from a config: a `dest` name (which macOS printer) and/or a drop directory
  (a path on the mounted host volume) — from a small `cups.device` config file or env
  (`AROS_CUPS_DEST` / `AROS_CUPS_DROPDIR`), the host-volume launcher precedent `[OURS]`.
- **`Open(unit)`** — set `io_Unit`; per unit, `cups_open_doc(dest, title, format)` lazily
  on first `CMD_WRITE` (mirror `printtotool`'s lazy `pu_OpenFile` on first BeginIO,
  `printtotool.c:121`) `[AROS]`. `format` defaults to `application/postscript` (the PS
  driver) or `application/pdf` (the CUPS driver), set by config/unit.
- **`BeginIO`** — switch on `io_Command` (`printtofile.c:97–115` shape) `[AROS]`:
  - `CMD_WRITE` → `cups_write(job, io_Data, io_Length)`; set `io_Actual`/`io_Error`.
  - `CMD_FLUSH`/`CMD_UPDATE` → no-op (`io_Error = 0`).
  - else → `IOERR_NOCMD`.
- **`Close`** — `cups_finish(job)` (submit, or in drop mode move the temp into the host
  folder), record the job id; release. (Driver `Close` → spooler `pd_Close` →
  `CloseDevice(pd_ior*)` naturally drives our `Close`.)
- **Decision.** All host code lives **here**, not in the driver. A printer driver can
  load/expunge repeatedly and is awkward to hang a `hostlib` handle on; the device is a
  stable single owner. The driver only chooses the *format* it emits; the device chooses
  *where it goes*. `[DERIVED]` (justification: driver lifecycle vs a single long-lived
  device owner; the in-tree spool-sink devices already own external handles —
  `printtotool` owns a pipe + spawned tool).

## MVP — PostScript out, no new driver (`[AROS]`)

1. R-MAGIC done → the in-tree PostScript driver compiles.
2. Configure a printer unit: `pp_Txt.pt_Driver = "PostScript"`,
   `pu_DeviceName = "printtofile"` (zero-host-code) or `"cups"` (our sink in drop mode).
   For `printtofile`, point its output (the ASL save path / our config) at a path on the
   mounted **`MacRW:`** host folder `[OURS]`.
3. Print: `PrintFiles` a known document, or `Copy doc PRT:` (text → unit 2). The PS
   driver emits DSC PostScript via `pd_PWrite` `[AROS]`.
4. The bytes land in the Mac folder (host-volume mount, already implemented) `[OURS]`.

**Asserted artifact** (no converter, no queue): a valid DSC PostScript file —
`%!PS-Adobe` header (`postscript.c:398`), one `%%Page:` per input page
(`postscript.c:352`), `showpage`/`%%EOF` (`postscript.c:411,572`) `[AROS]`. **[PR2].**

## Proper path — `DEVS:Printers/CUPS` driver, PDF-direct (`[AROS]`/`[PUB]`)

`workbench/printers/cups/cups.c`, cloned from `skeleton.c` `[AROS]`:

- `AROS_PRINTER_TAG(PED, 44, 0, .ped_PrinterName = "CUPS", .ped_PrinterClass =
  PPC_COLORGFX | PPCF_EXTENDED, .ped_ColorClass = PCC_YMCB, …)` (skeleton :311) `[AROS]`.
- `ped_Render` (the `PRS_*` machine) produces a **raster** that the *device/shim* turns
  into **PDF** (`cups_raster_to_pdf` via `CGPDFContext`) `[PUB]`, OR emits minimal valid
  PDF directly. The text path (`ped_Commands`/`ConvFunc`) similarly targets PDF text.
  **We do NOT emit PostScript-for-host-conversion** (Sonoma removed PS→PDF) `[PUB]`.
- `ped_DoPreferences` maps the AROS unit → a `cups_dest_t` name (which macOS printer);
  the device submits with `CUPS_FORMAT_PDF` `[PUB]`.
- `DriverData.h`, `mmakefile.src` cloned from skeleton (`%build_module_simple
  modtype=printer modname=CUPS`) `[AROS]`.

**Asserted artifact:** a valid PDF — `%PDF-` magic, page count via trailer `/Count` or
`/Type /Page`, and for a text job the **extracted text** contains the known sentinel
`[PUB]` (PDF format). **[PR4].**

## Printer-STATUS gadget — `Tools/PrinterStatus` (`[OURS]`/`[AROS]`)

A `muimaster` app modelled on SysMon (`SysMon/main.c`) `[AROS]`:

- **Layout.** A `ListObject` of printers (name · default · state) and a `ListObject` of
  jobs (id · title · state), a Refresh button, optional auto-refresh timer hook (the
  SysMon refresh-hook idiom) `[AROS]`.
- **Data.** On refresh: `cups_list_printers(infos, N)` → rows (state int → text:
  3 idle / 4 processing / 5 stopped) `[PUB]`; `cups_list_jobs(selectedDest, jobs, N)` →
  rows (`ipp_jstate_t` → Pending/Held/Processing/Stopped/Canceled/Aborted/Completed,
  values 3..9) `[PUB]`.
- **Shim access.** **Decision:** the gadget opens the shim **directly** via
  `hostlib.resource` (read-only enumeration; no spool ownership needed), separate from
  `cups.device`. Lock-bracket each call (R-LOCK). Justification: status is read-only and
  the gadget shouldn't depend on a printer unit being open. `[DERIVED]`.
- **Decision: standalone Tools app, not a Wanderer prefs panel.** Status is dynamic/
  observational (SysMon-shaped); prefs is static save/use/cancel (PrefsEditor-shaped).
  Conflating them fights the PrefsEditor model. The prefs editor may *launch* it.

## Verification (unattended — `[OURS]` H7/H11 discipline)

No human looks at paper; **no macOS print dialog, no TCC prompt** (libcups submit/
enumerate is not a documented TCC-protected operation `[PUB]`; host-folder writes are
ordinary file I/O by the AROS process, the host-volume posture already verified
`[OURS]`). The oracle is **a file on disk asserted numerically** + **queue-state reads**,
never an ear/eye on output.

- **PostScript oracle (MVP):** DSC structure — `%!PS-Adobe`, one `%%Page:` per input
  page, `showpage`, `%%EOF`; byte size in range. Catches dead/empty/truncated spool. No
  converter needed.
- **PDF oracle (proper):** a tiny host PDF probe — `%PDF-` magic, page count
  (`/Count` / `/Type /Page`), extracted text contains the sentinel for a text job. The
  strongest oracle ("the document actually printed right").
- **Queue-state oracle:** `cups_list_jobs`/`cups_list_printers` return the submitted job
  (expected title, sane `ipp_jstate_t`) and the default printer + state. Works against a
  paused/virtual queue (no paper needed).
- **Two-sided (H11 rule):** the harness creates the fixture on one side and asserts on
  the other (known input in AROS → print → re-read the host artifact independently).
- **Counters:** the shim exposes `cups_abi_version()` (load handshake, like
  `ca_abi_version`/`cm_abi_version`) `[OURS]`; non-zero error returns fail the marker.

The known **test document** is a 2-page text file with a fixed sentinel (page-count +
extracted-text pin down "right pages, right content, not empty"); for graphics, a small
solid-colour raster (assert non-blank, right dimensions).

**Markers** (one host binary / one booted assertion per marker, `[PR?]` PASS/FAIL via
`harness/run-hosted.sh` and `graft/printing-smoke`, clean-exit on PASS):

- **[PR0] aarch64 unblock.** R-MAGIC → in-tree PostScript + skeleton compile/link for
  aarch64-darwin; the magic word disassembles to `ret`. PASS = build map shows the
  `PostScript` printer module, no `#error`. `[PR0]`.
- **[PR1] host shim, asserted (bare spike).** `hosted/cups/c_test.c` drives the shim on
  a known PDF fixture: `cups_open_doc`/`cups_write`/`cups_finish` (drop mode → file in a
  temp dir; submit mode → job id), plus `cups_list_printers`/`cups_list_jobs` ≥ 0. PASS
  = artifact on disk (size + `%PDF-` if PDF). Built as `build/host-cups` and the dylib
  ABI proof `cups-abi` (dlopen boundary), mirroring `coreaudio-abi`. `[PR1]`.
- **[PR2] AROS PostScript → host folder (MVP, no new driver).** Boot AROS; PS driver on
  a unit spooling to `MacRW:`; print a known doc. PASS = harness re-reads the host
  folder, asserts valid DSC PS (page count matches). Thesis end-to-end, zero macOS-
  specific AROS code. `graft/printing-smoke`. `[PR2]`.
- **[PR3] `cups.device` sink in AROS.** Build `cups.device`, set as `pu_DeviceName`;
  driver `CMD_WRITE`s flow through it to the shim. PASS = a print yields, host-side,
  either a submitted job (`cups_list_jobs` shows it) or a dropped file — asserted by
  re-reading host state. `[PR3]`.
- **[PR4] `DEVS:Printers/CUPS` driver, PDF-direct.** Cloned driver emits PDF, submitted
  `CUPS_FORMAT_PDF`. PASS = valid PDF (magic + page count + extracted text). Dodges the
  Sonoma PS→PDF removal. Rides the graft. `[PR4]`.
- **[PR5] printer-STATUS gadget.** Build `Tools/PrinterStatus`; drive headless via
  `aros-ctl`. PASS = with a known queue/job state created Mac-side, the gadget's lists
  show the default printer + state and the test job's title + state, asserted via the
  shim counters / a debug dump the gadget prints (not pixels). Rides the control
  harness. `[PR5]`.

## Build / integration

- `libcups_shim.dylib` links `-lcups` (+ `CoreFoundation`; + `CoreGraphics` for the PDF
  path); built with host clang `-arch arm64`, `-install_name @rpath/libcups_shim.dylib`,
  `-exported_symbols_list hosted/cups/cups.exports` (the `_cups_*` symbols), codesigned
  (ad-hoc fine for spikes), loaded via `hostlib.resource`. Mirror the `coreaudio-dylib`
  Makefile rule exactly. **UNVERIFIED:** the on-disk path of `libcups.2.dylib` and any
  entitlement for the `dlopen`'d shim — confirm vs the existing `harness/run.sh` /
  `run-window.sh` path that already deploys/signs `libcoreaudio.dylib`/
  `libpasteboard.dylib`.
- Deploy like the audio shim: `graft/aros-ctl run` / `run-window.sh` copy
  `build/libcups_shim.dylib` → `~/lib/`; `graft/deploy-check` hashes it;
  `make-aros-app.sh` bundles it in `Macaros.app/Contents/Frameworks/`.
- Spikes compile to Mach-O via the existing `Makefile` pattern (`make hosted-cups` →
  `build/host-cups`; `make cups-abi`); `harness/run-hosted.sh '[PR?] …'` greps the
  marker → uniform `result=(PASS|FAIL)`. `graft/printing-smoke` runs the booted-AROS
  legs and stores evidence under `run/darwin-aarch64/`.
- The AROS-side modules (`cups.device`, `CUPS` driver, `PrinterStatus`) build with the
  **AROS crosstools** (`workbench-devs-quick` / `workbench-printers` mmake), **not** host
  clang. The shim must not link/include AROS headers; the AROS side must not include CUPS
  headers — the `cups_shim.h` ABI is the only contact surface.

## Open questions / UNVERIFIED

- The exact AArch64 `ps_runAlert` byte/endianness layout vs `driver.c:757` (R-MAGIC) —
  confirm `0xd65f03c0` reads back and the section `ret` is benign. [PR0].
- `libcups.2.dylib` on-disk path + codesign/entitlement for the `dlopen`'d shim —
  confirm vs `run.sh`/`run-window.sh`.
- PDF generation: CGPDFContext-in-shim from a raster vs minimal-PDF-from-`ped_Render` —
  default CGPDFContext (supported, non-deprecated); decide at [PR4].
- CI queue strategy: drop-to-host-folder (hard file oracle, no queue) vs a CUPS test
  queue (`lpadmin -p test -E -v file:/dev/null`) for the submit path — default drop-to-
  folder for the artifact assertion, submit as a softer "job enqueued" check.
- Whether a real PS RIP renders the in-tree PS driver's custom prolog faithfully
  (`postscript.c:398–531`) — irrelevant to the loop (we assert structure), a human-facing
  nicety. UNVERIFIED.
- `cupsGetDest2` does **not** exist in the public header — use `cupsGetNamedDest`
  (server query) / `cupsGetDest` (array search). `[PUB]`.
- Whether the status gadget should open the shim directly or via `cups.device` — default
  direct (read-only enumeration).

## Provenance summary

`[PUB]` CUPS C API (OpenPrinting `cups/cups.h`, `cups/ipp.h`; CUPS Programming Manual):
`cupsGetDests`/`cupsGetNamedDest`/`cupsGetDest`, `cups_dest_t {name,instance,is_default,
num_options,options}`, `cupsGetOption("printer-state"/"printer-state-reasons")`,
`cupsPrintFile`, `cupsCreateJob`/`cupsStartDestDocument`/`cupsWriteRequestData`/
`cupsFinishDestDocument`, `CUPS_FORMAT_PDF`/`CUPS_FORMAT_POSTSCRIPT`, `cupsGetJobs`/
`cups_job_t`/`ipp_jstate_t` (`IPP_JSTATE_PENDING=3 … COMPLETED=9`); Apple macOS 14
release note (bug 110019863) — PostScript→PDF removal (`CGPSConverter` errors, `pstopdf`
gone); Apple Core Graphics `CGPDFContextCreateWithURL` (supported PDF generation); the
PostScript DSC + PDF container formats; the A64 `RET` encoding `0xd65f03c0`; POSIX file
I/O. ·
`[AROS]` `workbench/devs/printer/` (`printer.c`, `driver.c` — LoadSeg+magic :740–757,
`pd_DriverTask` :526–667, `pd_PWrite`→`CMD_WRITE` :218–242, `pd_Init` :406–421,
`pd_SyncPrefs` :695–705 — `text.c`, `gfx.c`, `mmakefile.src`, `printer.conf`),
`compiler/include/devices/prtbase.h` (`PrinterData`, `PrinterExtendedData` :166–200,
`PrinterSegment` :232), `compiler/include/aros/printertag.h` (`AROS_PRINTER_MAGIC`
:16–30 — **no aarch64**, `AROS_PRINTER_TAG` :32–43, `PRS_*` :45–120),
`compiler/include/devices/printer.h` (`PRD_*`, `IODRPReq`, `PDERR_*`),
`workbench/printers/skeleton/skeleton.c` (clone-me driver :311–733),
`workbench/printers/postscript/postscript.c` (DSC PS emission :352–905),
`workbench/devs/printtotool/printtotool.c` (`PIPE:*`+`SystemTags` :24–49, `CMD_WRITE`
:136) + `workbench/devs/printtofile/printtofile.c` (`CMD_WRITE`→file :103),
`workbench/devs/DOSDrivers/PRT` + `workbench/fs/port/port-handler.c` (:222,247,289),
`compiler/include/prefs/printertxt.h` (`pu_DeviceName` :83), `workbench/prefs/printer/
editor.c` (`PrefsEditor` subclass + `pu_DeviceName` gadget :305, `DEVS:Printers` Dirlist
:275), `workbench/system/SysMon/main.c` (muimaster refresh-hook app),
`workbench/tools/{InitPrinter,PrintFiles,GraphicDump}.c`, `arch/all-unix/bootstrap/
hostlib.h` + `arch/all-hosted/hostlib/` (`HostLib_Open/GetPointer/Lock`). ·
`[OURS]` `hosted/coreaudio/{coreaudio_shim.c,.h,coreaudio.exports}` + the
`coreaudio-dylib`/`coreaudio-abi` Makefile rules (the dylib-shim pattern),
`hosted/cocoametal/abi_test.c:13–21` (the `hostlib.resource` load path),
`hosted/clipboard/`, `hosted/bsdsocket/` (peer shims), `hosted/device.c` (H11
IORequest→device task→host→reply), `hosted/abishim.S` (H3 variadic ABI),
`docs/features/host-volume/` (the `MacRW:` delivery path, already implemented),
`harness/run-hosted.sh` + `graft/{aros-ctl,deploy-check,make-aros-app.sh}` + a new
`graft/printing-smoke`. ·
`[DERIVED]` independently-derived points flagged for extra verification:
(a) the aarch64 `ret` magic value follows the header's per-arch pattern [R-MAGIC];
(b) keeping all host code in a single `cups.device` sink (not the driver) rests on the
driver-lifecycle-vs-stable-owner argument + the in-tree spool-sink devices owning
external handles; (c) PDF-direct (not host PS→PDF) rests wholly on the published macOS
PS→PDF removal `[PUB]` + the CG PDF-generation API `[PUB]`; (d) the status gadget is a
SysMon-shaped Tools app, not a PrefsEditor panel, on the dynamic-vs-static argument
`[AROS]`. No third-party code, identifiers, or call sequence used.
