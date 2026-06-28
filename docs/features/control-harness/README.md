# Control harness — puppet the hosted AROS from the command line

> Status: **built and in daily use (2026-06-27)** — the harness behind the
> Cocoa/Metal "live console" milestone and the unattended GUI tests.
> This is the practical "what & how". For the design and the as-built
> implementation spec, see [design.md](design.md) and [spec.md](spec.md).

## What it does

[`graft/aros-ctl`](../../../graft/aros-ctl) is one Bash script that drives the
hosted AROS running in the Cocoa/Metal window — with **no human at the keyboard and
no window-server session**:

- boot it windowed, type at the shell, press keys, click/drag the mouse, resize
  the host window;
- screenshot the framebuffer, tail the boot log, triage a crash;
- list the host dylibs the process actually mapped.

Injected input travels the **same path real `NSEvent`s do**, so it faithfully
reproduces input behaviour; screenshots read an **offscreen oracle**, so they need
no macOS Screen-Recording (TCC) approval. That combination is what makes the GUI
port testable in the unattended build→run→observe loop.

It is also the **seed of the embeddable library's input/capture API** — the
`K`/`M`/`B`/`S` wire protocol it speaks is exactly what a scriptable `libAROS`
would expose in-process (see [hosted/libaros/IDEAS.md](../../../hosted/libaros/IDEAS.md)).

## Quick start

```sh
graft/aros-ctl run                 # boot AROS windowed (background)
graft/aros-ctl wait 3              # let it reach the "1>" prompt
graft/aros-ctl type "echo hello"   # type at the shell
graft/aros-ctl enter               # press Return
graft/aros-ctl shot                # screenshot -> run/darwin-aarch64/ (no TCC)
graft/aros-ctl stop                # kill it
```

**Record a scripted demo** — `record SECS` returns immediately and the app auto-stops
at the deadline, so you drive the demo during the window (no manual stop, no TCC):

```sh
graft/aros-ctl record 25                              # 25s -> run/darwin-aarch64/movie-<ts>.mov
graft/aros-ctl type "version"; graft/aros-ctl enter;  graft/aros-ctl wait 3
graft/aros-ctl type "dir sys:"; graft/aros-ctl enter; graft/aros-ctl wait 3
# ...the movie finalizes itself at 25s. Frames are real-time-paced, so playback
#    matches the session. For an open-ended take use `record start` … `record stop`.
```

One process you start with `run` and end with `stop`; in between you puppet it with
the commands below. You'll see the boot it staged in the log:

```
boot dir: …/AROS/boot/darwin
AROS pid 12345  log=/tmp/aros-window.log  control=/tmp/aros-cm.ctl
```

## How it works (one channel, two directions)

There is **no shared memory and no emulated input hardware.** A control FIFO carries
one-line commands into the running window's host shim; the shim turns them into
synthetic events and reads back the framebuffer:

```
   aros-ctl type/key/click/resize     aros-ctl shot
        │  "K 14 1" / "M 80 40" / "R…"    │  "S /tmp/x.ppm"
        ▼  (one line per command)          ▼
   $AROS_CM_CONTROL  (a FIFO) ───► cocoametal_control.m ───► cm_readback (offscreen
        events → a 256-slot ring         (host main queue)     oracle, BGRA→PPM)
        │  drained FIRST, then live NSEvents, each VBlank
        ▼
   AROS input HIDD (macOS VK → Amiga RAWKEY) → input.device → console.device → shell
```

Two properties do the work:

- **Injected events are drained before live `NSEvent`s** in the same
  `cm_pump_events` call, so downstream they are indistinguishable from real typing.
- **The screenshot reads the offscreen oracle** (the last `cm_present` target), not
  the on-screen drawable — so it is deterministic and needs no window server / TCC.

## Command reference

Invoke as `aros-ctl <cmd> [args]`.

### Lifecycle
| Command | Effect |
|---|---|
| `run` | Boot AROS windowed in the background with the control FIFO; log to `$LOG`, pid to `$PIDF`. Stages a known-good boot (see [design.md](design.md) → "What `run` sets up"). |
| `stop` | Kill the recorded pid, `pkill AROSBootstrap`, remove the FIFO + pidfile. |
| `wait [SECS]` | `sleep` (default 1) — let AROS run/render between actions. |

### Input
| Command | Effect |
|---|---|
| `type TEXT...` | Type a string. Per char: `char2vk` → press/release; uppercase & shifted punctuation are wrapped in Left-Shift. Lowercase, digits, space, common punctuation. |
| `enter` | Press Return (`K 36`). |
| `key VK P [MODS]` | Raw key: macOS virtual keycode `VK`, pressed `P` (1/0), optional `MODS` bitmask. The escape hatch for keys `type` doesn't cover (Tab=48, Esc=53, arrows 123–126…). |
| `cmdc` | Command-C (arrives as Right-Amiga-C inside AROS) — copy via the clipboard bridge. |
| `cmdv` | Command-V (Right-Amiga-V) — paste. |
| `mouse X Y` | Move pointer to logical `X,Y`. |
| `resize W H` | Resize the native host window content area to `W,H` points and enqueue a resize event. AROS' logical framebuffer mode is unchanged. |
| `click [BTN] [X Y]` | Press+release a button (default 0=left, 1=right, 2=middle), optionally at `X,Y`. |
| `button BTN P [X Y]` | Raw button: `BTN` pressed `P` (for press-hold-drag-release), optionally at `X,Y`. |
| `menu [X Y]` | Diagnostic helper for focused app/screen menus at `X,Y` (default `70,8`): hold RMB and publish a tiny held-move. Leaves RMB held. Console menus open through this path; Wanderer's backdrop menu is still being investigated. |
| `menuup [X Y]` | Release the held RMB after `menu`. |

### App-Shell Settings
| Command | Effect |
|---|---|
| `setting KEY VALUE [Y]` | Inject one `CM_EV_SETTING` event directly into the Cocoa HIDD input path. `KEY` is a numeric `CM_OPT_*` value, `VALUE` is carried in `x`, and optional `Y` is carried in `y`. |
| `clipboard on\|off` | Convenience wrapper for `CM_OPT_CLIPBOARD_SHARE` (`0x14`) to pause/resume the NSPasteboard ↔ `PRIMARY_CLIP` bridge at runtime. |

### Capture & introspection
| Command | Effect |
|---|---|
| `shot [PATH]` | Screenshot → `PATH` (`.png` via `sips`, else raw `.ppm`). Default `run/darwin-aarch64/shot-<timestamp>.png`. Reads the offscreen oracle, so no TCC. |
| `record start [PATH] [FPS]` | Start recording the framebuffer to a `.mov` (AVFoundation H.264, no TCC). Default `run/darwin-aarch64/movie-<timestamp>.mov`; stop with `record stop`. |
| `record SECS [PATH] [FPS]` | Record `SECS` seconds, then **auto-stop** in the app — returns immediately so you script actions during the window. Ideal for demos: `record 25`. |
| `record stop` | Finalize a manual recording. |
| `log [N]` | Last `N` log lines (default 40) from `$LOG`. |
| `crash` | Grep the log for crash/alert markers (`Trap signal`, `Alert`, `stack limits`, `Module`, `Function`, `supervisor mode`, `Privilege`). |
| `libs` | List the **host** macOS dylibs mapped into the running `AROSBootstrap` (path + version, via `vmmap`/`otool`) — *which* host shim loaded and from where. (For AROS-side library versions use the `LibList` C: command inside the shell.) |

## Configuration & environment

Override these to isolate a second instance (set all three together):

| Var | Default | Purpose |
|---|---|---|
| `AROS_CTL_FIFO` | `/tmp/aros-cm.ctl` | the control FIFO (becomes `$AROS_CM_CONTROL`) |
| `AROS_CTL_LOG` | `/tmp/aros-window.log` | stdout/stderr of the booted AROS |
| `AROS_CTL_PIDF` | `/tmp/aros-cm.pid` | pidfile for `stop` |

Build inputs are **relocatable, with overrides** (the script resolves its own dir →
repo root, so in-repo artifacts are found wherever the checkout lives):

| Var | Default | Purpose |
|---|---|---|
| `AROS_CTL_DYLIB` | `<repo>/build/cocoametal.dylib` | host shim copied to `~/lib` for `DYLD_FALLBACK` |
| `AROS_CTL_ENT` | `<repo>/graft/aros-host.entitlements.plist` | entitlements signed onto `AROSBootstrap` |
| `AROS_CTL_BOOTD` | *auto-discovered* | the AROS `boot/darwin` dir (`AROSBootstrap` + `.conf`) |

`BOOTD` is a build output that lives outside the repo, so `run` discovers it
(`$AROS_CTL_BOOTD` → `${BUILD:-/tmp/arosbuild}/…` → in-repo `build/AROS/boot/darwin`
→ newest scratchpad build). If none is found, `run` prints how to set it or build one.

## Usage recipes

**A key `type` doesn't cover (Escape, then Tab):**
```sh
graft/aros-ctl key 53 1; graft/aros-ctl key 53 0     # Esc down/up
graft/aros-ctl key 48 1; graft/aros-ctl key 48 0     # Tab down/up
```

**Clipboard round-trip** (needs the ConClip bridge `run` starts):
```sh
graft/aros-ctl cmdc                      # copy selection → CLIPS:/host clips dir
graft/aros-ctl cmdv                      # paste back
```

**Mouse press-drag-release:**
```sh
graft/aros-ctl mouse 100 80
graft/aros-ctl button 0 1                # press
graft/aros-ctl mouse 200 140             # drag
graft/aros-ctl button 0 0                # release
```

**Exercise the focused app/screen menu path:**
```sh
graft/aros-ctl menu 70 8                 # open and hold the menu
graft/aros-ctl shot /tmp/menu.png        # capture while it is visible
graft/aros-ctl menuup 70 8               # release RMB
```

Injected button events remember the last injected mouse position, so `mouse X Y`
then `click 1` and `click 1 X Y` are equivalent. This matters for menu testing:
right-button events need a meaningful pointer position before Intuition sees the
button transition. `menu` keeps RMB held and emits a one-pixel held movement, so
menu screenshots do not depend on release timing. Treat
`AROS_DESKTOP_SMOKE_MENU=1 ./graft/desktop-smoke` as the Wanderer backdrop-menu
investigation, not as a baseline pass requirement, until that smoke passes.

**Resize stress path:**
```sh
graft/aros-ctl resize 1024 720
AROS_DESKTOP_SMOKE_RESIZE=1 graft/desktop-smoke
AROS_DESKTOP_SMOKE_STRESS=1 graft/desktop-smoke
```

`resize` changes only the native host window content size and enqueues the same
`CM_EV_RESIZE` event a live drag produces. The AROS screen mode remains whatever
the current framebuffer mode is; this is for host geometry/event-pump hardening.
`AROS_DESKTOP_SMOKE_STRESS=1` adds a bounded pointer/click burst and a final
stress screenshot to catch short freezes without requiring manual dragging.

**Two isolated instances in parallel:**
```sh
AROS_CTL_FIFO=/tmp/a.ctl AROS_CTL_LOG=/tmp/a.log AROS_CTL_PIDF=/tmp/a.pid graft/aros-ctl run
AROS_CTL_FIFO=/tmp/b.ctl AROS_CTL_LOG=/tmp/b.log AROS_CTL_PIDF=/tmp/b.pid graft/aros-ctl run
```

**Triage a boot that died:**
```sh
graft/aros-ctl crash         # fatal trap / alert / halt lines
graft/aros-ctl warn          # non-fatal warnings such as supervisor-mode semaphore reports
graft/aros-ctl log 80        # surrounding context
graft/aros-ctl libs          # is it ~/lib/cocoametal.dylib, and what version?
```

**Desktop smoke test:**
```sh
graft/desktop-smoke /tmp/aros-desktop.png
```

This wraps the normal desktop launch in the expected order: `deploy-check`, stop a
stale instance, boot with `AROS_CTL_STARTUP_MODE=desktop`, wait, scan the log for
traps, capture a screenshot, then move/click the pointer and capture a second
screenshot. Set `AROS_DESKTOP_SMOKE_RESIZE=1` to run a bounded host-window resize
sequence and save a post-resize screenshot too; set
`AROS_DESKTOP_SMOKE_STRESS=1` for a bounded pointer/click burst and stress
screenshot; set `AROS_DESKTOP_SMOKE_MENU=1` to hold the right button long enough
to prove the Wanderer menu opens and paints. It is the quickest "does Wanderer
visibly come up in the app and accept basic input?" check.

Run it from a normal macOS Terminal session. Some restricted automation sandboxes
can read the repo but cannot keep the GUI process alive or inspect processes; in
that case `desktop-smoke` may fail early with an empty/short log even though the
same build works from Terminal.

## Where the code lives

**Host side (this repo — `aros-aarch64`):**

- [graft/aros-ctl](../../../graft/aros-ctl) — the CLI: subcommands → FIFO lines, the
  `char2vk` table, the `run`/`stop` lifecycle and boot staging, PPM→PNG.
- [hosted/cocoametal/cocoametal_control.m](../../../hosted/cocoametal/cocoametal_control.m)
  — the control channel: the FIFO reader (a GCD source on the main queue), the
  256-slot injection ring, and `cm__control_shot` (framebuffer → binary PPM).
- [hosted/cocoametal/cocoametal_window.m](../../../hosted/cocoametal/cocoametal_window.m)
  — `cm__pump_events_appkit` drains injected events **first**, then live `NSEvent`s.
- [hosted/cocoametal/cocoametal.h](../../../hosted/cocoametal/cocoametal.h) — the
  `CMEvent` / `CM_EV_*` / `CM_MOD_*` ABI the injected events ride.

**AROS side (`aros-upstream`, branch `aarch64-darwin-graft`):**

- `arch/all-darwin/hidd/cocoa/cocoa_input.c` — polls `cm_pump_events` each VBlank,
  maps each macOS virtual keycode through `cocoa_keymap[]` to an Amiga `RAWKEY` (+
  qualifier bits), and feeds the keyboard/mouse `IrqHandler`s.

## Status

| Capability | State |
|---|---|
| Type / keys / Return | **working** — drives the live shell, same path as real input |
| Mouse move / click / drag | **working** |
| Host-window resize command | **working** — harness hook, not exported in the `cm_*` ABI |
| Screenshot (offscreen oracle → PNG) | **working** — TCC-free, deterministic |
| Wanderer backdrop menu | **working** — `AROS_DESKTOP_SMOKE_MENU=1 ./graft/desktop-smoke` proves it visually |
| `cmdc` / `cmdv` (R-Amiga C/V) | **wired** — depends on the [clipboard bridge](../clipboard-bridge/README.md) ConClip path |
| Parallel isolated instances | **working** via `AROS_CTL_FIFO/LOG/PIDF` |
| Relocatable (no hardcoded build paths) | **done (2026-06-27)** — discovery + overrides |
| Typed in-process API (`libAROS`) | planned — see [design.md](design.md) → "Roadmap" |
