# Deployment workflow — Darwin/aarch64 hosted AROS

This port has several runnable copies of the same moving parts: the AROS boot
tree, host dylibs in `~/lib`, the Cocoa monitor copied from `Storage/Monitors` to
`Devs/Monitors`, and the app settings/config files. Most "it still runs the old
bug" reports come from one of those copies being stale.

## Canonical Loop

Use this before manual desktop testing:

```sh
./graft/aros-ctl deploy
./graft/deploy-check
AROS_CTL_STARTUP_MODE=desktop ./graft/run-window.sh
```

`aros-ctl run` and the smoke scripts call the same deploy path before launching.
`run-window.sh` also deploys the host-facing files it owns, but `deploy-check` is
the explicit sanity pass when you are switching between manual and harness runs.

## What Gets Deployed

Host-side files copied to `~/lib`:

- `build/cocoametal.dylib`
- `build/libpasteboard.dylib`
- `build/libcoreaudio.dylib`
- `build/libbsdsockhost.dylib`
- `hosted/cocoametal/settings.json`

AROS boot image updates:

- `AROS/Storage/Monitors/Cocoa` is copied to `AROS/Devs/Monitors/Cocoa`
- stale `AROS/Devs/Monitors/headless` is removed
- `AROS/clips` is created for `CLIPS:`
- selected kickstart modules are appended to `AROSBootstrap.conf` if missing
- optional memory setting updates the `memory` line in `AROSBootstrap.conf`

The launchers also copy/sign `AROSBootstrap` as `Daedalos` so macOS presents the
native app shell with the right process/menu name.

`graft/make-aros-app.sh` creates `build/Daedalos.app`, which carries its own
copies of the bootstrap, config, host dylibs, settings schema, and
`aros-host-conf.sh`. Treat the bundle as another runnable deployment target; if
it exists, `deploy-check` compares its embedded copies too.

## Host Config

The Settings window writes a plain config file:

```text
~/Library/Application Support/AROS/aros-host.conf
```

The shared helper [graft/aros-host-conf.sh](../../../graft/aros-host-conf.sh) is
the only translator from that file to launch environment. It is sourced by
`run-window.sh`, `aros-ctl`, and the `.app` launcher.

Recognized keys:

- `memory <MB>` sets `AROS_HOST_MEMORY`, applied to `AROSBootstrap.conf`
- `hostvolume <path>` mounts that path as:

```text
MacRO:<path>
MacRW:<path>;WRITE
```

- `hostvolume <Name:path[;WRITE]>` preserves an explicit host-volume spec

If no config or hostvolume key is present, launchers default to
`~/AROS/Shared` as `MacRO:` and `MacRW:`.

## Verifying Deployment

Run:

```sh
./graft/deploy-check
```

The important lines:

- host dylibs/schema should be `OK`
- `Cocoa deployed` should be `OK`
- `effective AROS_HOST_VOLUME` should show the config-derived mount spec, or say
  the launcher default will be used
- the app-bundle section should be `OK` when `build/Daedalos.app` exists; `SKIP`
  only means the bundle has not been built
- key desktop files should be present: `Prefs/Zune`, `Prefs/Wanderer`,
  `System/About`, `Tools`, `Utilities`

`WARN` on a host dylib/schema means `~/lib` is stale. Run
`./graft/aros-ctl deploy`.

`WARN` on `Cocoa deployed` means the boot image is still using an older monitor.
Run `./graft/aros-ctl deploy`.

`MISS` on desktop presence means the boot tree is incomplete; loader/application
debugging after that is usually noise.

`WARN` in the app-bundle section means the double-clickable app is stale even if
`run-window.sh` is current. Rebuild it with `./graft/make-aros-app.sh`.

## Evidence-Smoke Commands

Use these after deployment-sensitive changes:

```sh
./graft/startup-loop 3
./graft/desktop-smoke
./graft/hostvol-smoke
./graft/clipboard-smoke
make cocoametal-shell
```

Screenshots and logs land in `run/darwin-aarch64/`.

## Desktop boot requirements (beyond the module set)

Even a complete module set drops to the **emergency shell** or pops a volume
**requester** unless two non-module payloads are staged into the AROS tree.
`run-window.sh` (`ensure_desktop_payloads`) now stages both automatically; they
are documented here because they are easy to miss when booting a tree by hand:

- **`AROS.boot`** (CPU signature, content `aarch64`) at the AROS root.
  `__dos_IsBootable()` opens `:AROS.boot` on each mounted volume and checks it
  contains the CPU string; with no match **no volume is bootable** → `SYS:`
  never resolves → `Display driver(s) failed to initialize. Entering emergency
  shell.` Normally written by `make boot`; staged here because we boot the
  `AROS/` build dir directly (no distfiles step).
- **`Prefs/Presets/Themes/AROSDefault/`** (copied from `images/Themes/AROSDefault`
  in the OS source). The desktop Startup-Sequence does `Assign THEMES:
  SYS:Prefs/Presets/Themes` then `Assign THEME: THEMES:AROSDefault`; if the dir
  is absent the assign fails and Wanderer pops **"Please insert volume THEMES"**,
  blocking the desktop. Normally installed by distfiles.

The display itself comes up via this chain — each link is a separate failure
mode (see the build doc §3b): `emul-handler` boot → runs `C:AROSMonDrvs` → opens
**`icon.library`** → enumerates `DEVS:Monitors` → runs the `Cocoa` monitor →
`cocoametal.dylib` loads → `AddDisplayDriverA()` → display registered → Wanderer.

## Current Boundary

Boot-time/configured host volumes are integrated. The optional live
`CM_OPT_VOLUME_ADD/REMOVE` string event from the native app shell is not yet an
AROS-side mount/unmount consumer; it is separate from the working `MacRO:` /
`MacRW:` launch path.
