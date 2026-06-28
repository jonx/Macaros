# Feature plans — host capabilities for hosted AROS on Apple Silicon

Grounded design/plan docs for things that would be **genuinely useful and don't exist
yet** for the `aarch64-darwin` hosted port. Each was checked against the upstream tree
(`/Users/user/Source/aros-upstream`) *and* the web before writing — every AROS contract
cited points at a real file, every external project at a real repo, and anything that
couldn't be confirmed is marked **UNVERIFIED**.

All of these are the project thesis applied to one more surface: *macOS owns the
drivers; AROS reaches them via standard exec I/O.* And all must verify in the
**unattended loop** — build → run → observe → one PASS/FAIL — with no macOS TCC /
Screen-Recording manual step. Each carries its own spike plan with greppable markers,
the same way Phase 1/2 used `[M*]` / `[H*]`.

## Layout

Each feature has its own folder:

```
<feature>/design.md   — the why, the grounded contracts, the spike plan
<feature>/spec.md      — implementation spec for a fresh implementer (as written)
CLEANROOM.md           — the independent-work process that governs every spec.md
```

## The features

| Feature | One-line | Status |
|---------|----------|--------|
| [68k JIT](68k-jit/design.md) · [spec](68k-jit/spec.md) | A host 68k→AArch64 translator so the hosted AROS runs real classic Amiga binaries at native speed | design + spec done · `[J0]` → adapt Emu68 |
| [Cocoa/Metal display](cocoa-metal-display/design.md) · [spec](cocoa-metal-display/spec.md) | A live macOS window (Apple-native Metal) replacing H7's render-to-PNG | design + spec done · **`[D1]` shim GREEN** |
| [Host app shell](host-app-shell/design.md) · [spec](host-app-shell/spec.md) | Make the window a first-class Mac app — menu bar, About, custom icon, two-tier Settings; the UI home for screenshot/video + drive/clipboard/sharing | design + spec done |
| [Clipboard bridge](clipboard-bridge/design.md) · [spec](clipboard-bridge/spec.md) | Two-way copy/paste between macOS NSPasteboard and AROS `clipboard.device` | design + spec done |
| [Host volume](host-volume/design.md) · [spec](host-volume/spec.md) | A real macOS folder mounted as an AROS volume, drag-from-Finder | design + spec done · foundation landed |
| [CoreAudio audio](coreaudio-audio/README.md) · [design](coreaudio-audio/design.md) · [spec](coreaudio-audio/spec.md) | Real sound via a CoreAudio-backed AHI sub-driver | **built** — AHI driver + audible smoke proven |
| [Host BSD sockets](bsdsocket-net/README.md) · [design](bsdsocket-net/design.md) · [spec](bsdsocket-net/spec.md) | Working TCP/IP by forwarding `bsdsocket.library` to the Mac's native sockets | **built** — sockets + WaitSelect + DNS proven live |

### New host integrations — devices, media & desktop gadgets (drafted 2026-06-28)

A second wave, each the project thesis applied to one more AROS surface. All seven
were checked against the upstream tree before drafting; every design + spec is ready
for review. They group three ways.

**Devices & I/O** — an AROS exec device/library backed by a host facility via a
`hostlib` shim (the audio/sockets shape):

| Feature | One-line | Status · key finding |
|---------|----------|----------------------|
| [Printing](printing/design.md) · [spec](printing/spec.md) · [readme](printing/README.md) | `printer.device` + the in-tree PostScript driver → macOS **CUPS** / **print-to-PDF**, plus the missing **printer-status** gadget | design + spec done · **print-to-PDF host engine BUILT & GREEN** (`hosted/printing/`, `[PRPDF]` — real text+raster PDFs via CGPDFContext/CoreText, no PS→PDF) · still **blocked at `[PR0]`** for the AROS driver: `printertag.h:29` `#error`s on aarch64 (no magic word) |
| [Serial bridge](serial-bridge/design.md) · [spec](serial-bridge/spec.md) · [readme](serial-bridge/README.md) | `serial.device` (+ `parallel.device`) → host tty / PTY via POSIX **termios** | design + spec done · backend **half-exists** (`arch/all-unix/hidd/serial` is `#if 0`'d) — finish + retarget; wakes via `unixio.hidd`'s timer-poll, no foreign thread |
| [MIDI (CoreMIDI)](midi-coremidi/design.md) · [spec](midi-coremidi/spec.md) · [readme](midi-coremidi/README.md) | `camd.library` driver → **CoreMIDI**, send + receive | design + spec done · inbound path is **new work** (in-tree templates are TX-only); gated on native-`LoadSeg` driver discovery; read-proc → SPSC ring → polled AROS task |
| [USB (IOKit/libusb)](usb-iokit/design.md) · [spec](usb-iokit/spec.md) · [readme](usb-iokit/README.md) | Poseidon HCD seam → **libusb** transfers, **IOKit** enumeration | design + spec done · **lowest priority / poor loop fit** — only enumeration (`[UB1]`–`[UB3]`) is unattended; AROS already ships a libusb vHCI (`rom/usb/vusbhc/`) to adapt; defer behind audio & sockets |

**Media & typography:**

| Feature | One-line | Status · key finding |
|---------|----------|----------------------|
| [Fonts (CoreText)](fonts-coretext/design.md) · [spec](fonts-coretext/spec.md) · [readme](fonts-coretext/README.md) | The Mac's installed fonts inside AROS via **CoreText** (+ CoreGraphics for the deep path) | design + spec done · MVP = *provision* host `.ttf`/`.otf` through the existing freetype engine (reuses host-volume); deeper = a `coretext.library` glyph engine on the verified `OT_Engine` seam |

**Desktop gadgets** — surface a bridge in the AROS UI (gated numerically; render
screenshotted by the control harness):

| Feature | One-line | Status · key finding |
|---------|----------|----------------------|
| [System monitor](system-monitor/design.md) · [spec](system-monitor/spec.md) · [readme](system-monitor/README.md) | New **Network / Disk / Host** pages in the in-tree `SysMon`, fed by a read-only host-stats shim | design + spec done · host-sourced CPU% is the **only** non-zero number on this port (AROS `GCIT_ProcessorLoad` is a darwin stub returning 0) — host-sourcing is correctness, not polish |
| [Volume / mixer](volume-mixer/design.md) · [spec](volume-mixer/spec.md) · [readme](volume-mixer/README.md) | A Commodities **volume slider** → AHI master volume (+ optional macOS output volume) | design + spec done · builds on [CoreAudio audio](coreaudio-audio/README.md); `DoMasterVolume` is gated on `AHIACF_POSTPROC` (`mixer.c:1045`), so a sub-driver-level gain (`R-GAIN`) is the documented fallback |

> **Deferred:** host-backed **Datatypes** (HEIC/AVIF/WebP/MP4/MOV/PDF decoders via
> ImageIO / AVFoundation / Quartz) was discussed alongside these but is **not in this
> batch** — left for later by decision.

### Foundations these depend on (drafted 2026-06-28)

Two low-level layers several of the features above need in order to *build and run* —
**not "ring 0"** (the hosted port runs entirely at EL0 by design), but the host-backed
equivalents of what bare metal gets from privileged registers and a real module loader.

| Foundation | What it provides | Status · key finding |
|------------|------------------|----------------------|
| [processor.resource backend](processor-resource/design.md) · [spec](processor-resource/spec.md) · [readme](processor-resource/README.md) | A darwin/AArch64 `processor.resource` backend — CPU model, P/E core counts, features, endianness, load — from host `sysctl`/Mach (since `MIDR_EL1`/`ID_AA64*` are unreadable at EL0) | **host shim BUILT & GREEN** (`hosted/hostcpu/`, `[CP1]`/`[CP2]`/`[CP3]` pass — "Apple M5", 4P+6E, LE, NEON; lossless dylib ABI) · AROS arch backend pending graft · unblocks **CPUInfo** (folds in its private hack), **ShowConfig**, the **SysMon Host page** · `PROCESSORARCH_ARM64` missing (reports `ARM`) |
| [native modules (LoadSeg + W^X)](native-modules/design.md) · [spec](native-modules/spec.md) · [readme](native-modules/README.md) | Build + `LoadSeg` natively-built AArch64 disk modules (drivers, libraries) into Apple-Silicon W^X executable memory | design + spec done · **largely already built** — commit `71f75760` tags loaded code `MEMF_EXECUTABLE` and flips it to `R/X` (`internalloadseg_elf.c:1247`); the doc is *verify-and-reconcile* · unblocks the loadable **printer / MIDI / AHI / fonts** drivers · risk: `CacheClearE` is a no-op (I-cache coherency), and the native path uses `mprotect`+entitlement, *different* from `[J1]`'s `MAP_JIT` |

These answer *"what do CPUInfo and the printer actually need?"* — CPUInfo needs the first;
the loadable drivers need the second. Neither is a privilege-escalation layer.

### Native software ports — not host bridges (drafted 2026-06-28)

A different shape from everything above: the deliverable is **ARM code built *for*
AROS** (compiled by the crosstools / Rust's own LLVM), not a macOS facility bridged
in through a `hostlib` shim. They share one substrate — **hardening `posixc`** (the
`printf` float bug is the prototype) unblocks both — so the libc-completeness grind
is done once for two payoffs, and Rust can FFI straight into native `libavcodec`
once both exist.

| Feature | One-line | Status |
|---------|----------|--------|
| [Native ffmpeg / libav*](ffmpeg-native/README.md) | `libav*` built natively for aarch64 AROS so AROS programs `-lavcodec` | scoping · a *completeness/correctness* port on `posixc`, not a host bridge; `--disable-asm` is the baseline |
| [Rust on AROS](rust-aros/README.md) | Rust targeting aarch64 AROS — no_std (core+alloc) now, std-on-`posixc` later | **`[RS0]`/`[RS1]` no_std start builds to aarch64-AROS ELF** ([`hosted/rust/`](../../hosted/rust/)) · codegen free; std is an OS port; `net` rides on `bsdsocket` |

## Driving & verifying it — the control harness (built)

Unlike the planned features above, one piece is **already built and in daily use**:
[`aros-ctl`](control-harness/README.md), the "puppet master". It boots the hosted
AROS windowed and drives it from the command line — type at the shell, click, move
the mouse, screenshot the framebuffer, tail the log — with **no window-server
session and no TCC/Screen-Recording prompt**. Injected input shares the exact drain
path as real NSEvents, and capture reads the offscreen oracle, so it both
faithfully reproduces input and stays inside the unattended loop. It is also the
seed of the embeddable library's input/capture API.
→ [README](control-harness/README.md) · [design](control-harness/design.md) · [spec](control-harness/spec.md)

Two more built tools live alongside it for **bring-up debugging** — the `TestLib`
load-tester and the toggleable `lddemon` loader trace — that turn "`OpenLibrary`
returned `NULL`" into a step-by-step picture (file loaded? resident registered?).
→ [Debug & bring-up tools](debug-tools/README.md)

The current **deployment workflow** is also documented, because this port has
several runnable copies of the same artifacts (`~/lib`, the AROS boot image, app
settings, and `aros-host.conf`). Use it whenever a fix appears not to take.
→ [Deployment workflow](deployment/README.md)

A third built tool **measures** rather than debugs: [`bench-run`](benchmarks/README.md)
(`make bench`) runs AROS's in-tree `exec`/`clib` micro-benchmarks on booted AROS via
the control harness and prints live numbers. It surfaced — and we fixed — a real
C-library bug (`printf` dropped floats on this target; `UPSTREAM-NOTES.md` item 34),
and it takes any self-terminating benchmark by name (a ported Dhrystone slots in).
→ [Benchmarks](benchmarks/README.md)

For the current source-tree gap map and active work order, see the
[Darwin AArch64 port inventory](darwin-aarch64-port-inventory.md). It records what
is already adapted, what is merely hardened enough for the desktop baseline, what
still needs to be created, and what is deferred until after port completion.

## How they group

- **The standout** — [68k JIT](68k-jit/design.md). The uniquely-Apple-Silicon payoff and
  the natural destination of the W^X / `MAP_JIT` work. Prior art exists out-of-tree:
  **Emu68** (MPL-2.0, adoptable) for the translator, **emumiga** for the AROS integration.
- **"Make it a real Mac app"** — the [host app shell](host-app-shell/design.md) is the
  umbrella (menu bar, About, icon, two-tier Settings, screenshot/video) that surfaces
  [display](cocoa-metal-display/design.md), [clipboard](clipboard-bridge/design.md), and
  [host volume](host-volume/design.md) as one native UI.
- **Infrastructure** — [audio](coreaudio-audio/design.md),
  [sockets](bsdsocket-net/design.md).
- **Devices, media & gadgets (2026-06-28 batch)** — seven more thesis surfaces:
  device bridges ([printing](printing/design.md), [serial](serial-bridge/design.md),
  [MIDI](midi-coremidi/design.md), [USB](usb-iokit/design.md)), the
  [Mac's fonts](fonts-coretext/design.md), and two desktop gadgets that *surface* the
  bridges ([system monitor](system-monitor/design.md), [volume](volume-mixer/design.md)).
  See the grouped table above.
- **Foundations (2026-06-28)** — two host-backed low-level layers the above lean on: the
  [processor.resource backend](processor-resource/design.md) (CPU facts via `sysctl`) and
  [native module loading](native-modules/design.md) (`LoadSeg` + W^X). **Not ring 0** — the
  port is EL0 by design; these fetch from the host what bare metal reads from privileged state.

## Provenance & licensing

These are independent work: no third-party implementation source — emulator, agent,
driver, or otherwise — was read, searched, or consulted in producing them, and any
resemblance to existing implementations is coincidental. Each `spec.md` is written
from public APIs, published standards, the AROS tree, and this project's own spikes,
under the **[independent-work process](CLEANROOM.md)**; the implementer works solely
from those footings, never from third-party implementation source. (**Emu68** is
MPL-2.0 — adoptable as isolated files with attribution, a different path; see the
68k-jit design.)

## Shared foundations (build once, several features lean on them)

- **W^X-aware executable memory** — two coexisting routes: the **native `LoadSeg`** path
  (`mprotect` R/W→R/X + `MEMF_EXECUTABLE`, **already in tree** — commit `71f75760`; see
  [native modules](native-modules/design.md)) and the 68k JIT's `MAP_JIT` +
  `pthread_jit_write_protect_np` (`[J1]`). Shared open risk: I-cache coherency
  (`CacheClearE` is a no-op on this target).
- **Host CPU/system facts** — `processor.resource` backed by host `sysctl`/Mach (CPU model,
  cores, features, load), since EL1 ID registers aren't readable at EL0. See the
  [processor.resource backend](processor-resource/design.md).
- **The host-call shim** (H3) and **`hostlib.resource`** — already built.
- **The device-I/O path** (H10/H11) — the `IORequest → device task → host syscall →
  reply` shape that clipboard, audio, sockets and the volume handler reuse.
- **A host-thread → AROS `Signal` pump** — the CoreAudio RT callback, the kqueue socket
  pump, and the NSPasteboard poll all need it. Factored once as a shared contract:
  [host-wake-pattern.md](host-wake-pattern.md) (atomics, ownership, lost-wakeup, the
  `Signal` seam) — all three host shims conform.

## Suggested order

1. **Unblock real software**: the executable-memory layer. Native `LoadSeg` is
   **largely landed** (commit `71f75760`; [native modules](native-modules/design.md)) —
   verify it and close the I-cache gap; `[J1]`'s `MAP_JIT` still gates the JIT.
2. **Finish the boot first** where needed: most features can't *run* until `dos.library`
   + the boot module set exist (kickstart still halts at cold-start — `graft/WORKFLOW.md`).
   [Host volume](host-volume/design.md)'s code is largely already there — cheapest win once DOS is up.
3. **Visible wins**: [clipboard](clipboard-bridge/design.md) and the
   [display window](cocoa-metal-display/design.md).
4. **Reach the world**: [sockets](bsdsocket-net/design.md), then
   [audio](coreaudio-audio/design.md).
5. **The mountain**: the rest of the [68k JIT](68k-jit/design.md).

Within the **2026-06-28 batch**, cheap wins first: [serial](serial-bridge/design.md)
(backend half-built) and the [fonts](fonts-coretext/design.md) MVP (reuses the freetype
engine); then [system monitor](system-monitor/design.md) and
[volume](volume-mixer/design.md) (small gadgets on top of an existing app / the audio
bridge); then [printing](printing/design.md) (once `[PR0]` unblocks driver compilation
on aarch64) and [MIDI](midi-coremidi/design.md); [USB](usb-iokit/design.md) last (only
its enumeration milestone fits the unattended loop).
