# Host app shell — Macaros

**Status: built.** Makes the AROS window a **first-class macOS app**: an app menu
bar, an About panel (the macaron), a custom icon, and a schema-driven **Settings**
window — all delivered by the cocoametal dylib, so it travels with the display,
not as a separate process.

**Macaros** = a macaron: AROS on a Mac.

## Package it

```sh
graft/make-aros-app.sh        # a dev Macaros.app that wraps the local boot tree
graft/make-aros-release.sh    # a SELF-CONTAINED, relocatable Macaros.app (+ --dmg)
```

`make-aros-app.sh` is the developer wrapper (points at your `~/aros-build`);
`make-aros-release.sh` embeds the whole prepared AROS volume so the `.app` boots
the Wanderer desktop on a clean Mac, ready to Developer-ID sign + notarize.
`--dmg` wraps it with [`graft/release-README.md`](../../../graft/release-README.md)
in a disk image.

## What the release desktop carries

Two applications sit on the Wanderer backdrop in the standard release: the
editor ([Zed](../zed-editor/README.md)) and the file manager
([Ferail](../feraille-gpui/README.md)). They stay in
`C:` so they also work as shell commands; the desktop icons come from
`SYS:.backdrop`, which lists one `:path` per leave-out icon.

Moonstone remains available to private builds through
`MACAROS_INCLUDE_MOONSTONE=1`, but its binary, icon, environment variable, and
assets are absent from the standard release.

The release differs from the dev tree in two ways that the editor depends on:
`HOME` is `MacRW:` (the embedded volume is inside a signed bundle and must stay
read-only), and the launcher writes `AROS_FSW_ROOTS` into the share as
`.macaros-boot` because only it knows where the share landed.

### Making an icon

[`graft/make-aros-icon.py`](../../../graft/make-aros-icon.py) turns a Mac PNG
into a Workbench icon: AROS `icon.library` reads OS4-style PNG icons, so the
`.info` file *is* a PNG with the Amiga attributes in a private `icOn` chunk.
The generated icons live in [`graft/icons/`](../../../graft/icons/).

### Two traps when an app is launched by icon

Both cost a full debugging round on 2026-08-01, and neither shows up when the
same program is started from a shell:

- **The icon's stack is the program's stack.** Workbench passes
  `do_StackSize` straight to `CreateNewProc`. Ferail needs 8 MB and gives up
  with "no CLI to relaunch from" below that, because its own big-stack
  workaround re-enters through the *CLI*, which a Workbench start does not
  have. Pass `--stack` to the icon generator to match what the program wants.
- **A dos requester blocks a program that has no one to answer it.** sqlite's
  VFS probes paths that no volume can satisfy; with the default
  `pr_WindowPtr` that raises a modal "please insert volume" over the desktop
  and the launch stops there. A GUI program should set `pr_WindowPtr = -1` in
  its entry shim and read the error instead.

The Settings window is *generated* from
[`hosted/cocoametal/settings.json`](../../../hosted/cocoametal/settings.json)
(`AROS_SETTINGS_SCHEMA`); its choices are written to `aros-host.conf`, which the
launcher turns into boot env (see
[`graft/aros-host-conf.sh`](../../../graft/aros-host-conf.sh)).

## Docs

- [design.md](design.md) — the app-shell architecture, the two-tier Settings
- [spec.md](spec.md) — implementation spec
- Surfaces it umbrellas: [display](../cocoa-metal-display/README.md) ·
  [clipboard](../clipboard-bridge/README.md) · [host volume](../host-volume/README.md)
