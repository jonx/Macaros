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
| [CoreAudio audio](coreaudio-audio/design.md) · [spec](coreaudio-audio/spec.md) | Real sound via a CoreAudio-backed AHI sub-driver | design + spec done |
| [Host BSD sockets](bsdsocket-net/design.md) · [spec](bsdsocket-net/spec.md) | Working TCP/IP by forwarding `bsdsocket.library` to the Mac's native sockets | design + spec done |

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

- **W^X-aware executable memory** (`MAP_JIT` + `pthread_jit_write_protect_np`) — blocks
  the 68k JIT *and* the deferred native `LoadSeg` path. Spike `[J1]`.
- **The host-call shim** (H3) and **`hostlib.resource`** — already built.
- **The device-I/O path** (H10/H11) — the `IORequest → device task → host syscall →
  reply` shape that clipboard, audio, sockets and the volume handler reuse.
- **A host-thread → AROS `Signal` pump** — the CoreAudio RT callback, the kqueue socket
  pump, and the NSPasteboard poll all need it. Factored once as a shared contract:
  [host-wake-pattern.md](host-wake-pattern.md) (atomics, ownership, lost-wakeup, the
  `Signal` seam) — all three host shims conform.

## Suggested order

1. **Unblock real software**: `[J1]` (the `MAP_JIT` layer) — unblocks both the JIT and
   native `LoadSeg`.
2. **Finish the boot first** where needed: most features can't *run* until `dos.library`
   + the boot module set exist (kickstart still halts at cold-start — `graft/WORKFLOW.md`).
   [Host volume](host-volume/design.md)'s code is largely already there — cheapest win once DOS is up.
3. **Visible wins**: [clipboard](clipboard-bridge/design.md) and the
   [display window](cocoa-metal-display/design.md).
4. **Reach the world**: [sockets](bsdsocket-net/design.md), then
   [audio](coreaudio-audio/design.md).
5. **The mountain**: the rest of the [68k JIT](68k-jit/design.md).
