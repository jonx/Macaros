# USB (IOKit / libusb)

> Status: **planned (not started)** · Target: aarch64-darwin hosted · Drafted 2026-06-28
> This is the **lowest-priority and hardest** bridge of the batch and a **poor fit for
> the unattended loop** — read "Why this is last" below before picking it up. For the
> full design and the implementation spec see [design.md](design.md) and [spec.md](spec.md).

## What & why

Give the hosted AROS access to the Mac's USB devices, the project-thesis way: *macOS owns
the drivers; AROS reaches them via standard exec I/O.* AROS already has a complete USB
stack — **Poseidon** (`rom/usb/`) — with class drivers (hub, keyboard, mouse, printer,
mass storage) and a clean **host-controller-driver (HCD) seam**: Poseidon drives any
`usbhardware.device` with `IOUsbHWReq` transfer requests. On this host there is no PCI to
probe, so we would supply a **host-backed pseudo-HCD** — a `usbhardware.device` whose
"hardware" is the Mac's USB stack, reached through a host shim (libusb / IOKit) via
`hostlib.resource`.

## The key finding: most of it already exists

AROS ships **`rom/usb/vusbhc/`** — a libusb-backed *virtual* USB host controller that is
**exactly** this pattern: a `usbhardware.device` (`vusbhci_device.c:229` `BeginIO` over
`UHCMD_*`) that bridges every transfer to **libusb** through `hostlib.resource`
(`vusbhci_bridge.c:219`), including hotplug. So the work is **bring-up, not authorship**:

1. build `vusbhci.device` for darwin-aarch64 and ship an arm64 `libusb.dylib` to `~/lib`
   (the bare name `vusbhci_bridge.c:222` opens), like the other shims;
2. confirm the **darwin-safe completion boundary** — `vusbhc` already pumps libusb on an
   **AROS timer-polled handler task** (`vusbhci_device.c:34`), never a foreign-thread
   `Signal`, which is exactly what [host-wake-pattern.md](../host-wake-pattern.md) requires
   on this port (re-verify on Apple Silicon);
3. scope verification honestly — see below.

## Why this is last (the honest note)

- **Only enumeration verifies unattended.** Listing the host's USB tree via IOKit
  (`IOServiceMatching(kIOUSBDeviceClassName)` + `io_iterator`) and asserting a known
  always-present device — the **USB root hub** / an Apple-vendor device — needs **no
  permission and no hardware plugged in**. That is the one fully-headless milestone
  (`[UB1]`), echoed by libusb (`[UB2]`) and Poseidon (`[UB3]`).
- **Every device milestone is attended.** Opening a device and running a control /
  interrupt / bulk transfer (`[UB4]`–`[UB6]`) needs a **physical device** that is
  **unclaimed by an Apple kext** and often **root or USB entitlements** — hostile to a
  `dlopen`'d shim in a hosted process. macOS has **no** user-space virtual-USB-device
  facility (no `usbip`/`gadgetfs`/`dummy_hcd` analogue without a DriverKit dext), so there
  is **no zero-hardware transfer path** to put in the loop. Unlike sockets (loopback),
  audio (render-to-WAV), or the host volume (two-sided files), USB's real payoff cannot be
  a CI gate.
- **The marginal capability is niche.** Keyboard/mouse already work natively (Cocoa input
  HIDD); USB adds *arbitrary class devices*, each needing the gadget in hand.
- **Little is learned cheaply.** The one new systems risk — async completion across the
  host-thread boundary — is shared with audio and sockets and is **already solved** there
  and conformed-to by `vusbhc`.

**Recommendation: defer behind audio and sockets.** When taken up, do `[UB1]` as a
permanently-green headless probe (cheap, no AROS, no hardware), get `[UB3]` once the boot
graft is solid and libusb is building, and leave `[UB4]+` as documented attended
`make usb-smoke` steps a user runs with hardware. **Do not block the loop on anything past
`[UB3]`.**

## Marker plan

| Marker | What | Loop status |
|--------|------|-------------|
| `[UB1]` | IOKit enumeration; assert an always-present device (root hub / Apple vendor) | **fully headless** |
| `[UB2]` | libusb enumeration agrees with IOKit (the dependency `vusbhc` needs) | headless (needs the libusb build) |
| `[UB3]` | Poseidon (`AddUSBHardware` + `PsdDevLister`) enumerates the same tree | headless on booted AROS (rides the boot graft) |
| `[UB4]` | control transfer — read a device descriptor | **attended** (unclaimed device) |
| `[UB5]` | interrupt-IN from a HID device | **attended** (hardware + kext detach + root) |
| `[UB6]` | a class device works through Poseidon (storage volume / printer) | **attended** (boot graft + hardware) |

## Links

- [design.md](design.md) — the grounded HCD contract, the libusb-vs-IOKit comparison, the
  full `[UB*]` plan, and the deferral case.
- [spec.md](spec.md) — the implementation spec (reuse `vusbhc`; the optional neutral
  `libusbhost.dylib` ABI; the darwin-safe completion model; provenance).
- [../host-wake-pattern.md](../host-wake-pattern.md) — the host-thread → AROS wake
  contract (the DARWIN-AARCH64 caveat the completion path obeys).
- [../bsdsocket-net/README.md](../bsdsocket-net/README.md) — the proven precedent for the
  same boundary (host pump for efficiency, timer-poll for the safe AROS handoff).
- In-tree: `rom/usb/vusbhc/`, `rom/usb/poseidon/`,
  `compiler/include/devices/{usbhardware.h,usb.h}` (the cited contracts).

## Provenance

Independent work: no third-party implementation source — emulator, agent, driver, or
otherwise — was read, searched, or consulted; built from the USB 2.0/3 spec, Apple
IOKit/IOUSBHost docs, the **libusb published API** (used as an interface, exactly as the
in-tree `vusbhc` already consumes it), the in-tree AROS Poseidon/`vusbhc` modules, and
this project's H-series spikes. See [spec.md](spec.md)'s provenance banner.
