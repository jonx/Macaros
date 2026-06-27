# Control harness — driving the hosted AROS headlessly

> Status: **built and in daily use** (unlike the *planned* features in this tree) ·
> Target: aarch64-darwin hosted · Drafted 2026-06-27
> Practical how-to: [README.md](README.md). As-built implementation spec: [spec.md](spec.md).
> Tool: [`graft/aros-ctl`](../../../graft/aros-ctl) · Host channel:
> [`hosted/cocoametal/cocoametal_control.m`](../../../hosted/cocoametal/cocoametal_control.m).

## What & why

The [Cocoa/Metal display](../cocoa-metal-display/design.md) gives AROS a real Mac
window. But a window is driven by **`NSEvent`s from a window-server session** — and
an unattended/CI process has no such session, and `screencapture` of a live window
needs macOS **Screen-Recording / TCC approval** (a manual click that breaks the
loop — the exact reason H7 rendered to PNG instead of showing a window).

The control harness closes both gaps with one host channel:

- **Input without a session** — it injects synthetic key/mouse events through a
  control FIFO. They are drained into the **same event stream real `NSEvent`s flow
  through**, so downstream (AROS input HIDD → `input.device` → `console.device` →
  shell) cannot tell them apart. Injected input therefore *faithfully reproduces*
  real input behaviour — it is not a shortcut around the HIDD.
- **Capture without TCC** — its screenshot reads the **offscreen oracle target**
  (the last `cm_present` result) via `cm_readback`, not the on-screen drawable. No
  Screen-Recording permission, no window-server, fully programmatic — the same
  oracle the display feature was designed around.

The result: a script with no GUI session can boot AROS windowed, type `echo hello`,
press Return, screenshot the console, and assert on the pixels — in one unattended
pass. That is the project thesis applied to *testing*: macOS owns the window and the
input devices; AROS reaches them through standard exec I/O; the harness reaches
*both* through one channel.

It is also, deliberately, the **seed of the embeddable library's input/capture
API**. The `K`/`M`/`B`/`S` wire protocol is exactly what a scriptable `libAROS`
would expose in-process — same contract, today over a FIFO, tomorrow a function call
(see [hosted/libaros/](../../../hosted/libaros/IDEAS.md) and "Roadmap").

## Does it already exist?

No equivalent existed; this is original to the project.

- **In the AROS tree** — there is no headless input/capture harness for a hosted
  display HIDD. The X11/SDL hosted drivers consume real host events from a live
  server connection; none expose a "inject a synthetic key, read back the
  framebuffer" channel for an attended-free agent. Verified by search over
  `/Users/user/Source/aros-upstream` (no control-FIFO / synthetic-event path in the
  hosted HIDDs).
- **GPL Amiga emulators** have scripting/automation (serial/debugger scripting,
  config-driven input, GUI hooks) — but they are GPL and, more to
  the point, automate an *emulated* machine, not a native hosted OS reaching host
  drivers. This is independent work: no such implementation source was read,
  searched, or consulted, and any resemblance is coincidental; the harness rides our
  own `cm_*` ABI.
- **macOS-native UI automation** (AppleScript UI scripting, `cliclick`, CGEvent
  posting) all require a window-server session and/or Accessibility/Screen-Recording
  TCC grants — exactly the manual approvals the unattended loop forbids. The harness
  avoids them by injecting *below* the window server (into the shim's own event
  queue) and capturing *off* the offscreen oracle rather than the screen.

So the niche — *attended-free, TCC-free, same-fidelity-as-real input* for a hosted
AROS desktop — is unfilled, and the harness fills it by reusing seams the display
feature already had.

## Background: the `cm_*` control-channel contract (grounded)

The harness is not a new subsystem so much as two existing seams wired to a FIFO:

- **The event ABI.** `cm_pump_events(cx, CMEvent *out, int max)` is how the AROS
  input HIDD pulls host input each VBlank ([cocoametal.h](../../../hosted/cocoametal/cocoametal.h)).
  A `CMEvent` is a neutral `{type, x, y, code, pressed, mods}` — `code` carries a
  macOS virtual keycode for keys, a button index for buttons; `mods` is a `CM_MOD_*`
  bitmask. Injected events are *just more `CMEvent`s*, so nothing downstream needs to
  know they were synthetic.
- **The offscreen oracle.** `cm_readback(cx, dst, stride, w, h)` copies the
  last-presented offscreen target as BGRA8 at logical size — the display feature's
  "unattended pixel channel," independent of the on-screen window
  ([cocoa-metal-display/design.md](../cocoa-metal-display/design.md), the
  "unattended-verification tension"). A screenshot is one `cm_readback` to a PPM.
- **The inversion.** AROS already runs as a guest thread under a host that owns the
  run loop and the AppKit main queue (the [Cocoa/Metal driver](../cocoa-metal-display/design.md)).
  That is what lets a host-side GCD source read the FIFO on the main queue and hand
  events to the AROS thread through the existing pump — no new threading model.

The harness adds: a FIFO reader, a small injection ring, and the CLI. Everything
load-bearing (the event ABI, the oracle, the inversion thread) predates it.

## Design

Four decisions define the harness; each is justified by the unattended-loop and
fidelity constraints above. (Mechanics — exact grammar, ring size, drain order — are
in [spec.md](spec.md).)

**1. Inject into the real event stream, drained *first*.** The host control channel
turns each `K`/`M`/`B` line into a `CMEvent` and pushes it onto a ring that
`cm__pump_events_appkit` drains **before** live `NSEvent`s, in the same
`cm_pump_events` call. *Why:* this is the difference between an automation harness
and a faithful one — a synthetic key and a real key become the identical `CMEvent`
and take the identical path through the AROS keyboard HIDD, so a passing test
reflects real behaviour, not a side door. The alternative (poking `input.device`
directly from AROS) would bypass exactly the HIDD code the GUI port needs to prove.

**2. Capture from the offscreen oracle, never the screen.** `shot` reads
`cm_readback`, not `screencapture`. *Why:* the live drawable is gated behind
window-server presence and Screen-Recording TCC; the offscreen target is not. This
keeps the whole verb set inside the unattended loop. The cost — present-only effects
(scale mode, scanline filter) are not in the shot — is acceptable because the oracle
is the *source of truth* for "what AROS drew," which is what a test wants to assert.

**3. A FIFO as the transport, held open with `O_RDWR`.** The channel is a named pipe
at `$AROS_CM_CONTROL`. The host opens it `O_RDWR | O_NONBLOCK` so a writer end is
always held on the reader side — a one-shot `echo … > fifo` from the CLI never makes
the GCD source see EOF and stop. *Why a FIFO:* zero dependencies, line-oriented,
shell-native, and trivially isolable per instance (`AROS_CTL_FIFO`). It is also the
*right shape to outgrow*: replacing the FIFO with an in-process call is a transport
swap, not a redesign (Roadmap).

**4. The CLI stages a known-good boot, then is a thin veneer.** `run` does more than
launch: it deploys the host shims to `~/lib` (so `HostLib_Open` resolves bare names
under the hardened runtime), installs the Cocoa monitor, assembles the kickstart
module set, writes a `Startup-Sequence`, codesigns with entitlements, and `mkfifo`s
the channel. *Why in the harness:* the boot has several load-bearing, easy-to-forget
steps (the resident `shell.resource`, the clipboard `ConClip` path, the entitlements);
folding them into `run` makes "boot a windowed AROS that can be driven" a single
command. Everything past `run` (`type`, `key`, `shot`, …) is sugar over the four
wire verbs.

### What `run` sets up before launching

The staging, in order (details in [spec.md](spec.md), and it tracks the sibling
[run-window.sh](../../../graft/run-window.sh)):

- Copies `build/cocoametal.dylib` → `~/lib/cocoametal.dylib` (the `DYLD_FALLBACK`
  target), and `build/libpasteboard.dylib` if present (the
  [clipboard bridge](../clipboard-bridge/design.md) host shim).
- Installs the **Cocoa monitor** (`Storage/Monitors/Cocoa` → `Devs/Monitors/Cocoa`)
  and removes the `headless` monitor (its teardown clashes with a real display).
- Creates `SYS:clips` as a **host-visible** dir (a copied clip lands as a readable
  file, no AROS `MakeDir` needed).
- Strips any `arguments` line from `AROSBootstrap.conf`, then appends the windowed
  module set if present and not already listed: `shell.resource`; the
  `hiddclass`/`gfx`/`inputclass`/`keyboard`/`mouse` HIDDs; the
  `console`/`input`/`keyboard`/`gameport`/`clipboard` devices;
  `keymap`/`graphics`/`layers`/`intuition`/`gadtools`/`iffparse` libraries; `con-handler`.
- Writes a `S/Startup-Sequence`: `Version`, `Assign CLIPS: SYS:clips`, `Run ConClip`
  — bringing up the console↔system clipboard bridge so `cmdc`/`cmdv` work.
- Codesigns `AROSBootstrap` with the shared entitlements, `mkfifo`s the control FIFO,
  and launches detached with `AROS_DARWIN_THREADED=1 AROS_CM_CONTROL=$FIFO`.

## How it works — the end-to-end path

```
  aros-ctl type "echo hi"                         (1) CLI: char -> "VK SHIFT"
        │  writes lines "K 14 1", "K 14 0", ...       (char2vk table)
        ▼
  $AROS_CM_CONTROL  (a FIFO, default /tmp/aros-cm.ctl)   (2) one command per line
        │
        ▼  cm__control_init: dispatch_source on MAIN queue reads lines
  cocoametal_control.m  ──► cm__control_exec()              (3) parse K/M/B/S
        │   K/M/B  → cm__inj_push() onto a 256-slot ring
        │   S      → cm__control_shot() immediately (readback → PPM)
        ▼
  injection ring  ──► cm__control_drain()                  (4) drained FIRST,
        │                                                      before live NSEvents,
        ▼  inside cm__pump_events_appkit (cocoametal_window.m)   each VBlank
  cm_pump_events()  →  CMEvent{type,code=macVK,pressed,mods}
        │
        ▼  AROS side, once per VBlank
  cocoa_input.c   cocoa_keymap[macVK] → AROS RAWKEY        (5) VK → Amiga rawkey
        │         + qualifier bits (shift/ctrl/alt/amiga)      feeds IrqHandler
        ▼
  input.device → console.device → the shell                (6) indistinguishable
                                                                from real typing
```

Screenshot is the mirror image: `aros-ctl shot out.png` writes `S /tmp/out.ppm`;
`cm__control_shot` calls `cm_readback` on the offscreen oracle (logical w×h, BGRA),
writes a binary PPM (P6, BGRA→RGB), and the CLI runs `sips` to PNG.

## How we verify it unattended

The harness is *itself* the verification mechanism for the GUI port, so its own
correctness shows up as the higher-level tests passing — but it has discrete,
attended-free checks:

- **The oracle is the assertion surface.** `shot` produces a deterministic PPM the
  harness can diff against a golden image or scan for expected glyphs — no screen
  capture, no TCC, no window-server. A booted-console test types a known command and
  asserts the framebuffer changed / shows the expected text.
- **Round-trip fidelity.** Type a string, screenshot, and confirm it rendered at the
  prompt — proving the `char2vk` → `CMEvent` → `cocoa_keymap` → `RAWKEY` chain end to
  end. Because injected and real events share the drain path, a pass also vouches for
  real typing.
- **Liveness / triage without a human.** `log`, `crash`, and `libs` turn a dead or
  misbehaving boot into greppable output (trap lines, which dylib loaded from where),
  so a failed run yields a diagnosis, not a black window.

No step waits on a person, and none needs a Screen-Recording grant — the property the
whole loop is organised around.

## Risks & open questions

- **`type` charset is limited.** Lowercase letters, digits, space, and common
  punctuation (shift for uppercase/shifted symbols). No control chars, no Unicode, no
  Tab/Esc/arrows — use `key` with the macOS virtual keycode. A neutral-keysym layer
  (Roadmap step 2) would remove the macOS-VK leak.
- **Injection ring drops on overflow.** The ring is 256 deep, drained one VBlank at a
  time; a burst pushed faster than it drains loses the tail (silent). Interactive
  lengths are fine; huge pastes should chunk with `wait`. A back-pressure or
  blocking-write mode is an open option.
- **`cmdc`/`cmdv` depend on the bridge.** macOS `kVK_Command` maps to `RAWKEY_RAMIGA`
  in `cocoa_input.c`, so they arrive as RAmiga-C/V — correct for ConClip, but no-ops
  if the [clipboard bridge](../clipboard-bridge/design.md) path isn't up. Their value
  is also gated on that feature's runtime status.
- **Screenshot excludes present-only effects.** By design (oracle, not drawable), so
  a test of the scanline filter or scale mode needs `cm_render_effect_readback`, not
  `shot`. Worth a documented note where someone might expect the live look.
- **Boot dir must be discoverable.** The dylib/entitlements travel with the repo, but
  `BOOTD` is a build output; on a fresh machine set `AROS_CTL_BOOTD` or build one
  (`run` prints the hint). Tracked, not a defect.

## Roadmap — toward the embeddable input/capture API

The harness already enforces the two properties an embeddable `libAROS` needs: the
control channel **pulls no AROS headers**, and capture works with **no window-server
session**. The migration is mostly mechanical:

1. Promote the `K`/`M`/`B`/`S` grammar to typed in-process calls
   (`aros_post_text`/`aros_post_key`/`aros_readback`, the
   [libaros.h](../../../hosted/libaros/libaros.h) seam) over the same `cm__inj_push`
   / `cm__control_shot` internals — the FIFO becomes one transport, not the only one.
2. Replace the macOS-VK wire value with a neutral keysym so callers needn't know
   `kVK_*` (the AROS side already owns the VK→RAWKEY table).
3. Keep `aros-ctl` as the CLI veneer over that API, so existing tests and recipes
   keep working unchanged.

## References

- This repo: [graft/aros-ctl](../../../graft/aros-ctl),
  [hosted/cocoametal/cocoametal_control.m](../../../hosted/cocoametal/cocoametal_control.m),
  [hosted/cocoametal/cocoametal_window.m](../../../hosted/cocoametal/cocoametal_window.m)
  (`cm__pump_events_appkit`), [hosted/cocoametal/cocoametal.h](../../../hosted/cocoametal/cocoametal.h)
  (`CMEvent`/`CM_MOD_*`), [hosted/libaros/](../../../hosted/libaros/IDEAS.md).
- AROS (`aros-upstream`, `aarch64-darwin-graft`):
  `arch/all-darwin/hidd/cocoa/cocoa_input.c` (`cocoa_keymap[]` VK→RAWKEY, qualifiers,
  the per-VBlank pump).
- Sibling features: [cocoa-metal-display](../cocoa-metal-display/design.md) (the
  window + the oracle), [clipboard-bridge](../clipboard-bridge/design.md) (what
  `cmdc`/`cmdv` drive), [host-volume](../host-volume/design.md) (a consumer of the
  harness for drag-in tests).
