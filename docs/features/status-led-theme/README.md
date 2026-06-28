# Status-bar LEDs + Theme switch

Amiga-style **Power + Activity LEDs** in the Daedalos window's status bar, plus a
**Dark / Light / System** appearance switch so the whole app — title bar, menus,
Settings, and the status bar — reads as one coherent theme.

All host-side (`hosted/cocoametal/`). **No `../aros-upstream` change; the cocoametal
ABI stays at version 2.**

## What you see

The footer under the AROS image is now a native-material status bar (an
`NSVisualEffectView`, so it tracks the active appearance for free):

```
Daedalos — AROS on Apple Silicon                    PWR ●   ACT ●
```

- **PWR** — green while AROS is running; amber on a soft power-down / reset request,
  red on a forced shut-down / quit (driven by `CM_OPT_POWER`).
- **ACT** — an activity light: it flickers up whenever AROS pushes a frame
  (`cm_present`) and fades when the machine goes idle. Honestly an *activity*
  indicator — **not** a real DF0 disk-I/O LED. The host cannot see AROS disk access
  without a hook in the OS tree; a true DF0 LED is a documented follow-up (it would
  need a `trackdisk`/emul hook + an ABI bump to v3 + a HIDD rebuild).

## Choosing a theme

Two equivalent surfaces, both host-acted (they set `NSApp.appearance`):

- **Settings ▸ General ▸ Appearance** — System / Light / Dark popup (persists).
- **View ▸ Theme** — System / Light / Dark menu (persists; radio checkmark).

Default is **System** (follow macOS). The choice is stored in `NSUserDefaults`
(`cocoametal.theme`) and re-applied at the next launch. The black Metal area (the
guest framebuffer) is deliberately never recolored — only the app chrome themes.

## How it works

- `CM_OPT_THEME = 0x05` is a host-acted option in the reserved `0x05..0x0F` range
  (the same precedent as `CM_OPT_RETINA = 0x04`), so the AROS-facing contract — and
  `CM_ABI_VERSION` — is unchanged at 2. `cm_set_option(CM_OPT_THEME, …)` records the
  value and calls `cm__apply_theme_appkit`, which sets `NSApp.appearance` (nil =
  System, Aqua = Light, DarkAqua = Dark) and forces each window to redraw.
- The status bar is built by `cm__build_status_bar` (`cocoametal_statusbar.m`), called
  from `cm_try_window`. The Activity LED samples `cm__present_count` on a 0.1 s
  main-run-loop timer (common modes, so it ticks under the hand-pumped CFRunLoop);
  `cm_present` itself is untouched.

## Verify it

```
make cocoametal-statusbar      # [STATUS] PASS
```

The footer is host chrome outside the Metal view, so the offscreen oracle
(`cm_readback`) can't capture it. `[STATUS]` instead asserts the AppKit objects
directly (the same unattended technique as `[GSHELL]`): the `NSVisualEffectView` +
`CMLEDView` are installed; `CM_OPT_THEME` drives `NSApp.appearance` for
Dark/Light/System (and rejects out-of-range); the Activity LED lights on `cm_present`
and decays when presenting stops. It also renders the LED view to a PNG via
`cacheDisplayInRect` (no Screen-Recording / TCC) and asserts real green + amber LED
pixels drew — written to `$AROS_RUN_DIR/aros-statusbar-leds.png` (or `$TMPDIR`).

To see it live, run the window (`graft/run-window.sh`) and switch themes from
**View ▸ Theme**.

## Files

| File | Role |
|------|------|
| `hosted/cocoametal/cocoametal_statusbar.m` | status bar view, LEDs, `cm__apply_theme_appkit` (new TU) |
| `hosted/cocoametal/cocoametal.h` | `CMTheme` enum + `CM_OPT_THEME = 0x05` |
| `hosted/cocoametal/cocoametal.m` | option plumbing, `theme`/`powerReq` fields, accessors, weak stubs |
| `hosted/cocoametal/cocoametal_window.m` | footer now calls `cm__build_status_bar` |
| `hosted/cocoametal/cocoametal_shell.m` | View ▸ Theme submenu |
| `hosted/cocoametal/settings.json` + `…_settings_schema.m` | Appearance popup + const map |
| `hosted/cocoametal/statusbar_test.m` | the `[STATUS]` test |

Independent work: Apple AppKit/CoreGraphics docs + Apple HIG only; no third-party
implementation source was read or consulted.
