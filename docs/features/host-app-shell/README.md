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

One control is not a single value: `"control": "media"` generates the Media tab,
a live list of the Mac's removable disks with a per-disk share setting. It
follows the hardware while the window is open and applies itself, so it takes no
store or apply of its own. See [host-media](../host-media/README.md).

### What each setting does, and when

Every entry reaches something. Where a choice cannot be applied to a running
machine its label says so, rather than the window quietly recording a value
nobody reads.

| Setting | Stored in | Reaches | When |
|---|---|---|---|
| Confirm before quitting | defaults | the app's terminate handler asks first | live |
| Show Dock icon | defaults | the app's activation policy | live |
| Appearance | defaults | `CM_OPT_THEME` → window + status bar | live |
| Release input grab | defaults | which combination ends input capture | live |
| Capture input entering full screen | defaults | capture turns on with full screen | live |
| Keyboard layout | `aros-host.conf` | a request in the shared folder that `C:KeymapWatch` applies | live |
| Recording frame rate / Movie codec | defaults | the arguments File ▸ Record starts with | at the next recording |
| Scaling · Filter · Scanlines · Full screen | defaults | `CM_OPT_*` on the live present | live |
| RAM | `aros-host.conf` | the launcher's `AROSBootstrap.conf` | next launch |
| Share clipboard | defaults | `CM_OPT_CLIPBOARD_SHARE` → the guest's bridge | live |
| Shared folder | `aros-host.conf` | the launcher's `AROS_HOST_VOLUME` | next launch |
| Disks AROS may see | `aros-host.conf` | mount descriptions the guest's `C:MediaWatch` mounts | live |
| Master volume | defaults | `CM_OPT_AUDIO_VOLUME` → host CoreAudio gain | live |

Two things are deliberately not live. **RAM** is fixed when the machine starts.
**Shared folder** re-points `MacRW:`/`MacRO:`, and AmigaDOS cannot move a
mounted volume out from under open file handles; File ▸ *Open Folder as Volume…*
offers an additional folder to a running system instead, which is a different
and safe operation.

Those two stay in the tab they belong to, marked where the choice is made, and
the marker earns its place by changing: it reads `next launch` until you change
the value, and `restart to apply` once the running machine and the window
disagree. A setting is found by subject, so collecting the start-up-only ones
into a section of their own would only make them harder to find, and would move
settings between tabs whenever one becomes live. Any entry with
`"apply": "bootOnly"` gets the marker; nothing says so in its label.

### Changes that come from somewhere else

`aros-host.conf` is shared with the command-line tools, so it is not only
written by the window. The running process watches the file: whatever writes it
— `aros-ctl media`, another Macaros, a text editor — is noticed, applied, and
shown in the window if it is open. That is also the path for machine settings
the guest applies to itself: `"apply": "guestFile"` leaves the value in the host
share (`MacRW:`), where a small task inside AROS picks it up, which is the only
route that works for a signed application whose own AROS tree is read-only.

A schema entry whose values are plain names rather than numbers or `CM_*`
constants is a string setting: the keyboard layout popup stores `pc105_f`, not
the index of the item. (Before that was supported it stored `0` for every
layout, so choosing a layout in the window did nothing.)

## Docs

- [design.md](design.md) — the app-shell architecture, the two-tier Settings
- [spec.md](spec.md) — implementation spec
- Surfaces it umbrellas: [display](../cocoa-metal-display/README.md) ·
  [clipboard](../clipboard-bridge/README.md) · [host volume](../host-volume/README.md)
