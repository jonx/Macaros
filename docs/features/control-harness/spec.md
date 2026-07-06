# Implementation spec — the `aros-ctl` control harness

> Status: **as-built (2026-06-27)** — this documents shipped, working code, not a
> forward plan. Companion to [design.md](design.md) / [README.md](README.md).
> Process note: [../CLEANROOM.md](../CLEANROOM.md) (the independent-work process).

## Provenance banner

This is independent work: no third-party implementation source — emulator, agent,
driver, or otherwise — was read, searched, or consulted in producing the harness, and
any resemblance to existing implementations is coincidental. The host control channel
([`cocoametal_control.m`](../../../hosted/cocoametal/cocoametal_control.m)) pulls no
AROS headers and uses only the public `cm_*` ABI. The code is original, built on three
approved footings — public APIs, the AROS tree, and this project's own surfaces —
tagged inline:

- **`[PUB]`** — Apple/POSIX published APIs only: Grand Central Dispatch
  (`dispatch_source` of type `DISPATCH_SOURCE_TYPE_READ` on the main queue), POSIX
  named pipes (`mkfifo`, `open(O_RDWR|O_NONBLOCK)`, `read`), `sscanf`/`fprintf`, and
  `sips` for PPM→PNG.
- **`[AROS]`** — in-tree AROS headers/drivers (APL/LGPL), specifically
  `compiler/include/devices/rawkeycodes.h` and the hosted input HIDD shape in
  `arch/all-darwin/hidd/cocoa/cocoa_input.c`.
- **`[OURS]`** — this project's own surfaces: the `cm_*` display ABI
  ([`cocoametal.h`](../../../hosted/cocoametal/cocoametal.h)), the inversion thread,
  and the harness itself ([`graft/aros-ctl`](../../../graft/aros-ctl)).

## Scope

**In.** A command channel that drives a hosted, windowed AROS with **no
window-server session and no TCC grant**: inject key/mouse events that enter the
*same* stream real `NSEvent`s do, and read back the rendered framebuffer to a file.
Plus a CLI (`aros-ctl`) that fronts the channel with ergonomic verbs and stages a
known-good windowed boot (`run`/`stop`).

**Out (non-goals).** Capturing the *live* presented drawable (effects/scale) — the
oracle is offscreen by design; audio/clipboard *content* (those are their own
features — the harness only drives the keystrokes that trigger them); a typed
in-process API (that is the `libAROS` Roadmap, [design.md](design.md)); non-darwin
hosts; gamepad/tablet event classes beyond key + mouse-move + mouse-button.

## Architecture

Two layers joined by the existing `cm_*` event ABI plus a POSIX FIFO. No shared
memory, no emulated input device, no new thread model — the harness reuses the
inversion's main queue and per-VBlank pump.

```
CLI (Bash, host)                        Host shim (Apple clang, in the AROS process)
┌───────────────────────────┐           ┌────────────────────────────────────────┐
│ graft/aros-ctl   [OURS]   │           │ cocoametal_control.m            [OURS]  │
│  · char2vk: char→"VK SH"  │   FIFO    │  · open($AROS_CM_CONTROL,O_RDWR|NONBLK) │
│  · K/M/B/S line writers   │ ────────► │  · dispatch_source (MAIN queue) reads   │
│  · run/stop boot staging  │  one line │    lines, cm__control_exec parses K/M/B/S│
│  · S→PPM then sips→PNG     │ ◄──────── │  · K/M/B → cm__inj_push (256-ring)      │
└───────────────────────────┘   PPM     │  · S     → cm__control_shot (readback)  │
                                         └───────────────────┬────────────────────┘
                                                             │ cm__control_drain (FIRST)
                                         ┌───────────────────▼────────────────────┐
                                         │ cocoametal_window.m  cm__pump_events_*  │
                                         │  injected CMEvents, then live NSEvents  │  [OURS]
                                         └───────────────────┬────────────────────┘
                                                             │ cm_pump_events (per VBlank)
                                         ┌───────────────────▼────────────────────┐
                                         │ cocoa_input.c  cocoa_keymap[VK]→RAWKEY  │  [AROS]
                                         │  + qualifiers → keyboard/mouse IrqHandler│
                                         └───────────────────┬────────────────────┘
                                                  input.device → console.device → shell
```

## The wire protocol — `[OURS]`

One command per line into `$AROS_CM_CONTROL`, parsed by `cm__control_exec` with
`sscanf`. The whole grammar:

| Line | Parse | Effect |
|---|---|---|
| `K <vk> <pressed> [mods]` | `"%d %d %d"`, ≥2 fields | enqueue `CM_EV_KEY` `{code=vk, pressed, mods}` |
| `M <x> <y>` | `"%d %d"`, ==2 | enqueue `CM_EV_MOUSEMOVE` `{x, y}` |
| `B <button> <pressed>` | `"%d %d"`, ==2 | enqueue `CM_EV_MOUSEBTN` `{code=button, pressed}` |
| `S <path>` | `" %1023s"`, ==1 | `cm__control_shot(path)` immediately (not enqueued) |
| `V start <path> [fps] [secs]` | `" %15s %1023s %d %lf"` | `cm_record_start(path,fps)`; if `secs>0`, schedule an in-app auto-stop after `secs` (`cm__record_autostop`). Not enqueued. |
| `V stop` | subcmd `stop` | `cm_record_stop()` — finalize the `.mov`. |

- `vk` is a **macOS virtual keycode** (`kVK_*`); `pressed` is 1 (down) / 0 (up);
  `mods` is an optional `CM_MOD_*` bitmask, default 0.
- `x`,`y` are **logical** (pre-scale) pixels, top-left origin — the `cm_*`
  coordinate convention.
- `button` is `0`=left, `1`=right, `2`=middle.
- Unknown leading chars and malformed lines are ignored (no error, no enqueue).

`CM_MOD_*` bits (`[OURS]`, [cocoametal.h](../../../hosted/cocoametal/cocoametal.h)):
`SHIFT=1`, `CONTROL=2`, `ALT=4`, `CMD=8`.

## Host control channel (`cocoametal_control.m`) — `[OURS]` + `[PUB]`

**FIFO lifecycle.** `cm__control_init(cx, getenv("AROS_CM_CONTROL"))` is a no-op when
the env var is unset (so the shim is unaffected outside the harness). When set it
`open`s the path `O_RDWR | O_NONBLOCK` `[PUB]` — `O_RDWR` so the reader **always holds
a writer end**, meaning a one-shot `echo … > fifo` from the CLI never makes the source
see EOF; `O_NONBLOCK` so the `open`/`read` never block the main queue.

**Reader.** A `dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, …,
dispatch_get_main_queue())` `[PUB]` fires on readable bytes; the handler reads into a
1 KiB temp, accumulating into a 4 KiB line buffer (`g_lineBuf`), and on each `'\n'`
(or buffer-full) calls `cm__control_exec` on the completed line. Running on the
**main queue** means the reader and the event *drainer* (below) are on the same
thread — no locking on the ring.

**Injection ring.** `cm__inj_push` writes into a fixed 256-slot ring
(`CM_INJ_MAX=256`, `g_inj[]`); when full it **drops** the event (returns silently —
the producer is faster than AROS drains). `cm__control_drain(out, max)` empties FIFO
order into `out[]` and returns the count. Single-producer/single-consumer, both on the
main thread, so the ring needs no atomics.

**Drain order (the fidelity guarantee).** `cm__pump_events_appkit`
([cocoametal_window.m:460](../../../hosted/cocoametal/cocoametal_window.m#L460))
calls `cm__control_drain` **first**, then appends pending resize/close transitions,
then live `NSEvent`s — all into the same `out[]` the AROS HIDD reads via
`cm_pump_events`. An injected `CM_EV_KEY` is therefore byte-identical to, and
processed by the same code as, a real keypress.

**Screenshot.** `cm__control_shot(path)` `[OURS]`: `cm_target_size` → backing
`tw,th,scale`; readback at **logical** size `w=tw/scale, h=th/scale` via
`cm_readback` (the offscreen oracle, BGRA8); write a binary **PPM P6**
(`P6\n%d %d\n255\n` header, then BGRA→RGB per pixel). Synchronous; no window or TCC
involved.

## AROS-side translation (`cocoa_input.c`) — `[AROS]`

The injected `CMEvent`s leave the harness's jurisdiction here; this is stock hosted
AROS code (cited for completeness, not changed by the harness):

- Once per VBlank the input task calls `cm_pump_events` and, for each event, maps the
  macOS virtual keycode through `cocoa_keymap[vk]` → an Amiga `RAWKEY_*`
  (`compiler/include/devices/rawkeycodes.h` `[AROS]`); e.g. `0x00→RAWKEY_A`,
  `0x24→RAWKEY_RETURN`, `0x37→RAWKEY_RAMIGA`, `0x38→RAWKEY_LSHIFT`.
- Modifier keys also accumulate `IEQUALIFIER_*` qualifier bits
  (`LSHIFT/RSHIFT/CONTROL/LALT/RALT/LCOMMAND/RCOMMAND/CAPSLOCK`).
- The mapped rawkey + qualifiers feed the keyboard `IrqHandler` (and mouse events the
  mouse `IrqHandler`), i.e. straight into `input.device` → `console.device`.

Consequence for the CLI: the harness must speak **macOS** virtual keycodes (what
`cocoa_keymap` is indexed by), which is why `char2vk` and the `key` verb use `kVK_*`
values, and why `cmdc`/`cmdv` (macOS `kVK_Command=55`) arrive inside AROS as
`RAWKEY_RAMIGA` — Right-Amiga — exactly what ConClip listens for.

## The CLI (`graft/aros-ctl`) — `[OURS]`

**Verb → wire mapping.** `ctl()` writes one line to the FIFO. `enter` → `K 36 …`;
`click`/`button` → `B …`; `mouse` → `M …`; `key` → raw `K`; `shot` → `S` then
`sips`. `cmdc`/`cmdv` emit the press/hold/release sequence of `kVK_Command` (55) +
`C`(8)/`V`(9) carrying `CM_MOD_CMD` (8).

**`type`.** Per character, `char2vk` returns `"VK SHIFT"` (macOS keycode + a
shift flag); uppercase `[A-Z]` lower-case then set shift; shifted punctuation has an
explicit `1`. The CLI wraps the keypress in Left-Shift (`LSHIFT=56`) press/release
when the flag is set. Covers lowercase, digits, space, common punctuation; anything
else yields empty and is skipped (use `key`).

**`run` staging** (the load-bearing part; mirrors [run-window.sh](../../../graft/run-window.sh)):

1. Resolve `BOOTD` via `find_bootd` (override `AROS_CTL_BOOTD` → `${BUILD:-~/aros-build}/… → legacy /tmp/arosbuild/…`
   → in-repo `build/AROS/boot/darwin` → newest scratchpad build); error with a hint if none.
2. Copy `cocoametal.dylib` (and `libpasteboard.dylib` if present) → `~/lib`.
3. Install `Devs/Monitors/Cocoa`, remove `Devs/Monitors/headless`; `mkdir SYS:clips`.
4. Strip `arguments` from `AROSBootstrap.conf`; append the windowed module set
   (`shell.resource`, the HIDD classes, `console`/`input`/`keyboard`/`gameport`/`clipboard`
   devices, `keymap`/`graphics`/`layers`/`intuition`/`gadtools`/`iffparse`, `con-handler`)
   if present and not already listed.
5. Write `S/Startup-Sequence`: `Version`, `Assign CLIPS: SYS:clips`, `Run ConClip`.
6. Codesign `AROSBootstrap` with `$ENT` (entitlements) `-o runtime`, falling back to a
   plain ad-hoc signature if the plist is absent.
7. `rm`+`mkfifo -m 600 $FIFO`; launch detached with
   `AROS_DARWIN_THREADED=1 AROS_CM_CONTROL=$FIFO DYLD_FALLBACK_LIBRARY_PATH=$HOME/lib`,
   `</dev/null >$LOG 2>&1`; record `$!` to `$PIDF`.

**Environment contract** (`[OURS]`): instance isolation via
`AROS_CTL_FIFO`/`AROS_CTL_LOG`/`AROS_CTL_PIDF`; relocatable build inputs via
`AROS_CTL_DYLIB`/`AROS_CTL_ENT`/`AROS_CTL_BOOTD` (resolved relative to the script's
own dir → repo root). See [README.md](README.md) for the tables.

## Verification (unattended) — `[OURS]`

No TCC anywhere: input is injected below the window server and capture reads the
offscreen oracle, so the agent never hits an approval dialog.

- **[V-shot] Capture is real.** After a windowed boot, `shot` writes a PPM whose
  header is `P6 <w> <h> 255` and whose body is `3·w·h` bytes — diff against a golden
  image or scan for expected glyphs. PASS = a non-blank console framebuffer at the
  logical size from `cm_target_size`.
- **[V-type] Input round-trips end to end.** `type "<marker>"` + `enter`, then `shot`;
  assert the marker rendered at the prompt. PASS proves
  `char2vk → CM_EV_KEY → cm__control_drain → cocoa_keymap → RAWKEY → console`. Because
  injected and real events share the drain path, this also vouches for real typing.
- **[V-triage] Liveness without a human.** `crash` surfaces trap/alert lines from the
  log; `libs` confirms which host dylib loaded and from where (e.g.
  `~/lib/cocoametal.dylib`). A dead boot yields a diagnosis, not a black window.

Clean exit: `stop` kills the pid and `pkill`s any stray `AROSBootstrap`, removing the
FIFO + pidfile; a hung run is reaped by the outer harness watchdog.

## Build / integration

- The control channel is compiled into `cocoametal.dylib`; no separate build. It is
  inert unless `$AROS_CM_CONTROL` is set, so it costs nothing in normal runs.
- `aros-ctl` is a standalone script (`#!/bin/bash`); its only hard host deps are
  `mkfifo`, `codesign`, and (for PNG) `sips` — all macOS-stock. `libs` additionally
  uses `vmmap`/`otool`.
- The shim pulls **no** AROS headers; the CLI knows nothing of AROS internals beyond
  the `kVK_*` values the AROS keymap consumes. The `cm_*` ABI is the only contact
  surface, which is what keeps the Roadmap (in-process `libAROS` transport) a swap.

## Open questions / UNVERIFIED

- **Ring back-pressure.** Whether a blocking-write / coalescing mode is worth adding
  for very large injected bursts, vs. the current silent drop-on-full + `wait`
  chunking. Unmeasured where the 256-deep ring starts dropping under real VBlank
  cadence.
- **Neutral keysym layer.** The macOS-VK leak in `type`/`key` (Roadmap step 2) — the
  exact keysym set and whether it lives in the CLI or the shim is open.
- **`shot` of present effects.** A variant reading `cm_render_effect_readback` (to
  capture scale/scanline) is unspecified; today `shot` is oracle-only on purpose.
- **`cmdc`/`cmdv` runtime.** Their end value tracks the clipboard bridge's runtime
  status (currently blocked on an emul-handler `CLIPS:`→host path crash, per that
  feature) — the keystroke injection itself is verified.

## Provenance summary

`[PUB]` Apple Grand Central Dispatch (`dispatch_source`, main queue), POSIX named
pipes (`mkfifo`/`open(O_RDWR|O_NONBLOCK)`/`read`), `sscanf`/`fprintf`, `sips`,
`vmmap`/`otool`/`codesign`. ·
`[AROS]` `compiler/include/devices/rawkeycodes.h` (`RAWKEY_*`),
`arch/all-darwin/hidd/cocoa/cocoa_input.c` (`cocoa_keymap[]` VK→RAWKEY, qualifier
mapping, the per-VBlank `cm_pump_events` consumer). ·
`[OURS]` [`hosted/cocoametal/cocoametal_control.m`](../../../hosted/cocoametal/cocoametal_control.m)
(FIFO reader, injection ring, `cm__control_shot`),
[`hosted/cocoametal/cocoametal_window.m`](../../../hosted/cocoametal/cocoametal_window.m)
(`cm__pump_events_appkit` drain order),
[`hosted/cocoametal/cocoametal.h`](../../../hosted/cocoametal/cocoametal.h)
(`CMEvent`/`CM_EV_*`/`CM_MOD_*`, `cm_readback`/`cm_target_size`),
[`graft/aros-ctl`](../../../graft/aros-ctl) (the CLI + boot staging),
[`hosted/libaros/libaros.h`](../../../hosted/libaros/libaros.h) (the in-process API
this protocol seeds). **Independent work: no third-party implementation source —
emulator, agent, driver, or otherwise — was read, searched, or consulted in producing
it, and any resemblance to existing implementations is coincidental.**
