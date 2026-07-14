# Manual testing — driving the Workbench desktop by hand

The headless way to exercise the port is [`aros-ctl`](../control-harness/README.md)
(type/click/shot/log, for scripts and smokes) and the
[evidence smokes](../deployment/README.md#evidence-smoke-commands). This doc is
the **interactive** companion: boot the real Workbench window and click around in
it yourself. Use it to sanity-check a feature the way a user would, or to poke at
something a smoke does not cover.

> Everything here boots the tree in `~/aros-build`, so build/deploy first
> ([build](../build/README.md), then `graft/aros-ctl deploy`). Media and test
> files go in `~/AROS/Shared` — that is `MacRW:` inside AROS.

## Boot the interactive desktop

```sh
cd ~/Source/aros-aarch64
AROS_CTL_STARTUP_MODE=desktop graft/run-window.sh
```

A macOS window titled **AROS** opens with the blue Workbench screen (RAM Disk /
MacRO / MacRW / System icons). Run it from a GUI terminal (Terminal.app), not
plain ssh. `Ctrl-C` in the terminal shuts everything down.

- **Focus:** click the window to give it keyboard/mouse focus.
- **The boot console** ("AROS" titled) sits on top at first — push it behind with
  its depth gadget (top-right corner) or drag its title bar down.
- **Get a Shell:** hold the **right mouse button** to pull down the Wanderer menu
  bar, then **Wanderer → Shell** (or **Execute Command…** for a one-off). Most of
  the recipes below run from that Shell.
- **Console-only** boot (a plain shell, no desktop) is the default:
  `graft/run-window.sh` with no `AROS_CTL_STARTUP_MODE`.

## Test recipes

| Feature | From a Shell (unless noted) |
|---|---|
| **French / any keymap** | `LoadKeymap pc105_f` then type — top row is AZERTY. `LoadKeymap pc104_usa` switches back. Live from the Mac side (no Shell needed): `graft/aros-ctl keymap pc105_f`. |
| **Audio (AHI/CoreAudio)** | `AHISmoke` — a 1-second tone through the Mac speakers. The desktop startup already registers the CoreAudio mode (`AddAudioModes`). |
| **ffmpeg datatype** | `MultiView MacRW:test.avi` — decodes and shows the first video frame via `ffmpeg.datatype`. (`test.avi` ships in `~/AROS/Shared`.) |
| **FFView viewer** | `FFView MacRW:big.m4v` plays the clip; SPACE pauses, R restarts, Q quits. |
| **FFView drag-and-drop** | `Run C:FFView` opens the empty "drop a file here" window. Open the MacRW drawer (double-click its icon) and drag a media icon onto the FFView window. |
| **Feraille** | Needs a memory bump — see gotchas. Then: `Stack 16000000`, `SetEnv HOME SYS:`, `C:Feraille --theme dark`. |
| **Clipboard bridge** | Copy on the Mac, then in the Shell paste with RAmiga+V; or copy in AROS and paste on the Mac. |

## Gotchas

- **Drawer shows no icons.** Files without a `.info` (anything you drop in
  `~/AROS/Shared` from the Mac) are hidden until you turn on **Window → Show →
  All Files** in that drawer's menu. `test.jpg` has an icon; `big.m4v` is a good
  drag-and-drop target.
- **Feraille needs > 256 MB RAM.** The binary is ~200 MB, so the default 256 MB
  hosted RAM cannot LoadSeg it (silent fail, no window). Set **1280** MB via the
  Macaros app **Settings…** window, or edit
  `~/Library/Application Support/AROS/aros-host.conf` to `memory 1280`, then boot.
  The conf value wins over the `AROS_HOST_MEMORY` env. First load takes ~90 s.
- **Launch Feraille from an interactive Shell, not the Startup-Sequence.** A
  launch appended after the desktop Startup-Sequence's `EndCLI` never runs; an
  interactive Shell has no such issue.
- **A fix "doesn't take".** You are probably running a different copy than you
  built — see the several-copies trap in [deployment](../deployment/README.md).
  `graft/aros-ctl deploy` + `graft/deploy-check` before an interactive run.

## When to script it instead

For repeatable/regression checks, prefer the headless path — it needs no
Screen-Recording permission and emits one PASS/FAIL:

- One feature end-to-end: the smokes (`graft/desktop-smoke`, `graft/audio-smoke`,
  `graft/ffmpeg-smoke`, `graft/clipboard-smoke`, `graft/resize-smoke`, …).
- Ad-hoc driving: [`aros-ctl`](../control-harness/README.md)
  (`run`/`type`/`click`/`wheel`/`menu`/`shot`/`tasks`/`log`).
