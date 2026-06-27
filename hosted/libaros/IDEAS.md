# libAROS — what it unlocks

> Status: **vision / idea list** (speculative). Companion to the contract sketch in
> [libaros.h](libaros.h). Captures the "what could we do with it?" brainstorm so the
> ideas are committable, not just spoken. Each is tied to a real piece of this repo.

libAROS is the **engine layer**: load the kickstart, run AROS on a dedicated thread,
and expose three clean seams — screen+input ([`cm_*`](../cocoametal/cocoametal.h)
inverted), console (emul-handler stdio), and lifecycle. Host-agnostic, no Cocoa.
The contract is sketched in [libaros.h](libaros.h).

The single fact that unlocks everything below: **the inversion is complete — AROS is
now a guest *subsystem* you call into, not a process that owns the machine.** Three
buckets follow from that.

---

## A. Embed AmigaOS *into* Mac software

The cocoametal surface is already GPU-texture-shaped, so a booted Workbench becomes a
**subview you drop anywhere** — a tab in a SwiftUI app, a panel in a dev tool, a pane
in a larger workspace. A *component*, not "an emulator window."

- **AmigaOS as a document.** Because shutdown returns control to the host (libaros.h
  rule #2 — no unilateral `exit()`), a running machine is a value the host owns:
  snapshot/restore, `.aros` save-states, two Workbenches side by side.
- **One engine, many hosts.** Host-agnostic means the *identical* libAROS embeds in a
  Mac app, a CLI, a screensaver — and, since it's already arm64 and upstream ships a
  UIKit HIDD, an **iPad app**. An embeddable framework is App-Store-shaped in a way a
  monolithic emulator isn't.

## B. Drive AmigaOS *from* Mac software (the harness, matured)

This is the one we've literally been building toward:
[`aros-ctl`](../../graft/aros-ctl)'s `K`/`M`/`B`/`S` protocol is the embeddable
input/capture API in disguise (see the [control-harness](../../docs/features/control-harness/design.md)
doc). Promote it to typed in-proc calls (`aros_post_text`, `aros_post_mouse_*`,
`aros_readback`) over the same seams and you get:

- **Language bindings.** `import aros` in Python/Swift/JS — boot Workbench in a
  Jupyter cell, assert on framebuffer pixels.
- **Headless CI for Amiga software.** Our whole unattended loop (boot → run → observe
  → PASS/FAIL) collapses into a *library call*. Regression-test a classic app's
  rendering against a golden PPM.
- **An LLM agent that *uses* AmigaOS.** The harness was already "the seed of the
  input/capture API"; with libAROS it's an in-process tool an agent calls.
  Computer-use, but the computer is Workbench. Genuinely novel, and on-brand.
- **AppleScript / Shortcuts / Automator / Raycast actions** backed by a live AROS.

## C. Fuse the two worlds (the thesis at its limit)

The project thesis — *macOS owns the drivers; AROS reaches them via exec I/O* — lets
the arrow point **both** ways once AROS is a library:

- **Two-way calls.** AROS programs calling host frameworks (CoreML, Metal compute,
  the network via the [bsdsocket bridge](../../docs/features/bsdsocket-net/design.md),
  the Mac FS via [host-volume](../../docs/features/host-volume/design.md)) as if they
  were native Amiga calls — and host code invoking Amiga programs as functions.
- **Zero-copy framebuffer.** AROS renders straight into something that's already a
  Metal texture → host post-processing (the CRT shader exists), screen-capture, or
  compositing Workbench into a 3D scene. (This is the optional `AROSDisplayBackend`
  seam in libaros.h.)
- **The JIT payoff.** libAROS + [Emu68 68k JIT](../../docs/features/68k-jit/design.md)
  = real classic 68k binaries (games, demos, LightWave, ProTracker) at native
  Apple-Silicon speed, **embedded**. A modern *embeddable* Amiga player a museum
  kiosk or an archive site can host — and the door to wilder things like an
  AudioUnit whose DSP is an Amiga sound routine
  ([CoreAudio AHI](../../docs/features/coreaudio-audio/design.md) for the reverse path).
- **Clipboard & drag both ways.** The [clipboard bridge](../../docs/features/clipboard-bridge/design.md)
  plus host-volume drag-in make the embedded machine feel native — copy out of
  Workbench, paste into a Mac app, and vice versa.

---

## Why this is different from "an emulator"

Not emulating the OS. It's *native* arm64 AROS with *host-native* drivers
(Metal / CoreAudio / sockets / FS). The 68k JIT is only for legacy app binaries.
That's exactly why "embed AmigaOS as a component in your app" is realistic here and
wouldn't be for a 68k emulator.

## The three I'd chase first

1. **AROS-as-a-subview in a real Mac app** — falls straight out of the seams we're
   already building.
2. **The scripting API + an agent driving Workbench** — we're one refactor of
   `aros-ctl` away.
3. **The embeddable JIT player** — the standout payoff.

## What "having it" quietly presupposes

So this stays grounded, not fantasy:

- The boot is finished past the current cold-start halt (`dos.library` + the boot
  module set).
- The engine/shell split is actually done, with the three seams clean.
- The good-citizen rules hold (signal-handler chaining, no `exit()`, lifecycle
  returns control).
- The one genuinely hard unknown: **multiple instances in one process.** AROS has
  process-global state, so it's likely one engine per process with many surfaces, or
  process-per-instance under a shared host supervisor (see the closing note in
  [libaros.h](libaros.h)).
