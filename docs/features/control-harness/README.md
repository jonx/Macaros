# Control harness — puppet the hosted AROS from the command line

> Status: **built and in daily use (2026-06-27)** — the harness behind the
> Cocoa/Metal "live console" milestone and the unattended GUI tests.
> This is the practical "what & how". For the design and the as-built
> implementation spec, see [design.md](design.md) and [spec.md](spec.md).

## What it does

[`graft/aros-ctl`](../../../graft/aros-ctl) is one Bash script that drives the
hosted AROS running in the Cocoa/Metal window — with **no human at the keyboard and
no window-server session**:

- boot it windowed, type at the shell, press keys, click/drag the mouse;
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
graft/aros-ctl shot /tmp/x.png     # screenshot the console (no TCC)
graft/aros-ctl stop                # kill it
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
   aros-ctl type/key/click            aros-ctl shot
        │  "K 14 1" / "M 80 40"            │  "S /tmp/x.ppm"
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
| `click [BTN]` | Press+release a button (default 0=left, 1=right, 2=middle). |
| `button BTN P` | Raw button: `BTN` pressed `P` (for press-hold-drag-release). |

### Capture & introspection
| Command | Effect |
|---|---|
| `shot [PATH]` | Screenshot → `PATH` (`.png` via `sips`, else raw `.ppm`). Default `/tmp/aros-shot.png`. Reads the offscreen oracle, so no TCC. |
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

**Two isolated instances in parallel:**
```sh
AROS_CTL_FIFO=/tmp/a.ctl AROS_CTL_LOG=/tmp/a.log AROS_CTL_PIDF=/tmp/a.pid graft/aros-ctl run
AROS_CTL_FIFO=/tmp/b.ctl AROS_CTL_LOG=/tmp/b.log AROS_CTL_PIDF=/tmp/b.pid graft/aros-ctl run
```

**Triage a boot that died:**
```sh
graft/aros-ctl crash         # the trap / alert lines
graft/aros-ctl log 80        # surrounding context
graft/aros-ctl libs          # is it ~/lib/cocoametal.dylib, and what version?
```

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
| Screenshot (offscreen oracle → PNG) | **working** — TCC-free, deterministic |
| `cmdc` / `cmdv` (R-Amiga C/V) | **wired** — depends on the [clipboard bridge](../clipboard-bridge/README.md) ConClip path |
| Parallel isolated instances | **working** via `AROS_CTL_FIFO/LOG/PIDF` |
| Relocatable (no hardcoded build paths) | **done (2026-06-27)** — discovery + overrides |
| Typed in-process API (`libAROS`) | planned — see [design.md](design.md) → "Roadmap" |
