# hosted/ — the host-side implementation code

This directory holds the code that runs on the **macOS host**: the Act-2 bring-up
spikes (H1–H12) and the real host shims that back each hosted-AROS feature. Every
shim applies the project thesis — *macOS owns the drivers; AROS reaches them via
standard exec I/O* — as a flat-C-ABI dylib the AROS side calls through
`hostlib.resource`.

Design/spec for most of these live under [docs/features/](../docs/features/README.md);
this page is the code-side map. Maturity is shown in three tiers:

- **Integrated** — built and working; there is a Makefile target and/or a shipped
  dylib, verified in use.
- **Started** — real code exists but the feature is incomplete or not fully wired.
- **Idea** — a sketch/placeholder; nothing meaningful implemented yet.

## Integrated

| Subdir | What it is | Build / docs |
|---|---|---|
| `cocoametal/` | The live Cocoa/Metal window that is the AROS display, plus the **Macaros** first-class Mac app (menu/About/icon/Settings) and the control FIFO | `make cocoametal-dylib` · [display](../docs/features/cocoa-metal-display/design.md) · [app shell](../docs/features/host-app-shell/design.md) |
| `clipboard/` | Two-way `NSPasteboard` ↔ `clipboard.device` bridge | `make pasteboard-dylib` · [clipboard-bridge](../docs/features/clipboard-bridge/README.md) |
| `coreaudio/` | CoreAudio-backed AHI sub-driver shim (SPSC ring, RT render callback) | `make coreaudio-dylib` · [coreaudio-audio](../docs/features/coreaudio-audio/README.md) |
| `bsdsocket/` | `bsdsocket.library` → native BSD sockets pump (TCP + WaitSelect + DNS) | `make bsdsock-dylib` · [bsdsocket-net](../docs/features/bsdsocket-net/README.md) |
| `hostvolume/` | A real Mac folder mounted as an AROS volume (`MacRO:` / `MacRW:`) | `make hosted-hostvolume` · [host-volume](../docs/features/host-volume/README.md) |
| `hostshell/` | The Macaros settings/shell host pieces that fold into the cocoametal app | [host-app-shell](../docs/features/host-app-shell/design.md) |
| `jit68k/` | The 68k→AArch64 JIT and the `run68k` CLI. **Adopts Emu68 (MPL-2.0)** — see below | `make run68k` · [run68k.md](jit68k/run68k.md) · [68k-jit](../docs/features/68k-jit/design.md) |
| `ffmpeg/` | Native `libav*` cross-built for aarch64 AROS + the FFView image/video viewer | [ffmpeg-native](../docs/features/ffmpeg-native/README.md) |
| `rust/` | Rust on aarch64 AROS — full `std` runs (net/fs/env/args/process/time/thread) | [rust-aros](../docs/features/rust-aros/README.md) |
| `hostbind/` | Sample + `HostBind` helper showing how AROS code taps a host library via `hostlib.resource` | [host-bridge](../docs/features/host-bridge/README.md) |
| `x18probe/` | Diagnostic probe that proved the macOS host clobbers `x18` on signal entry (drove the port-wide `-ffixed-x18`) | [README](x18probe/README.md) |
| `*.c` / `*.S` (top level) | The Act-2 spikes H1–H12 (`host.c`, `preempt.c`, `abishim.*`, `exec.c`, `execboot.c`, `display.c`, `device.c`, `mem.c`, `kern.c`, `signal.c`, `msgport.c`, `library.c`, `switch.S`) — all green via `make hosted-test` | [PHASE2.md](../PHASE2.md) |

## Started

| Subdir | What it is | State |
|---|---|---|
| `hostcpu/` | `processor.resource` host backend — CPU model/cores/features via `sysctl`/Mach | Host shim **built & green** (`cp_abi_test`); the AROS-side `processor.resource` backend is the remaining graft step. [processor-resource](../docs/features/processor-resource/README.md) |
| `printing/` | Print-to-PDF host engine (CGPDFContext/CoreText) for `printer.device` | Host PDF engine **built & green** (`[PRPDF]`); the AROS `printer.device` driver is **blocked** at `[PR0]` (`printertag.h` `#error`s on aarch64). [printing](../docs/features/printing/README.md) |
| `feraille/` | Porting the Feraille (GPUI) file manager to AROS | Stage-1 probe **passes** on booted AROS (UI-free crates run); the `gpui_aros` backend is underway. [feraille-gpui](../docs/features/feraille-gpui/README.md) |

## Idea

| Subdir | What it is | State |
|---|---|---|
| `libaros/` | An embeddable, scriptable AROS (engine/shell split) | Design sketch only — the header self-labels "nothing behind this is implemented yet." [IDEAS.md](libaros/IDEAS.md) |

## Third-party code (not ours, not clean-room)

| Subdir | Origin | License |
|---|---|---|
| `jit68k/emu68/` | **Emu68** decoders + AArch64 emitter, vendored verbatim (pinned commit `305f686`, v1.0.7) | **MPL-2.0** — see [NOTICE](jit68k/emu68/NOTICE) and the repo-root [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) |

Everything else under `hosted/` is original work (AROS Public License; see the
repo [LICENSE](../LICENSE) and the [independent-work process](../docs/features/CLEANROOM.md)).
</content>
