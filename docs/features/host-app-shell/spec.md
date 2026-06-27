# Implementation spec — Host app shell (macOS app frontend for hosted AROS)

> Status: drafting (Role A) · Target: aarch64-darwin hosted · Drafted 2026-06-26
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Clean-room banner

**Role B (implementer): do NOT read vAmiga, WinUAE, FS-UAE, Amiberry, E-UAE/Janus-UAE,
or any GPL emulator source.** Implement only from this spec + the approved sources cited
by tag: `[PUB]` Apple framework docs / HIG / published standards, `[AROS]` in-tree AROS
headers and drivers (paths given), `[OURS]` this project's existing shim + spikes
(`hosted/cocoametal/*`, the `cm_*` ABI, the H-series). `[REF-CONFIRM]` items were
sanity-checked by Role A against GPL hosts but are **restated here from HIG/AppKit** —
implement from that, not from any reference. **UTM is Apache-2.0 (permissive)**, so its
*patterns* (menu layout, two-tier settings, toolbar order) are safe to learn from
directly and are tagged `[PUB-UTM]`; even so, write our own code. The overwhelming
majority of this feature is plain `[PUB]` AppKit + HIG — there is very little GPL
expression to wall off (it is UI chrome, not an emulation algorithm).

## Scope

**In.** Turn the existing bare-binary-with-a-window (`hosted/cocoametal/`) into a
first-class macOS application: (1) an `.app` bundle with `Info.plist`, custom icon, and a
standard About panel; (2) a full HIG menu bar wired to the existing `cm_*` option ABI and
a small set of append-only ABI extensions; (3) a two-tier Settings UI (global app
Settings ⌘, + a per-machine Configuration) that absorbs the current display panel and
adds Drives/Sharing, Sound, Input, and Captures surfaces; (4) two new host-capture
features — **Take Screenshot** (PNG) and **Record Movie** (AVFoundation) — over the
existing offscreen-oracle readback; (5) folder drag-and-drop to mount a host volume.

**Decision (confirmed in [design.md](design.md)).** Single-window app (the AROS display
*is* the app) — **no** library/gallery, **no** document-based snapshot model, **no**
guest-agent requirement, **no** general-VM hardware UI. The host shell owns **only the
host boundary**; anything a running AROS can set from inside the guest (ScreenMode,
keymap, locale, palette, in-guest mixer…) stays in AROS Prefs and is **not** mirrored
(design.md §"The line we will not cross"). Append-only ABI: the frozen `cm_*` contract
([../cocoa-metal-display/INTERFACE.md](../cocoa-metal-display/INTERFACE.md)) never moves;
new symbols/enums append and bump `CM_ABI_VERSION`.

**Out (non-goals, this spec).** ADF/floppy/disk-image media (AROS has no floppy concept
yet — "drives" = host-folder volumes only); save-states/snapshots; USB passthrough;
network-adapter config; multi-display switching; an `NSToolbar` (optional, may follow);
the "Open 68k Program…" menu integration (future hook, reserved in the menu).

## Architecture

All host-shell-side. The AROS engine sees **no new contract it doesn't already have** —
it keeps pulling `CM_EV_SETTING` events via `cm_pump_events`; the shim never calls into
AROS (the [../cocoa-metal-display/spec.md](../cocoa-metal-display/spec.md) §Threading
rule). New code lives beside the existing `.m` files; new AROS-facing keys ride the
existing pull path.

```
 macOS app frontend (host shim + launcher)          AROS side (engine — unchanged contract)
 ┌───────────────────────────────────────┐          ┌─────────────────────────────────────┐
 │ CMShellController (NSApplicationDelegate)│        │ Cocoa display HIDD + display-server  │
 │  · NSMenu tree → actions               │          │   task (cm_pump_events drain)        │
 │  · Settings windows (toolbar-tab)      │  cm_*    │                                      │
 │  · About / icon / Info.plist           │ ───────► │  pulls CM_EV_SETTING (CM_OPT_* keys) │
 │  · drag-and-drop (folder → volume)     │  set_opt │   → host-volume MakeDosNode/AddDosNode│
 │ cocoametal.dylib (existing + new ABI)  │ ◄─────── │   → clipboard-bridge on/off          │
 │  · cm_capture_png / cm_record_*        │ CM_EV_*  │   → AHI device/volume                │
 │  · cm_volume_list/add/remove           │  (pull)  │   → clean shutdown request           │
 └───────────────────────────────────────┘          └─────────────────────────────────────┘
        AppKit on the MAIN thread          AROS on a DEDICATED pthread (the inversion)
```

The **threaded inversion** (`AROS_DARWIN_THREADED=1`, [[cocoa-metal-display-driver]]) puts
AppKit on the main thread and AROS on a dedicated pthread — the standard Cocoa-app shape,
which is what makes a real menu bar + About panel + `[NSApp run]` natural. **R-RUNLOOP**
(below) pins whether the main thread adopts `[NSApp run]` or keeps the hand-pump.

## Requirements

### R-RUNLOOP — main-thread run loop coexists with the AROS event drain `[OURS]`+`[PUB]`

The main thread runs AppKit; the AROS display-server task drains events via
`cm_pump_events` (`cocoametal_window.m:443`). A native menu bar needs the menu-tracking
run loop. **Requirement:** prove (spike [G1]) that menu tracking — a nested run loop on
the main thread — does **not** stall the AROS-side drain, under whichever model is
chosen: (a) adopt `[NSApp run]` on the main thread (standard; gives free menu tracking +
key-equivalent dispatch; the fullscreen-*exit* finding already shows the app run loop is
sometimes required, [../cocoa-metal-display/spec.md](../cocoa-metal-display/spec.md)
§Settings), or (b) keep `CFRunLoopRunInMode(...,0,true)` hand-pumping and install menus by
hand. **Either is acceptable if the drain keeps advancing during tracking.** This is the
gating decision; everything below assumes it is resolved.

### R-IDENTITY — `.app` bundle, icon, About `[PUB]`

- **Bundle.** `Foo.app/Contents/{MacOS/AROSBootstrap, Frameworks/cocoametal.dylib,
  Resources/<icon>.icns, Info.plist}`. `Info.plist` keys: `CFBundleName`,
  `CFBundleIdentifier`, `CFBundleExecutable`, `CFBundleIconFile`,
  `CFBundleShortVersionString`, `CFBundleVersion`, `NSHighResolutionCapable=YES`,
  `LSMinimumSystemVersion`. The dylib's install name becomes
  `@rpath/cocoametal.dylib` (or `@loader_path/../Frameworks/…`), retiring the
  `DYLD_FALLBACK_LIBRARY_PATH` hack (`run-window.sh:99`).
- **Codesign preserved.** Keep the ad-hoc + hardened-runtime signature
  (`run-window.sh:93–95`) and entitlements — including `allow-jit` (the J1 / 68k-JIT
  path) and whatever the hostlib `dlopen` needs. Sign the dylib and the bundle; verify the
  hardened-runtime `dlopen` of `cocoametal.dylib` still resolves ([G2]).
- **Icon.** `.icns` referenced by `CFBundleIconFile`; **also** set at runtime with
  `[NSApp setApplicationIconImage:[[NSImage alloc] initWithContentsOfFile:…]]` so a
  bare-binary test run shows it in the Dock. The artwork is a deliverable; a placeholder
  ships first.
- **About.** `[NSApp orderFrontStandardAboutPanel:opts]` where `opts` carries
  `NSAboutPanelOptionCredits` (an attributed string: AROS license + port credits) and
  optionally `NSAboutPanelOptionApplicationVersion`. No custom window. `[PUB]`
  ([`orderFrontStandardAboutPanel(_:)`](https://developer.apple.com/documentation/appkit/nsapplication/orderfrontstandardaboutpanel(_:))).

### R-MENU — the menu bar (HIG order; disable, don't hide) `[PUB]`+`[PUB-UTM]`

Built once at app init beside the existing `sharedApplication`/`setActivationPolicy`
(`cocoametal_window.m:165–170`). Items target `CMShellController`. Unavailable items are
**disabled (dimmed), not hidden** (`[PUB]` HIG). Menu order is canonical
(App→File→Edit→View→Machine→Window→Help) per `[PUB]`
[HIG menu bar](https://developer.apple.com/design/human-interface-guidelines/the-menu-bar).
Use the label **"Settings…"** (Ventura+), not "Preferences…".

| Menu | Items (key equiv) | Action routes to |
|------|-------------------|------------------|
| **AROS** (app) | About AROS · Settings… (⌘,) · Services · Hide (⌘H) · Quit (⌘Q) | About panel; open global Settings; standard |
| **File** | Take Screenshot (⇧⌘3) · Record Movie… · ── · Open Folder as Volume… (⌘O) · Open Recent · ── · Open 68k Program… *(disabled/future)* · Close (⌘W) | `cm_capture_png`; `cm_record_start/stop`; `cm_volume_add`; reserved |
| **Edit** | Undo · Cut · Copy · Paste · Select All | clipboard bridge (host-side copy/paste affordance) |
| **View** | Enter Full Screen (⌃⌘F) · Scaling ▸ · Filter ▸ · Scanlines (toggle) · Retina/HiDPI · Show/Hide Toolbar | `CM_OPT_FULLSCREEN`/`SCALE_MODE`/`FILTER`/`EFFECT` (`cocoametal.h:116–122`) |
| **Machine** | Reset (⌃⌘R) · Power ▸ (Request Power Down · Force Shut Down · Force Quit) · ── · Capture Input (⌃⌘I) · Volumes ▸ (eject/reveal) · Share Clipboard (✓) · Send Key ▸ | `CM_OPT_POWER`; capture grab; `cm_volume_*`; `CM_OPT_CLIPBOARD_SHARE`; synthesized rawkeys |
| **Window** | Minimize (⌘M) · Zoom · Bring All to Front | standard |
| **Help** | AROS Help · AROS Website · Report an Issue | URLs |

`[REF-CONFIRM]`→`[PUB-UTM]`: the **Machine** menu and its graded **Power ▸** submenu
mirror UTM's "Virtual Machine"/"Power" shape (Apache-2.0,
[UTM controls](https://docs.getutm.app/basics/controls/)); restated here as a plain HIG
app-specific menu. **Power ▸ replaces** the current close→`exit(0)` (`cocoametal_window.m:143`)
with a clean shutdown *request* to the engine (the [[aros-embeddable-library-goal]] "no
unilateral `exit()`" rule).

### R-SETTINGS — two-tier toolbar-tab settings `[PUB]`+`[REF-CONFIRM]`

Two non-resizable toolbar-tab windows (`[PUB]` HIG settings: non-resizable, noncustomizable
pane toolbar, title = active pane, restore last pane). General first. The existing panel
(`cocoametal_settings.m:173–266`) is refactored into the **Display** tab of Machine
Configuration; persistence stays on `NSUserDefaults` `cocoametal.*`
(`cocoametal_settings.m:54–84`). `[REF-CONFIRM]`: the global-vs-per-machine split is the
UTM/vAmiga convergence, restated as the host-presentation-vs-guest-function split this
project's ABI already encodes (`[OURS]`).

**R-SETTINGS-SCHEMA — the window is generated from a data-file schema (decided
2026-06-27).** The settings are **not** hand-coded per control. One declarative schema —
shipped as a **JSON data file** (`settings.json`, in the `.app` Contents/Resources,
resolved via `NSBundle`) — is the single source of truth; the window, the persistence, and
the apply-routing are all derived from it. Each entry binds (1) presentation
(`tier`/`tab`/`label`/`control`/choices/range), (2) a `(store, key)` storage binding, and
(3) an `apply` kind. Adding a setting = adding a JSON entry (no rebuild, non-coder
editable); the loader validates and reports a clear error on malformed input. Two stores:
`defaults` → NSUserDefaults (`cocoametal.*`, interops with the existing panel); `conf` → a
**dedicated host config file `aros-host.conf`** (line-oriented `keyword value`, shared with
the launcher + scripts), edited **in place** so only the bound keyword's line changes and
every other line/comment/order is preserved. `apply`: `hostOption` (live `cm_set_option`),
`arosOption`/`arosOptionStr` (AROS-facing, relayed as `CM_EV_SETTING`), or `bootOnly` (conf
only; `aros-host.conf` is read by `graft/run-window.sh` at next launch). Proven in the POC
(`hosted/hostshell/`, `[GS]` green: validate/schema/conf/model/gen). The merge wires the
`conf` store path to `aros-host.conf` and the launcher to read it.

**Global app Settings (⌘,):** *General* (start behavior, confirm-on-quit, Dock/menu-bar
icon presence) · *Input* (release hotkey, auto-capture on fullscreen/focus, right-click /
Option-as-Meta) · *Captures* (screenshot format=PNG + location, movie codec H.264/HEVC +
fps + location). All host-only, `NSUserDefaults`.

**Machine Configuration:** *Display* (the existing effect/scale/filter/fullscreen/resolution
panel) · *Drives & Sharing* (mounted host-folder list + Share-clipboard toggle) · *Sound*
(output device, master volume, mute).

### R-CAPTURE-PNG — screenshot `[OURS]`+`[PUB]`

`int cm_capture_png(CMContext *cx, const char *path)` — `cm_readback` the offscreen oracle
(BGRA8, `cocoametal.h:163`) → encode PNG via the proven H7 ImageIO path
(`hosted/display.c`) → write `path`. Returns 0 on success. The control FIFO's `S`-command
PPM writer (`cocoametal_control.m:57–86`) is the throwaway precursor; this is the public,
PNG, menu-driven version. **No TCC** — we own the readback pixels.

### R-CAPTURE-VIDEO — movie recording `[OURS]`+`[PUB]`

`int cm_record_start(CMContext *cx, const char *path, int fps, int codec)` /
`int cm_record_stop(CMContext *cx)`. `[PUB]` AVFoundation: an `AVAssetWriter` +
`AVAssetWriterInput` + `AVAssetWriterInputPixelBufferAdaptor`; per present, convert the
offscreen-oracle readback into a `CVPixelBuffer` and append with a monotonically paced
presentation timestamp (fps-derived). `codec` selects `AVVideoCodecTypeH264`/`HEVC`.
**No ScreenCaptureKit, no Screen-Recording/TCC** — the frames are our own buffer, not a
screen grab. `stop` finalizes the file. Recommend AVFoundation over FFmpeg (native, no
external dependency). Frame pacing is the one subtlety — settle the pts model in [G5].

### R-VOLUME — runtime host-folder volume management `[AROS]`+`[OURS]`

AROS-facing (the mount is an AmigaDOS op, [../host-volume/spec.md](../host-volume/spec.md)
R-LAUNCH `mount_hostvol` via `MakeDosNode`/`AddDosNode(ADNF_STARTPROC)`):

- `int cm_volume_add(CMContext*, const char *spec)` — `spec` = `Name:hostpath[;WRITE]`
  (the host-volume `fssm_Device` grammar). Shim records it and enqueues a `CM_EV_SETTING`
  (new `CM_OPT_VOLUME_ADD`); the AROS side pulls it and mounts at runtime.
- `int cm_volume_remove(CMContext*, const char *name)` — enqueues `CM_OPT_VOLUME_REMOVE`;
  AROS unmounts. **Clean unmount of a live `emul-handler` volume is UNVERIFIED** (open
  locks / in-flight packets) — may require a "no open handles" guard or deferred unmount.
- `int cm_volume_list(CMContext*, char *out, int outLen)` — the live list, pulled back for
  the Drives UI (the AROS side publishes it; mechanism mirrors the existing get-option
  pull).
- **Drag-and-drop:** register the content view for `NSPasteboardTypeFileURL`; a dropped
  **folder** → `cm_volume_add` (default RO; modifier → `;WRITE`). The vAmiga drop-zone
  idiom (`[REF-CONFIRM]`, restated as plain AppKit drag handling).

### R-OPTKEYS — new append-only option keys / events `[OURS]`

Append to `CMOption` (`cocoametal.h:116–133`), all **AROS-facing** (relayed via
`CM_EV_SETTING`, never host-acted) except where noted:

```
CM_OPT_CLIPBOARD_SHARE = 0x14   /* 0/1 — clipboard-bridge on/off            */
CM_OPT_AUDIO_DEVICE    = 0x15   /* host audio output device index            */
CM_OPT_VOLUME_ADD      = 0x16   /* paired with a string side-channel spec    */
CM_OPT_VOLUME_REMOVE   = 0x17
CM_OPT_POWER           = 0x18   /* 0=request-shutdown 1=reset 2=force        */
```
`CM_OPT_AUDIO_VOLUME=0x13` already exists (stubbed, `cocoametal.h:130`). Bump
`CM_ABI_VERSION` (3). The new `cm_capture_*`/`cm_record_*`/`cm_volume_*` symbols append to
`cocoametal.exports` (`cocoametal.exports`) with default visibility; the dylib stays
unstripped + ad-hoc signed ([../cocoa-metal-display/spec.md](../cocoa-metal-display/spec.md)
§Build). **String-valued requests** (the volume spec) need a small side-channel beyond the
`long value` of `cm_set_option` — add `cm_set_option_str(cx, key, const char*)` paired with
the `CM_EV_SETTING` (append-only), so the AROS side pulls the string with the event.

## Verification (unattended — no TCC; `[OURS]` marker discipline)

The visible UI is never the oracle (the H7/TCC wall). Assert structure + behavior; each
spike prints one `[G#]` marker and exits clean.

- **[G1] R-RUNLOOP.** Stand-in worker pthread + AppKit main thread; install the menu; start
  menu tracking; assert the worker's drain counter advances during tracking and a menu
  item action fires. **PASS** = drain advances + action observed under the chosen run-loop
  model.
- **[G2] R-IDENTITY.** Build the `.app`; launch it; assert the dylib resolves from
  `Contents/Frameworks` (no `DYLD_FALLBACK`), codesign + entitlements verify
  (`codesign -dv`, `--verify`), `Info.plist` keys present, `orderFrontStandardAboutPanel:`
  with the options dict runs without raising, hardened-runtime `dlopen` of the dylib still
  succeeds. **PASS** = all asserted.
- **[G3] R-MENU + R-SETTINGS (View/Display).** Walk the `NSMenu` tree: assert titles,
  submenu shape, key equivalents, `target/action`. Invoke each View/Display action selector
  directly; assert `cm_get_option` reflects it (reuse the `[SET]` oracle,
  [../cocoa-metal-display/spec.md](../cocoa-metal-display/spec.md) §Verification→SET).
  **PASS** = tree matches + every control round-trips the option ABI.
- **[G4] R-CAPTURE-PNG.** `cm_capture_png` of a known pattern → assert the PNG file exists,
  valid header, correct dimensions, marker pixels (H7 `px_is`). **PASS** = valid PNG matches.
- **[G5] R-CAPTURE-VIDEO.** Record N frames of an animated pattern; stop; probe the movie
  off-process (`AVAsset` track/duration/frame-size or `ffprobe`); assert N frames at the
  expected size; decode one frame and pixel-check. **PASS** = movie has N frames, right
  dims, frame matches.
- **[G6] R-VOLUME.** `cm_volume_add(tmpdir)` → pump `CM_EV_SETTING` → AROS mounts → assert
  two-sided (host writes a fixture, AROS `Dir`s it) exactly as host-volume `[V4]`/`[V5]`
  ([../host-volume/spec.md](../host-volume/spec.md) §Verification). `cm_volume_remove` →
  assert unmount (or record the unmount-gap finding). Drag-drop exercises the same path via
  a synthesized drop. **PASS** = runtime volume round-trips; removal clean or flagged.
- **[G7] R-SETTINGS (full).** Both windows built as toolbar-tab panels; every control
  asserts through the option ABI / `CM_EV_SETTING`; `NSUserDefaults` round-trips (seed →
  fresh open re-applies). **PASS** = all tabs verify; defaults restore on relaunch.
- **[G8] (stretch) lifecycle + capture-input.** Power ▸ routes a clean shutdown request
  (assert engine-side, no `exit(0)`); Capture Input toggles the grab; the configurable
  release hotkey releases (synthesize the chord via `[NSApp postEvent:]`, the `[D4D5]`
  no-TCC path). **PASS** = shutdown request observed + capture flag toggles.

## Build / integration

- New ABI in `cocoametal.dylib` (built by `make cocoametal-dylib`): `cm_capture_png`,
  `cm_record_start`/`stop`, `cm_volume_list`/`add`/`remove`, `cm_set_option_str`, the new
  `CM_OPT_*` enums; appended to `cocoametal.exports`; `CM_ABI_VERSION=3`. The shim links
  **`AVFoundation`, `CoreVideo`, `CoreMedia`** in addition to the existing
  `AppKit/Metal/QuartzCore/CoreGraphics` (+ `ImageIO` for the PNG encode). The dylib still
  pulls **no** AROS headers; the C ABI is the only contact surface.
- Menu/About/settings/icon code is host-shell `.m` (new `cocoametal_shell.m` or extend
  `cocoametal_window.m`); built into the dylib or the bootstrap as appropriate. It must
  **not** be in the verification-path translation units that stay AppKit-free for the
  oracle.
- `graft/run-window.sh` becomes a thin "assemble the `.app` (copy binary + dylib + icon +
  `Info.plist`), codesign, then `open` it" wrapper, retaining a `--terminal` mode that runs
  the in-bundle binary directly for the unattended serial channel + `$AROS_CM_CONTROL`
  FIFO (`cocoametal_control.m`).
- AROS-side: the new `CM_OPT_*` pulls land in the display-server task's `CM_EV_SETTING`
  handler; `CM_OPT_VOLUME_*` call host-volume's `mount_hostvol`/an unmount counterpart;
  `CM_OPT_CLIPBOARD_SHARE` toggles the clipboard-bridge sync task; `CM_OPT_AUDIO_*` reach
  the AHI driver; `CM_OPT_POWER` triggers a clean shutdown. These are AROS-side and land
  with their respective features at graft.

## Open questions / UNVERIFIED

- **R-RUNLOOP model** — `[NSApp run]` vs. hand-pump under the threaded inversion, and menu
  tracking vs. the AROS drain. **[G1] gates everything.**
- **`.app` bundling** vs. hardened-runtime + `allow-jit` + `dlopen(cocoametal.dylib)` all
  surviving together; the `@rpath` install-name move off `DYLD_FALLBACK`. **[G2].**
- **Runtime volume *unmount*** of a live `emul-handler` volume (open locks / in-flight
  packets). **[G6].**
- **Video pts pacing** — present-rate-driven frames vs. a fixed-fps writer. **[G5].**
- **String side-channel** for `cm_volume_add` (`cm_set_option_str` shape) — confirm it
  marshals cleanly through the H3 host-call boundary.
- **Which ⌘-chords AROS needs** vs. which AppKit/menus keep (Capture Input + Send Key as
  the escape valves).
- **Icon artwork** — design deliverable, placeholder first.

## Provenance summary

`[PUB]` Apple AppKit (`NSMenu`/`NSApplication`/`orderFrontStandardAboutPanel`/
`NSToolbar`/drag-and-drop), AVFoundation (`AVAssetWriter`/`AVAssetWriterInputPixelBufferAdaptor`),
CoreVideo (`CVPixelBuffer`), ImageIO (PNG), HIG (menu bar, settings, full screen, About). ·
`[AROS]` host-volume `mount_hostvol`/`MakeDosNode`/`AddDosNode`
([../host-volume/spec.md](../host-volume/spec.md)); clipboard-bridge sync task
([../clipboard-bridge/spec.md](../clipboard-bridge/spec.md)); the AHI sub-driver
([../coreaudio-audio/spec.md](../coreaudio-audio/spec.md)). ·
`[OURS]` the frozen `cm_*` ABI + `CM_EV_SETTING` pull model + the host/AROS ownership
split (`cocoametal.h:116–133`); the existing settings panel + `NSUserDefaults` persistence
(`cocoametal_settings.m`); `cm_readback` + the H7 ImageIO oracle (`hosted/display.c`); the
`[SET]`/`[LIVE]`/`[FS]`/`[D4D5]` no-TCC verification techniques
([../cocoa-metal-display/spec.md](../cocoa-metal-display/spec.md)); the threaded inversion
([[cocoa-metal-display-driver]]); the control FIFO (`cocoametal_control.m`). ·
`[PUB-UTM]` UTM (Apache-2.0, [licenses](https://mac.getutm.app/licenses/)) — the lifecycle
**Machine** menu + graded **Power ▸**, the toolbar order, and the two-tier settings split
([controls](https://docs.getutm.app/basics/controls/),
[macOS prefs](https://docs.getutm.app/preferences/macos/)); permissively licensed, learned
from directly but reimplemented. ·
`[REF-CONFIRM]` vAmiga (GPL) — per-instance vs. global settings split + the DF0–DF3
drop-zone idiom (restated as plain AppKit drag-to-mount); the snapshot-as-document
fragility we avoid (issue #870). UAE family (GPL) — "directory mounted as an Amiga volume"
sharing (restated; already implemented in host-volume). Implement every `[REF-CONFIRM]`
item from its HIG/AppKit/`[AROS]` justification, never from a reference.
