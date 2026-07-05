# USB (IOKit / libusb) — host-backed USB for AROS (Poseidon HCD)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

> **READ THIS FIRST — honest framing.** This is the **lowest-priority and hardest**
> of the current host-bridge batch, and a **poor fit for the unattended loop**: real
> USB devices cannot be driven headlessly without physical hardware, and the modern
> macOS USB API (IOUSBHost) wants exclusive device access that is hostile to a hosted
> process. Exactly **one** milestone here is fully unattended — *device enumeration*
> (list the host's USB tree via IOKit, assert a known always-present device shows up).
> Everything past enumeration needs either real hardware, a virtual-device kext, or
> elevated permissions, and is **out** of the loop (attended fallback only). This doc
> gives a real, grounded design so the owner can decide *whether and when* to build it
> — but the recommendation is **defer it behind audio and sockets** (see "Why this is
> last"). Do not read enthusiasm into the level of detail; the detail is here so the
> deferral is an informed one.

## What & why

Give the hosted AROS access to the Mac's USB devices. AROS already has a complete USB
stack — **Poseidon** (`rom/usb/`) — with class drivers for hubs, HID (keyboard/mouse),
printers and mass storage, and a clean **host-controller-driver (HCD) seam**: Poseidon
talks to any number of `usbhardware.device` units, each one a host controller it drives
with `IOUsbHWReq` transfer requests. What's missing on darwin-aarch64 is a controller
at the bottom — there is no PCI to probe, so the real UHCI/OHCI/EHCI/XHCI drivers
(`rom/usb/pciusb`, `rom/usb/pcixhci`) have nothing to bind to. We would supply a
**host-backed pseudo-HCD**: a `usbhardware.device` whose "hardware" is the Mac's USB
stack, reached through a host shim (libusb or IOKit/IOUSBHost) via `hostlib.resource`.

This is the USB instance of the project thesis — *"macOS owns the drivers; AROS reaches
them via standard exec I/O."* The Mac owns the real xHCI controller, the kernel USB
stack, and device matching; AROS reaches a device through the HCD LVO interface
(`UHCMD_CONTROLXFER`/`INTXFER`/`BULKXFER` on `IOUsbHWReq`) it already speaks. It reuses
the host-call boundary de-risked by H3 (`hosted/abishim.S`) and the `hostlib.resource`
symbol mechanism, and — critically — the **host-thread → AROS boundary** lessons from
`host-wake-pattern.md` and the shipped darwin drivers (input, clipboard, sockets): on
this port a foreign host thread must **not** `Signal` AROS, so USB transfer completion
must arrive on an **AROS-side timer-polled handler task**, not a callback thread.

**But be clear about the payoff.** Unlike sockets (the internet) or audio (sound you can
verify with a file), USB's payoff is *a specific physical device working* — and a hosted
AROS that wants a keyboard/mouse already has them natively through the Cocoa input HIDD
(`arch/all-darwin/hidd/cocoa/`). The genuinely new capability USB adds is **arbitrary
class devices** (a specific MIDI box, a label printer, a mass-storage stick surfaced as
an AROS volume) — niche, and each needs the device in hand. That is the loop-fit problem
in one sentence.

## Does it already exist?

**Mostly — and that is the single most important finding in this doc.** AROS ships a
**libusb-backed virtual host controller** that is *exactly* the pattern we would build,
already wired through `hostlib.resource`:

- **`rom/usb/vusbhc/`** — the "Virtual USB host controller", a `usbhardware.device`
  (`vusbhci.conf`: `modtype=device`, `moduledir=Devs/USBHardware`,
  `beginio_func BeginIO`) that implements the Poseidon HCD command set
  (`vusbhci_device.c:229` `BeginIO` switches on `UHCMD_QUERYDEVICE/CONTROLXFER/INTXFER/
  BULKXFER/ISOXFER`) and **bridges each transfer to libusb** through `hostlib.resource`:
  `vusbhci_bridge.c:219` `OpenResource("hostlib.resource")` →
  `hostlib_load_so("libusb.so", …)` → a `struct libusb_func` pointer table
  (`vusbhci_bridge.h`), then `do_libusb_ctrl_transfer`/`intr`/`bulk` map `IOUsbHWReq`
  fields onto `libusb_control_transfer`/`libusb_fill_*_transfer`
  (`vusbhci_bridge.c:303,463,…`). It even installs a **libusb hotplug callback**
  (`libusb_hotplug_register_callback`, `vusbhci_bridge.c:243`) and reflects
  attach/detach into the AROS root-hub port status (`uhwCheckRootHubChanges`).

So the *shape* we want — Poseidon → `usbhardware.device` → host shim → libusb → real
device — **exists in the tree today**. The work is not "invent an HCD"; it is:
1. **make `vusbhc` (or a `darwinusbhc` peer) build and load on darwin-aarch64**, against
   a **macOS-native** host shim — either libusb's own Darwin/IOKit backend, or our own
   IOKit shim exposing the same verbs;
2. **fix the host-thread boundary** for this port — `vusbhc`'s completion path already
   avoids a foreign-thread `Signal` (it pumps libusb on an **AROS timer-polled handler
   task**, `vusbhci_device.c:34` `handler_task` → `DoIO(timer)` at a 100 ms tick →
   `call_libusb_event_handler()` → the completion callback `ReplyMsg`s on that task),
   which is precisely the darwin-safe pattern — but it must be re-verified end to end on
   Apple Silicon and the libusb dependency must be satisfiable;
3. **scope it to what can actually be verified headlessly** (enumeration), and accept
   that the device transfer paths need hardware/permissions (attended).

**What is absent.** `grep -rilE 'iokit|IOUSBHost|IOServiceMatching|kIOUSB'` over both
`../aros-upstream` and `.` returns nothing:
there is **no IOKit/IOUSBHost code anywhere**, and `libusb` is referenced only by the
four `vusbhc` files. There is no darwin-aarch64 USB host shim in this repo (no
`hosted/usb*`, no `build/libusbhost.dylib`). So `vusbhc`'s libusb dependency is currently
unsatisfied on this port (no `libusb.so`/`.dylib` is built or deployed), and IOKit is
greenfield.

**External prior art (web-grounded, not in the AROS tree).**
- **libusb has a first-class macOS backend built on IOKit/IOUSBHost** (`libusb/os/
  darwin_usb.c` upstream). So the cleanest path to a working pseudo-HCD is: build/ship
  **libusb for arm64-darwin** and load it the way `vusbhc` already expects (renaming the
  bare-name target from `libusb.so` to the darwin form). libusb is **LGPL-2.1**, which is
  compatible with AROS's licence as a *separately-built, dynamically-loaded* dependency
  (we load it through `hostlib.resource` exactly like `libSystem.dylib` and the other
  shims — no AROS source incorporates it). We do **not** read libusb's implementation;
  we use its **published API** as `vusbhc` already does (an interface, not source).
- **IOKit `IOServiceMatching(kIOUSBDeviceClassName)` + an `io_iterator`** is the
  classic, permission-free way to *enumerate* USB devices and read their
  vendor/product/class registry properties. This needs **no entitlement and no exclusive
  claim** — any process can walk the USB registry. This is the one fully-headless path.
- **IOUSBHost (modern) / IOUSBLib (legacy) for actually opening a device** generally
  needs the device **not claimed by an Apple kext** (Apple's class drivers bind HID,
  mass-storage, audio, CDC, etc. on attach) and, for many device classes, a code-signed
  app with USB entitlements or a custom DriverKit/dext. A hosted AROS process is hostile
  to all of that. This is the loop-fit wall, restated at the API level.

Net: the AROS side is **largely already written** (`vusbhc`); the missing pieces are a
darwin host shim + bring-up + an honest, narrow verification scope.

## Background: AROS USB contracts (grounded)

AROS USB is the **Poseidon** stack. Three layers matter; we hook the lowest one.

### Layer 1 — Poseidon core (`poseidon.library`) — NOT the hook point

`rom/usb/poseidon/poseidon.library.c` is the device/config/endpoint manager and the
class-driver dispatcher. It is host-agnostic and reused unchanged. Its relevant public
contract (the HCD-registration seam) is two LVOs in `poseidon.conf`:

```
APTR psdAddHardware(STRPTR name, ULONG unit)   (A0,D0)   poseidon.conf:53
APTR psdEnumerateHardware(APTR phw)            (A0)      poseidon.conf:55
VOID psdRemHardware(APTR phw)                  (A0)      poseidon.conf:54
```

`psdAddHardware(name, unit)` (impl `poseidon.library.c:4388`) allocates a
`struct PsdHardware`, copies the device `name`, and spawns a per-controller subtask
(`pDeviceTask`) that **`OpenDevice(name, unit, …)`** — i.e. *name* is a
`usbhardware.device`-class device name (e.g. `"vusbhci.device"`, or our
`"darwinusbhc.device"`) and *unit* selects a controller instance. `psdEnumerateHardware`
then drives the root hub and walks attached devices. The `AddUSBHardware` shell command
(`rom/usb/poseidon/shellcommands/AddUSBHardware.c`) is the user-facing entry:
`psdAddHardware(devname, unit)` then `psdEnumerateHardware(phw)`. **This is the seam a
new host-backed controller plugs into** — we do not touch Poseidon; we provide a device
whose name we hand to `psdAddHardware`.

### Layer 2 — the HCD interface (`usbhardware.device`) — THE hook point

A Poseidon host controller is an exec **device** implementing the `usbhardware.device`
command protocol over `struct IOUsbHWReq`. This is the precise contract a host-backed
pseudo-HCD implements. From `compiler/include/devices/usbhardware.h`:

```c
struct IOUsbHWReq {                         /* usbhardware.h:80 (+ V1/V2 unrolled) */
    struct IORequest    iouh_Req;           /* :27  basic IORequest (io_Command, io_Unit, …) */
    UWORD               iouh_Dir;           /* :30  UHDIR_IN / UHDIR_OUT / UHDIR_SETUP */
    UWORD               iouh_DevAddr;       /* :31  USB device address (0–127) */
    UWORD               iouh_Endpoint;      /* :32  endpoint (0–15) */
    UWORD               iouh_MaxPktSize;    /* :33  max packet size */
    ULONG               iouh_Actual;        /* :34  bytes actually transferred (we fill) */
    ULONG               iouh_Length;        /* :35  buffer size */
    APTR                iouh_Data;          /* :36  in/out buffer */
    ULONG               iouh_NakTimeout;    /* :38  timeout ms */
    struct UsbSetupData iouh_SetupData;     /* :39  8-byte SETUP for control transfers */
    ...
};
```

The **command set** (`usbhardware.h:116`):

```
UHCMD_QUERYDEVICE   (CMD_NONSTD+0)   advertise controller capabilities (tag list)
UHCMD_USBRESET      (CMD_NONSTD+1)   reset the bus
UHCMD_USBRESUME / USBSUSPEND / USBOPER               bus power state
UHCMD_CONTROLXFER   (CMD_NONSTD+3)   control transfer (uses iouh_SetupData)
UHCMD_ISOXFER       (CMD_NONSTD+4)   isochronous transfer
UHCMD_INTXFER       (CMD_NONSTD+5)   interrupt transfer
UHCMD_BULKXFER      (CMD_NONSTD+6)   bulk transfer
```

A controller is opened via `BeginIO`/`AbortIO` (the device's `beginio_func`/
`abortio_func`). `UHCMD_QUERYDEVICE` returns capability tags (`usbhardware.h:187`):
`UHA_Manufacturer`, `UHA_ProductName`, `UHA_Capabilities` (a bitfield —
`UHCF_USB20`/`UHCF_USB30`/`UHCF_ISO`, `usbhardware.h:207`), etc. Errors are the
`UHIOERR_*` codes (`usbhardware.h:131`: `UHIOERR_NO_ERROR`, `STALL`, `TIMEOUT`,
`USBOFFLINE`, `HOSTERROR`, …) written into `iouh_Req.io_Error`.

**The in-tree reference implementation of this contract over a host library is
`vusbhc`** — `vusbhci_device.c:229` `BeginIO` is the literal dispatch table for the
commands above, and `vusbhci_device.c:349` `cmdQueryDevice` is the literal capability
advertisement (`UHA_Manufacturer = "The AROS Development Team"`,
`UHA_ProductName = "Hosted Host Controller Interface (libusb)"`,
`UHA_Capabilities = UHCF_USB20|UHCF_ISO`). A darwin HCD copies this shape.

### Layer 3 — class drivers (`rom/usb/classes/`, `workbench/devs/USB/classes/`)

`hub.class`, `bootmouse.class`, `bootkeyboard.class`, `printer.class`,
`massstorage.class` (per `rom/usb/README.md`) sit *above* the controller and are
host-agnostic; reused unchanged. They are why "enumerate + open one device" is the right
MVP: once a device enumerates and its descriptors read, the matching class driver binds
without further host work.

### USB descriptors & device identity (`devices/usb.h`)

The descriptors that flow over control transfers and feed enumeration are standard USB
2.0/3 layouts (`compiler/include/devices/usb.h`): `struct UsbStdDevDesc` (`usb.h:118` —
`bDeviceClass` `:123`, `idVendor` `:127`, `idProduct` `:128`), `UsbStdCfgDesc` (`:137`),
`UsbStdIfDesc` (`:154`), `UsbStdEPDesc` (`:169`). Descriptor-type constants `UDT_DEVICE`
… `UDT_ENDPOINT` (`usb.h:66`). These are dictated by the **USB specification** (`[PUB]`),
not authored by AROS — the same fields IOKit/libusb expose for a device. Enumeration
verification (below) asserts on `idVendor`/`idProduct`/`bDeviceClass`.

### Reference points already de-risked in this repo

- **Host-call boundary**: H3 (`hosted/abishim.S`) proved Apple's variadic ABI; non-
  variadic host calls (all of libusb/IOKit's USB entry points) go straight through.
- **Host symbol resolution**: `hostlib.resource` — `vusbhc` already uses it for libusb
  (`vusbhci_bridge.c:219`), the same mechanism the audio/socket/clipboard shims use.
- **Device-on-the-I/O-path**: H11 (`hosted/device.c`) ran a real exec `IORequest` →
  device task → real macOS syscall → reply under preemption. A `usbhardware.device` is
  exactly this — `IOUsbHWReq` is an `IORequest` subclass.
- **Host-thread → AROS wake (the load-bearing one)**: `host-wake-pattern.md` and the
  bsdsocket spec's `§R-DARWIN-WAKE` establish the darwin rule — a foreign host thread
  must not `Signal` AROS; the AROS side **timer-polls** a readiness/completion stash on a
  `Delay()`/`timer.device` tick. `vusbhc` already conforms (its handler task polls).

## Design

### Host side (the USB shim) — choose libusb, keep an IOKit enumeration fallback

Three host options, compared honestly:

| Option | What it gives | Headless? | Permissions | Verdict |
|--------|---------------|-----------|-------------|---------|
| **(A) libusb (Darwin/IOKit backend)** | full transfer API (`control/interrupt/bulk`, hotplug) — the exact verbs `vusbhc` already calls | **enumerate: yes**; open/transfer: only for **unclaimed** devices | open needs the device free of an Apple kext; some classes need root/entitlements | **design target** — AROS side already written against it |
| **(B) IOKit/IOUSBHost direct** | native, no extra dependency | enumerate: yes | open/transfer hostile to a hosted process (exclusive access, matching, often a dext) | enumeration only |
| **(C) IOKit enumeration-only** | `IOServiceMatching(kIOUSBDeviceClassName)` + `io_iterator`, registry properties | **yes, always** | **none** | **the only fully-unattended milestone** |

**Recommendation: (A) libusb as the transfer backend + (C) IOKit as the enumeration
oracle.** libusb on macOS *is* a thin layer over IOKit/IOUSBHost, so (A) and (C) agree
on the device list; we use (C)'s permission-free enumeration as the **headless ground
truth** and (A) for the (attended) transfer paths. The AROS side is already written
against libusb's API (`vusbhc`), so (A) costs the least AROS work.

The shim is native arm64 C (peer of `hosted/coreaudio/coreaudio_shim.c`,
`hosted/bsdsocket/bsdsock_shim.c`), reached via `hostlib.resource`. Two sub-shapes:

1. **Use libusb directly** (cheapest): build/ship `libusb.dylib` for arm64-darwin and
   point `vusbhc`'s `hostlib_load_so` at it (the bare-name target becomes the darwin
   form; `vusbhci_bridge.c:222`). No new shim file — `vusbhc` *is* the bridge. The only
   new host artifact is the libusb build itself.
2. **A thin `libusbhost.dylib` of our own** (more control, mirrors the other features):
   expose a small neutral C ABI (`uhw_enumerate`, `uhw_open`, `uhw_control`,
   `uhw_submit_int`/`bulk`, `uhw_poll_completions`) that the shim implements over
   **either** libusb **or** raw IOKit, and load *that*. This decouples the AROS side from
   libusb's exact API and lets enumeration go through permission-free IOKit while
   transfers (optionally) go through libusb. Cleaner for the loop; one more file.

Lean toward **(1) for the transfer milestones** (least AROS work, reuses `vusbhc`
verbatim) and **a tiny IOKit enumeration probe for the headless milestone** (option C —
it need not even touch AROS for the first spike).

### AROS side (the pseudo-HCD)

**Reuse `rom/usb/vusbhc/` essentially as-is** — it already implements the HCD contract
(`BeginIO`/`AbortIO`, `UHCMD_*`, `cmdQueryDevice`, root-hub emulation, the libusb
bridge, the timer-polled handler task). The darwin work is bring-up, not authorship:

- Make `vusbhci.device` build for darwin-aarch64 and install under `Devs/USBHardware`
  (`vusbhci.conf` `moduledir`). Confirm the mmake target `kernel-usb-vusbhci`
  participates in the darwin build (`vusbhc/mmakefile.src`).
- Satisfy the libusb dependency: the bare name `vusbhci_bridge.c:222` opens (`libusb.so`)
  must resolve to the deployed arm64 `libusb.dylib` via the same
  `DYLD_FALLBACK_LIBRARY_PATH=~/lib` mechanism the other shims use (run-window.sh/
  aros-ctl set it). Either build libusb to that name or add a tiny alias.
- Register it with Poseidon: `AddUSBHardware vusbhci.device` (or seed it at boot like
  the other controllers). Then `psdEnumerateHardware` walks whatever libusb/IOKit sees.

If we prefer not to carry libusb, fork `vusbhc` to a `darwinusbhc.device` that calls our
`libusbhost.dylib` neutral ABI instead — same files, the bridge swapped. Decision in
spec.md; default is **reuse `vusbhc` + ship libusb**.

### The bridge (async completion across the host boundary — the darwin-safe pattern)

USB transfers are inherently asynchronous: you submit, and completion arrives later. On
a "normal" host that completion lands on a host run-loop/callback thread (libusb's event
thread, or an IOKit `CFRunLoopSource`). **On darwin-aarch64 that thread must not touch
AROS** (the `host-wake-pattern.md` caveat: a task woken from host-thread context trips
semaphore ops under the threaded scheduler — `cocoa_input.c:546`). The shipped darwin
drivers all solve this by **polling on a timer tick**, and `vusbhc` already does exactly
the right thing:

- Submitted transfers are queued per type and a **dedicated AROS handler task**
  (`vusbhci_device.c:34` `handler_task`) loops: drain the queues → submit to libusb →
  **`call_libusb_event_handler()`** (which runs `libusb_handle_events`, firing
  `callbackUSBTransferComplete` **on this AROS task**, not a foreign thread) →
  `DoIO(timer.device, 100 ms)` and repeat (`vusbhci_device.c:107–118`).
- `callbackUSBTransferComplete` (`vusbhci_bridge.c`) fills `iouh_Actual`/`io_Error` and
  `ReplyMsg`s the `IOUsbHWReq` — all on the AROS handler task, so the reply is a normal
  AROS `ReplyMsg`, never a host-thread `Signal`. This is the design's whole correctness
  argument and it is already in the tree.

**What we must change/verify for this port** (conform to `host-wake-pattern.md`):
- The 100 ms fixed tick (`vusbhci_device.c:109`) is fine for mass-storage/printer but
  too coarse for interactive HID; tighten to the ~50 Hz the Cocoa input HIDD uses, or
  make it adaptive (poll faster while transfers are pending — the code already tracks
  `*xfer_pending`). This is a latency tune, not a correctness change.
- **libusb's own event thread / hotplug thread must not call AROS.** `vusbhc` keeps
  libusb event handling on the AROS handler task (good); but the **hotplug callback**
  (`vusbhci_bridge.c:243` `hotplug_callback_event_handler` → `uhwCheckRootHubChanges` →
  manipulates AROS unit state) runs inside `libusb_handle_events`, which `vusbhc` calls
  from the AROS task, so it is also on-task — **verify libusb on Darwin does not spawn a
  separate hotplug thread** that would invoke the callback off-task. If it does, the
  callback must only set an `_Atomic` flag and let the handler task react (R-W1/R-W3 of
  host-wake-pattern). **UNVERIFIED** until measured on Apple Silicon.
- The `vusbhci_bridge.c` control path is **synchronous** (`libusb_control_transfer`
  blocks); on the AROS handler task that blocks *all* USB while it waits. For the
  headless MVP this never executes (no device opened); for the attended transfer
  milestones, prefer the async `libusb_submit_transfer` path for control too, or accept
  the serialisation (it matches upstream `vusbhc`). Flagged.

## Plan — spikes in the loop

Each marker is a standalone host binary (one-binary-per-marker, like `hosted/*`) with a
single PASS/FAIL verdict the agent reads. **Marker prefix `[UB*]`.** Be ruthless about
which are headless vs attended — most are attended, and that is the point.

- **[UB1] host USB enumeration, asserted (FULLY HEADLESS — the only one).** A pure host
  probe (no AROS): via IOKit `IOServiceMatching(kIOUSBDeviceClassName)` + an
  `io_iterator`, list every USB device and read `idVendor`/`idProduct`/`bDeviceClass`
  from the registry. PASS = a **known always-present device** appears with expected IDs —
  on Apple Silicon the **USB root hub(s)** and the Apple internal devices (e.g. an Apple
  vendor-ID `0x05ac` device, or the always-present `AppleUSBXHCI` root hub) are present
  on every Mac, so assert "≥1 device, and the root hub / an Apple-vendor device is in the
  list". No permissions, no hardware plugged, no human. This is the project's only
  unattended USB proof. *(grounds the harness, like H7's `pngprobe` / the socket [N1].)*
- **[UB2] libusb enumeration agrees (HEADLESS, gated on libusb building).** Same probe
  but through **libusb** (`libusb_get_device_list` / `libusb_get_device_descriptor`).
  PASS = libusb's device list matches [UB1]'s IOKit list on (vendor, product) for the
  always-present devices. Proves the libusb arm64 build works and sees the same tree
  IOKit does — the dependency `vusbhc` needs. Still no AROS. **Headless.**
- **[UB3] AROS Poseidon enumerates the same tree (HEADLESS-ish — needs booted AROS).**
  Build `vusbhci.device` for darwin, deploy libusb to `~/lib`, boot windowed AROS, run
  `AddUSBHardware vusbhci.device` then `PsdDevLister` (the in-tree lister,
  `rom/usb/poseidon/shellcommands/PsdDevLister.c`). PASS = Poseidon reports the **same
  always-present device(s)** [UB1] asserted (assert on vendor/product in the captured
  console). This is the thesis end-to-end *for enumeration* — the real win that fits the
  loop. Verified via `aros-ctl` screenshot + the host [UB1] oracle. **Caveat:** booting
  AROS is itself currently fragile (see bsdsocket README gotchas), and Poseidon may need
  the root-hub/device-class drivers resident — so this rides the boot graft.
- **[UB4] control transfer: read a device descriptor (ATTENDED — needs an unclaimed
  device).** Open one device via libusb and issue `GET_DESCRIPTOR(DEVICE)` (a control
  transfer), asserting the returned 18-byte descriptor's `idVendor`/`idProduct` match the
  enumerated values. PASS = descriptor bytes match. **OUT of the unattended loop:**
  opening a device requires it be **unclaimed by an Apple kext**; most always-present
  devices are claimed, so this needs a deliberately-attached, driver-free device (or
  detaching the kext — root). **Attended fallback:** a documented manual step ("plug in a
  known CDC/HID dongle, run `make usb-smoke`"); not a CI gate.
- **[UB5] interrupt-IN from a HID device (ATTENDED — needs hardware + permissions).**
  Open a HID device, claim its interface (detaching the macOS HID kext — needs root or an
  entitlement), and read one interrupt-IN report, asserting plausible bytes. PASS = a
  report arrives. **OUT of the loop entirely** — needs a physical HID device, kext
  detach, and elevated permission. Documented as the proof that a *class device* works,
  run by a human with hardware in hand.
- **[UB6] graft: a USB class device works through Poseidon (ATTENDED — the full thesis,
  hardware-bound).** With `vusbhci.device` loaded and a supported device attached
  (mass-storage stick → AROS volume, or a USB printer → `printer.class`), drive it from
  AROS (`Dir USB0:` for storage, or print a test page). PASS = the device's function
  works through AROS. **Far OUT of the loop** — the destination, not a spike; needs the
  boot graft *and* hardware *and* permissions.

Headless tally: **[UB1] fully headless; [UB2] headless (gated on libusb build); [UB3]
headless-on-booted-AROS (rides the boot graft).** Everything from **[UB4] onward is
attended** and explicitly not a loop gate. That ratio is the deferral case.

## How we verify it unattended

The honest answer: **only enumeration verifies unattended.** No human, no TCC prompt
(reading the USB device *registry* needs no entitlement — it is not camera/mic/screen-
recording; `IOServiceMatching`/`io_iterator` is open to any process):

1. **Primary headless oracle: IOKit enumeration ([UB1]).** Walk the USB registry, assert
   a known always-present device (root hub / Apple-vendor device) appears with expected
   `idVendor`/`idProduct`/`bDeviceClass`. Deterministic on any Mac, no hardware to plug.
2. **Cross-check ([UB2]):** libusb's device list must equal IOKit's for the always-
   present devices — proves the transfer backend sees the same world.
3. **End-to-end-for-enumeration ([UB3]):** Poseidon's `PsdDevLister` over the darwin HCD
   reports the same device — asserted from the `aros-ctl` console capture against the
   [UB1] host oracle (two-sided, the H11 discipline).
4. **Everything else is attended.** Transfers ([UB4]+) need a real, unclaimed/claimable
   device and often elevated permission — verified by a human running `make usb-smoke`
   with hardware attached, never by the loop. A **software-loopback** alternative was
   investigated: macOS has no built-in user-space virtual-USB-device facility (unlike
   Linux's `usbip`/`gadgetfs`/`dummy_hcd`), so there is **no zero-hardware transfer path
   on darwin** without writing a DriverKit virtual-device dext (large, out of scope).
   This absence is the core reason the transfer milestones cannot join the loop.

A watchdog bounds every spike (a stuck libusb/IOKit call can never wedge the loop), the
[UB*] markers are unique and greppable, and the headless ones clean-exit on PASS.

## Why this is last (the deferral recommendation)

Stated plainly so the owner can decide:

- **Loop-fit is poor.** Exactly one milestone ([UB1], + its [UB2]/[UB3] echoes) runs
  unattended. The actual payoff — a *device* working — is [UB4]–[UB6], all of which need
  physical hardware and/or elevated permissions and **cannot** be a CI gate. Every other
  bridge in this batch verifies its real payoff headlessly (sockets → file/loopback,
  audio → render-to-WAV, clipboard → byte-exact, volume → two-sided file). USB cannot.
- **Permissions/entitlements are hostile to a hosted process.** Modern macOS gives
  Apple kexts first claim on attached devices; opening one generally needs it unclaimed,
  a code-signed app with USB entitlements, or a custom dext — none of which a `dlopen`'d
  shim in a hosted AROS process has. Enumeration is free; *use* is gated.
- **The marginal capability is niche.** Keyboard/mouse already work natively (Cocoa
  input HIDD). USB adds *arbitrary class devices* — valuable to a few users with a
  specific gadget, not to the desktop baseline.
- **The hard parts are already de-risked elsewhere.** The async-completion / host-thread
  boundary — the one genuinely new systems risk — is shared with audio and sockets and
  is *already solved* there and conformed-to by `vusbhc`. So little is *learned* by doing
  USB that isn't learned cheaper from audio/sockets.

**Recommended order:** ship sockets (done) and audio first; treat USB as a **post-port,
opportunistic** feature. When it is taken up, do **[UB1] only** as a real, permanently-
green headless probe (cheap, satisfying, no AROS needed), get **[UB3]** for free once the
boot graft is solid and libusb is building, and leave **[UB4]+** as documented attended
smokes a user runs with hardware. Do **not** block the loop on anything past [UB3].

## Risks & open questions

- **Loop-fit (the headline).** Only enumeration is unattended; no zero-hardware transfer
  path exists on darwin (no `dummy_hcd`/`gadgetfs` analogue). Mitigation: scope the loop
  to [UB1]–[UB3]; everything else attended. **This is a feature property, not a bug to
  fix.**
- **Entitlements / exclusive access.** `libusb_open`/`libusb_claim_interface` fail with
  `LIBUSB_ERROR_ACCESS` on kext-claimed devices (the `vusbhc` hotplug code already logs
  exactly this — `vusbhci_bridge.c`: *"access error, try running as superuser or create
  udev"*). On macOS the analogue is a claimed device + no entitlement. **Open:** which (if
  any) always-present device is *both* present *and* openable without root — likely none,
  hence [UB4] is attended.
- **Host-thread boundary on Darwin.** `vusbhc` pumps libusb on an AROS timer task (good),
  but **whether libusb-darwin spawns its own hotplug/event thread** that would fire the
  callback off-task is **UNVERIFIED**. If so, the callback must degrade to an `_Atomic`
  flag + handler-task reaction (host-wake-pattern R-W1/R-W3). Must be measured on Apple
  Silicon before [UB4].
- **libusb availability & licence.** Needs an arm64-darwin `libusb.dylib` built and
  deployed to `~/lib` under the bare name `vusbhc` opens (`vusbhci_bridge.c:222`). libusb
  is LGPL-2.1, used as a separately-built dynamically-loaded dependency (interface only,
  no source incorporated) — same posture as `libSystem.dylib`. Confirm the licence
  strategy with the owner before shipping (CLEANROOM note).
- **Poseidon residency / boot.** `psdEnumerateHardware` and the class drivers must be
  available; booting AROS is itself currently fragile (bsdsocket README gotchas). [UB3]
  rides the boot graft.
- **Polling latency for HID.** The 100 ms `vusbhc` tick (`vusbhci_device.c:109`) is too
  coarse for interactive HID; tighten to ~50 Hz or make adaptive. Latency tune, not
  correctness.
- **Control-transfer serialisation.** `vusbhc`'s control path is synchronous and blocks
  the handler task while waiting; acceptable for the rare control transfer, but prefer
  the async submit path if it bites. Flagged.
- **IOUSBHost vs libusb vs IOKit-enum — decided.** Enumeration via permission-free IOKit
  (the headless oracle); transfers via libusb (the AROS side is already written for it);
  IOUSBHost-direct only if we later need a transfer path libusb can't give. No reason to
  hand-write an IOKit transfer stack when libusb wraps it.

## References

AROS upstream (`../aros-upstream`):
- HCD contract: `compiler/include/devices/usbhardware.h` (`struct IOUsbHWReq` :80,
  fields :27–62, `UHCMD_*` :116, `UHA_*`/`UHCF_*` capability tags :187/:207,
  `UHIOERR_*` :131), `compiler/include/devices/usb.h` (`UsbStdDevDesc` :118 with
  `bDeviceClass`/`idVendor`/`idProduct`, descriptor types `UDT_*` :66).
- Poseidon HCD-registration seam: `rom/usb/poseidon/poseidon.conf` (`psdAddHardware`
  :53, `psdEnumerateHardware` :55, `psdRemHardware` :54),
  `rom/usb/poseidon/poseidon.library.c` (`psdAddHardware` impl :4388 — spawns the
  per-controller task that `OpenDevice`s the named `usbhardware.device`),
  `rom/usb/poseidon/shellcommands/AddUSBHardware.c` (the user entry: `psdAddHardware` +
  `psdEnumerateHardware`), `rom/usb/poseidon/shellcommands/PsdDevLister.c` (the lister
  used by [UB3]).
- **The in-tree libusb-backed HCD we reuse**: `rom/usb/vusbhc/` —
  `vusbhci.conf` (`modtype=device`, `moduledir=Devs/USBHardware`, `beginio_func BeginIO`),
  `vusbhci_device.c` (`BeginIO` dispatch :229, `cmdQueryDevice` :349, the timer-polled
  `handler_task` :34 / 100 ms tick :109–118, `BeginIO`/`AbortIO` LVOs),
  `vusbhci_bridge.c` (`hostlib.resource` open :219, `hostlib_load_so` libusb table,
  hotplug callback :243, `do_libusb_ctrl_transfer`/`intr`/`bulk`, the on-task completion
  `callbackUSBTransferComplete` → `ReplyMsg`), `vusbhci_bridge.h` (the libusb function
  table), `vusbhci_device.h` (`struct VUSBHCIUnit`/`VUSBHCIBase`, the per-type queues),
  `vusbhc/mmakefile.src` (`kernel-usb-vusbhci`).
- Class drivers (host-agnostic, reused): `rom/usb/classes/`,
  `workbench/devs/USB/classes/` (`hub`, `bootmouse`, `bootkeyboard`, `printer`,
  `massstorage` per `rom/usb/README.md`).
- The real PCI HCDs (no PCI on this host, so unbound): `rom/usb/pciusb/`
  (uhci/ohci/ehci), `rom/usb/pcixhci/` (xhci) — cited as the contract reference, not used.
- Host-symbol mechanism: `arch/all-hosted/hostlib/hostlib.conf` (`HostLib_Open` :14,
  `HostLib_Close` :15, `HostLib_GetPointer` :16, `HostLib_GetInterface` :18),
  `arch/all-unix/bootstrap/hostlib.h`.

This repo (`.`):
- `hosted/abishim.S` (H3 variadic ABI), `hosted/device.c` (H11 IORequest→task→host
  syscall→reply — the shape a `usbhardware.device` follows), `hosted/coreaudio/` +
  `hosted/bsdsocket/` (host shim + `hostlib.resource` + deploy-to-`~/lib` pattern this
  USB shim copies).
- `docs/features/host-wake-pattern.md` (the host-thread → AROS wake contract; the
  **DARWIN-AARCH64 CAVEAT** — foreign thread must not `Signal`, AROS timer-polls instead
  — that the USB completion path must obey, and `vusbhc`'s handler task already does).
- `docs/features/bsdsocket-net/README.md` + spec `§R-DARWIN-WAKE` (the proven precedent:
  kqueue/host pump for efficiency, timer-poll for the safe AROS handoff), and the boot
  gotchas that gate [UB3].

External prior art (web, not in the AROS tree — interfaces only, no implementation read):
- **libusb** — published cross-platform USB API with a macOS IOKit/IOUSBHost backend
  (`libusb/os/darwin_usb.c`); LGPL-2.1; loaded as a separate dynamic dependency exactly
  as `vusbhc` already expects. We use its API (an interface), not its source.
- **Apple IOKit** — `IOServiceMatching(kIOUSBDeviceClassName)` + `io_iterator` for
  permission-free USB enumeration and registry properties (`idVendor`/`idProduct`/
  `bDeviceClass`); **IOUSBHost**/legacy **IOUSBLib** for device open/transfer, which
  generally require the device unclaimed and/or USB entitlements — the loop-fit wall.
- **No user-space virtual-USB-device facility on macOS** (no `usbip`/`gadgetfs`/
  `dummy_hcd` analogue without a DriverKit dext) — the concrete reason a zero-hardware
  transfer milestone is impossible here, fixing USB at "enumeration-only" for the loop.
