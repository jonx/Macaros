# Cocoa/Metal display

**Status: built (`[D1]` green).** A live macOS window — Apple-native AppKit +
Metal — that is the AROS display. AROS draws a framebuffer from its own heap;
the shim presents it in the window, and keyboard + mouse drive the AROS shell and
Wanderer desktop.

## Run it

```sh
AROS_CTL_STARTUP_MODE=desktop graft/run-window.sh   # AROS in a live Cocoa/Metal window
```

Click the window for keyboard focus, then type at the shell prompt. **The window
is resizable and the resolution follows** (ABI v3): dragging the window edge
snaps AROS to the nearest of 16 display modes at drag end (via
`screenmode.prefs` + IPrefs, so it needs the desktop startup path), and picking
a mode in ScreenMode Preferences resizes the window. See INTERFACE.md §10. The shim
(`hosted/cocoametal/`, built with `make cocoametal-dylib`) also carries the
**control FIFO** that lets [`aros-ctl`](../control-harness/README.md) drive and
screenshot the window headlessly — no window-server session, no Screen-Recording
prompt — so the GUI stays inside the unattended loop.

## Boot resolution

The window comes up at **1366x768**. That is *not* the driver's doing: with no
`screenmode.prefs` on disk, intuition's `OpenWorkbench` asks for
`GfxBase->NormalDisplayColumns/Rows`, which graphics.library seeds from
`AROS_NOMINAL_WIDTH/HEIGHT` in the generated `aros/config.h`. The knob is
therefore a **configure** flag, not a source constant:

```sh
configure ... --with-resolution=1366x768x8
#  -> "checking for default resolution of WBScreen... 1366 x 768 x 8"
```

Changing it needs `kernel-graphics` + `kernel-intuition` rebuilt (they bake the
constants) and any stale `SYS:Prefs/Env-Archive/SYS/screenmode.prefs` deleted,
since a saved prefs file wins over the nominal default. Reordering the driver's
mode ladder does **not** move the boot default; the first ladder entry is only
the default monitor *sync*. It matters because a resolution change needs the
desktop idle, so the boot default is the one that has to be right.

This is the display half of **Macaros**; the first-class Mac app around it (menu
bar, About, icon, Settings) is the [host app shell](../host-app-shell/README.md).

## Docs

- [design.md](design.md) — why AppKit+Metal, the present path, the H7 lineage
- [spec.md](spec.md) — implementation spec
- [INTERFACE.md](INTERFACE.md) — the shim ↔ AROS boundary + control protocol
