# Printing — host-backed output for AROS (printer.device / PRT: via CUPS)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

Give the hosted AROS real printing on the Mac. AROS already has a complete generic
printing stack — `printer.device` with the `PRT:` text spooler, the graphics-dump
path, a per-driver plugin model, and a **working PostScript driver**. What's missing
is the last hop: getting the rendered spool *off* AROS and onto a real macOS print
queue (or onto a PDF on disk). We write that hop.

Why it's the right shape: it's the printing instance of the standing thesis —
*"macOS owns the drivers; AROS reaches them via standard exec I/O."* The Mac owns the
printers through **CUPS**; AROS reaches CUPS through the printer-driver plugin
interface (the `PrinterExtendedData` / `ped_*` hooks) and the spool-redirection seam
(`pd_PWrite` → `CMD_WRITE` to a configurable backing device). It re-uses the host-call
boundary already de-risked (H3's Apple-variadic ABI shim `hosted/abishim.S`, the
`hostlib.resource` symbol-resolution mechanism), the device/IORequest pattern proven
by H11 (`hosted/device.c`), and — crucially — the **host-volume mount** that already
lands AROS files in a real Mac folder. PostScript out of AROS plus a Mac folder is
already a complete delivery path with zero new host code.

The genuinely *new* design points this feature forces — and the reason it earns a
doc — are two:

1. **Where to cut the redirection.** AROS printing has three candidate seams (a fake
   spool *device*, a real printer *driver*, the `PRT:` *handler*). Picking the cheap
   one for the first win and the deep one for the proper path, without rewriting
   `printer.device`, is the bulk of the Design section.
2. **CUPS is being hollowed out under us.** macOS 14 (Sonoma) **removed** system
   PostScript→PDF conversion (`CGPSConverter` now errors, `pstopdf` gone), and CUPS
   drivers/filters are deprecated CUPS-wide in favour of driverless IPP. Our natural
   hand-off format (PostScript, the one driver AROS ships) is exactly what modern
   macOS stopped converting. The design has to be honest about that and route around
   it (PDF-first, or submit PS straight to a real PS/IPP queue, never lean on a
   PS→PDF step that no longer exists).

## Does it already exist?

**On the AROS side: almost entirely — the stack is there, only the macOS backend is
missing.** Evidence (all `/Users/user/Source/aros-upstream`):

- **The generic spooler.** `workbench/devs/printer/` builds `printer.device`
  (`mmakefile.src` `files = printer driver prefs gfx text`,
  `%build_module mmake=workbench-devs-printer modtype=device`). `printer.c` is the
  thin "system" device (unit −1, no real commands — `printer.c:109–113` just returns
  `IOERR_NOCMD`); the real per-unit work is in `driver.c`: it `LoadSeg`s a driver
  from `DEVS:Printers/<name>` (`driver.c:745–747`), validates it against
  `AROS_PRINTER_MAGIC` (`driver.c:757`), spins a per-unit driver task
  (`pd_DriverTask`, `driver.c:526`), and dispatches `CMD_WRITE` / `PRD_RAWWRITE` /
  `PRD_DUMPRPORT*` to the driver's hooks (`driver.c:581–657`). `text.c` and `gfx.c`
  hold the ANSI-text and raster-dump engines.
- **The `PRT:` device exists.** `workbench/devs/DOSDrivers/PRT` mounts `port-handler`
  with `Startup = 2`; `workbench/fs/port/port-handler.c` defaults its target to
  `"printer.device"` (`:222,247`) and `OpenDevice()`s it (`:289`). So `Copy file PRT:`
  already flows text into `printer.device` unit 2.
- **A working PostScript driver.** `workbench/printers/postscript/postscript.c` emits
  DSC-conformant PostScript — `%!PS-Adobe-2.0` header (`:398`), `%%Page:` (`:352`),
  `colorimage` raster (`:430`), `showpage`/`%%EOF` (`:411,572`) — via the same
  `pd->pd_PWrite` path every driver uses. **PostScript is the natural macOS hand-off
  format**, and AROS already produces it.
- **The driver template.** `workbench/printers/skeleton/skeleton.c` is the documented
  clone-me driver: it shows the full `PrinterExtendedData` declaration
  (`AROS_PRINTER_TAG(PED, 44, 0, …)`, `:311`), the `ped_Init/Open/Close/Render/
  DoSpecial/ConvFunc/DoPreferences` hook set, and the `pd_PWrite`-based output idiom
  (`:368–400`).
- **The redirection precedent — the key one.** `workbench/devs/printtotool/
  printtotool.c` is a *spool sink device*: `OpenTool()` opens `PIPE:*`
  (`printtotool.c:24`), gets its name (`NameFromFH`), and `SystemTags()`es an
  arbitrary C: tool with the pipe as `SYS_Input` (`:29–32`); its `BeginIO` just
  `Write()`s `CMD_WRITE` data into the pipe (`:136`). Its sibling
  `workbench/devs/printtofile/printtofile.c` writes the spool to a plain file
  (`:103`). **These are the in-tree precedent for "pipe printer output to a host
  helper," and they are devices the driver's spool can target with zero change to
  `printer.device`.**
- **The prefs editor + the redirection field.** `workbench/prefs/printer/editor.c` is
  a Zune `PrefsEditor` subclass; it exposes a **`pu_DeviceName` String gadget**
  (`editor.c:305`, field `PrinterUnitPrefs.pu_DeviceName[DEVICENAMESIZE=32]`,
  `compiler/include/prefs/printertxt.h:83`). `driver.c`'s `pd_SyncPrefs` copies that
  name into `PrtDevName` (`driver.c:704`) and `pd_Init` opens `<PrtDevName>.device`
  (`driver.c:409–421`). **So the spool backing device is already user-configurable —
  setting `pu_DeviceName = "cups"` routes the spool to a `cups.device` we write, no
  source change to the spooler.**
- **The tools.** `workbench/tools/{InitPrinter.c,PrintFiles.c,GraphicDump.c}` drive
  the device (open `printer.device`, `CMD_WRITE` / `PRD_DUMPRPORT`) — ready-made
  unattended exercisers.

**On the macOS side: nothing exists in this repo.**
`grep -rniE 'cups|cupsfilter|libcups|printer' /Users/user/Source/aros-aarch64/hosted`
returns nothing; there is no `libcups_shim.dylib`, no `cups.device`, no STATUS gadget.
The host shims that exist (`hosted/coreaudio/`, `hosted/clipboard/`,
`hosted/bsdsocket/`, `hosted/cocoametal/`) are the pattern to mirror, not printing.

**The real porting gap (concrete, grounded).** `compiler/include/aros/printertag.h`
defines `AROS_PRINTER_MAGIC` per-arch with branches for `__mc68000` / `__i386__` /
`__x86_64__` / `__arm__` / `__ppc__` / `__riscv` and an `#else #error` fallthrough
(`printertag.h:16–30`). **There is no `__aarch64__` branch**, so *any* printer driver
(postscript, skeleton, or ours) fails to compile for aarch64-darwin with
`#error AROS_PRINTER_MAGIC is not defined for your architecture`. The magic is the
first instruction of the driver segment, used as a "this is a printer driver" sentinel
checked at load (`driver.c:757`); on AArch64 it should be a `RET` opcode
(`0xd65f03c0`, **UNVERIFIED** — pick the encoding and confirm it disassembles to `ret`
and is harmless if executed). This one-line fix gates everything.

**External prior art (web-grounded, *not* in the AROS tree).** The web sharpens the
macOS-side constraints; it surfaced no usable code, but four facts matter and are
load-bearing for the design:

- **libcups is the macOS print client and ships with the OS.** `cups/cups.h` (the
  published CUPS API) defines `cupsGetDests`/`cupsGetNamedDest`/`cupsGetDest`
  (enumerate, with `cups_dest_t {name,instance,is_default,num_options,options}`),
  `cupsPrintFile(name,filename,title,num_options,options)` → job id, the streaming
  `cupsCreateJob`/`cupsStartDestDocument`/`cupsWriteRequestData`/
  `cupsFinishDestDocument` quartet, the format constants `CUPS_FORMAT_PDF =
  "application/pdf"` and `CUPS_FORMAT_POSTSCRIPT = "application/postscript"`, and for
  status `cupsGetJobs` (`cups_job_t {id,dest,title,state,…}`) + the `ipp_jstate_t`
  enum (`IPP_JSTATE_PENDING=3 … IPP_JSTATE_COMPLETED=9`). Printer state is read with
  `cupsGetOption("printer-state"/"printer-state-reasons", dest->num_options,
  dest->options)`. (CUPS Programming Manual; OpenPrinting `cups/cups.h`, `cups/ipp.h`.)
  Submitting/enumerating via libcups is **not** a documented TCC-protected operation —
  no privacy prompt — which is exactly what the unattended loop needs (UNVERIFIED by an
  *affirmative* Apple statement; it is simply absent from the TCC categories).
- **macOS 14 removed PostScript→PDF conversion.** Apple's Sonoma release note
  (bug 110019863): *"macOS has removed the functionality for converting PostScript and
  EPS files to PDF … CGPSConverter returns an error when invoked … PMPrinterPrintWithFile
  does not accept a PostScript file for non-PostScript print queues."* `pstopdf` is
  gone. **This kills the obvious "AROS PostScript → CGPSConverter → PDF" route on
  current macOS.** Consequences for us, baked into the design: prefer **PDF generation
  on the AROS side or in our shim's own drawing** over relying on a host PS→PDF step;
  and if we keep PostScript, submit it to a queue that *accepts* PostScript (a real PS
  printer or an IPP queue advertising PS), not through a converter.
- **`cupsfilter` exists but is deprecated** (man page literally says "deprecated"),
  and on Sonoma+ the PS→PDF leg of its chain is the very thing that was removed — so
  `cupsfilter -m application/pdf input.ps` is **not** a reliable headless PS→PDF on
  current macOS. The CLI tools `lp -d <printer> file`, `lpr`, `lpstat -p/-o` are
  present and fine for *submitting an already-PDF* file or reading status.
- **CUPS drivers are deprecated; driverless IPP (AirPrint / IPP Everywhere) is the
  supported path.** We are a *client*, not a driver, so this does not block us, but it
  means: target real driverless queues, and for headless verification do **not** rely
  on driver/PPD machinery. (OpenPrinting `drivers.html`.)

So the work is: (a) add the `__aarch64__` `PRINTER_MAGIC` branch; (b) for the MVP,
reuse the existing PostScript driver and redirect its spool to a host sink (the
host-volume folder, or a tiny `cups.device`); (c) for the proper path, a new
`DEVS:Printers/CUPS` driver + `libcups_shim.dylib`; (d) a printer-STATUS gadget over
`cupsGetJobs`/`cupsGetDests`. The spooler, the PostScript driver, the prefs model, and
the host-volume mount are reused unchanged.

## Background: AROS printing contracts (grounded)

AROS printing has **three layers / three candidate seams**. We must hook the right
one(s).

### Layer 1 — `printer.device` + the per-unit driver task (the spooler)

`printer.device` is opened per *unit*; unit −1 is the bare "system" device with no
real commands (`printer.c:61–65,109–113`). For unit ≥ 0, `OpenDevice` calls
`Printer_Unit()` (`driver.c:734`), which:

1. `Printer_LoadPrefs` for that unit, then `Lock("DEVS:Printers")` and
   `LoadSeg(prefs.pp_Txt.pt_Driver)` (`driver.c:740–747`);
2. validates `((struct PrinterSegment*)seg)->ps_runAlert == AROS_PRINTER_MAGIC`
   (`driver.c:754–757`);
3. `MakeLibrary` over the driver's 6-vector stub + `CreateNewProcTags(NP_Entry,
   pd_DriverTask, NP_UserData, pd)` (`driver.c:768,808`), then handshakes the task up
   via a private `PrinterMessage` (`driver.c:822–844`).

`pd_DriverTask` is the unit's server loop (`driver.c:526–667`): `WaitPort(&pd_Unit)` →
`GetMsg` → switch on `io_Command`. The commands it serves:
`CMD_WRITE` → `Printer_Text_Write` (`:609–616`), `PRD_RAWWRITE` → `pd_PWrite`
(`:618–626`), `PRD_DUMPRPORT`/`PRD_DUMPRPORTTAGS`/`PRD_TPEXTDUMPRPORT` →
`Printer_Gfx_DumpRPort` (`:638–650`), `PRD_QUERY`, the `PRD_*PREFS` family
(`:627–634`), `PRD_SETERRHOOK`, `CMD_RESET/STOP/START/FLUSH`, `CMD_CLOSEDEVICE`.

**The output seam — load-bearing.** Both the text engine (`text.c`) and every driver's
`ped_Render` ultimately call `pd->pd_PWrite(data, len)` (`driver.c:218,797`), which
issues `CMD_WRITE` to one of two backing IORequests `pd_ior0`/`pd_ior1`
(`driver.c:226–238`). Those are opened in `pd_Init` against `<PrtDevName>.device`
(`driver.c:406–421`), where `PrtDevName` came from `pd_SyncPrefs` (parallel / serial /
**`printtofile`** / or a custom `pu_DeviceName`, `driver.c:695–705`). **So the printed
bytes leave `printer.device` as `CMD_WRITE`s to a configurable exec device.** That is
the cleanest interception point: any device named in `pu_DeviceName` becomes the
printer's "port."

### Layer 2 — the printer-driver plugin (the `PED` contract)

A driver is a `LoadSeg`-able segment whose first words are a `PrinterSegment`
(`devices/prtbase.h:232`): `ps_NextSegment`, `ps_runAlert` (= `AROS_PRINTER_MAGIC`),
`ps_Version`, `ps_Revision`, and an embedded `struct PrinterExtendedData ps_PED`. The
`AROS_PRINTER_TAG(PED, version, revision, …)` macro (`aros/printertag.h:32–43`) lays
that out in a `.tag.printer` section with the magic as the leading opcode.

`PrinterExtendedData` (`prtbase.h:166–200`) — the hooks a driver exports:

```
ped_PrinterName        char*                       (e.g. "PostScript")
ped_Init(pd)           LONG   ; 0 = success        (called once at unit bring-up)
ped_Expunge()          VOID
ped_Open(ior)          LONG   ; 0 = success        (per OpenDevice)
ped_Close(ior)         VOID
ped_PrinterClass       UBYTE  PPC_* | PPCF_EXTENDED (graphics/color/extended)
ped_ColorClass         UBYTE  PCC_*
ped_MaxColumns/NumRows/MaxXDots/MaxYDots/XDotsInch/YDotsInch  (geometry)
ped_Commands           STRPTR*  aRIS..aRAW ANSI->printer escape table (77 entries)
ped_DoSpecial(...)     LONG   (text command formatter)
ped_Render(ct,x,y,status) LONG (the GRAPHICS dump state machine; PRS_* below)
ped_ConvFunc(...)      LONG   (per-char text conversion)
ped_TagList            TagItem* (V44 PRTA_* features)
ped_DoPreferences(ior,cmd) LONG (V44, PPCF_EXTENDED)
ped_CallErrHook(...)   VOID
```

`ped_Render` is the raster engine, called by `gfx.c` with `PRS_*` states
(`aros/printertag.h:45–120`): `PRS_PREINIT` (pick DPI/page geometry, set
`ped_MaxXDots`…), `PRS_INIT` (alloc buffer), `PRS_TRANSFER` (one colour row),
`PRS_FLUSH`, `PRS_CLEAR`, `PRS_NEXTCOLOR`, `PRS_CLOSE`. The skeleton driver
(`skeleton.c:629–680`) is the worked example; the PostScript driver wraps each into
PostScript drawing operators (`postscript.c:602–905`). **All of it ends at
`pd->pd_PWrite`** — a driver never touches a real port; it just produces a byte
stream. That is why a "CUPS driver" is mostly *format choice* (what bytes to emit) plus
*where they go* (the spool device), not new transport.

### Layer 3 — `PRT:` (the text handler)

`PRT:` (`DOSDrivers/PRT` → `port-handler`, `port-handler.c`) is the DOS-level face:
`Copy x PRT:` or `ed`/`type`-style output flows as text to `printer.device` unit 2
(`port-handler.c:222,247,289`). It is a pure consumer of Layer 1; we do not modify it,
but it is one of the unattended exercisers ("write text to `PRT:`, assert a file came
out the host side").

### The three seams, compared

| Seam | What you change | Reuse | Fit for the unattended loop |
|------|-----------------|-------|------------------------------|
| **(A) spool *device*** (`pu_DeviceName = "cups"`) | a tiny new `cups.device` that consumes `CMD_WRITE` and drops/submits the bytes | PostScript driver **and** spooler unchanged | **best first win** — proven by `printtotool`/`printtofile` |
| **(B) printer *driver*** (`DEVS:Printers/CUPS`) | a new `PED` driver + `libcups_shim.dylib` | spooler unchanged | the *proper* path — enumerates real printers, picks PDF format, reads status |
| **(C) `PRT:` handler** | rewrite `port-handler` | — | **rejected** — it is just a Layer-1 client; nothing to gain |

### Reference points already de-risked in this repo

- **Host-call boundary**: H3 proved Apple's variadic ABI and built the marshaller
  (`hosted/abishim.S`). libcups calls are mostly fixed-arg; any varargs path goes
  through it.
- **Host symbol resolution + the dylib shim pattern**: every host bridge (`coreaudio`,
  `clipboard`, `bsdsocket`, `cocoametal`) is a native arm64 `.dylib` exporting a flat
  hand-written C ABI, `dlopen`'d via `hostlib.resource`
  (`OpenResource("hostlib.resource")` → `HostLib_Open` → `HostLib_GetPointer` per
  symbol — the exact path modelled in `hosted/cocoametal/abi_test.c:13–21`). The
  `libcups_shim.dylib` is the printing sibling.
- **The device-I/O path**: H11 (`hosted/device.c`) ran a real exec IORequest → device
  task → real macOS syscall → reply. The `cups.device` (seam A) is that pattern with
  `CMD_WRITE` → host write/submit.
- **The host volume**: already mounts a Mac folder as an AROS volume (read-only and
  `;WRITE`). PostScript/PDF written to that folder *is already on the Mac* — the
  cheapest delivery with no new host code at all.

## Design

### Path (a) — MVP: PostScript out, redirected to a host sink (cheapest real win)

No new printer driver. Select the **existing PostScript driver** for a printer unit
(`pp_Txt.pt_Driver = "PostScript"`) and redirect its spool one of two ways:

1. **Zero-host-code: spool to the host volume.** Set `pu_DeviceName = "printtofile"`
   (or our `cups.device` in "drop" mode) and point the output at a path on the
   already-mounted **`MacRW:`** host folder. The PostScript (or, see below, PDF) lands
   directly in a Mac directory. Nothing macOS-specific runs inside AROS; the bytes are
   on the Mac the instant `Close` patches the trailer. This is the **`[PR2]`** win and
   it requires *only* the `__aarch64__` magic fix + a mount.
2. **`cups.device` "submit" mode.** A tiny new exec device (cloned from
   `printtofile`/`printtotool`) whose `CMD_WRITE` appends to a temp host file and whose
   `Close` either (i) drops the file in the host folder, or (ii) calls a one-line host
   helper — `lp -d <printer> <file>` / `cupsPrintFile` via the shim — to submit it to a
   real queue. Modelled exactly on `printtotool.c` (which already `SystemTags()`es an
   arbitrary tool); the only macOS-specific code is the optional submit call.

**Format caveat (the Sonoma trap).** AROS emits **PostScript**. On current macOS you
must NOT assume a host PS→PDF converter exists (Sonoma removed it). So the MVP's
delivered artifact is honestly **PostScript** unless: (a) you submit it to a queue that
accepts PostScript (a real PS printer, or an IPP queue advertising
`application/postscript`), or (b) we generate **PDF on the AROS side** (a `PDFprinter`
driver — see path (b)'s format note). For unattended *verification*, asserting a valid
DSC PostScript file (`%!PS-Adobe`, `%%Pages`, `showpage`, `%%EOF`) on disk is a clean
PASS that needs no converter at all.

### Path (b) — proper: a `DEVS:Printers/CUPS` driver + libcups shim

A new driver directory `workbench/printers/cups/` cloned from `skeleton/`:

- `cups.c` — the `PED`: `AROS_PRINTER_TAG(PED, 44, 0, .ped_PrinterName="CUPS",
  .ped_PrinterClass = PPC_COLORGFX | PPCF_EXTENDED, …)`, the `ped_Init/Open/Close/
  Render/DoSpecial/ConvFunc/DoPreferences` set, all emitting through `pd_PWrite`. Two
  realistic format strategies:
  - **PS-passthrough**: emit the *same PostScript* the in-tree PS driver emits (clone
    its `ped_Render`), and let the *backend* decide whether to submit PS or convert.
    Simplest driver, but inherits the Sonoma PS→PDF problem at submit time.
  - **PDF-direct (recommended)**: have the driver/backend produce **PDF** — either by
    drawing into a `CGPDFContext` in the shim (the supported, non-deprecated CG path
    that needs no PS), or by emitting a minimal valid PDF for the raster. This sidesteps
    the removed PS→PDF entirely and yields `CUPS_FORMAT_PDF` for libcups. (Choosing/
    sizing this is an open question — see Risks.)
- `cups-bridge/` — native arm64 C, the *only* file naming CUPS symbols; exposes a flat
  `CUPS_*` C ABI (enumerate, open job, write data, finish, list jobs, printer state),
  `dlopen`'d via `hostlib.resource` exactly like the Alsa/coreaudio bridges. Backed by
  **`libcups_shim.dylib`** linking `-lcups` (+ CoreFoundation, + CoreGraphics if PDF
  drawing).
- `DriverData.h`, `mmakefile.src` — copy `skeleton`'s (`%build_module_simple
  modtype=printer`). `ped_DoPreferences` surfaces the chosen macOS printer (mapping the
  AROS unit → a `cups_dest_t name`).

The driver still routes through the spooler's `pd_PWrite` → backing device. To reach
the shim from a *driver* (rather than a separate `cups.device`), the cleanest split is:
driver emits the format → `pd_PWrite` → a **`cups.device`** sink (path (a) §2) whose
backend is the shim. I.e. (b) is (a)'s sink + a format-aware driver. Keeps the host
code in *one* device, not smeared across the driver.

### Path (c) — the printer-STATUS gadget (the gap the owner called out)

A small Zune/MUI app, **`SYS:Tools/PrinterStatus`** (or a tab in the existing prefs
editor), modelled on the in-tree printer prefs editor (`workbench/prefs/printer/
editor.c`, a `PrefsEditor` subclass with `CycleObject`/`StringObject`/`DirlistObject`)
and on `SysMon` (`workbench/system/SysMon/main.c`, a `muimaster` app with a refresh
hook and `MUIM_Application_NewInput` loop). It reads, via the shim:

- **Printers** — `cupsGetDests` → for each `cups_dest_t`: name, `is_default`, and
  `cupsGetOption("printer-state"/"printer-state-reasons", …)` → a status line.
- **Jobs** — `cupsGetJobs(&jobs, name, 0, CUPS_WHICHJOBS_ALL)` → for each `cups_job_t`:
  id, title, `ipp_jstate_t state` mapped to text (Pending/Held/Processing/Stopped/
  Canceled/Aborted/Completed).

UI: a `ListObject` of printers (name + state) and a `ListObject` of jobs, refreshed on
a timer hook (the SysMon pattern) or a "Refresh" button. **Decision: a standalone
Tools app, not a Wanderer prefs panel** — status is dynamic/observational (like
SysMon), whereas prefs is static configuration; conflating them fights the
`PrefsEditor` save/use/cancel model. It can launch from the prefs editor.

### The bridge (the `CUPS_*` C ABI + the host shim)

`libcups_shim.dylib` — native arm64 C (host clang, NOT AROS crosstools), peer of
`hosted/coreaudio/coreaudio_shim.c`. It owns every CUPS object and exposes a flat,
hand-written C ABI (the *only* contact surface; the shim pulls no AROS headers, the
AROS side pulls no CUPS headers). Shape mirrors the *role* of the coreaudio shim's
opaque-handle verbs:

```
/* delivery (the cups.device backend) */
cups_job   *cups_open_doc(const char *dest, const char *title, const char *format);
int         cups_write(cups_job *, const void *buf, int len);   /* append spool bytes */
int         cups_finish(cups_job *);                            /* submit; return job id */
int         cups_drop_to_path(const char *srcTmp, const char *destDir); /* host-folder fallback */

/* enumeration + status (the STATUS gadget) */
int         cups_list_printers(cups_printer_info *out, int max); /* name,isDefault,state,reasons */
int         cups_list_jobs(const char *dest, cups_job_info *out, int max); /* id,title,state */

/* offline / headless verification */
int         cups_render_to_pdf(const char *srcPath, const char *pdfPath); /* IF a host path exists */
```

`format` is `CUPS_FORMAT_PDF` or `CUPS_FORMAT_POSTSCRIPT`. Submission uses
`cupsCreateJob`/`cupsStartDestDocument`/`cupsWriteRequestData`/`cupsFinishDestDocument`
(streaming) or `cupsPrintFile` (file). No varargs in the hot path. **No host call
blocks an AROS scheduler thread in a way different from H11** — these are
pull/request-reply (AROS asks, blocks for the reply under `HostLib_Lock`), unlike
CoreAudio's RT callback. There is **no foreign-thread / RT-callback hazard here**; this
is the easy direction, which is one reason printing is a softer target than audio.

## Plan — spikes in the loop

Each marker is a standalone host binary or one booted-AROS assertion (one-binary /
one-assert-per-marker, like `hosted/*.c` + the `graft/*-smoke` wrappers), with a single
PASS/FAIL the agent reads — no human looks at paper.

- **[PR0] aarch64 unblock.** Add the `__aarch64__` branch to `AROS_PRINTER_MAGIC`
  (`printertag.h`) with a `ret`-equivalent opcode. PASS = the in-tree **PostScript**
  driver (and `skeleton`) compile+link for aarch64-darwin (grep the build map for
  `PostScript`); the magic word disassembles to `ret`. *(Pure compile gate — the
  prerequisite for every later marker.)*
- **[PR1] host shim drops/submits, asserted (bare spike).** A pure-host probe (no
  AROS): `hosted/cups/c_test.c` calls the shim — `cups_open_doc`/`cups_write`/
  `cups_finish` against a *known PDF* fixture the harness generated — and either drops
  it to a host dir or submits to a queue, then the harness asserts the file/job. Also
  `cups_list_printers`/`cups_list_jobs` return ≥0 without crashing. PASS = artifact on
  disk with right size + (if PDF) parseable header `%PDF-`. Grounds the file path +
  assert harness like coreaudio's `[A1]`. Built as `build/host-cups` + the
  `libcups_shim.dylib` ABI proof (`cups-abi`), exactly mirroring `coreaudio-abi`.
- **[PR2] AROS PostScript → host folder (MVP, end-to-end, NO new driver).** Boot AROS,
  select the in-tree PostScript driver on a unit whose `pu_DeviceName` spools to the
  **`MacRW:`** host volume; `PrintFiles`/`Copy text PRT:` a known document. PASS = the
  harness re-reads the host folder and asserts a **valid DSC PostScript** file
  (`%!PS-Adobe`, a `%%Page:` per input page, `showpage`, `%%EOF`) — page count matches,
  no converter involved. This is the thesis end-to-end with zero macOS-specific AROS
  code. *(Leans on the proven host-volume mount; `graft/printing-smoke` wraps it.)*
- **[PR3] `cups.device` sink (submit/drop) inside AROS.** Build the small
  `cups.device` (cloned from `printtofile`/`printtotool`), set it as the unit's
  `pu_DeviceName`. Driver `CMD_WRITE`s flow through it to the shim. PASS = a print from
  AROS yields, host-side, either a submitted CUPS job (`cups_list_jobs` shows it) or a
  file in the host dir — asserted by the harness re-reading host state (the two-sided
  check, H11-style).
- **[PR4] `DEVS:Printers/CUPS` driver + PDF-direct.** Build the cloned driver emitting
  **PDF** (CGPDFContext in the shim, or AROS-side PDF), submitted via `CUPS_FORMAT_PDF`.
  PASS = the produced artifact is a valid PDF (`%PDF-`, correct page count via a tiny
  host PDF probe, extracted text matches a known input string for a text job). The full
  proper path, end-to-end, dodging the Sonoma PS→PDF removal.
- **[PR5] printer-STATUS gadget.** Build `Tools/PrinterStatus`; drive it headless via
  `aros-ctl` (type/click/shot). PASS = the harness, having created a known queue/job
  state on the Mac side, asserts the gadget's printer list shows the default printer +
  state and the job list shows the test job's title+state (read back via `aros-ctl shot`
  OCR-free: assert through the shim's own counters / a debug dump the gadget prints, not
  pixels). *(Rides the control harness; the visible panel is a human nicety verified
  separately.)*

Build/run in the existing harness style (`make hosted-cups` → `[PR?]` markers;
`graft/printing-smoke` for the booted-AROS legs), clean-exit on PASS.

## How we verify it unattended

No human looks at paper; **no macOS print dialog and no TCC prompt is ever hit**
(libcups submit/enumerate is not TCC-protected; writing to the host folder is ordinary
file I/O by the AROS process under the launching terminal's permissions, the exact
posture the host-volume feature already verified). Verification = **produce a file
headlessly and assert it numerically**, never paper:

1. **Primary oracle: the artifact on disk.** Every spike yields a file the agent reads:
   - **PostScript** (MVP): assert DSC structure — `%!PS-Adobe` header, one `%%Page:`
     per input page, `showpage`, `%%EOF`; byte size in range. Catches a dead/empty/
     truncated spool. No converter needed.
   - **PDF** (proper path): a tiny host PDF probe asserts `%PDF-` magic, the **page
     count** (count `/Type /Page` or trailer `/Count`), and for a text job the
     **extracted text** contains the known input string. This is the strongest oracle
     and is the one that proves "the document actually printed correctly."
2. **Queue state.** `cups_list_jobs`/`cups_list_printers` return values asserted: the
   submitted job appears with the expected title and a sane `ipp_jstate_t`; the default
   printer is reported with a state. (Against a real or a test/loopback queue.)
3. **Two-sided assertion (H11 rule).** The harness creates the fixture on one side and
   asserts on the other: it generates the known input in AROS (or a known PDF on the
   Mac for the shim spike), prints, then re-reads the host artifact independently.
4. **Markers** are unique per spike (`[PR0]`…`[PR5]`); a hung print is reaped by the
   existing bash watchdog; `harness/run-hosted.sh` greps the marker and emits the
   uniform `result=(PASS|FAIL)` block; `graft/printing-smoke` stores screenshot/log
   evidence under `run/darwin-aarch64/` for the booted legs.

The known **test document** is a 2-page text file with a fixed sentinel string
(page-count + extracted-text assertions pin down "right pages, right content, not
empty"), and for graphics a small solid-colour raster (assert non-blank, right
dimensions) — the print analogue of audio's known sine.

## Risks & open questions

- **macOS removed PS→PDF (the headline risk).** Sonoma's removal of `CGPSConverter`/
  `pstopdf`/PS-on-non-PS-queues means our natural format (PostScript, the one AROS
  driver) cannot be converted to PDF *by the host* on current macOS. Mitigation: the
  MVP delivers/asserts **PostScript directly** (no conversion needed for verification),
  and the proper path generates **PDF on our side** (CGPDFContext in the shim, or
  AROS-side PDF) so we hand libcups `application/pdf` and never need a PS→PDF step.
  **Open:** is drawing the raster into a `CGPDFContext` in the shim simpler than
  emitting minimal PDF from a cloned `ped_Render`? Lean toward CGPDFContext (supported,
  non-deprecated) for the raster path. **UNVERIFIED** until [PR4].
- **CUPS deprecation drift.** Drivers/PPDs/filters are deprecated CUPS-wide; we are a
  *client* so it doesn't block us, but `cupsfilter` is deprecated and unreliable on
  Sonoma+. We must not depend on it. Submit to driverless IPP/AirPrint queues.
- **No printer attached in CI.** Unattended verification cannot assume a physical
  printer. Mitigation: the file-on-disk oracle (drop-to-host-folder) needs no queue at
  all; the *submit* path is verified against a test/loopback queue or by asserting the
  job appears in `cups_list_jobs` (which works with a paused/virtual queue), not by
  asserting paper. **Open:** create a CUPS test queue in CI (`lpadmin -p test -E -v
  file:/dev/null` style) vs always using drop-to-folder — **UNVERIFIED** which is
  cleaner in the loop; default to drop-to-folder for the hard oracle and treat submit
  as a softer "job enqueued" check.
- **`AROS_PRINTER_MAGIC` opcode for aarch64.** The magic is the first instruction of
  the driver segment and is also a real opcode (it must be harmless if executed — the
  other arches use `ret`/`rts`/`bx lr`). AArch64 `ret` is `0xd65f03c0`. **UNVERIFIED:**
  endianness/placement in the `.tag.printer` section vs. how `ps_runAlert` is read at
  `driver.c:757`; confirm the 32-bit `ps_runAlert` compare still matches (the field is
  `ULONG`; AArch64 `ret` is a full 32-bit word, fine, but verify the macro stores the
  intended bytes). Pin down in [PR0].
- **PostScript fidelity.** The in-tree PS driver is V44 and emits language-level-2 PS
  with custom `aSHOW`/`aFF` operators in its prolog (`postscript.c:398–531`). A real PS
  printer or Preview should accept it, but it is **UNVERIFIED** whether a modern PS RIP
  renders the custom prolog correctly. The drop-to-folder + DSC-structure assertion
  sidesteps this for the loop; full render fidelity is a human-facing nicety.
- **Where the shim lives vs the driver.** A *driver* cannot easily own a `hostlib`
  handle across its load/expunge; routing host calls through a **`cups.device` sink**
  (one device owning the shim handle) is cleaner than embedding `hostlib` in the
  driver. **Decision (lean):** keep all host code in `cups.device`; the driver only
  chooses the format. Confirm at [PR3]/[PR4].
- **`libcups.dylib` path + codesign.** The shim links `-lcups`; the exact on-disk path
  of `libcups.2.dylib` and whether the `dlopen`'d shim needs a specific entitlement in
  the hosted process are **UNVERIFIED** — confirm vs the existing `harness/run.sh` /
  `run-window.sh` signing path that already handles `libcoreaudio.dylib`/
  `libpasteboard.dylib`.
- **The graft, not a spike.** [PR2]–[PR5] depend on the AROS aarch64-darwin crosstools
  + `mmake` producing `printer.device`, the PS driver, and the new modules (the
  `workbench-devs-quick` path). [PR0]/[PR1] are session-sized (a one-line header fix +
  a bare host shim) that stand alone; the rest ride the boot/graft, which still gates on
  the desktop baseline being up (port inventory §10).
- **Apple variadic ABI.** libcups is fixed-arg in the hot path; any varargs (logging)
  goes through the H3 shim (`hosted/abishim.S`). Low risk, already de-risked.

## References

AROS upstream (`/Users/user/Source/aros-upstream`):
- Spooler: `workbench/devs/printer/printer.c` (system device, no-op commands
  :109–113), `driver.c` (`Printer_Unit` LoadSeg+magic :740–757, `pd_DriverTask`
  :526–667, `pd_PWrite`→`CMD_WRITE` :218–242, `pd_Init` opens `<PrtDevName>.device`
  :406–421, `pd_SyncPrefs` PrtDevName select :695–705), `text.c`, `gfx.c`,
  `mmakefile.src` (`%build_module modtype=device`), `printer.conf` (basetype/beginio),
  `printer_intern.h`.
- Driver contract: `compiler/include/devices/prtbase.h` (`PrinterData`,
  `PrinterExtendedData` :166–200, `PrinterSegment` :232, `PPC_*`/`PCC_*`),
  `compiler/include/aros/printertag.h` (`AROS_PRINTER_MAGIC` per-arch :16–30 —
  **no aarch64**, `AROS_PRINTER_TAG` :32–43, `PRS_*` :45–120),
  `compiler/include/devices/printer.h` (`PRD_*` cmds, `IODRPReq`, `SPECIAL_*`,
  `PDERR_*`, `aRIS..aRAW`).
- Drivers: `workbench/printers/postscript/postscript.c` (DSC PS: header :398,
  `%%Page:` :352, prolog/operators :398–531, render :602–905, `mmakefile.src`
  `modtype=printer`), `workbench/printers/skeleton/skeleton.c` (the clone-me template:
  `AROS_PRINTER_TAG` :311, hooks :346–733, `pd_PWrite` idiom :368–400).
- Redirection precedents: `workbench/devs/printtotool/printtotool.c` (`PIPE:*` +
  `SystemTags` an arbitrary tool :24–49, `CMD_WRITE`→pipe :136),
  `workbench/devs/printtofile/printtofile.c` (`CMD_WRITE`→file :103).
- `PRT:`: `workbench/devs/DOSDrivers/PRT` (`Handler=port-handler Startup=2`),
  `workbench/fs/port/port-handler.c` (defaults `printer.device` :222,247, `OpenDevice`
  :289).
- Prefs + the redirection field: `compiler/include/prefs/printertxt.h`
  (`PrinterUnitPrefs.pu_DeviceName[DEVICENAMESIZE=32]` :83, `PaperSize` defs,
  `PP_PARALLEL/SERIAL`), `workbench/prefs/printer/editor.c` (`PrefsEditor` subclass
  :211–213, `pu_DeviceName` String gadget :305, `DEVS:Printers` Dirlist :275).
- STATUS-gadget UI models: `workbench/prefs/printer/editor.c` (MUI prefs editor),
  `workbench/system/SysMon/main.c` (muimaster app + refresh hook).
- Tools (unattended exercisers): `workbench/tools/{InitPrinter.c,PrintFiles.c,
  GraphicDump.c}`.
- Host-symbol mechanism: `arch/all-unix/bootstrap/hostlib.h`
  (`Host_HostLib_Open`/`GetPointer`), `arch/all-hosted/hostlib/`.

This repo (`/Users/user/Source/aros-aarch64`):
- `hosted/coreaudio/{coreaudio_shim.c,.h,coreaudio.exports}` + `Makefile`
  (`coreaudio-dylib`/`coreaudio-abi`) — the dylib-shim pattern to mirror;
  `hosted/cocoametal/abi_test.c:13–21` (the `hostlib.resource` load path);
  `hosted/clipboard/`, `hosted/bsdsocket/` (peer shims); `hosted/device.c` (H11 IORequest
  →device task→host→reply); `hosted/abishim.S` (H3 variadic ABI).
- `docs/features/host-volume/{design.md,spec.md}` — the host-folder delivery path
  printing leans on (already implemented: `MacRO:`/`MacRW:`).
- `docs/features/darwin-aarch64-port-inventory.md` — current gap map (§4 desktop
  baseline, §10 tooling).

External prior art (web, not in the AROS tree):
- CUPS Programming Manual + OpenPrinting `cups/cups.h`, `cups/ipp.h` — `cupsGetDests`/
  `cupsGetNamedDest`/`cups_dest_t`, `cupsPrintFile`/`cupsCreateJob`/
  `cupsStartDestDocument`/`cupsWriteRequestData`/`cupsFinishDestDocument`,
  `CUPS_FORMAT_PDF`/`CUPS_FORMAT_POSTSCRIPT`, `cupsGetJobs`/`cups_job_t`/`ipp_jstate_t`
  (`IPP_JSTATE_PENDING=3 … COMPLETED=9`), `cupsGetOption("printer-state"…)`.
- Apple macOS 14 (Sonoma) release notes, bug 110019863 — removal of PostScript/EPS→PDF
  conversion (`CGPSConverter` errors; `pstopdf` gone; `PMPrinterPrintWithFile` refuses
  PS on non-PS queues). The reason the design is PDF-first / PS-passthrough, never
  host-PS→PDF.
- Apple `CGPDFContextCreateWithURL` (Core Graphics) — supported, non-deprecated
  in-process PDF generation (the shim's PDF path).
- CUPS man pages: `cupsfilter` (deprecated), `lp`/`lpr`/`lpstat`; OpenPrinting
  `drivers.html` — CUPS drivers deprecated, driverless IPP / AirPrint the supported
  client target.
