# Cocoa/Metal display

**Status: built (`[D1]` green).** A live macOS window — Apple-native AppKit +
Metal — that is the AROS display. AROS draws a framebuffer from its own heap;
the shim presents it in the window, and keyboard + mouse drive the AROS shell and
Wanderer desktop.

## Run it

```sh
AROS_CTL_STARTUP_MODE=desktop graft/run-window.sh   # AROS in a live Cocoa/Metal window
```

Click the window for keyboard focus, then type at the shell prompt. The shim
(`hosted/cocoametal/`, built with `make cocoametal-dylib`) also carries the
**control FIFO** that lets [`aros-ctl`](../control-harness/README.md) drive and
screenshot the window headlessly — no window-server session, no Screen-Recording
prompt — so the GUI stays inside the unattended loop.

This is the display half of **Macaros**; the first-class Mac app around it (menu
bar, About, icon, Settings) is the [host app shell](../host-app-shell/README.md).

## Docs

- [design.md](design.md) — why AppKit+Metal, the present path, the H7 lineage
- [spec.md](spec.md) — implementation spec
- [INTERFACE.md](INTERFACE.md) — the shim ↔ AROS boundary + control protocol
