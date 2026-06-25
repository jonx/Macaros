# Implementation spec — Cocoa/Metal display HIDD (Apple-native)

> Status: drafting (Role A) · Target: aarch64-darwin hosted · Drafted 2026-06-24
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Clean-room banner

**Role B (implementer): do NOT read vAmiga, WinUAE, FS-UAE, Amiberry, or any GPL
emulator source.** Implement only from this spec + the approved sources cited by tag:
`[PUB]` Apple framework docs / published standards, `[AROS]` in-tree AROS headers
(paths given), `[OURS]` this project's spikes. `[REF-CONFIRM]` items were sanity-checked
against vAmiga's GPL Metal pipeline by Role A but are restated here with an independent
`[PUB]`/`[AROS]` justification — implement from that, not from any reference.

## Scope

**In.** A hosted display driver for `aarch64-darwin` that: opens a real `NSWindow`;
presents the AROS chunky framebuffer to it via **Metal**; supports HiDPI/retina;
delivers mouse + keyboard input back into AROS; and exposes a **CoreGraphics readback**
path so the rendered frame can be pixel-asserted unattended (no Screen-Recording / TCC).

**Decision (confirmed with the project owner).** **Apple-native only:** AppKit +
Metal + QuartzCore, with CoreImage/ImageIO/CoreGraphics for readback. **No** SDL, **no**
OpenGL, **no** MoltenVK/Vulkan, **no** cross-platform abstraction. Rationale in
[design.md](design.md) (the in-tree SDL HIDD's macOS path is SDL 1.2 Quartz using
APIs Apple removed by High Sierra — a dead end).

**Out (non-goals, this spec).** 3D / GPU compositing; multi-monitor; hardware cursor
sprite (software cursor only at first); palette/LUT display modes (TrueColor only);
live on-screen verification via `screencapture` (TCC-blocked — we verify by readback).

## Architecture

Two layers joined by a **flat hand-written C ABI** (the ABI header is ours, ASCII, no
GPL lineage):

```
AROS side (aarch64, AROS crosstools)              Host side (Apple toolchain)
┌────────────────────────────────┐                ┌──────────────────────────────┐
│ Cocoa display HIDD             │  hostlib +     │ libcocoametal.dylib          │
│  · CLID_Hidd_Gfx subclass      │  H3 host-call  │  · NSApplication/NSWindow    │
│  · CLID_Hidd_BitMap subclass   │ ─────────────► │  · CAMetalLayer + MTLDevice  │
│  · 1 "display-server" task     │   C ABI        │  · MTLCommandQueue + textures│
│    pumps events + presents     │ ◄───────────── │  · CoreGraphics readback     │
└────────────────────────────────┘   CMEvent[]    └──────────────────────────────┘
        chunky framebuffer in AROS memory  ──upload──►  MTLTexture  ──present──► drawable
```

- **Host shim** `[OURS]` — Objective-C (`.m`) + C, built with the **host** clang (NOT
  AROS crosstools), the peer of `hosted/display.c` (the H7 ImageIO shim). It owns every
  Cocoa/Metal object and exposes the C ABI below. It pulls **no** AROS headers.
- **AROS HIDD** `[AROS]` — the `Hidd_Gfx` + `Hidd_BitMap` subclasses, reaching the shim
  through `hostlib.resource` (`dlopen` of the dylib, `HostLib_GetPointer` for each ABI
  symbol) across the H3 host-call boundary.
- Spike-phase paths: shim in `hosted/cocoametal/`; at graft, AROS side lands in the
  proposed `arch/all-darwin/hidd/cocoa/`.

## The C ABI (`cocoametal.h`)

Hand-authored, neutral. Types are ours; the *behaviour* of each call is specified
below. `[PUB]` Apple objects under the hood, `[AROS]` shapes driven by the HIDD's needs.

```c
typedef struct CMContext CMContext;

/* TrueColor only; matches one MTLPixelFormat with no swizzle. See "Pixel format". */
typedef struct {
    int  bytesPerPixel;   /* 4 */
    int  redShift, greenShift, blueShift, alphaShift;  /* bit positions */
    unsigned redMask, greenMask, blueMask, alphaMask;
} CMPixelDesc;

typedef enum { CM_EV_NONE=0, CM_EV_MOUSEMOVE, CM_EV_MOUSEBTN, CM_EV_KEY, CM_EV_CLOSE,
               CM_EV_RESIZE,
               CM_EV_SETTING /* v2 (append-only): user changed an AROS-facing option;
                                code=CMOption key, x=value, y=2nd value (paired W/H) */
             } CMEventType;
typedef struct {
    CMEventType type;
    int x, y;             /* logical (pre-scale) pixel coords, top-left origin */
    int code;             /* button index, or macOS virtual keycode */
    int pressed;          /* 1=down 0=up */
    unsigned mods;        /* CM_MOD_* bitmask */
} CMEvent;

/* Present-time fragment-shader effect (cm_set_effect). PRESENTATION-ONLY: it
   changes how the framebuffer is drawn into the live drawable, never the
   offscreen oracle (which cm_readback reads and which is fixed at pass-through/
   nearest). Append new effects here AND to the fragment-shader switch. */
typedef enum {
    CM_FX_NEAREST  = 0,   /* pass-through, nearest — identical to the oracle */
    CM_FX_SCANLINE = 1,   /* example CRT: odd-row darkening + a simple gamma */
    CM_FX__COUNT
} CMEffect;

/* w,h are the LOGICAL framebuffer size — arbitrary, not fixed (proved at two
   resolutions by [D1]/[D2]). The live drawable scales from that to the window's
   backing size; the offscreen oracle target is w*scale x h*scale. */
CMContext *cm_open(int w, int h, const CMPixelDesc *fmt, const char *title);
void       cm_close(CMContext *);

/* Copy a chunky sub-rect from the AROS-owned framebuffer into the GPU texture.
   src points at the top-left of the WHOLE framebuffer; srcStride = bytes/row. */
void       cm_upload_rect(CMContext *, const void *src, int srcStride,
                          int x, int y, int w, int h);

/* Render the framebuffer texture to the offscreen oracle target via the FIXED
   pass-through/nearest pipeline, then PRESENT it to the live drawable with a
   render pass (color attachment, NOT a blit copy — see "Present" below) applying
   the selected effect. The offscreen target is the source of truth for readback. */
void       cm_present(CMContext *);

/* Select the present-time effect (default CM_FX_NEAREST). Presentation-only;
   never affects the oracle / cm_readback. 0 on success, nonzero if out of range. */
int        cm_set_effect(CMContext *, CMEffect effect);

/* Drain pending NSEvents (must run on the host main thread — see Threading).
   Returns count written to out[0..maxEvents-1]. */
int        cm_pump_events(CMContext *, CMEvent *out, int maxEvents);

/* Copy the last-presented offscreen target into dst as BGRA8 (logical w*h, no scale).
   The unattended oracle — independent of the on-screen window. Returns 0 on success. */
int        cm_readback(CMContext *, void *dst, int dstStride, int w, int h);

/* Introspection oracle: offscreen-target / drawable dims (== logical * scale). */
int        cm_target_size(CMContext *, int *outW, int *outH, int *outScale);

/* [D]-check hook: render the framebuffer through `effect` into a throwaway
   offscreen target and read it back, either at the logical grid (w,h — scale-
   downsampled) or the native target grid (tw,th from cm_target_size — identity,
   so a test can inspect real per-target-row effects like scanlines at any
   scale). Proves the effect pipeline runs; never touches the live oracle. */
int        cm_render_effect_readback(CMContext *, CMEffect effect,
                                     void *dst, int dstStride, int w, int h);

/* ---- Settings & options (v2, append-only — see "Settings & options" below) --
   #define CM_ABI_VERSION 2 (was 1). */
typedef enum { CM_SCALE_FIT=0, CM_SCALE_INTEGER_NEAREST, CM_SCALE_PIXEL_PERFECT,
               CM_SCALE_ASPECT_FIT,  /* =3, appended at [LIVE]; DEFAULT: aspect-preserving
                                        fill + BLACK letterbox (no distortion, no white) */
               CM_SCALE__COUNT } CMScaleMode;
typedef enum { CM_FILTER_NEAREST=0, CM_FILTER_LINEAR, CM_FILTER__COUNT } CMFilter;
typedef enum {                       /* numbered with gaps so each group grows alone */
    CM_OPT_EFFECT=0x00, CM_OPT_SCALE_MODE=0x01, CM_OPT_FULLSCREEN=0x02,  /* host-acted */
    CM_OPT_FILTER=0x03,
    CM_OPT_REQUEST_MODE_W=0x10, CM_OPT_REQUEST_MODE_H=0x11,              /* AROS-facing */
    CM_OPT_KEYMAP=0x12, CM_OPT_AUDIO_VOLUME=0x13                         /* (stubbed)   */
} CMOption;

/* HOST-ACTED keys (effect/scale/fullscreen/filter) take effect on the LIVE present
   only — never the offscreen oracle (§ readback). AROS-FACING keys are NOT acted
   on: the shim records the value and enqueues a CM_EV_SETTING for the AROS side to
   pull (no callback into AROS). 0 on success, nonzero for unknown key/bad value. */
int        cm_set_option(CMContext *, int key, long value);
int        cm_get_option(CMContext *, int key, long *value);

/* Open the native AppKit settings panel (best-effort, display-server task, manual
   CFRunLoop; idempotent; no-op nonzero when headless). Non-blocking. */
int        cm_open_settings(CMContext *);
```

## Threading model (the load-bearing constraint)

`[PUB]` **AppKit rule:** `NSApplication`/`NSWindow`/`NSView`/`NSEvent` must be used on
the **main thread**, with a running run loop. `[OURS]` AROS models the macOS main thread
as the low-priority boot/anchor task (H4, NOTES.md). `[OURS]` the hosted scheduler runs
on a **single underlying thread** (H6), so there is no true host-thread parallelism to
coordinate — only run-loop servicing.

**Requirement R-THREAD.** Confine **all** Cocoa + Metal calls to a single AROS task —
the **"display-server" task** — which is the one anchored to the host main thread. That
task: creates the window (`cm_open`), pumps events (`cm_pump_events`), and performs
presents (`cm_present`). Any other AROS task that needs a window operation posts a
request to the display-server task (an AROS message port) and signals it; it never calls
the shim directly. Justification is independent of any GPL reference: it follows directly
from the AppKit main-thread rule `[PUB]` + the single-thread scheduler `[OURS]`, and it
matches the **in-tree** hosted precedent — the iOS UIKit HIDD runs a VBlank-signalled
AROS task that services `CFRunLoopRunInMode(...)` under `HostLib_Lock`
(`arch/all-ios/hidd/uikit/eventtask.c`) `[AROS]`. The shim must call
`CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true)` (non-blocking drain) inside
`cm_pump_events` so AppKit delivers queued events without the shim owning the run loop.

**Requirement R-LOCK.** Every shim entry is wrapped by the caller in `HostLib_Lock` /
`HostLib_Unlock` (the H3 boundary) `[AROS]`, so even the boot task touching libc and the
display-server task touching Cocoa are serialised.

Because exactly one AROS task ever touches the shim, the shim itself needs **no** internal
locking beyond what AppKit/Metal require for their own objects.

**RESOLVED at D2t (`[D2t]` green — `hosted/cocoametal/d2t_test.m`, this Mac: M5 /
macOS 26.5).** Driving `cm_open` + `cm_present` + `cm_pump_events` from the **main
pthread** under hand-pumped `CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true)` —
with **NO** `NSApplicationMain` and **NO** `[NSApp run]` — works end to end:

- **Minimal AppKit init the display-server / boot task does ONCE, before the first
  `cm_open`:** `[NSApplication sharedApplication]` then
  `[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular]`. This is
  *initialization only* — it does **not** start a run loop (`[NSApp isRunning]`
  stays 0). No `finishLaunching`, no `run`, no `NSApplicationMain`. (The shim's
  `cm_try_window` performs this same pair idempotently as a safety net.)
- **`cm_open` creates the `NSWindow` + `CAMetalLayer`** under this model; backing
  scale is reported correctly (2× here).
- **`cm_present`'s `[layer nextDrawable]` yields a live `CAMetalDrawable`** every
  call (3/3 at 320×200 and 640×512) — the hand-pumped run loop is sufficient; the
  app run loop is **not** required for `nextDrawable`. This answers R-THREAD's open
  risk: no `[NSApp run]` is needed for presenting.
- The **offscreen oracle (`cm_readback`) is independent of all of the above** and
  passes whether or not a window/drawable exists, so a headless launch context
  (no window server → `scale=1`, present pass skipped) still satisfies the contract.

## Metal pipeline (host shim internals) — `[PUB]` Apple, `[REF-CONFIRM]` vAmiga

All from Apple's Metal/QuartzCore documentation; vAmiga only confirmed this exact
sequence ships on M-series. Implement from the Apple contracts:

1. **Device + queue.** `MTLCreateSystemDefaultDevice()` once; one `MTLCommandQueue`.
2. **Layer.** Attach a `CAMetalLayer` to the window's content `NSView`
   (`view.wantsLayer = YES; view.layer = caMetalLayer`). Set `layer.device`,
   `layer.pixelFormat = MTLPixelFormatBGRA8Unorm`, `layer.framebufferOnly = YES`
   (we present *to* the drawable, never read *from* it, and never **copy** into it —
   readback uses the offscreen target instead). `framebufferOnly = YES` therefore
   **requires** the present to be a render pass, not a blit copy — see step 5c.
3. **Framebuffer texture.** One persistent `MTLTexture`, `w×h`, `BGRA8Unorm`,
   `storageModeShared` (Apple-silicon unified memory — CPU writes, GPU reads, no copy)
   `[PUB]`. `cm_upload_rect` writes the sub-rect with
   `replaceRegion:mipmapLevel:withBytes:bytesPerRow:` for the spike; a staging-buffer +
   `MTLBlitCommandEncoder` upload is a later optimisation (note, don't build yet).
4. **Offscreen target.** One `MTLTexture`, `(w*scale)×(h*scale)`, `BGRA8Unorm`,
   `renderTargetUsage | shaderRead`, `storageModeShared` so `cm_readback` can read it on
   the CPU. **This is the source of truth for verification.**
5. **Present (`cm_present`).** Per frame:
   a. **Oracle pass.** Encode a render pass into the offscreen target that draws a
      full-screen triangle sampling the framebuffer texture (a 3-vertex pass, no vertex
      buffer needed — emit positions/UVs from `vertex_id` in the shader). Sampler:
      **nearest**, clamp — integer-crisp Amiga pixels, no blur `[PUB]` (design decision).
      This pass is **always** the fixed pass-through pipeline — it is the verification
      source of truth, so the selected effect must never touch it. `commit` +
      `waitUntilCompleted` so `cm_readback` sees the finished frame.
   b. **Present pass.** Acquire `id<CAMetalDrawable> d = [layer nextDrawable]`; if nil,
      skip this frame. Encode a **second render pass** with `d.texture` as the color
      attachment that draws the same full-screen triangle, sampling the framebuffer
      texture and applying the **selected effect** (`cm_set_effect`). Then
      `[cmdbuf presentDrawable:d]; [cmdbuf commit];`.
   c. **Why a render pass, not a blit `[PUB]`.** `layer.framebufferOnly = YES` (step 2)
      means the drawable may be used **only** as a render-target attachment — a
      `MTLBlitCommandEncoder` copy *into* `d.texture` needs the drawable to be a copy
      destination, which `framebufferOnly = YES` forbids. Drawing the framebuffer into
      the drawable as a **color attachment** is the correct, fast path and keeps
      `framebufferOnly = YES` (do NOT flip it to `NO`). The present pass scales the
      logical framebuffer to `d.texture.width × d.texture.height` via normalized UVs, so
      any drawable size — integer or fractional window scale — upscales correctly.
6. **Shaders.** Three tiny MSL functions, compiled from an embedded source string at
   `cm_open`: a shared fullscreen-triangle **vertex** (`vertex_id` → clip-space + UV); a
   pass-through **fragment** (`fs_sample`, used by the oracle pass and `CM_FX_NEAREST`);
   and an effect **fragment** (`fs_effect`) that takes a small constant buffer with the
   effect selector and branches — `CM_FX_NEAREST` = the same pass-through, `CM_FX_SCANLINE`
   = darken odd target rows (`uint(in.pos.y) & 1`) + a `pow(c, 1/1.25)` gamma. New effects
   append a branch + an enum value. Author from scratch — none lifted from any reference.

**HiDPI / arbitrary scale** `[PUB]`. `layer.contentsScale = window.backingScaleFactor`
(the **true**, possibly fractional, backing factor); `layer.drawableSize =
view.convertRectToBacking(view.bounds).size` — the view's actual backing-pixel size, NOT
a hard-coded `w*scale`, so a resized or fractional-scale window still maps correctly. AROS
always sees the **logical** `w×h`; the present pass (step 5b) upscales to the drawable's
pixel size via normalized UVs with nearest filtering, so **any** integer or fractional
window scale works. The **offscreen oracle target** is sized `w*S × h*S` for an **integer**
oracle scale `S` (rounded backing factor, ≥1) — integer keeps the readback block-exact and
deterministic; the live drawable is decoupled from `S` and uses the fractional drawableSize.
Headless (no window server) → `S = 1`. `cm_target_size` reports `S` and the oracle dims.

> **The live drawable MUST track the content view (`[LIVE]`).** `layer.drawableSize`
> is **not** a one-time value: the `CAMetalLayer` is the content view's **backing
> layer** and the view's geometry hooks (`-layout` / `-setFrameSize:` /
> `-viewDidChangeBackingProperties`), the `windowDidEnterFullScreen:`/`…ExitFullScreen:`
> delegate callbacks, and a resync in `cm_present` (just before `nextDrawable`) all
> recompute `contentsScale` + `drawableSize` from the view's **current** backing size.
> A user-reported bug (2026-06-25) was a drawable frozen at the windowed size across a
> fullscreen enter → the framebuffer rendered into a small corner ("small white rect in
> a black fullscreen window"). The **offscreen oracle was blind** (it is never resized);
> the **live-drawable readback** (`make cocoametal-livedraw` → `[LIVE] PASS`) catches it.
> The default present mode is `CM_SCALE_ASPECT_FIT`: aspect-preserving fill with a
> **black** letterbox; window + content-view backgrounds are black so **no white shows**.

## Pixel format mapping — `[AROS]` + `[PUB]`

Pick the AROS `StdPixFmt` that equals `MTLPixelFormatBGRA8Unorm` byte-for-byte so neither
side swizzles: on little-endian AArch64 that is the AROS pixfmt with
`blue` at byte 0, `green` 1, `red` 2, `alpha` 3. Register that exact format in the
gfx-class `aHidd_Gfx_PixFmtTags` (ColorModel = `vHidd_ColorModel_TrueColor`,
`BytesPerPixel=4`, `Depth=24`, `BitsPerPixel=32`, the matching `*Shift`/`*Mask`,
`BitMapType = vHidd_BitMapType_Chunky`) — attribute names per
`rom/hidds/gfx/gfx.conf:564–583` `[AROS]`. Cross-check the mechanism (not the values)
against the SDL driver's format emit in
`arch/all-hosted/hidd/sdl/sdlgfx_hiddclass.c` (`SDLGfx__Root__New`) `[AROS]`. Fill the
`CMPixelDesc` passed to `cm_open` from the same constants so both sides agree.

The **exact `CMPixelDesc` field values are now pinned** in INTERFACE.md §2a
(`bytesPerPixel=4`; `blueShift/greenShift/redShift/alphaShift = 0/8/16/24`;
`blueMask/greenMask/redMask/alphaMask = 0x000000FF/0x0000FF00/0x00FF0000/0xFF000000`;
top-left origin; logical `W×H`) and the host side **asserts** that round-trip in the
`[HIDDSIM]` harness (a known B/G/R/A pixel survives upload→present→readback with no
swizzle). The AROS-side action: advertise the TrueColor `vHidd_StdPixFmt_*` whose
**in-memory byte order is B, G, R, A** (blue at the lowest address) — the AROS side
confirms the exact constant against `rom/hidds/gfx/include/gfx.h`.

## AROS HIDD binding — `[AROS]`, contract from [design.md](design.md)

- **Gfx-class `New`.** Build the three init-only taglists — `aHidd_Gfx_PixFmtTags` (above),
  `aHidd_Gfx_SyncTags` (one mode: `HDisp=w`, `VDisp=h`, plus `HTotal/VTotal/PixelClock/
  Description`; `gfx.conf:649–677`), `aHidd_Gfx_ModeTags` — pass to the superclass, then
  start the display-server task and `cm_open(w,h,&fmt,title)`.
- **`moHidd_Gfx_CreateObject`.** For a displayable bitmap, hand the base class our bitmap
  class via `aHidd_BitMap_ClassPtr` (mandatory per `gfx_hiddclass.c:1144`).
- **BitMap class.** Backing store = a chunky `w*h*4` buffer in AROS memory (the
  framebuffer). `moHidd_BitMap_PutPixel`/`GetPixel` write/read that buffer (the only
  mandatory methods; `gfx_bitmapclass.c:1392`). `PutImage`/`FillRect`/`Clear` may use the
  base fallbacks initially (they decompose to PutPixel) — flag for a later direct-blit
  optimisation.
- **`moHidd_BitMap_UpdateRect(x,y,w,h)`** — the present hook (`gfx_bitmapclass.c:5057`).
  Gate on the visible flag, then request the display-server task to
  `cm_upload_rect(fb, stride, x,y,w,h)` and `cm_present()`. Batch coalescing of rapid
  UpdateRects is a later optimisation.
- **`moHidd_Gfx_Show`.** Select the displayable bitmap as the front buffer; request a
  full-frame `cm_present`.

## Input — `[PUB]` + `[AROS]` (cm_pump_events now REAL; D4/D5 green)

`cm_pump_events` is **implemented** (no longer a stub) in `cocoametal_window.m`
(`cm__pump_events_appkit`, the strong override of the weak stub in `cocoametal.m` —
the same weak/strong split as `cm_try_window`, so the AppKit-free translation unit
pulls no AppKit headers). It runs on the single AROS/main thread (§3 single caller —
the only caller), drains pending `NSEvent`s **NON-BLOCKING** in a loop bounded by
`maxEvents`:

```objc
[NSApp nextEventMatchingMask:NSEventMaskAny
                   untilDate:[NSDate distantPast]   /* => returns immediately if empty */
                      inMode:NSDefaultRunLoopMode
                     dequeue:YES]
```

Translation (all coords **logical, top-left**, per §2; `mods` map below):
- `MouseMoved`/`{Left,Right,Other}MouseDragged` → `CM_EV_MOUSEMOVE`.
- `{Left,Right,Other}MouseDown/Up` → `CM_EV_MOUSEBTN`, `code` 0=left/1=right/2=middle,
  `pressed` 1/0.
- Coords: `locationInWindow` is **points = logical**, **bottom-left** origin; convert to
  top-left logical with `x = locationInWindow.x`, `y = contentHeightPoints −
  locationInWindow.y` (content height in points == logical H, §2 — no `/scale`: the
  `contentsScale` lives below the layer), then clamp to `[0,w)×[0,h)`.
- `KeyDown`/`KeyUp` → `CM_EV_KEY`, `code = event.keyCode` (macOS virtual keycode),
  `pressed` 1/0, `mods` from `event.modifierFlags`.
- `FlagsChanged` → `CM_EV_KEY` for the modifier transition: `code = keyCode`, `pressed`
  derived from whether that modifier's flag is now set in `modifierFlags`.
- `mods`: `NSEventModifierFlag{Shift,Control,Option,Command}` →
  `CM_MOD_{SHIFT,CONTROL,ALT,CMD}`.
- Window **close button** → `CM_EV_CLOSE`; live **resize** → `CM_EV_RESIZE` (best-effort).
  These are delegate callbacks / notifications, NOT plain dequeuable `NSEvent`s, so a tiny
  `NSWindowDelegate` (`windowShouldClose:` returns NO — report, don't tear down; AROS owns
  the lifecycle — and `windowDidResize:`) sets one-shot flags on the context that the pump
  drains as `CM_EV_CLOSE`/`CM_EV_RESIZE`.
- Events the shim does NOT translate (window drag, system/app-defined, etc.) are forwarded
  to `[NSApp sendEvent:]` so the window still behaves, then draining continues — untranslated
  events never accumulate. (Translated `KeyDown`/`KeyUp` are NOT forwarded — AROS owns the
  keyboard and AppKit would beep on an unhandled key with no responder.)

AROS side feeds these into the input subsystem the way the hosted `sdl`/`x11` HIDDs post
mouse/keyboard events to `input.device` — **UNVERIFIED** exact class/path; resolve
against `arch/all-hosted/hidd/sdl/` at build `[AROS]`. The **macOS-virtual-keycode →
AROS rawkey** table is committed (`keycode2rawkey.table`; left = `CMEvent.code`); specific
entries as noted in INTERFACE.md §5. D4/D5 are GREEN; D1–D3 are display-only.

## Verification (unattended — `[OURS]` H7 discipline)

The live `NSWindow` cannot be screen-captured without TCC, so the window is **never** the
judge — the offscreen Metal target is. `cm_readback` copies it to a BGRA8 buffer; AROS
encodes a PNG via the H7 ImageIO path (`hosted/display.c`) and pixel-asserts. Every spike
asserts **values**, never "it didn't crash":

- **D1 (shim alone).** Host test: `cm_open(320,200,…)`; `cm_upload_rect` a 4-quadrant
  colour pattern + a known marker pixel; `cm_present`; `cm_readback` (the **pass-through
  oracle**) → assert the four quadrant colours are exact and `target == 320*S × 200*S`.
  **PASS** = pixels match. Marker `[D1]`. The oracle path is fixed/nearest; the effect
  stage is NOT in it.
- **D2-res (resolution-parametric).** The same host test at a **second** resolution
  (`cm_open(640,512,…)`) — proves `cm_open` is not hard-coded to 320×200 and the present
  path scales arbitrary `W×H`. Marker `[D2]`. (Distinct from D2-through-AROS below; the
  spike file runs the resolution check, the AROS bring-up runs the HIDD-path check.)
- **D-shader (shader stage).** Upload the scene; `cm_render_effect_readback` it through
  `CM_FX_NEAREST` and `CM_FX_SCANLINE` (the latter read at the **native** target grid so
  per-target-row parity is visible at any scale). Assert: (0) `CM_FX_NEAREST` ==
  the offscreen oracle byte-for-byte (the verification path is untouched by the shader
  stage); (1) `CM_FX_SCANLINE` differs from pass-through; (2) the difference has the
  expected shape — odd target rows darker than the even rows above. Marker `[D]`. Proves
  the selectable-effect hook actually runs without putting it in the oracle path.
- **D2t (threading model — the de-risk).** Host test (`hosted/cocoametal/d2t_test.m`,
  `make cocoametal-d2t`): drives `cm_open` + `cm_present` + `cm_pump_events` from the
  **main pthread** under hand-pumped `CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0,
  true)` — **no** `NSApplicationMain`, **no** `[NSApp run]`. Asserts the §6 oracle is
  exact at 320×200 + 640×512 under this model, and **reports** whether window +
  `nextDrawable` succeed (they do — see the Threading-model RESOLVED note above).
  Marker `[D2t]`. This is the live-window/run-loop risk retired before D3.
- **D2 (through AROS).** AROS bitmap class: `PutImage` the pattern, `UpdateRect` it,
  `cm_readback` → identical assert. Proves the HIDD→shim path. `[D2]`.
- **D3 (partial update).** Change one sub-rect; assert only that rect changed and the rest
  is byte-identical to the prior frame. `[D3]`.
- **D3 host-support (HIDD-shaped harness) — GREEN.** Host test
  `hosted/cocoametal/hiddsim_test.c` (`make cocoametal-hiddsim`, marker
  `[HIDDSIM] PASS`) — the de-risk + behavioral reference for the AROS bitmap-class
  UpdateRect wiring. Plain C, links **none** of the `.m` files; it `dlopen`s
  `build/cocoametal.dylib` (the REAL boundary) and drives it the way the HIDD will,
  beyond `abi_test`'s single sequence: AROS owns a host-side `W*H*4` BGRA8
  framebuffer (the `AllocMem` stand-in) filled with the pinned §2a `CMPixelDesc`;
  **lazy `cm_open` on the first "Show"**; a **dirty-rect stream** of partial +
  *overlapping* `cm_upload_rect`(only the changed sub-rect)`+cm_present` — the
  many-small-UpdateRects pattern, never a full-frame blit — pumping `cm_pump_events`
  between presents. After the stream, `cm_readback` asserts the composed offscreen
  oracle is **byte-exact** against an *independent* host reference framebuffer
  (320×200 BGRA8, 256000 B, last-writer-wins on the overlaps, no stale pixels) —
  the §6 oracle under realistic usage. A final known-pixel (B=0x11,G=0x22,R=0x33,
  A=0xFF) upload+present+readback proves each channel lands in its asserted byte
  position, so a **swizzle** bug is caught at the host boundary where the AROS side
  can't easily see it. Bounded + watchdog. Additive — no ABI change. This is the
  host-side reference the AROS-side `[D2]`/`[D3]` path verifies against unchanged
  (§6). See INTERFACE.md §2a for the pinned desc + call-mapping table.
- **D4/D5 (input) — GREEN.** Host test `hosted/cocoametal/input_test.m`
  (`make cocoametal-input`, marker `[D4D5] PASS`, gates on D4 **and** D5). Under the
  SAME main-pthread / manual-`CFRunLoopRunInMode` / no-`NSApplicationMain` model as D2t,
  it **synthesises** `NSEvent`s with the public `+[NSEvent mouseEventWithType:…]` /
  `+[NSEvent keyEventWithType:…]` constructors and **injects them IN-PROCESS** with
  `[NSApp postEvent:ev atStart:NO]` — which pushes onto `NSApp`'s own queue, exactly what
  `cm_pump_events` drains via `nextEventMatchingMask:…dequeue:YES`. This is a pure
  in-process round trip through the app event queue and needs **NO TCC / accessibility**
  (unlike `CGEventPost`, which would prompt — that path is therefore unused).
  - `[D4]` mouse: post a mouse-move to a known logical point + LMB down/up; assert
    `CM_EV_MOUSEMOVE` with **exact** logical `x,y` (Y-flip correct) and `CM_EV_MOUSEBTN`
    `code=0` `pressed` 1 then 0.
  - `[D5]` keyboard: post `keyDown`+`keyUp` with a known `keyCode` and Shift held; assert
    `CM_EV_KEY` `code==keyCode`, `pressed` 1/0, `mods & CM_MOD_SHIFT`.
  - **Injection caveat (measured, documented in the test):** `[NSApp postEvent:]` does NOT
    preserve a posted `locationInWindow` bit-exactly — on a Retina (2×) display the event
    is stored in backing pixels and re-resolved through the window's on-screen backing
    geometry at dequeue, applying a small **stable affine** `got = a·posted + b`
    (≈1.019·p−3 on this Mac; lossless `convertPointToScreen` confirms the loss is the
    queue's pixel-snapping, not the shim's math). A genuine hardware mouse event carries an
    already window-local, exact location and suffers none of this. The test therefore
    **calibrates** that affine at runtime (two NSEvent-level probes) and **pre-inverts** the
    posted point so the location the shim receives lands on the integer target — then the
    shim's Y-flip + clamp are asserted **exactly**. Key/button `code`/`pressed`/`mods` round-
    trip exactly with no such correction. The window is made key+front before injection.
- **SET (settings & options) — GREEN.** Host test `hosted/cocoametal/settings_test.m`
  (`make cocoametal-settings`, marker `[SET] PASS`), under the same D2t threading model.
  Asserts: (1) **host-acted** `cm_set_option(CM_OPT_EFFECT, CM_FX_SCANLINE)` → `cm_present`
  makes the **presented** path reflect the effect (odd target rows darker, via
  `cm_render_effect_readback`) while the **offscreen oracle** (`cm_readback`) stays
  byte-for-byte pass-through (the contract — host options never touch the oracle), and
  `cm_get_option` reflects it; (2) **AROS-facing** `cm_set_option(CM_OPT_REQUEST_MODE_W=640,
  _H=512)` is NOT acted on (`cm_target_size` unchanged) and instead surfaces as two
  `CM_EV_SETTING` events from `cm_pump_events` (pull-only); (3) **NSUserDefaults**
  persistence round-trips (seed defaults → fresh `cm_open` re-applies them → restored).
  See "Settings & options" below.
- **LIVE (the present FILLS the drawable) — GREEN.** Host test
  `hosted/cocoametal/livedraw_test.m` (`make cocoametal-livedraw`, marker `[LIVE] PASS`),
  under the D2t threading model. The **live-drawable readback** the offscreen oracle was
  blind to: sets the live layer `framebufferOnly=NO`, composes the 4-quadrant+marker scene
  into the live drawable via the real present pass, **reads the live drawable back**, and
  asserts — **windowed AND after entering fullscreen** — drawable == content-view backing
  size, the scene fills the aspect-fit content rect at full size, and the letterbox is
  **black, never white**. Fixes a user-reported fullscreen bug (small white rect): the
  `CAMetalLayer` now tracks the content view (backing layer + geometry hooks + fullscreen
  delegate + `cm_present` resync). Plus `make cocoametal-show` — a persistent human-facing
  "look at it" build (bounded, auto-exits). Host-only; `CM_SCALE_ASPECT_FIT` appended
  (append-only, no `cm_*` symbol moved).

## Settings & options (v2 — host/AROS ownership split)

A native host **settings panel** + a key/value option ABI (`cm_set_option` /
`cm_get_option` / `cm_open_settings`). **Host owns presentation** — effect, scale mode
(`CM_SCALE_ASPECT_FIT` (**default** — aspect-preserving fill + black letterbox; the
`[LIVE]` fix) / `FIT` / `INTEGER_NEAREST` / `PIXEL_PERFECT`), fullscreen, filter
(`CM_FILTER_NEAREST`/`LINEAR`) — and applies those to the **live present only** (never
the offscreen oracle, so `cm_readback` is unaffected). **AROS owns everything
functional** — mode/keymap/volume; the panel can only *request* those, and the shim
surfaces the request as a `CM_EV_SETTING` (packing: `code`=key, `x`=value, `y`=2nd value
e.g. paired mode height+width) for the AROS side to **pull** via `cm_pump_events` — the
shim never calls into AROS (matches the threading model). The panel
(`hosted/cocoametal/cocoametal_settings.m`) wires the host controls to `cm_set_option`,
**persists host-owned state via `NSUserDefaults`** (loaded+applied at `cm_open`, saved on
change), and routes its AROS-owned resolution picker through the `CM_EV_SETTING` enqueue.
It lives on the display-server / main thread under the manual `CFRunLoop` (D2t model),
degrading to a no-op headless. `CM_OPT_FULLSCREEN` is now **REAL** native AppKit
fullscreen — `cm_set_option(CM_OPT_FULLSCREEN,1)` issues `-[NSWindow toggleFullScreen:]`
(enter), `0` exits, while still recording/persisting the flag; the request is async +
non-blocking (issues the toggle and returns). Verified `make cocoametal-fullscreen` →
**`[FS] PASS`** (entered/exited asserted via `styleMask & NSWindowStyleMaskFullScreen`,
not a screencapture; the §6 oracle byte-exact across the transition). **Hand-pumped-
transition finding (the graft must honor):** ENTER completes under the hand-pumped
`CFRunLoopRunInMode(...,0,true)` model (the `styleMask` bit flips at the toggle), but
the fullscreen **EXIT** state machine needs the **app run loop** to advance — bare
hand-pumping never clears it, so the display-server task must spin a *bounded* `[NSApp
run]` for the exit transition (or accept it lands on a later cycle). Frozen detail +
the full finding in INTERFACE.md §9 / §9f.

## Build / integration

- Shim links `AppKit, Metal, QuartzCore, CoreGraphics` (+ `ImageIO` only in the D1
  test, for PNG output); built from `cocoametal.m` + `cocoametal_window.m` +
  `cocoametal_settings.m` as **`build/cocoametal.dylib`** via
  **`make cocoametal-dylib`** (`-dynamiclib`, `-fobjc-arc`, `-arch arm64`). The **13**
  frozen `cm_*` symbols (10 at v1 + `cm_set_option`/`cm_get_option`/`cm_open_settings`
  at v2) are exported with **default visibility** through an explicit
  `-exported_symbols_list hosted/cocoametal/cocoametal.exports` (so the dynamic
  export table is exactly the contract; the internal `cm__*` accessors stay
  link-visible but unexported), the dylib is **NOT stripped** (`dlsym` resolves by
  name), and it is **ad-hoc codesigned** (`codesign -s -`) — which on this Mac
  (M5 / macOS 26.5) is sufficient for a hosted process to `dlopen` it (RESOLVED;
  no hardened-runtime entitlement needed for a plain `dlopen`, unlike the J1 MAP_JIT
  path). Test `.m` files are **not** in the dylib. The dylib pulls **no** AROS headers.
- **`make cocoametal-abi`** builds + runs `hosted/cocoametal/abi_test.c` — a plain-C
  test that `dlopen`s `build/cocoametal.dylib` the exact way `HostLib_Open` does,
  `dlsym`s every name in the frozen `cocoametal_symbols[]` array into an interface
  struct (mirroring `HostLib_GetInterface`; **`errcount` must be 0** — now **13**
  names), asserts `cm_abi_version() == CM_ABI_VERSION` (**2**), then drives
  open→upload→present→readback(asserts the §6 oracle)→pump→set_effect/target_size→
  set/get_option/open_settings→close through the resolved function pointers.
  `[ABI] PASS` = the seam wires. It links **none** of the `.m` files.
- **`make cocoametal-settings`** builds + runs the panel/option test
  (`hosted/cocoametal/cocoametal_settings.m` + `settings_test.m`) → `[SET] PASS`
  (see Verification → SET above).
- **`make cocoametal-hiddsim`** builds + runs the D3 host-support harness
  (`hosted/cocoametal/hiddsim_test.c`, plain C, **none** of the `.m` files) → it
  `dlopen`s `build/cocoametal.dylib` (the real boundary) and drives the HIDD-shaped
  dirty-rect stream + compose/swizzle asserts → `[HIDDSIM] PASS` (see Verification →
  D3 host-support above). The behavioral reference for the AROS UpdateRect wiring;
  additive, no ABI change.
- The C ABI header is shared source, hand-written, no GPL provenance.
- The shim must not link or include AROS headers; the AROS side must not include Cocoa
  headers. The C ABI is the only contact surface.

## Open questions / UNVERIFIED

- Exact AROS-side input posting path for a hosted HIDD (mirror `sdl`/`x11`).
- ~~Whether `nextDrawable` starvation under AROS scheduling needs an explicit frame
  cadence / `maximumDrawableCount` tuning.~~ **Partly RESOLVED at D2t:** under the
  real main-pthread/manual-CFRunLoop model `nextDrawable` yielded a drawable on every
  present (3/3 at two resolutions) with no tuning. Long-run starvation under the full
  AROS VBlank cadence is still worth watching at D3, but the basic hand-pumped path
  is proven not to starve `[OURS]`.
- ~~Codesign/entitlements for a `dlopen`'d Metal dylib in the hosted process.~~
  **RESOLVED:** a plain **ad-hoc** signature (`codesign -s -`) on
  `build/cocoametal.dylib` is sufficient for a hosted process to `dlopen` it on this
  Mac (M5 / macOS 26.5) — `[ABI] PASS`. No hardened-runtime entitlement is needed for
  `dlopen` (contrast the J1 MAP_JIT path, which does need `allow-jit` under the
  hardened runtime) `[OURS]`.
- ~~Unattended input injection without TCC (synthesised `NSEvent` vs. queue injection).~~
  **RESOLVED at D4/D5:** synthesise with `+[NSEvent mouse/keyEventWithType:…]` and inject
  in-process with `[NSApp postEvent:atStart:]` — this rides `NSApp`'s own event queue that
  `cm_pump_events` already drains, so it needs **NO TCC/accessibility** (no `CGEventPost`).
  `make cocoametal-input` → `[D4D5] PASS` (mouse + keyboard CMEvents asserted field-exact).
  One measured caveat retained in the test: posted mouse `locationInWindow` is not bit-exact
  on a 2× Retina display (a stable affine the test calibrates+inverts; genuine hardware
  events are exact) — see the D4/D5 verification bullet.
- ~~Whether to keep `framebufferOnly = YES` + offscreen-target readback~~ **RESOLVED.**
  Keep `framebufferOnly = YES`. The present is a **render pass** drawing the framebuffer
  into the drawable as a color attachment (step 5b/5c), which is valid with
  `framebufferOnly = YES`; the earlier blit-copy into the drawable was the bug
  (FINDING 6 — a copy destination needs `framebufferOnly = NO`). Readback stays on the
  offscreen target, so the live path is fast and the oracle deterministic.

## Spike status vs production contract

The spike (`hosted/cocoametal/`) proves the pipeline; a few things are deliberately
split between "the contract verification depends on" and "presentation-only sugar":

- **Oracle path is fixed / nearest.** `cm_readback` always reads the offscreen target,
  which is **always** rendered by the pass-through/nearest pipeline. `[D1]`/`[D2]` are
  deterministic pixel-asserts against it. Nothing in the effect stage can change a PASS.
- **Effects are presentation-only.** `cm_set_effect` / `CM_FX_*` change only how the live
  drawable is drawn (the present render pass). They are never in the verification path;
  `[D]` proves `CM_FX_NEAREST` == the oracle byte-for-byte and that `CM_FX_SCANLINE` runs.
  Adding effects = append an enum value + a fragment-shader branch; no ABI break.
- **Resolution is a parameter, not a constant.** `cm_open(w,h,…)` takes arbitrary `W×H`;
  the offscreen target is `W*S × H*S` (integer oracle scale `S`), the live drawable is the
  window's real backing size (any integer/fractional scale). Proved at 320×200 and 640×512.
- **Live window is a non-fatal bonus.** Headless (no window server) still runs the oracle
  at `S = 1`; the present pass is skipped when `nextDrawable` is nil. The spike's `[D]`
  parity proof reads the **native** target grid so it holds at `S = 1` or `S = 2`. D2t
  additionally proves that *when* a window server is present, the window + `nextDrawable`
  come up under the real main-pthread/manual-CFRunLoop model (no `[NSApp run]`).
- **The dylib seam is the deliverable, and it is green.** `make cocoametal-dylib`
  emits `build/cocoametal.dylib` (10 `cm_*` exported, unstripped, ad-hoc signed, no
  `d1_test.m`, no AROS headers); `make cocoametal-abi` `dlopen`s + `dlsym`s it exactly
  as `HostLib_GetInterface` will (`errcount==0`), version-handshakes, and re-asserts
  the §6 oracle. This is the contract surface D3 (the AROS HIDD) binds against.
- **Not yet built (noted, not done):** staging-buffer/`MTLBlitCommandEncoder` upload
  optimisation; UpdateRect batch coalescing; the AROS-side HIDD binding (D2-through-AROS,
  D3 partial update, D4/D5 input). The shim ABI above is the stable contact surface for them.

## Provenance summary

`[PUB]` Apple AppKit/Metal/QuartzCore/CoreGraphics; macOS `kVK_*` keycodes. ·
`[AROS]` `rom/hidds/gfx/gfx.conf`, `gfx_hiddclass.c`, `gfx_bitmapclass.c`,
`arch/all-hosted/hidd/sdl/`, `arch/all-ios/hidd/uikit/eventtask.c`, `hostlib.resource`. ·
`[OURS]` `hosted/display.c` (H7 ImageIO + pixel-assert), the H3 host-call boundary, the
H4/H6 scheduler model. · `[REF-CONFIRM]` vAmiga `GUI/Metal/*` confirmed the present
sequence ships on M-series — restated here from Apple contracts only.
