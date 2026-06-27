# Host app shell — making the AROS window a first-class Mac app

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-26
> Scope: the **macOS application frontend** around the existing Cocoa/Metal window —
> *not* the AmigaDOS Shell. Implementation spec: [spec.md](spec.md).
> Builds directly on [../cocoa-metal-display/](../cocoa-metal-display/design.md) and is
> the UI home for [../host-volume/](../host-volume/design.md),
> [../clipboard-bridge/](../clipboard-bridge/design.md), and
> [../coreaudio-audio/](../coreaudio-audio/design.md).

## What & why

The [Cocoa/Metal display](../cocoa-metal-display/design.md) feature got AROS into a real
Mac window — keyboard drives the shell, mouse works, close→quit
([[cocoa-metal-display-driver]] memory; goal met 2026-06-26). But the process is still a
**bare binary that happens to open a window**, not a Mac *application*:

- **No menu bar** — no `NSMenu` anywhere; no App/File/Edit/Window/Help menus
  (`cocoametal_window.m` sets `NSApplicationActivationPolicyRegular` at line 170 but
  installs no main menu).
- **No About panel, no app icon, no app identity** — no
  `orderFrontStandardAboutPanel`, no `applicationIconImage`, no `.app` bundle, no
  `Info.plist`; the window title is the hard-coded string `"AROS"`
  (`cocoametal_window.m:181`), and `run-window.sh` launches the bare `AROSBootstrap`
  binary under Terminal with env vars (`run-window.sh:98–101`).
- **A display-only settings panel exists** but nothing else: `cocoametal_settings.m`
  builds a 320×210 panel with effect / scale-mode / filter / fullscreen / resolution
  controls, persisted to `NSUserDefaults` under `cocoametal.*` keys
  (`cocoametal_settings.m:54–84, 193–255`). It is good — but it is *one* panel about
  *display*, opened only via the `cm_open_settings` ABI call, with no menu, no ⌘, and no
  reach into the other host features.

This feature is the **host shell** layer becoming a first-class Mac citizen, and the
single place the project's *other* host features surface to the user. That split is
already the project's stated architecture: the [[aros-embeddable-library-goal]] memory
defines the future as **AROS engine** (host-agnostic `libAROS`) + **host shell** (the
thin embedder that sets policy and presentation) — "*the Cocoa app is shell #1.*" Menus,
About, icon, and the settings UI are precisely the host shell's job; **none of it
touches the engine.** Building it now flows with that architecture instead of against it.

Concretely, the shell becomes the home for four things the guest cannot do for itself:

1. **App citizenship** — menu bar, standard About panel, custom icon, an `.app` bundle,
   standard Mac shortcuts (⌘Q/⌘W/⌘,/⌃⌘F), lifecycle (quit/reset/fullscreen).
2. **A two-tier Settings UI** — global app Settings (⌘,) vs. a per-machine Configuration
   editor, the split many comparable hosts converge on (UTM; below). The current
   display panel becomes one tab.
3. **Surfacing the in-flight host features** — manage the mounted host-folder list
   ([host-volume](../host-volume/design.md)), toggle clipboard sharing
   ([clipboard-bridge](../clipboard-bridge/design.md)), set audio output/volume
   ([coreaudio-audio](../coreaudio-audio/design.md)) — each as one clean UI surface
   instead of an env var.
4. **Two new host-capture features the readback path already enables** — **Take
   Screenshot** (PNG) and **Record Movie** (H.264/HEVC), the gap even UTM leaves open
   (issue [#6676](https://github.com/utmapp/UTM/issues/6676)).

The hard constraint the whole project lives under still holds: **the live window /
menus / About panel cannot be verified by screen-capture** (`screencapture` needs macOS
Screen-Recording / TCC — a manual click that breaks build→run→observe→verdict). So every
surface here keeps a *programmatic* oracle alongside the visible UI. See "The
unattended-verification tension".

## Does it already exist?

Partly — the window and a display-only settings panel exist; the app-level citizenship
and the cross-feature UI do not. As-built inventory (cited to source):

| Surface | State today | Where |
|---------|-------------|-------|
| `NSApplication` | created, `ActivationPolicyRegular` set; **no menu bar**, **no delegate** | `cocoametal_window.m:165,170` |
| Main menu (App/File/Edit/…) | **NOT present** — no `NSMenu`/`NSMenuItem` anywhere | — |
| About panel / app icon / `.app` bundle | **NOT present**; bare binary, title hard-coded `"AROS"` | `cocoametal_window.m:181`; `run-window.sh:98–101` |
| Window chrome | `Titled|Closable|Miniaturizable`, fullscreen-capable, black bg; close → `exit(0)` | `cocoametal_window.m:176–194, 143` |
| Settings panel | built + persisted: effect / scale / filter / fullscreen / resolution | `cocoametal_settings.m:54–84, 193–255` |
| Option ABI | `cm_set_option`/`cm_get_option`/`cm_open_settings`; host-acted vs AROS-facing split; `CM_EV_SETTING` pull model | `cocoametal.h:116–133`, `cocoametal.m:574–589` |
| Screenshot | `cm_readback` (BGRA8) exists; control FIFO writes **PPM** (no PNG) | `cocoametal.h:163–166`, `cocoametal_control.m:57–86` |
| Video / recording | **NOT present** — single-frame readback only | — |
| Headless control | FIFO (`$AROS_CM_CONTROL`): `K`/`M`/`B`/`S` (key/move/button/shot) | `cocoametal_control.m:89–127` |
| Host-volume mounts | env-var only (`AROS_HOST_VOLUME`, two volumes RO+RW at boot); **no runtime add/remove** | `run-window.sh:88–89`; [host-volume spec](../host-volume/spec.md) R-LAUNCH |
| Launch / packaging | bare binary, ad-hoc + hardened-runtime codesign, `DYLD_FALLBACK_LIBRARY_PATH`; **no `.app`/`Info.plist`/icon** | `run-window.sh:29,93–101` |

So the existing `cm_*` option ABI and the persisted settings panel are a **real
foundation** — the host/AROS ownership split (`CM_OPT_*` host-acted vs AROS-facing,
`cocoametal.h:116–133`) is exactly the principle this feature scales up to the whole
app. What's missing is the *app* around it.

## External prior art (web-grounded, *not* in the AROS tree)

The best worked example of "present a virtual machine as a first-class Mac app" is
**UTM** (Apache-2.0 — permissively licensed, [licenses](https://mac.getutm.app/licenses/)),
with Apple's **HIG** as the baseline. UTM's **publicly-visible design** — its docs and
shipping app, **never its source** — intentionally inspired this feature's UI patterns
(menu/Power shape, two-tier settings, toolbar order); UTM is Apache-2.0, so those public
patterns are safe to learn from directly and are tagged `[PUB-UTM]`. No third-party
implementation *source* — UTM's or any emulator/agent/driver's — was read, searched, or
consulted; everything else is derived from published standards (HIG/AppKit) and our own
spikes (flagged `[DERIVED]`, restated never copied), and any resemblance at the code level
is coincidental. The overwhelming majority of this feature is plain `[PUB]` AppKit + HIG.

**The convergent patterns worth replicating (grounded):**

- **Two-tier settings split** — separate *global app Settings* from a *per-machine
  Configuration*. UTM: a tabbed ⌘, Settings window (Application / Display /
  Sound / Input / Network / File…) vs. a per-VM editor with a left-rail device list
  ([UTM macOS prefs](https://docs.getutm.app/preferences/macos/),
  [QEMU settings](https://docs.getutm.app/settings-qemu/settings-qemu/)). The same
  global-emulator-settings vs. per-instance-machine-settings split (ROM/Chipset/Memory/
  Peripherals/Audio/Video, all specific to a single machine instance) recurs across
  comparable hosts.
- **A lifecycle menu with run-state shown inline** — UTM's frontmost-window "Virtual
  Machine" menu owns Start, a dynamic **Pause/Resume**, **Restart**, and a graded
  **Power ▸** submenu (Request power down → Force shut down → Force kill)
  ([UTM controls](https://docs.getutm.app/basics/controls/)). Parallels uses an
  **Actions** menu, VMware Fusion a **Virtual Machine** menu — all the same shape.
- **An `NSToolbar` of control buttons** on the machine window (UTM default order: Stop →
  Start/Pause → Restart → Capture Input → Drives → Shared Folder → Displays; auto-hidden
  in fullscreen). A leading toolbar is a common shape across comparable hosts.
- **Input capture with a configurable release hotkey** — exclusive grab so guest
  combos reach AROS, released by a hotkey (UTM: Ctrl+Option default, changeable;
  Alt+Cmd is another common default), plus auto-capture-on-fullscreen / on-focus.
- **Host-folder sharing modeled as a mounted volume**, surfaced in a Sharing pane + a
  runtime "Shared Folder" menu. "Directory mounted as an Amiga volume" is the *natural*
  Amiga sharing idiom (a host directory exposed as an Amiga volume, with an on-disk
  metadata sidecar for attributes/protection bits) — which is **exactly what
  [host-volume](../host-volume/design.md) already implements** (its own on-disk
  metadata sidecar format).
- **Drag-and-drop of media onto the window** — DF0–DF3-style drop zones are a clearly
  better affordance than UTM's menu-only mounting and the right Amiga idiom.
- **Clipboard sharing as a single toggle** — a near-universal convention across host
  applications (UTM, Parallels, Fusion). Maps onto
  [clipboard-bridge](../clipboard-bridge/design.md).
- **Screenshot + video as first-class** — screenshot-plus-recorder and screenshot
  collections are common in comparable hosts. UTM has **neither** (open issue
  [#6676](https://github.com/utmapp/UTM/issues/6676)) — a gap we can beat, because
  `cm_readback` already hands us the exact pixels.
- **Apple HIG baseline** — canonical menu order App→File→Edit→View→[app]→Window→Help;
  standard About via `orderFrontStandardAboutPanel`; **Settings… (⌘,)** as a
  non-resizable toolbar-tab window; **disable, don't hide** unavailable items; use the
  Ventura+ label **"Settings…"** not "Preferences…"
  ([HIG menu bar](https://developer.apple.com/design/human-interface-guidelines/the-menu-bar),
  [HIG settings](https://developer.apple.com/design/human-interface-guidelines/settings)).

**What to deliberately NOT copy (single-machine AROS host ≠ general VM manager):**

- **No library/gallery window.** UTM, VirtualBuddy, Parallels, Fusion all center
  on a list of *many* VMs. An AROS host runs **one** machine — go single-window (the AROS
  display *is* the app), which also aligns with the [[aros-embeddable-library-goal]]
  one-engine-one-window model.
- **No snapshot-as-document model.** When a save-state *is* the document format, the
  format tends to break across releases; don't tie the app's document type to a
  save-state. UTM ships fine with *no* named snapshots at all.
- **No backend zoo / raw-args escape hatch / multiple sharing protocols.** UTM's sprawl
  exists because it fronts two hypervisors; we have one path.
- **No guest-agent install requirement.** UTM/Parallels/Fusion/VirtualBuddy all gate
  sharing+clipboard behind a separately-installed guest tools package. Since AROS *is*
  our OS, host-volume and clipboard-bridge are built into the AROS side — they "just
  work," a genuine advantage over every host surveyed.
- **No general-VM hardware cruft** — USB passthrough, network-adapter zoo, TPM/RNG,
  Unity/Coherence, multi-display switching, Rosetta/balloon. Irrelevant to one AROS box.

## The line we will not cross — host concerns vs. guest Prefs

The user's explicit constraint: **do not mirror AROS's own settings into the host UI.**
AROS already owns, *in-guest* (Wanderer/Prefs), everything the guest can set itself. The
host shell owns **only the host boundary** — things AROS physically cannot reach because
they live on the Mac side of the wall. The rule of thumb, and the litmus test for every
candidate menu item or settings control:

> **If a running AROS could change it from inside the guest, it does NOT belong in the
> host menu.**

| Stays in-guest (AROS Prefs — do NOT mirror) | Host shell owns (the Mac boundary) |
|----|----|
| ScreenMode, palette, Wanderer/Workbench prefs | Window scaling / filter / retina / fullscreen *presentation* |
| Keymap, key-repeat, Input prefs | Input **grab/release** (a host-window concern) |
| Locale, Time, Fonts, Printer, serial | **Which host folders** are mounted as volumes |
| In-guest copy/paste, clipboard.device units | Host **clipboard bridge** on/off (crosses the wall) |
| AHI mixing / in-guest volume mixer | Host **audio output device** + master volume/mute |
| Network stack config (AROS network prefs) | (status only — host sockets are guest-owned) |
| Reboot from the guest shell | App **lifecycle** (Quit, Force Quit, Reset, About) |

This mirrors the ownership split the existing ABI already encodes — `CM_OPT_*` are
"host-acted" *presentation* keys vs. "AROS-facing" *functional* keys the shim only
relays (`cocoametal.h:116–133`, [display spec](../cocoa-metal-display/spec.md) §Settings).
This feature scales that one principle from a settings panel to the whole application.

### How each existing feature maps to the host UI

| Feature | Host-UI surface | Notes |
|---------|-----------------|-------|
| [cocoa-metal-display](../cocoa-metal-display/design.md) | **Display** settings tab + **View** menu (scaling, filter, retina, fullscreen) | the existing panel becomes the Display tab; reuse `CM_OPT_EFFECT/SCALE_MODE/FILTER/FULLSCREEN` verbatim |
| [host-volume](../host-volume/design.md) | **Drives / Sharing** tab — the mounted-volume **list** (add/remove, RO↔RW), drag-drop a folder to mount; runtime **Shared Folder** menu | the headline new UI; needs a *runtime* mount/unmount path (today env-var-only) |
| [clipboard-bridge](../clipboard-bridge/design.md) | **Sharing** tab — one **"Share clipboard"** checkbox | single toggle, universal convention |
| [coreaudio-audio](../coreaudio-audio/design.md) | **Sound** settings tab — output device + master volume + mute | `CM_OPT_AUDIO_VOLUME` is already stubbed (`cocoametal.h:130`) |
| [bsdsocket-net](../bsdsocket-net/design.md) | **status indicator only** (e.g. a Machine-menu line "Networking: host sockets") | guest-owned; not a host setting |
| [68k-jit / run68k](../68k-jit/design.md) | **File ▸ Open 68k Program…** (future) — launch a hunk via the JIT | optional menu integration, deferred |
| **screenshot** (new) | **File ▸ Take Screenshot** (⇧⌘3) + a Captures settings tab | `cm_readback` → PNG via the H7 ImageIO path |
| **video** (new) | **File ▸ Record Movie…** + Captures tab | AVFoundation `AVAssetWriter` over `cm_readback` frames |

## Design

The whole feature is **host-shell-side** — it lives in the Objective-C shim and the
launcher, and it talks to AROS only through the existing `cm_*` ABI (extended,
append-only) and the existing `CM_EV_SETTING` pull mechanism. **The AROS engine sees no
new contract it doesn't already have**, honoring the engine/shell split.

### The threading insight that makes this natural (the load-bearing point)

The display spec's host-only tests deliberately avoid `[NSApp run]` and hand-pump
`CFRunLoopRunInMode(...,0,true)` (the D2t model,
[display spec](../cocoa-metal-display/spec.md) §Threading). That made it *look* like a
real menu bar (which wants the app's menu-tracking run loop) would fight the design. **It
does not** — because in the real **threaded inversion** (`AROS_DARWIN_THREADED=1`, commit
`c88b70ed`, [[cocoa-metal-display-driver]]) AROS runs on a **dedicated pthread** and the
**main thread pumps the CoreFoundation run loop**. That is *exactly* the shape of an
ordinary Cocoa app: AppKit + menus + `[NSApp run]` on the main thread, the app's real
work on another thread. So a standard menu bar and About panel are **more** natural in
production than in the single-thread host tests, not less.

The one genuine question this raises (resolved by the first spike): does the main thread
run a full `[NSApp run]` (standard, gives free menu tracking + key-equivalent dispatch),
or keep the current hand-pump and install menus by hand? The display spec already found
that **fullscreen *exit* needs the app run loop to advance**
([display spec](../cocoa-metal-display/spec.md) §Settings, "Hand-pumped-transition
finding") — a hint that adopting `[NSApp run]` on the main thread (with AROS on its
pthread) is the cleaner production model. Either way the AROS display-server task's
`cm_pump_events` drain (`cocoametal_window.m:443`) must keep working; menu tracking is a
*nested* run loop on the main thread and must not stall the AROS-side event drain. **This
is what spike [G1] settles before anything else is built.**

### App identity — bundle, icon, About

- **`.app` bundle.** Wrap `AROSBootstrap` + `cocoametal.dylib` + the icon in a normal
  `Foo.app/Contents/{MacOS,Frameworks,Resources}` layout with an `Info.plist`
  (`CFBundleName`, `CFBundleIdentifier`, `CFBundleIconFile`, `CFBundleShortVersionString`,
  `NSHighResolutionCapable=YES`, `LSMinimumSystemVersion`). Today `run-window.sh` runs a
  bare binary and resolves the dylib via `DYLD_FALLBACK_LIBRARY_PATH=$HOME/lib`
  (`run-window.sh:42,99`); bundling moves the dylib to `Contents/Frameworks` with an
  `@rpath`/`@loader_path` install-name so the `DYLD_*` hack goes away. **The bundle must
  preserve** the hardened-runtime + ad-hoc codesign that `run-window.sh:93–95` applies
  and the entitlements (the J1 `allow-jit` path for the 68k JIT, and the
  hostlib `dlopen`). `run-window.sh` becomes a thin "build the bundle, then `open` it"
  wrapper (keeping a `--terminal` mode for the unattended serial channel).
- **Custom icon.** An `.icns` (an Amiga-checkmark / AROS mark over a Mac-rounded-rect) in
  `Contents/Resources`, referenced by `CFBundleIconFile`; also set at runtime via
  `NSApp.applicationIconImage` so a bare-binary run (the test path) still shows it in the
  Dock. Asset is a deliverable, not blocking.
- **About panel.** The standard AppKit panel — `[NSApp orderFrontStandardAboutPanel:]`
  pulls name/version/build/copyright from `Info.plist` for free; pass an
  `NSAboutPanelOptionCredits` attributed string (AROS license + this port's credits). No
  custom window. Wired to the App-menu "About AROS" item.

### Menu bar (the proposal — HIG order, UTM-grounded items)

Built once at app init (in the threaded inversion's main-thread startup, beside the
existing `sharedApplication`/`setActivationPolicy` at `cocoametal_window.m:165–170`).
Every item targets an `NSApplicationDelegate` / a small `CMShellController` whose actions
route to the existing `cm_*` ABI or the new ABI below. **Disable, don't hide** items that
don't apply (HIG).

```
 AROS  (app menu)   About AROS · Settings… (⌘,) · Services · Hide (⌘H) · Quit (⌘Q)
 File               Take Screenshot (⇧⌘3) · Record Movie… ·
                    ── · Open Folder as Volume… (⌘O) · Open Recent ·
                    ── · Open 68k Program…  (future) · Close (⌘W)
 Edit               Undo · Cut/Copy/Paste · Select All        (guest text + clipboard)
 Machine            Reset (⌃⌘R) ·
                    Power ▸ (Request Power Down · Force Shut Down · Force Quit) ·
                    ── · Capture Input (⌃⌘I) ·
                    Volumes ▸ (mounted host folders — eject / reveal) ·
                    Share Clipboard  (checkmark toggle) ·
                    Send Key ▸ (Ctrl-Amiga-Amiga, …)
 View               Enter Full Screen (⌃⌘F) ·
                    Scaling ▸ (Aspect Fit · Integer · Pixel-Perfect · Stretch) ·
                    Filter ▸ (Nearest · Linear) · Scanlines (toggle) ·
                    Retina / HiDPI (toggle) · Show/Hide Toolbar
 Window             Minimize (⌘M) · Zoom · Bring All to Front
 Help               AROS Help · AROS Website · Report an Issue
```

Grounding: **Machine** = UTM's "Virtual Machine" menu + the Power submenu verbatim shape
([UTM controls](https://docs.getutm.app/basics/controls/)); **View** scaling/filter map
1:1 onto the existing `CM_OPT_SCALE_MODE`/`CM_OPT_FILTER`/`CM_OPT_EFFECT`
(`cocoametal.h:116–122`), so those items are *already wired underneath*; menu order +
shortcuts per HIG. The **Edit** menu's Cut/Copy/Paste route through the clipboard bridge
(they are the host-side affordance for it). "Send Key" handles combos AppKit would
otherwise eat (⌘ chords). Note **Power ▸** replaces the current brutal close→`exit(0)`
(`cocoametal_window.m:143`) with a graded shutdown that lets the engine end its thread
(the [[aros-embeddable-library-goal]] "no unilateral `exit()`" rule).

An **`NSToolbar`** on the window mirrors the high-frequency items (Power · Capture Input ·
Volumes · Screenshot · Record), auto-hidden in fullscreen (a common host pattern; UTM
does this). Optional for v1; the menu bar is the contract.

### Settings — two tiers (the split both references use)

Both as non-resizable **toolbar-tab** windows (HIG settings pattern), opened from the App
menu, **General first**. The existing `cm_open_settings` panel
(`cocoametal_settings.m:173`) is refactored into the **Display** tab of the Machine
Configuration; persistence stays on `NSUserDefaults` (`cocoametal.*`,
`cocoametal_settings.m:54–84`).

**Global app Settings (⌘,)** — apply to every session:

| Tab | Controls | Backed by |
|-----|----------|-----------|
| General | start behavior, confirm-on-quit, Dock/menu-bar-icon presence | `NSUserDefaults` (host-only) |
| Input | **release hotkey** (Ctrl+Opt default), auto-capture on fullscreen / on focus, right-click / Option-as-Meta | host-only (UTM Input pane; common host conventions) |
| Captures | screenshot format (PNG) + location, movie codec (H.264/HEVC) + fps + location | host-only |

**Machine Configuration** — the one AROS instance:

| Tab | Controls | Backed by |
|-----|----------|-----------|
| Display | effect / scale-mode / filter / fullscreen / requested resolution (**existing panel**) | `CM_OPT_EFFECT/SCALE_MODE/FILTER/FULLSCREEN` + `CM_OPT_REQUEST_MODE_W/H` (`cocoametal_settings.m:209–255`) |
| Drives & Sharing | mounted host-folder **list** (add/remove, RO↔RW), **Share clipboard** toggle | new `cm_volume_*` ABI ([host-volume](../host-volume/design.md)) + clipboard toggle |
| Sound | output device, master volume, mute | `CM_OPT_AUDIO_VOLUME` (stubbed today) + new `CM_OPT_AUDIO_DEVICE` ([coreaudio-audio](../coreaudio-audio/design.md)) |

### ABI extensions (append-only — the existing contract is frozen, never moved)

The display ABI is explicitly **append-only** ([display spec](../cocoa-metal-display/spec.md)
§Build). New surfaces add new symbols / enum values at the end, bumping
`CM_ABI_VERSION`; nothing existing moves. The AROS engine keeps using the same
`cm_pump_events` → `CM_EV_SETTING` pull model — **no host→AROS callback**, matching the
threading rule.

- **Screenshot** — `int cm_capture_png(CMContext*, const char *path)`: `cm_readback` →
  the proven H7 ImageIO encoder (`hosted/display.c`) → PNG file. (The control FIFO's
  `S`-command PPM path, `cocoametal_control.m:57–86`, is the throwaway precursor.)
- **Video** — `cm_record_start(CMContext*, const char *path, int fps, int codec)` /
  `cm_record_stop(CMContext*)`: an `AVAssetWriter` + `AVAssetWriterInputPixelBufferAdaptor`
  fed a `CVPixelBuffer` per present from the same offscreen-oracle readback. **No
  ScreenCaptureKit, no TCC** — we own the pixels (the readback buffer), so writing them
  to a movie needs *no* Screen-Recording permission. Native AVFoundation, no FFmpeg
  dependency (contrast recorders that lean on an external FFmpeg).
- **Runtime volume management** — `cm_volume_list/add/remove`. These are **AROS-facing**
  (the mount is an AmigaDOS-side `MakeDosNode`/`AddDosNode` op,
  [host-volume spec](../host-volume/spec.md) R-LAUNCH): the shim records the request and
  enqueues a `CM_EV_SETTING` (new `CM_OPT_VOLUME_*` keys); the AROS side pulls it and does
  the mount/unmount, then reports the live list back via a new pull. Today host-volume
  mounts only at boot from `AROS_HOST_VOLUME` (`run-window.sh:88`); **runtime add is new
  and runtime *remove* is UNVERIFIED** (clean unmount of a live `emul-handler` volume —
  flagged below).
- **Clipboard toggle / audio device** — `CM_OPT_CLIPBOARD_SHARE`, `CM_OPT_AUDIO_DEVICE`
  (AROS-facing), same pull pattern.
- **Lifecycle** — `CM_OPT_POWER` (AROS-facing: request-shutdown / reset) so Power ▸
  routes a clean request to the engine instead of `exit(0)`.

### Drag-and-drop

Register the content view for `NSFilenamesPboardType` / `NSPasteboardTypeFileURL`. Drop a
**folder** → `cm_volume_add` (mount as a volume — the familiar DF0–DF3 drop-zone idiom,
applied to AROS's folder-as-volume model). Drop a **68k hunk** → File ▸ Open 68k Program
path (future). AROS has no ADF/floppy concept yet, so "drives" = host-folder volumes for
now; real disk-image support waits on a `hostdisk`-style device and is out of scope.

## The unattended-verification tension (and how we solve it)

Menus, the About panel, the Dock icon, and the settings windows are *visible UI* that
`screencapture` would need TCC to read — the same wall H7 hit. So, as everywhere in this
project, **the visible UI is never the oracle**; we assert structure and behavior
programmatically:

- **Menu bar** — walk the `NSMenu` tree after init and assert titles, submenu shape, key
  equivalents, and `target/action` wiring. No pixels.
- **Menu actions** — invoke each item's action selector directly (`[item.target
  performSelector:item.action]`) and assert the resulting `cm_*` state or the enqueued
  `CM_EV_SETTING` (the exact technique the existing `[SET]` test uses,
  [display spec](../cocoa-metal-display/spec.md) §Verification→SET).
- **About panel** — assert `Info.plist` keys and that `orderFrontStandardAboutPanel:`
  with our options dict runs without raising; the panel needn't be seen.
- **Screenshot** — `cm_capture_png` → assert the PNG file exists, has a valid header, the
  right dimensions, and known marker pixels (the H7 `px_is` discipline,
  `hosted/display.c`).
- **Video** — record N frames of a known animated pattern, stop, then probe the movie
  off-process (`AVAsset` track count / duration / frame size, or `ffprobe`) and assert N
  frames at the expected dimensions; optionally decode one frame and pixel-check it.
- **Runtime volume add/remove** — invoke `cm_volume_add(tmpdir)`, pump the
  `CM_EV_SETTING`, let AROS mount, then assert two-sided exactly as host-volume's
  `[V4]`/`[V5]` do (host writes a fixture, AROS lists it, [host-volume
  spec](../host-volume/spec.md) §Verification).
- **Input capture / release hotkey** — synthesize the release chord via the in-process
  `[NSApp postEvent:]` path the `[D4D5]` test already uses (no TCC,
  [display spec](../cocoa-metal-display/spec.md) §Verification→D4/D5) and assert the
  capture flag toggles.

Every spike prints one greppable marker and exits clean — the `[M*]`/`[H*]`/`[D*]`
discipline (NOTES.md). The window/menus *do* appear on screen during a run (that's the
feature); the *verdict* never depends on a human seeing them.

## Plan — spikes in the loop

Each marker is one binary, one PASS/FAIL, grounded-then-built, ordered so each de-risks
one unknown before the next. (Marker family `[G*]` = host-shell GUI.)

- **[G1] Run-loop + menu bar coexistence (the de-risk).** Under the real threaded
  inversion shape (AppKit on main, a stand-in worker pthread for "AROS"), install a full
  `NSMenu` and either adopt `[NSApp run]` or keep the hand-pump; prove the worker's
  `cm_pump_events`-equivalent drain keeps running *while* a menu is tracking, and that
  menu key-equivalents dispatch. **PASS:** menu tree asserted + an item action fires +
  the worker drain advances during tracking. This settles the `[NSApp run]` question for
  everything below.
- **[G2] App identity.** `.app` bundle with `Info.plist` + `.icns`; `applicationIconImage`
  set at runtime; About panel via `orderFrontStandardAboutPanel:`. **PASS:** bundle
  launches, dylib resolves from `Contents/Frameworks` (no `DYLD_FALLBACK`), codesign +
  entitlements verify, `Info.plist` keys + About options asserted, hardened-runtime
  `dlopen` still works.
- **[G3] Menu → existing-ABI wiring.** Wire View (scaling/filter/effect/fullscreen) and
  the Display settings tab to the existing `CM_OPT_*` and assert state changes via
  `cm_get_option` — reusing the `[SET]` oracle. **PASS:** every View/Display control round-
  trips through the option ABI.
- **[G4] Screenshot.** `cm_capture_png` → PNG; assert header + dimensions + marker pixels
  (H7 oracle). **PASS:** valid PNG matches the known pattern.
- **[G5] Video.** `cm_record_start`/`stop` over an animated pattern; probe the movie
  off-process. **PASS:** movie exists with N frames at the expected size; one decoded
  frame matches.
- **[G6] Drives & Sharing UI → runtime mount.** `cm_volume_add`/`list`/`remove` →
  `CM_EV_SETTING` → AROS mount/unmount; two-sided assert (host-volume `[V*]` style). Drag-
  and-drop a folder exercises the same path. **PASS:** a runtime-added volume lists the
  host fixture from AROS; remove unmounts cleanly (or flag the unmount gap).
- **[G7] Settings tabs end-to-end.** Both windows (global ⌘, + Machine Config) built as
  toolbar-tab panels; persistence round-trips through `NSUserDefaults`; the clipboard
  toggle + audio volume/device controls enqueue their `CM_EV_SETTING`s. **PASS:** every
  tab's controls assert through the option ABI / events; defaults restore on relaunch.
- **[G8] (stretch) Lifecycle + capture-input.** Power ▸ routes a clean shutdown request
  (no `exit(0)`); Capture Input toggles the grab; release hotkey (configurable) releases.
  **PASS:** shutdown request observed engine-side; capture flag toggles via the synthesized
  release chord.

## Risks & open questions

- **`[NSApp run]` vs. hand-pumped run loop (the central question).** A real menu bar wants
  the app's menu-tracking run loop; the display tests deliberately avoid `[NSApp run]`.
  The threaded inversion *should* make `[NSApp run]` on the main thread natural (AROS is
  on another pthread), and the fullscreen-exit finding already hints the app run loop is
  needed — but the interaction between menu tracking (a nested run loop) and the AROS
  display-server task's `cm_pump_events` drain is **UNVERIFIED**. **[G1] is gating.**
- **`.app` bundling vs. the hardened-runtime / JIT / `dlopen` story.** The bundle must keep
  the ad-hoc + hardened-runtime codesign (`run-window.sh:93–95`), the `allow-jit`
  entitlement (68k JIT, J1), and a working `dlopen` of `cocoametal.dylib` (now in
  `Contents/Frameworks` via `@rpath`). Moving off `DYLD_FALLBACK_LIBRARY_PATH` is the
  cleanup but must not break resolution. **UNVERIFIED** that all three survive bundling
  together — **[G2]**.
- **Runtime volume *unmount*.** host-volume mounts at boot today; runtime *add* is a
  natural `MakeDosNode`/`AddDosNode`, but cleanly *removing* a live `emul-handler` volume
  (open locks, in-flight packets) is **UNVERIFIED** — may need a "no open handles" guard
  or a deferred unmount. Flagged for [G6].
- **Video API choice.** AVFoundation `AVAssetWriter` (native, no dep, H.264/HEVC, and —
  crucially — **no TCC** because we feed it our own readback pixels, not a screen capture)
  vs. FFmpeg (a common external-dependency path). **Recommend AVFoundation.** Frame cadence
  ties to the present rate; a fixed-fps writer may need pts pacing — settle in [G5].
- **Menu items that AppKit eats.** ⌘-chords and some keys are consumed by the menu before
  reaching AROS. Capture Input (exclusive grab) is the answer for guest combos; "Send Key"
  covers the rest. Decide which chords AROS *needs* vs. which the host keeps.
- **Single-window vs. document model.** We deliberately reject the library/gallery and
  document-snapshot models (above). If save-states ever land, make them explicit versioned
  files with thumbnails (the common explicit-save-file shape), never the app's document type.
- **Icon asset.** A custom `.icns` is a design deliverable, not engineering-blocking; a
  placeholder ships first.
- **Not a W^X concern.** Menus/About/icon/AVFoundation are ordinary signed code; no
  executable-memory generation here (contrast the JIT). Noted so we don't build a
  workaround for a non-problem.

## References

In-project:
- `hosted/cocoametal/cocoametal_window.m` — `NSApplication`/window setup (`:165–194`),
  close→`exit(0)` (`:143`), the event-pump drain (`:443`).
- `hosted/cocoametal/cocoametal_settings.m` — the existing settings panel (`:173–266`),
  `NSUserDefaults` persistence (`:54–84`), control wiring (`:209–255`).
- `hosted/cocoametal/cocoametal.h` — the `cm_*` option ABI + `CMOption`/`CMEvent`/effect/
  scale/filter enums (`:33–166`).
- `hosted/cocoametal/cocoametal_control.m` — the FIFO control channel + PPM screenshot
  (`:57–127`).
- `hosted/cocoametal/cocoametal.m` — `cm_open_settings`/`cm_set_option`/`CM_EV_SETTING`
  enqueue + persisted-option apply (`:574–589, 799–801`).
- `graft/run-window.sh` — bare-binary launch, codesign, `AROS_HOST_VOLUME`, `DYLD_*`
  (`:29,88–101`).
- `hosted/display.c` — H7 ImageIO encode + `px_is` pixel-assert (the screenshot oracle).
- `docs/features/cocoa-metal-display/{design,spec,INTERFACE}.md` — the window + the frozen,
  append-only `cm_*` contract this extends; the SET/LIVE/FS/D4-D5 oracles reused here.
- `docs/features/host-volume/{design,spec}.md` — the folder-as-volume model the Drives UI
  drives; the `[V*]` two-sided verification reused.
- `docs/features/clipboard-bridge/{design,spec}.md`,
  `docs/features/coreaudio-audio/{design,spec}.md` — the Sharing/Sound surfaces.
- Memories: [[aros-embeddable-library-goal]] (engine/shell split — the architectural home
  of this feature), [[cocoa-metal-display-driver]] (the threaded inversion: AROS on a
  pthread, AppKit on main), [[aros-aarch64-project]].

Publicly-documented prior art (web, *not* in either tree — no implementation source read):
- UTM — Apache-2.0 ([licenses](https://mac.getutm.app/licenses/)); the lifecycle menu +
  Power submenu, the toolbar order, and the two-tier settings split:
  [controls](https://docs.getutm.app/basics/controls/),
  [macOS prefs](https://docs.getutm.app/preferences/macos/),
  [QEMU settings](https://docs.getutm.app/settings-qemu/settings-qemu/),
  [sharing](https://docs.getutm.app/guest-support/sharing/directory/); the missing
  screenshot/record feature: issue [#6676](https://github.com/utmapp/UTM/issues/6676).
- "Directory mounted as an Amiga volume" sharing + the on-disk metadata-sidecar
  format: our own [host-volume](../host-volume/design.md) design.
- Apple HIG — [the menu bar](https://developer.apple.com/design/human-interface-guidelines/the-menu-bar),
  [settings](https://developer.apple.com/design/human-interface-guidelines/settings),
  [going full screen](https://developer.apple.com/design/human-interface-guidelines/going-full-screen);
  About panel API:
  [`orderFrontStandardAboutPanel(_:)`](https://developer.apple.com/documentation/appkit/nsapplication/orderfrontstandardaboutpanel(_:)).
