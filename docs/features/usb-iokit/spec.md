# Implementation spec — host-backed USB for AROS (Poseidon pseudo-HCD)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

> **SCOPE WARNING (read before implementing).** This is the lowest-priority, hardest,
> and worst-loop-fitting feature of the batch. **Only enumeration verifies unattended.**
> The transfer/device milestones need physical hardware and/or elevated macOS
> permissions and are explicitly **out** of the unattended loop (attended smokes only).
> The AROS side is **largely already written** — the in-tree libusb-backed virtual host
> controller `rom/usb/vusbhc/` — so this spec is mostly *bring-up + a headless
> enumeration oracle*, not new HCD authorship. See design.md "Why this is last".

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent, driver, or
otherwise — was read, searched, or consulted in producing it, and any resemblance to
existing implementations is coincidental.** Implement only from this spec + the approved
sources cited by tag: `[PUB]` Apple IOKit/IOUSBHost docs, the USB 2.0/3 specification,
and the **libusb published API** (used as a documented interface, exactly as the in-tree
`vusbhc` already does — we use the API, never read libusb's implementation); `[AROS]`
in-tree AROS headers, Poseidon, and the `vusbhc` source (paths given; APL/LGPL — ours);
`[OURS]` this project's spikes (H-series, `hosted/*`, the host-wake pattern). `[DERIVED]`
items are independently-derived requirements flagged for extra verification; each stands
solely on its cited `[PUB]`/`[AROS]`/`[OURS]` justification — implement from that, never
from any reference. No identifier name, call sequence, file layout, or transfer-management
algorithm in this spec derives from any third-party implementation. (libusb's API
*signatures* are an external interface, like POSIX or the AHI struct — not third-party
expression; we already consume them via `vusbhc`.)

## Scope

**In (this spec).**
1. A **fully-headless host USB enumeration oracle** — IOKit
   `IOServiceMatching(kIOUSBDeviceClassName)` + `io_iterator` listing devices and reading
   `idVendor`/`idProduct`/`bDeviceClass`, asserting a known always-present device. The
   one milestone in the unattended loop. `[UB1]`.
2. A **libusb cross-check** that libusb's arm64-darwin build sees the same device tree
   (the dependency `vusbhc` needs). `[UB2]`.
3. **Bring-up of `vusbhci.device`** for darwin-aarch64 against a deployed arm64
   `libusb.dylib`, registered with Poseidon so `psdEnumerateHardware` walks the host's
   USB tree; assert Poseidon reports the same always-present device. `[UB3]`.
4. The **darwin-safe async-completion conformance** of the HCD's completion path
   (timer-polled handler task; no foreign-thread `Signal`) — `vusbhc` already conforms;
   this spec states the requirement and the verification.

**Out (non-goals, this spec).**
- Any **device transfer** path as a *loop gate* — `GET_DESCRIPTOR` control transfer
  ([UB4]), interrupt-IN from HID ([UB5]), and a class device working end-to-end ([UB6])
  are specified as **attended smokes**, not unattended markers, because they need
  hardware and/or root/entitlements.
- Writing a hand-rolled IOKit/IOUSBHost **transfer** stack (libusb wraps it; reuse).
- Isochronous transfers (`UHCMD_ISOXFER`) — `vusbhc` stubs them; we keep that stub.
- Authoring a new HCD from scratch (reuse `vusbhc`); a DriverKit virtual-USB dext (the
  only zero-hardware transfer path on macOS — far out of scope).
- USB device-mode / OTG.

## Decisions (confirmed-with-owner items flagged)

- **D1 — Reuse `rom/usb/vusbhc/`, do not author a new HCD.** It already implements the
  `usbhardware.device` contract over libusb via `hostlib.resource` (`vusbhci_device.c:229`
  `BeginIO`, `vusbhci_bridge.c:219` hostlib open). The darwin work is build + dependency +
  verification. `[AROS]`. *(If we later need to decouple from libusb, fork it to
  `darwinusbhc.device` calling our neutral ABI — §The C ABI; default is reuse.)*
- **D2 — Enumeration via permission-free IOKit; transfers via libusb.** IOKit
  `IOServiceMatching` enumeration needs no entitlement and is the headless oracle;
  libusb is the transfer backend the AROS side is already written against. `[PUB]`.
- **D3 — libusb is a separately-built, dynamically-loaded dependency** (LGPL-2.1), loaded
  through `hostlib.resource` like `libSystem.dylib`; no libusb source is incorporated
  into AROS. **Owner confirms the licence strategy before shipping.** `[OURS]`/`[PUB]`.
- **D4 — The loop is gated only on [UB1]–[UB3].** [UB4]+ are attended `make usb-smoke`
  steps, never CI gates. `[DERIVED]` (loop-fit reasoning, design.md).

## Architecture

```
AROS side (aarch64, AROS crosstools)                 Host side (Apple toolchain)
┌───────────────────────────────────────┐            ┌─────────────────────────────┐
│ poseidon.library  +  class drivers     │            │ IOKit (enumeration oracle)  │
│   (reused unchanged)            [AROS] │            │  IOServiceMatching(kIOUSB…) │ [PUB]
│        │ psdAddHardware / Enumerate    │            │  io_iterator → vendor/prod  │
│        ▼                               │  hostlib   ├─────────────────────────────┤
│ vusbhci.device  (the pseudo-HCD)       │  +H3 ABI   │ libusb.dylib (arm64-darwin) │
│   · BeginIO: UHCMD_* on IOUsbHWReq     │ ─────────► │  · libusb_* transfer API    │ [PUB]
│   · timer-polled handler task (50Hz)   │  libusb_*  │  · IOKit/IOUSBHost backend  │
│   · queues per xfer type, ReplyMsg     │ ◄───────── │    (built-in, we don't read)│
│        │ libusb completion ON-TASK     │ completion └─────────────────────────────┘
└───────────────────────────────────────┘
  reused verbatim from rom/usb/vusbhc — the only NEW host artifacts are the arm64
  libusb.dylib build/deploy and the standalone [UB1]/[UB2] enumeration probe binaries.
```

- **Host enumeration probe** `[OURS]` — native arm64 C ([UB1]/[UB2]), peer of
  `hosted/coreaudio/a_test.c`. Pulls **no** AROS headers. Links `IOKit` +
  `CoreFoundation` (and optionally libusb for [UB2]). This is the headless oracle.
- **`libusb.dylib`** `[PUB]` — a standard arm64-darwin libusb build, deployed to `~/lib`
  under the bare name `vusbhc` opens (`vusbhci_bridge.c:222`), resolved via
  `DYLD_FALLBACK_LIBRARY_PATH=~/lib` like every other shim. No code authored.
- **`vusbhci.device`** `[AROS]` — the reused in-tree HCD; the only file naming libusb is
  `vusbhci_bridge.c` (already so). Built by the AROS crosstools for darwin-aarch64.

## The C ABI — `[PUB]` (libusb) or `[OURS]` (optional neutral shim)

**Default (D1): no new ABI.** `vusbhc` already binds to the libusb published API via the
function table in `vusbhci_bridge.h` (`libusb_init`, `libusb_get_device_descriptor`,
`libusb_open`, `libusb_submit_transfer`, `libusb_control_transfer`,
`libusb_handle_events`, `libusb_hotplug_register_callback`, …). That table **is** the
contact surface; we satisfy it with a real `libusb.dylib`. These are libusb's *published*
signatures — an external interface (`[PUB]`), like POSIX `socket()` or the AHI struct —
not third-party expression. No new header.

**Optional neutral shim (only if D1's libusb dependency is rejected).** A hand-authored,
neutral C ABI a `darwinusbhc.device` would call instead, implemented over **either**
libusb **or** raw IOKit. Stated here so the option is concrete; **not built by default.**

```c
/* libusbhost.h — OURS, only if we fork vusbhc off libusb. Neutral, hand-authored. */
typedef struct UhwCtx UhwCtx;

UhwCtx *uhw_open_controller(void);                       /* init host USB, NULL on fail   */
void    uhw_close_controller(UhwCtx *);

/* Enumeration (permission-free, IOKit-backed) — feeds root-hub emulation & UB1 parity. */
int     uhw_enumerate(UhwCtx *, void *out, int max);    /* fills {vid,pid,class,addr}[]  */

/* Open one device by (vid,pid) or address; fails if kext-claimed (ATTENDED paths only). */
int     uhw_open_device(UhwCtx *, unsigned vid, unsigned pid);   /* 0 ok, <0 = EACCES…   */

/* Submit an async transfer; completion is COLLECTED by uhw_poll_completions, never by a
   host-thread callback into AROS (R-USB-WAKE). xferId echoes back on completion. */
int     uhw_submit_control(UhwCtx *, const unsigned char setup[8], void *data,
                           int len, unsigned long xferId);
int     uhw_submit_int(UhwCtx *, int ep, int dir, void *data, int len, unsigned long xferId);
int     uhw_submit_bulk(UhwCtx *, int ep, int dir, void *data, int len, unsigned long xferId);

/* PULLED by the AROS handler task on its timer tick. Drains completed transfers into
   `out` (xferId, actual, errcode); returns count. The ONLY completion delivery. */
typedef struct { unsigned long xferId; int actual; int err; } UhwDone;
int     uhw_poll_completions(UhwCtx *, UhwDone *out, int max);

/* Diagnostics for the oracle: completions delivered off the AROS poll path MUST be 0. */
typedef struct { unsigned long submitted, completed, offTaskCallbacks; } UhwStats;
void    uhw_get_stats(UhwCtx *, UhwStats *);
```

The neutral shim's value is that **completion is always *pulled*** by the AROS task
(`uhw_poll_completions`), removing any chance of a foreign-thread callback into AROS — the
darwin requirement (R-USB-WAKE). With D1 (raw libusb) we get the same property by only
calling `libusb_handle_events` from the AROS handler task, as `vusbhc` already does.

## Concurrency model — async completion across the host boundary (R-USB-WAKE)

USB is async: submit, complete later. The one genuinely new systems risk — and it is
**shared with audio and sockets and already solved** — is delivering completion to AROS
without a foreign host thread touching AROS state.

**R-USB-WAKE — completion arrives only on the AROS handler task, never via a host-thread
`Signal`.** `[OURS]`+`[AROS]`. Per `host-wake-pattern.md`'s **DARWIN-AARCH64 CAVEAT**, a
task woken from host-thread/interrupt context runs in supervisor mode under the threaded
darwin scheduler and trips semaphore ops (`arch/all-darwin/hidd/cocoa/cocoa_input.c:546`);
the shipped darwin drivers (input ~50 Hz, clipboard ~5 Hz, sockets) therefore **poll on a
timer tick** instead of `Signal`ing from the host thread. **`vusbhc` already conforms:**

- A dedicated **AROS handler task** (`vusbhci_device.c:34` `handler_task`) loops:
  drain the per-type queues → submit to libusb → **`call_libusb_event_handler()`** (runs
  `libusb_handle_events`, which fires `callbackUSBTransferComplete` **on this AROS task**)
  → `DoIO(timer.device, …)` and repeat (`vusbhci_device.c:107–118`).
- `callbackUSBTransferComplete` (`vusbhci_bridge.c`) fills `iouh_Actual`/`io_Error` and
  `ReplyMsg`s the request — a normal AROS reply on the AROS task, **not** a host `Signal`.

**Requirements (each restated from `[OURS]` host-wake + the `[AROS]` `vusbhc` precedent):**
- **R-USB-WAKE1 — libusb event handling runs only on the AROS handler task.** Call
  `libusb_handle_events`/`libusb_handle_events_completed` solely from `handler_task`
  (`vusbhci_device.c:103`), never from a host thread. Completion callbacks therefore run
  on-task. `[AROS]` (already so) + `[OURS]` (host-wake R-W2/darwin caveat).
- **R-USB-WAKE2 — no foreign-thread callback into AROS; verify libusb-darwin spawns no
  off-task event thread.** **UNVERIFIED on Apple Silicon.** If libusb-darwin delivers
  completions or hotplug events on its own thread, the callback must degrade to setting an
  `_Atomic` flag (R-W1: atomics, never `volatile`) and let `handler_task` react on its
  next tick (R-W3 re-probe). A guard counter (`offTaskCallbacks`, asserted `== 0` in the
  neutral-shim option) proves it. `[OURS]` host-wake R-W1/R-W3.
- **R-USB-WAKE3 — bounded, watchdogged lifecycle.** Start is idempotent; stop joins the
  handler task without hanging; every spike is watchdog-bounded so a stuck libusb/IOKit
  call cannot wedge the loop. `[OURS]` host-wake R-W5.
- **R-USB-WAKE4 — tighten the poll for HID.** `vusbhc`'s fixed 100 ms tick
  (`vusbhci_device.c:109`) is too coarse for interactive HID; poll at ~50 Hz, or adaptively
  faster while `*xfer_pending` is set (the code already tracks these). Latency tune, not
  correctness. `[DERIVED]` — restated from the Cocoa input HIDD's ~50 Hz rate `[AROS]` and
  the host-wake "≈one tick of latency" stance `[OURS]`.

There is no SPSC-ring-style RT thread here (unlike audio): USB completion is comfortably
tick-granular, so the timer-poll handler task is the whole model.

## AROS-side binding — `[AROS]`, the HCD contract (reused from `vusbhc`)

The pseudo-HCD is an exec **device** implementing the `usbhardware.device` protocol.
`vusbhc` already implements it; this section is the contract it satisfies and the bring-up
deltas. Module config `[AROS]` (`vusbhc/vusbhci.conf`): `basename VUSBHCI`,
`libbasetype struct VUSBHCIBase`, `beginio_func BeginIO`, `abortio_func AbortIO`;
`mmakefile.src`: `modtype=device`, `moduledir=Devs/USBHardware`, `uselibs="stdc.static"`.

**The BeginIO dispatch (the contract) — `[AROS]`** (`vusbhci_device.c:229`, args from
the `usbhardware.device` model): `BeginIO(struct IOUsbHWReq *ioreq)` switches on
`ioreq->iouh_Req.io_Command` and routes:

- **`UHCMD_QUERYDEVICE`** → `cmdQueryDevice` (`vusbhci_device.c:349`): walk the caller's
  tag list (`iouh_Data` is a `struct TagItem *`) and fill `UHA_Manufacturer`,
  `UHA_ProductName`, `UHA_Version`/`Revision`, **`UHA_Capabilities`** (return
  `UHCF_USB20|UHCF_ISO` or `UHCF_USB30|UHCF_ISO` per the emulated root-hub `bcdUSB`,
  `vusbhci_device.c:388`). `[AROS]` (`UHA_*`/`UHCF_*` = `usbhardware.h:187/:207`).
- **`UHCMD_USBRESET`/`USBRESUME`/`USBSUSPEND`/`USBOPER`** → bus power state (`vusbhc`
  mostly no-ops these for a virtual controller). `[AROS]`.
- **`UHCMD_CONTROLXFER`** → `cmdControlXFer` (`vusbhci_commands.c:1033`): if the request
  targets the **root hub** address, answer from the emulated root-hub descriptors
  (`cmdControlXFerRootHub`, `:882`); else queue for the handler task →
  `do_libusb_ctrl_transfer` (`vusbhci_bridge.c`) → `libusb_control_transfer`. `[AROS]`.
- **`UHCMD_INTXFER`** → `cmdIntXFer` (`:1072`): root-hub status-change endpoint emulated
  (`cmdIntXFerRootHub`, `:968`); else queue → `do_libusb_intr_transfer` →
  `libusb_fill_interrupt_transfer` + `libusb_submit_transfer`. `[AROS]`.
- **`UHCMD_BULKXFER`** → `cmdBulkXFer` (`:1111`) → `do_libusb_bulk_transfer`. `[AROS]`.
- **`UHCMD_ISOXFER`** → `cmdISOXFer` — **stubbed** in `vusbhc` (`do_libusb_isoc_transfer`
  is commented out); keep stubbed (out of scope). `[AROS]`.
- **`NSCMD_DEVICEQUERY`** → the NSD supported-command list (`vusbhci_device.c:283`).

`AbortIO` → `cmdAbortIO` (`vusbhci_commands.c:34`) dequeues a pending request and sets
`IOERR_ABORTED`. Completion (sync control path, or async via `callbackUSBTransferComplete`)
sets `iouh_Actual` + maps the host error to a `UHIOERR_*` code
(`UHIOERR_STALL`/`TIMEOUT`/`USBOFFLINE`/`HOSTERROR`, `usbhardware.h:131`) into
`iouh_Req.io_Error`, then `ReplyMsg`. `[AROS]`.

**The host bridge — `[AROS]` (file) + `[PUB]` (libusb API).** `vusbhci_bridge.c`:
`OpenResource("hostlib.resource")` (`:219`) → `hostlib_load_so` opens the libusb dylib and
fills the `struct libusb_func` table from `libusb_func_names` (`vusbhci_bridge.h`) via
`HostLib_GetPointer` (`hostlib.conf:16`). The only delta for darwin is the **bare dylib
name** (`vusbhci_bridge.c:222`, today `"libusb.so"`) — point it at the deployed arm64
`libusb.dylib` (or alias it), resolved by `DYLD_FALLBACK_LIBRARY_PATH=~/lib`. `[AROS]`.

**Poseidon registration — `[AROS]`.** No code change: hand the device name to
`psdAddHardware("vusbhci.device", 0)` (`poseidon.conf:53`, impl `poseidon.library.c:4388`)
then `psdEnumerateHardware(phw)` (`poseidon.conf:55`) — exactly what
`shellcommands/AddUSBHardware.c` does. Seed it at boot or run `AddUSBHardware
vusbhci.device` from the shell. `psdEnumerateHardware` walks the root hub (emulated) and
the attached devices (libusb), binding the host-agnostic class drivers. `[AROS]`.

**Bring-up tasks (the actual darwin work):**
- **B1 — build `vusbhci.device` for darwin-aarch64.** Confirm `kernel-usb-vusbhci`
  participates in the darwin build (`vusbhc/mmakefile.src`) and installs under
  `Devs/USBHardware`. **UNVERIFIED until it links.**
- **B2 — ship arm64 `libusb.dylib`** to `~/lib` under the name `vusbhci_bridge.c` opens.
- **B3 — Poseidon residency** — root-hub/device/class drivers available; rides the boot
  graft (booting AROS is currently fragile — bsdsocket README gotchas).
- **B4 — tighten the poll tick (R-USB-WAKE4).**

## Verification (the honest split — `[OURS]` H7/H11 discipline)

No human, no TCC (reading the USB *registry* via IOKit needs no entitlement — not
camera/mic/screen-recording). **Only [UB1]–[UB3] are in the unattended loop.** One host
binary per marker, `[UB?]` PASS/FAIL via the existing `harness/run-hosted.sh`, clean-exit
on PASS; a watchdog bounds each. The always-present-device oracle is the key idea: every
Mac has at least one USB **root hub** and Apple internal devices, so enumeration is
deterministic with **nothing plugged in**.

**Unattended markers (loop gates):**

- **[UB1] host USB enumeration, asserted — FULLY HEADLESS.** Pure host probe, no AROS.
  IOKit `IOServiceMatching(kIOUSBDeviceClassName)` + `IOServiceGetMatchingServices` →
  `io_iterator`; for each service read `idVendor`/`idProduct`/`bDeviceClass` from the
  registry (`IORegistryEntryCreateCFProperty`). PASS = device count ≥ 1 **and** a known
  always-present device is present — assert on the **USB root hub** (an
  `AppleUSBXHCI`-rooted hub) and/or an **Apple-vendor device** (`idVendor == 0x05ac`).
  Deterministic on any Apple-Silicon Mac, no permission, no hardware. `[PUB]`. Grounds the
  harness like H7's `pngprobe`. **The project's only unattended USB proof.** `[UB1]`.
- **[UB2] libusb enumeration agrees — HEADLESS (gated on B2).** Same list via
  `libusb_init` → `libusb_get_device_list` → `libusb_get_device_descriptor`
  (`vusbhci_bridge.h` table). PASS = libusb's (vid,pid) set for the always-present devices
  equals [UB1]'s IOKit set. Proves the arm64 libusb build works and sees IOKit's tree —
  the dependency `vusbhc` needs. `[PUB]`. `[UB2]`.
- **[UB3] AROS Poseidon enumerates the same tree — HEADLESS on booted AROS (rides the
  boot graft).** Build `vusbhci.device` (B1), deploy libusb (B2), boot windowed AROS via
  `aros-ctl`, run `AddUSBHardware vusbhci.device` then `PsdDevLister`
  (`rom/usb/poseidon/shellcommands/PsdDevLister.c`). PASS = Poseidon's output names the
  same always-present device(s) [UB1] asserted (grep the captured console for the
  vendor/product), verified two-sided against the [UB1] host oracle (H11 discipline).
  `[AROS]`+`[OURS]`. `[UB3]`.

**Attended smokes (NOT loop gates — documented `make usb-smoke`, human + hardware):**

- **[UB4] control transfer — read a device descriptor.** libusb-open one device, issue
  `GET_DESCRIPTOR(DEVICE)` (control transfer), assert the 18-byte descriptor's
  `idVendor`/`idProduct` match enumeration. **ATTENDED:** needs a device **unclaimed by an
  Apple kext** (`libusb_open` returns `LIBUSB_ERROR_ACCESS` otherwise —
  `vusbhci_bridge.c` already logs this). Manual: "attach a known driver-free dongle, run
  `make usb-smoke`". `[PUB]`. `[UB4]`.
- **[UB5] interrupt-IN from HID.** Claim a HID interface (detaching the macOS HID kext —
  root/entitlement) and read one interrupt report. **OUT of the loop** — physical HID +
  kext detach + permission. `[PUB]`. `[UB5]`.
- **[UB6] graft: class device works through Poseidon.** Mass-storage stick → AROS volume
  (`Dir USB0:`) or USB printer → `printer.class` test page, driven from AROS. **Far OUT of
  the loop** — boot graft + hardware + permission; the destination, not a spike.
  `[AROS]`. `[UB6]`.

**Why no zero-hardware transfer path.** macOS ships **no** user-space virtual-USB-device
facility (no `usbip`/`gadgetfs`/`dummy_hcd` analogue) without writing a DriverKit dext
(out of scope). So unlike audio (render-to-WAV) or sockets (loopback), USB transfers
**cannot** be verified without a real device. This is the structural reason the loop stops
at enumeration. `[DERIVED]` (restated from the absence in `[PUB]` macOS facilities).

**Assertions are on values, never "it didn't crash":** device count, exact
`(idVendor, idProduct, bDeviceClass)` tuples for the always-present device, IOKit↔libusb
set equality, and for the neutral-shim option `offTaskCallbacks == 0` (R-USB-WAKE2 guard).

## Build / integration

- **[UB1]/[UB2] probes** — native arm64 C, host clang `-arch arm64`, link `-framework
  IOKit -framework CoreFoundation` (+ `-lusb-1.0` for [UB2]); compile to Mach-O via the
  existing `Makefile` pattern (`make hosted-usb` → `build/host-usb*` →
  `harness/run-hosted.sh '[UB?] …'`), clean-exit on PASS. Add `[UB1]`/`[UB2]` to the
  regression set; **[UB1] is the permanently-green headless gate** (no AROS, no
  hardware). `[OURS]`.
- **`libusb.dylib`** — a standard arm64-darwin libusb build (Homebrew/source), deployed to
  `~/lib` under the bare name `vusbhci_bridge.c:222` opens, codesigned consistent with the
  other shims (confirm vs. `harness/run.sh` / `graft/run-window.sh`, **UNVERIFIED**).
  LGPL-2.1, separate dynamic dependency — owner confirms licence (D3). `[PUB]`.
- **`vusbhci.device`** — built by the **AROS crosstools** (`kernel-usb-vusbhci`), installed
  under `Devs/USBHardware`; the only file naming libusb is `vusbhci_bridge.c`. `[AROS]`.
- **`make usb-smoke`** — the attended path: stage a device, boot AROS, run the [UB4]+
  flow, capture proof under `run/darwin-aarch64/`. Documented as human-run, never CI.

## Open questions / UNVERIFIED

- **Which always-present device is openable without root** (for any future attempt to drag
  [UB4] toward headless) — likely **none** (Apple kexts claim them); hence [UB4] stays
  attended. Confirm empirically.
- **Whether libusb-darwin spawns an off-AROS-task event/hotplug thread** (R-USB-WAKE2) —
  must be measured on Apple Silicon; if so, the callback degrades to an `_Atomic` flag +
  handler-task reaction.
- **`vusbhci.device` builds/links for darwin-aarch64** (B1) — UNVERIFIED until a link.
- **The exact bare dylib name + `DYLD_FALLBACK` resolution** for libusb (B2) — confirm vs.
  the other shims' deploy path.
- **Codesign/entitlements** for a `dlopen`'d libusb in the hosted process — confirm vs.
  `harness/run.sh`.
- **Poseidon root-hub/class-driver residency** at boot (B3) — rides the boot graft.
- **Control-transfer serialisation** — `vusbhc`'s control path is synchronous and blocks
  the handler task; prefer the async submit path if it bites (attended paths only).
- **HID poll latency** (R-USB-WAKE4) — 100 ms → ~50 Hz / adaptive; measure.
- **Decision D1 vs the neutral shim** — default reuse `vusbhc`+libusb; the
  `libusbhost.dylib` neutral ABI is the fallback if the libusb dependency is rejected.

## Provenance summary

`[PUB]` Apple IOKit (`IOServiceMatching(kIOUSBDeviceClassName)`,
`IOServiceGetMatchingServices`, `io_iterator`, `IORegistryEntryCreateCFProperty` for
`idVendor`/`idProduct`/`bDeviceClass`), IOUSBHost/IOUSBLib (device open/transfer +
its entitlement/exclusive-access constraints); the USB 2.0/3 specification (descriptor
layouts, control/interrupt/bulk transfer semantics, the always-present root hub); the
**libusb published API** (`libusb_init`/`get_device_list`/`get_device_descriptor`/`open`/
`submit_transfer`/`control_transfer`/`handle_events`/`hotplug_register_callback`) used as
an external interface, exactly as the in-tree `vusbhc` consumes it; the absence of a
user-space virtual-USB facility on macOS. ·
`[AROS]` `compiler/include/devices/usbhardware.h` (`struct IOUsbHWReq` :80, `UHCMD_*`
:116, `UHA_*`/`UHCF_*` :187/:207, `UHIOERR_*` :131),
`compiler/include/devices/usb.h` (`UsbStdDevDesc`/`bDeviceClass`/`idVendor`/`idProduct`
:118, `UDT_*` :66); `rom/usb/poseidon/` (`psdAddHardware`/`psdEnumerateHardware`/
`psdRemHardware` `poseidon.conf:53–55`, `psdAddHardware` impl `poseidon.library.c:4388`,
`shellcommands/AddUSBHardware.c`, `shellcommands/PsdDevLister.c`);
**`rom/usb/vusbhc/`** (the reused libusb-backed HCD — `vusbhci.conf`, `vusbhci_device.c`
`BeginIO` :229 / `cmdQueryDevice` :349 / `handler_task` :34 / poll tick :109,
`vusbhci_commands.c` `cmdAbortIO` :34 / `cmdControlXFer` :1033 / `cmdIntXFer` :1072 /
`cmdBulkXFer` :1111 / root-hub emulation :882,:968, `vusbhci_bridge.c` hostlib open :219 /
bare dylib name :222 / hotplug :243 / on-task completion `callbackUSBTransferComplete`,
`vusbhci_bridge.h` libusb table, `vusbhci_device.h` `VUSBHCIUnit`/`VUSBHCIBase`,
`mmakefile.src` `kernel-usb-vusbhci`); host-agnostic class drivers `rom/usb/classes/`,
`workbench/devs/USB/classes/`; `arch/all-hosted/hostlib/hostlib.conf`
(`HostLib_Open`/`Close`/`GetPointer`/`GetInterface` :14–18). ·
`[OURS]` H3 (`hosted/abishim.S`, Apple ABI), H11 (`hosted/device.c`, IORequest→task→host
syscall→reply — the `usbhardware.device` shape), the host-shim + `hostlib.resource` +
deploy-to-`~/lib` pattern (`hosted/coreaudio/`, `hosted/bsdsocket/`),
`docs/features/host-wake-pattern.md` (the DARWIN-AARCH64 caveat: no foreign-thread
`Signal`, AROS timer-polls — which `vusbhc`'s handler task already obeys), the
bsdsocket-net `§R-DARWIN-WAKE` precedent and boot gotchas, `harness/run-hosted.sh`. ·
`[DERIVED]` independently-derived points flagged for extra verification: (a) the loop must
gate only on enumeration ([UB1]–[UB3]) because no zero-hardware transfer path exists on
macOS, (b) the HID poll tick should tighten to ~50 Hz / adaptive, and (c) on darwin the
completion callback must stay on the AROS handler task (degrade to `_Atomic` + poll if
libusb threads it) — each restated from `[PUB]` macOS facilities + the `[AROS]` `vusbhc`
precedent + the `[OURS]` host-wake model. No third-party code, identifiers, or call
sequence used; libusb signatures are an external interface already consumed by `vusbhc`.
