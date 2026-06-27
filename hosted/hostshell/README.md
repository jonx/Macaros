# hosted/hostshell — host app-shell POC

The first cut of the [host app shell](../../docs/features/host-app-shell/design.md):
turning the AROS-on-macOS window into a **first-class Mac app** — menu bar, About
panel, app icon — implemented as an **isolated POC** that touches none of the existing
`hosted/cocoametal/` code, so it can be developed in parallel and merged later.

> Scope = the parts that are independent of the Metal window, so the whole thing
> verifies **headless** (no window server, no TCC, no screenshot). It proves the
> *load-bearing risk* (`[G1]`: a real menu bar coexisting with the AROS thread) before
> we wire any of it into the live shim.

## Run it

```sh
./build.sh                  # build + run BOTH verifiers → "[G] PASS" and "[GS] PASS"
./build.sh --show           # LIVE app — menu bar; ⌘, opens the generated Settings window
./build.sh --show-settings  # LIVE — just the generated Settings window
./build.sh --build          # build only (build/hostshell/{shell_poc,settings_poc})
```

Self-contained: `build.sh` calls the host `clang` directly (host-clang, ARC, arm64,
ad-hoc codesign — the same flags as the cocoametal spikes) and **does not touch the
project Makefile**. Output lands in `build/hostshell/`.

## Files

| File | What |
|------|------|
| `cmshell.h` | the shell's C interface + the **`CMShellSink`** seam (the engine/shell split) |
| `cmshell.m` | `CMShellController` — builds the HIG menu bar, the About panel, the app icon; routes every menu action to the sink. Pure AppKit, **no AROS headers, no Metal** |
| `cmsettings.h` | the settings **schema** vocabulary (`CMSetting`) + the loader/model/store/window API |
| `cmsettings.m` | the JSON schema **loader**, the two stores (NSUserDefaults + the in-place **config-file** store), the load/set/apply model, and the **generated** toolbar-tab window |
| `settings.json` | **the schema as data** — one entry per setting (the single source of truth); loaded + validated at runtime |
| `shell_test.m` | menu verifier (`[G-*]`) + the `--show` live app |
| `cmsettings_test.m` | settings verifier (`[GS-*]`) + the `--show` live Settings window |
| `build.sh` | standalone build+run (both binaries; copies `settings.json` next to them) |

## The seam (why this merges cleanly)

`CMShellController` never calls AROS or the Metal shim. Each menu item becomes a neutral
*intent* handed to a **`CMShellSink`** (a struct of C function pointers). The POC plugs in
a recording **mock** sink so every action is asserted in-process. At merge, the sink's
fields forward to the real ABI — `set_option → cm_set_option`, `capture_png →
cm_capture_png`, `volume_add → cm_volume_add`, `power → CM_OPT_POWER`, … — and nothing in
`cmshell.m` changes. This is the [[aros-embeddable-library-goal]] engine/shell split: the
shell stays host-agnostic, the engine sees no new contract.

## What it proves (all green)

- **`[G-MENU]`** — the full menu bar matches the design's R-MENU: App / File / Edit / View
  / Machine / Window / Help, with the UTM-inspired **Machine ▸ Power** submenu (Request
  Power Down · Force Shut Down · Force Quit), View scaling/filter/scanlines, the right key
  equivalents (⌘, ⇧⌘3, ⌃⌘F, ⌃⌘R, ⌃⌘I …), and target/action wiring. Asserted by **walking
  the `NSMenu` tree**, never a screenshot.
- **`[G-ACTION]`** — every custom item, invoked directly, routes the correct intent+value
  through the sink (e.g. Scanlines toggles `CM_OPT_EFFECT` NEAREST↔SCANLINE; Power ▸ Force
  Quit → `power(FORCE_QUIT)`; Share Clipboard → `CM_OPT_CLIPBOARD_SHARE`).
- **`[G-IDENTITY]`** — the Dock/app icon is set (drawn with CoreGraphics, no window server
  needed) and the delegate is wired.
- **`[G-RUNLOOP]` — the `[G1]` de-risk.** An "AROS" worker pthread keeps draining (~830
  ticks / 0.25 s) while the **main thread is held in a nested run loop** (the menu-tracking
  shape), and a **worker→main GCD hop** (the real `cm__sync_main` pattern) is serviced in
  default mode **and in the actual `NSEventTrackingRunLoopMode`** — with an off-common
  control mode correctly *not* serviced, proving the probe discriminates.
  **Finding:** worker→main hops survive a live menu bar, so the threaded inversion
  (`AROS_DARWIN_THREADED=1`: AROS on a pthread, AppKit on main) is compatible with a
  standard menu bar + `[NSApp run]`. To be re-confirmed against the live shim at merge.

## Settings — one schema (JSON) → window + config-file binding

The settings window is **generated from a declarative schema**, not hand-coded per
control. The schema is a **data file** (`settings.json`), loaded + validated at runtime —
so adding/editing a setting needs no rebuild, and non-coders can edit it. Each entry binds
three things, so **adding a setting = adding one JSON entry** and the GUI / persistence /
apply all follow:

1. **Presentation** — `tier` (App ⌘, vs Machine), `tab`, `label`, `control`
   (checkbox / popup / slider / text / path), choices/range. The window is built by
   walking the schema.
2. **Storage** — a `(store, key)` binding into a pluggable backend:
   - `defaults` → NSUserDefaults (the `cocoametal.*` host-display keys — interops with the
     existing panel);
   - `conf` → a **dedicated host config file** (`aros-host.conf`, a line-oriented
     `keyword value` file the launcher + scripts share), edited **in place**: only the
     bound keyword's line is rewritten; every other line + comment + the file order are
     preserved. This is the "map GUI settings to *part* of the config file" mechanism —
     the GUI owns a subset of keys; humans and scripts can edit the rest.
3. **Apply** — `hostOption` (live `cm_set_option`, presentation), `arosOption` /
   `arosOptionStr` (AROS-facing, relayed via `CM_EV_SETTING` in the real shim), or
   `bootOnly` (conf only; takes effect next launch, e.g. `memory`). Routing goes through
   the same `CMShellSink` seam as the menu.

Values, defaults, and `applyKey` may be a JSON number **or** a named `CM_*` constant
(e.g. `"CM_SCALE_ASPECT_FIT"`, `"CM_OPT_FILTER"`) — readable and resolved at load.

What `[GS]` proves (all green): **`[GS-VALIDATE]`** the loader rejects malformed schema
with a clear error (bad JSON / missing field / bad control kind); **`[GS-SCHEMA]`** the
loaded schema is well-formed; **`[GS-CONF]`** the config-file store **edits one keyword in
place** (a 3-line `aros-host.conf` fixture: `memory 64`→`memory 256` keeps it 3 lines with
the comment + unrelated key intact; a new key appends exactly one line); **`[GS-MODEL]`**
persist + apply route correctly per entry (host-acted vs AROS-facing vs boot-only —
boot-only fires *no* live apply); **`[GS-GEN]`** the generated window has **exactly the
schema's tabs+controls** (Machine 4 tabs/8 controls, App 3 tabs/6 controls). `./build.sh
--show-settings` shows the generated window live.

## Not yet here (next, per [spec.md](../../docs/features/host-app-shell/spec.md))

- `cm_capture_png` (screenshot, `[G4]`) and `cm_record_*` (AVFoundation movie, `[G5]`) —
  need the Metal `cm_readback`, so they couple to the real shim; the menu items + sink
  hooks are already in place.
- Wiring the `conf` store's `aros-host.conf` into `graft/run-window.sh` (the launcher
  reads `memory`/`hostvolume`/… and translates them into the bootstrap args + env) so the
  GUI's edits drive the actual launch. The POC points the store at a temp fixture.
- Runtime **volume** add/remove + drag-and-drop (`[G6]`) — `cmshell` has the `volume_add`
  intent + `dropFolder:writable:`; the AROS-side runtime mount is the merge work.
- `.app` **bundle** packaging + `.icns` (`[G2]`) — the runtime icon is a CoreGraphics
  placeholder; bundling (and keeping hardened-runtime + `allow-jit` + `dlopen`) is the
  launcher work in `graft/run-window.sh`.

## Merge plan

1. Move `cmshell.{h,m}` into `hosted/cocoametal/` (or keep as a sibling TU), delete the
   mirrored enum block in `cmshell.h`, `#include "cocoametal.h"` instead.
2. Replace the mock sink with a real one whose fields call the `cm_*` ABI; build the
   sink once in the threaded-inversion main-thread startup (beside the existing
   `sharedApplication`/`setActivationPolicy` in `cocoametal_window.m`).
3. Add the new ABI symbols (`cm_capture_png`, `cm_record_*`, `cm_volume_*`,
   `cm_set_option_str`) + `CM_OPT_*` keys to `cocoametal.{h,exports}`, bump
   `CM_ABI_VERSION`, and land the `[G4]`/`[G5]`/`[G6]` spikes.
4. Settings: keep `cmsettings.{h,m}` + `settings.json` as-is; ship `settings.json` in the
   `.app` Contents/Resources (already resolved via `NSBundle`); point
   `cmsettings_set_conf_path` at the dedicated `aros-host.conf`; and let `cmshell`'s
   `open_settings` sink field build the window (already wired through `settingsAction:`).
   Extend the settings by editing `settings.json` — no rebuild, no UI code.

## Provenance

Implemented from Apple AppKit/Foundation/CoreGraphics docs + the Apple HIG + this
project's spec ([../../docs/features/host-app-shell/spec.md](../../docs/features/host-app-shell/spec.md)).
**Independent work: no third-party implementation source — emulator, agent, driver, or
otherwise — was read, searched, or consulted in producing it, and any resemblance to
existing implementations is coincidental.** The menu-bar shape was inspired by UTM's
publicly-visible design (Apache-2.0 — observed as users / from its public docs, never
its source) and otherwise follows the Apple HIG.
