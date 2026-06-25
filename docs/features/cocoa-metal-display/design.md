# Cocoa/Metal display HIDD — a live macOS window for AROS

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-24
> **Decision (confirmed):** Apple-native — AppKit + Metal, CoreGraphics for readback.
> **No** SDL / OpenGL / MoltenVK. Implementation spec: [spec.md](spec.md).
> **Frozen AROS↔host contract (build against this): [INTERFACE.md](INTERFACE.md)** —
> symbol list, load sequence, pixel/geometry/scale, threading, the D3 hand-off.

## What & why

H7 (`hosted/display.c`) presents the AROS framebuffer by encoding it to a PNG via
macOS ImageIO. That keeps the unattended loop intact but produces no on-screen
window — you can't *use* Wanderer through a PNG. This feature replaces the PNG
sink with a real **Cocoa `NSWindow`** whose contents are a GPU surface
(**`CAMetalLayer`**, or `CGBitmapContext` as the simple fallback) that AROS blits
its bitmap into. The result: the hosted AROS desktop shows up in a Mac window,
ideally retina/HiDPI with GPU-accelerated blits.

This is the natural next driver under the project thesis — *"macOS owns the
drivers; AROS reaches them via standard exec I/O"* (NOTES.md, Phase-2). The host
side does the real Cocoa/Metal work; AROS talks to it only through the standard
graphics HIDD / bitmap-class LVOs. Nothing about the AROS-facing contract changes
from what graphics.library already expects of the SDL/X11 drivers; only the host
backend is new.

The hard constraint this whole document is organised around: **a live on-screen
window cannot be verified unattended by screen-capture** (`screencapture` needs
macOS Screen-Recording / TCC approval — a manual click that breaks the
build→run→observe→verdict loop, exactly why H7 deferred the window — NOTES.md
"H7"). So the design must keep a *programmatic* pixel channel alongside the live
window. See "The unattended-verification tension".

## Does it already exist?

No. Verified by search over `/Users/user/Source/aros-upstream`:

- `grep -rliE "cocoa|cametallayer|nswindow|mtkview|metalkit"` returns **only**
  `arch/all-ios/hidd/uikit/uikit_hiddclass.c` and `.../eventtask.c` — the **iOS
  UIKit** HIDD, a different platform (Cocoa Touch, not AppKit), and not a
  hosted-darwin desktop driver.
- `grep -rlE "CAMetalLayer|MTLDevice|MTLCreateSystemDefaultDevice"` → **no hits**
  anywhere in the tree. There is no Metal code in AROS at all.
- The only `.m` Objective-C files are under `arch/all-ios/` (UIKit bootstrap +
  hidd) and two WirelessManager host drivers (`driver_osx.m`,
  `driver_iphone.m`) — none is a display HIDD for hosted macOS.
- The hosted display HIDDs present are exactly two:
  `/Users/user/Source/aros-upstream/arch/all-hosted/hidd/sdl/` and
  `/Users/user/Source/aros-upstream/arch/all-hosted/hidd/x11/`. Confirmed by
  `ls arch/all-hosted/hidd/` → `{sdl, x11}`.

So a native Cocoa/AppKit + Metal display HIDD is genuinely new. (The iOS UIKit
driver is the closest *Apple-family* precedent and we mine its shape below — but
note its `UpdateRect` present path is literally a `/* TODO */` stub:
`arch/all-ios/hidd/uikit/uikit_bitmapclass.c:191` — even Apple's in-tree HIDD
never finished the per-frame present, so we are not copying working code, only its
structure.)

### External prior art (web-grounded, *not* in the AROS tree)

The web confirms the gap — no native macOS-Cocoa AROS display HIDD exists
anywhere — and adds one strong design precedent and one historical correction:

- **AROS's own Darwin/macOS hosted port has always been X11-only.** The official
  ports page lists the i386/x86_64 *and* PPC Darwin hosted builds as *"requires an
  X11 server for the display"* (i.e. XQuartz) — there is no, and never was, a
  native Quartz/Cocoa AROS display backend
  (<https://aros.sourceforge.io/introduction/ports.html>). This *strengthens* the
  doc's claim: the in-tree `sdl`/`x11` HIDDs are the whole story, and on macOS the
  real-world AROS desktop is shown through X11. Native Cocoa/Metal is unbroken
  new ground for AROS. **Relevance:** confirms the gap. **Catch:** none — it just
  means there's no AROS code to crib the macOS-window path from.
- **SDL's *own* macOS video backend is Cocoa underneath** — `SDL_QuartzVideo.m`
  drives `NSWindow`/`NSView`/`CGContextRef`
  (<https://github.com/klange/SDL/blob/master/src/video/quartz/SDL_QuartzVideo.h>).
  So the in-tree SDL HIDD on macOS is *indirectly* a Cocoa window already; our
  driver collapses that indirection (AROS → host `libSDL` → Quartz becomes
  AROS → our Cocoa shim). **Catch:** that SDL1.2 Quartz backend uses APIs Apple
  deprecated in Lion and *removed* by High Sierra (`CGDirectPaletteRef`, etc.;
  <https://github.com/libsdl-org/SDL-1.2/issues/739>), i.e. it does not build for
  modern macOS at all — a concrete reason the "just use the SDL HIDD" alternative
  is a dead end here and writing Cocoa directly is warranted. (A community **SDL2**
  port to AROS exists — Kalamatee's — but it is a *guest* library inside AROS,
  still missing sound/OpenGL, **UNVERIFIED** as a hosted display HIDD:
  <https://www.arosworld.org/infusions/forum/viewthread.php?thread_id=1443>.)
- **vAmiga** — `github.com/dirkwhoffmann/vAmiga`, **GPL**, native Apple-Silicon
  macOS Amiga emulator. The closest *working* analogue to what we're building:
  it presents an emulated Amiga framebuffer in a Cocoa window via **Metal** —
  `GUI/Metal/{MetalView,Renderer,RendererSetup,Shaders,TextureToolbox}.swift`
  drive an `MTKView` + `MTLDevice` + `MTLCommandQueue`, present through a
  `drawable`, and bridge a C++/C emulation core to the Swift UI through an
  `ObjCProxy` layer
  (<https://github.com/dirkwhoffmann/vAmiga/tree/master/GUI/Metal>). This is the
  exact "guest pixel buffer → `MTLTexture` → drawable in an `NSWindow`" pipeline
  our D1 spike posits, proven shipping on M-series. **Relevance:** a real-world
  reference for the Metal present path and the C↔Cocoa bridge shape. **Catch:** it's
  Swift+GPL and a monolithic app, not a loadable HIDD — a *reference for the call
  sequence*, not code we can link; we still write the flat-C shim ourselves.
- **The in-tree iOS UIKit HIDD's author is Pavel "Sonic" Fedin**
  (<https://github.com/Sonic-Amiga>), who wrote AROS's Apple-platform and
  hostlib-based host-driver lineage. Confirms the `arch/all-ios` structure we mine
  is a deliberate Apple-family template, not an orphan — but (as noted above) its
  present path was never finished, so it's a skeleton, not a worked example.

## Background: the AROS display HIDD contract (grounded)

A display driver is an OOP subclass tree registered with graphics.library. The
authoritative interface definition — every method ID, attribute tag, interface-ID
string and stub macro — is one file:
**`/Users/user/Source/aros-upstream/rom/hidds/gfx/gfx.conf`** (genmodule generates
the `<interface/Hidd_*.h>` headers from it; the public umbrella is
`/Users/user/Source/aros-upstream/rom/hidds/gfx/include/gfx.h` = `<hidd/gfx.h>`).
Base-class implementations:
`/Users/user/Source/aros-upstream/rom/hidds/gfx/gfx_hiddclass.c` (graphics class)
and `.../gfx_bitmapclass.c` (bitmap class).

**Two classes to subclass:**

- **Graphics HIDD class** — `basename GFXHIDD`, `classid CLID_Hidd_Gfx`,
  `superclass CLID_Hidd`, `interfaceid hidd.gfx.driver` → `IID_Hidd_Gfx`,
  `methodstub HIDD_Gfx` (`gfx.conf:28–34, 330–334`). This is the driver root: it
  manufactures bitmaps and shows them.
- **BitMap class** — `basename BM`, `classid CLID_Hidd_BitMap`,
  `superclass CLID_Root`, `interfaceid hidd.gfx.bitmap` → `IID_Hidd_BitMap`,
  `methodstub HIDD_BM` (`gfx.conf:79–83, 399–403`). This is where pixels live and
  get presented. (`CLID_Hidd_* == IID_Hidd_*` string aliases are defined in
  `gfx.h`, e.g. `#define CLID_Hidd_Gfx IID_Hidd_Gfx` at `gfx.h:585`,
  `#define CLID_Hidd_BitMap IID_Hidd_BitMap` at `gfx.h:333`.)

**Graphics-class methods a driver overrides** (decl `gfx.conf:363–394`; base impl
`gfx_hiddclass.c`):

- **`moHidd_Gfx_CreateObject(cl, attrList)`** → `OOP_Object*` — the **bitmap
  factory** (there is *no* `NewBitMap` method; bitmaps come from CreateObject).
  For a displayable bitmap the driver must hand the base class its own bitmap
  class via **`aHidd_BitMap_ClassPtr`** (or `aHidd_BitMap_ClassID`). This is
  spelled out verbatim at `gfx_hiddclass.c:1144`:
  *"aHIDD_BitMap_ClassPtr or aHIDD_BitMap_ClassID must be provided to the base
  class for a displayable bitmap."* Impl: `GFXHIDD__Hidd_Gfx__CreateObject`
  (`gfx_hiddclass.c:1168`).
- **`moHidd_Gfx_Show(bitMap, flags)`** → `OOP_Object*` — put a displayable bitmap
  on screen (the mode-switch / front-buffer-select path). Impl
  `GFXHIDD__Hidd_Gfx__Show` (`gfx_hiddclass.c:2502`); base only handles the
  framebuffer-mirror model, so real drivers override it.
- **`moHidd_Gfx_CopyBox`**, **`moHidd_Gfx_GetMode`**, `moHidd_Gfx_NextModeID`,
  `moHidd_Gfx_QueryModeIDs`, optional `SetCursorShape/Pos/Visible`,
  `NominalDimensions`. C invocation macros are `HIDD_Gfx_*` (e.g.
  `HIDD_Gfx_GetMode`).

**BitMap-class methods** (decl `gfx.conf:435–498`; base impl `gfx_bitmapclass.c`;
stub `HIDD_BM_*`):

- **`moHidd_BitMap_PutPixel(x, y, pixel)`** / **`moHidd_BitMap_GetPixel(x, y)`** —
  the *only mandatory* methods; the base does **not** implement them
  (`gfx_bitmapclass.c:1392` comments `/* PutPixel must be implemented in a
  subclass */`) and builds every higher-level fallback on top of them.
- **`moHidd_BitMap_PutImage` / `GetImage`** (chunky-row blit; base impls
  `gfx_bitmapclass.c:2804 / 2649`), **`FillRect`** (`:1884`), **`Clear`**
  (`:2550`), `PutImageLUT/GetImageLUT`, `SetColors`, plus `*MemRect*`/`*MemBox*`
  software helpers.
- **`moHidd_BitMap_UpdateRect(x, y, width, height)`** — *the present hook.* Base
  impl `BM__Hidd_BitMap__UpdateRect` (`gfx_bitmapclass.c:5057`), contract doc at
  `:5007–5055`: the system calls it **after drawing ops** and **after
  `moHidd_Gfx_Show`** to flush a region to the display; a mirror-buffer driver
  copies the rectangle to VRAM/screen here. Also called on offscreen bitmaps, so
  gate on a visible flag (base gates on `data->visible`, `:5072`). **Our host
  present call hangs off this method.** (Not called when
  `moHidd_Gfx_ShowViewPorts` succeeds — no composition initially.)

**Mode / pixelformat registration** — the driver's gfx-class `New` builds three
init-only tag lists and passes them to the superclass:

- **`aHidd_Gfx_PixFmtTags`** — a `struct TagItem*` of `aHidd_PixFmt_*` attrs:
  `ColorModel` (`vHidd_ColorModel_TrueColor`), `RedShift/Green/Blue/AlphaShift`,
  `RedMask/…/AlphaMask`, `Depth`, `BitsPerPixel`, `BytesPerPixel`, `StdPixFmt`,
  `BitMapType` (`vHidd_BitMapType_Chunky`) (`gfx.conf:564–583`; runtime mirror
  `HIDDT_PixelFormat`, `gfx.h:301–323`).
- **`aHidd_Gfx_SyncTags`** — one `struct TagItem*` per resolution of `aHidd_Sync_*`
  attrs: `HDisp` (= width), `VDisp` (= height), `HTotal/VTotal`, `PixelClock`,
  `Description` (`gfx.conf:649–677`). Note **bytes-per-row is a *bitmap* attr**
  (`aHidd_BitMap_BytesPerRow`), not a sync attr.
- **`aHidd_Gfx_ModeTags`** — the assembled list referencing the above
  (`gfx.conf:343–345`).

The SDL driver is the cleanest worked example of building these:
`/Users/user/Source/aros-upstream/arch/all-hosted/hidd/sdl/sdlgfx_hiddclass.c`
(`SDLGfx__Root__New`, lines ~88–284) maps the host's reported format to a
`vHidd_StdPixFmt_*` and emits the pixfmt + per-mode sync taglists.

**Registration with graphics.library:** the driver `main()` (it's a resident
*program* installed to `Storage/Monitors`, not a `.library`) creates the classes,
creates a `CLID_Hidd_Kbd`/`CLID_Hidd_Mouse` and attaches its input drivers via
`HIDD_Input_AddHardwareDriver`, then calls **`AddDisplayDriverA(gfxclass, …)`**
(`rom/graphics/adddisplaydrivera.c:28`, `AROS_LH3 AddDisplayDriverA`). The SDL
`sdl_Startup()` (`arch/all-hosted/hidd/sdl/startup.c`) is the template for this
boot sequence.

**Input HIDDs** (grounded against the SDL driver): `sdl_kbdclass.c` /
`sdl_mouseclass.c` subclass plain **`CLID_Hidd`** (not `CLID_Hidd_Kbd/Mouse`) and
register as *hardware drivers* under the system kbd/mouse HIDDs via
`HIDD_Input_AddHardwareDriver(kbd, xsd->kbdclass, tags)`
(`arch/all-hosted/hidd/sdl/startup.c`, `sdl_Startup`). Each gets an
`aHidd_Input_IrqHandler` callback at New; a private `HandleEvent` method
translates a host event into a `struct pHidd_Kbd_Event` / `pHidd_Mouse_Event`
(rawkey from a keymap table, `vHidd_Mouse_Motion/Press/Release`) and fires that
callback (`sdl_kbdclass.c:58`, `sdl_mouseclass.c:57`).

**Host-call mechanism:** hosted drivers do not link the host library; they
`OpenResource("hostlib.resource")` and `HostLib_Open("libSDL.dylib")` /
`HostLib_GetPointer(...)` at runtime, and wrap every host call in
`HostLib_Lock()/HostLib_Unlock()` (`arch/all-hosted/hidd/sdl/sdl_hostlib.c`, the
`S/SP/SV` macros in `sdl_hostlib.h`; per-arch filename: `darwin*` →
`libSDL.dylib`, `sdl_hostlib.h:53`). Our driver will instead open its own host
shim dylib (the Objective-C/Metal code lives there; see below).

## Design

A new driver tree `arch/all-darwin/hidd/cocoa/` (mirroring `…/hidd/sdl/`),
following the SDL template's class structure, with the iOS UIKit driver as the
Apple-platform reference for the Objective-C bridge and the run-loop task.

### Host side (Cocoa / Metal)

A small Objective-C shim **dylib** (the only `.m` in the project), loaded via
`hostlib.resource` exactly like SDL's `libSDL.dylib`. It exposes a flat C ABI so
the AROS side never sees Objective-C:

- `cocoa_open_window(int w, int h, double scale) -> CocoaWindow*` — create an
  `NSWindow` with an `NSView` whose `layer` is a `CAMetalLayer`
  (`view.wantsLayer = YES`). On a notch/HiDPI Mac the drawable is sized
  `w*backingScaleFactor × h*…`; we set `layer.contentsScale = backingScaleFactor`
  for retina. The Metal device comes from
  `MTLCreateSystemDefaultDevice()` (**UNVERIFIED against the tree — no Metal
  exists in AROS upstream; grounded only against the live macOS SDK, to be proven
  by spike D1 the way H7's ImageIO sequence was proven by `pngprobe`**).
- `cocoa_present(CocoaWindow*, const void *pixels, int bytesPerRow, x, y, w, h)` —
  upload the dirty rectangle to a `MTLTexture` (`replaceRegion:…withBytes:`),
  then encode a blit/quad to the layer's `nextDrawable` and `presentDrawable:`.
  Simple fallback path (D1-first): a `CGBitmapContext` over the AROS pixels →
  `CGBitmapContextCreateImage` → set as `layer.contents` (this is exactly the
  ImageIO/CoreGraphics call shape already proven in H7 `hosted/display.c:113`
  `host_present`, and the same `CGBitmapContextCreate(...)` the iOS driver uses at
  `arch/all-ios/hidd/uikit/native_api.m:119` `NewContext`).
- `cocoa_pump_events(CocoaWindow*)` — drain pending NSEvents
  (`[NSApp nextEventMatchingMask:…]` / `CFRunLoopRunInMode(kCFRunLoopDefaultMode,
  0, true)`), translating key/mouse events into a flat struct the AROS input
  drivers read. The iOS driver's `PollEvents` does precisely this:
  `CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, TRUE)`
  (`arch/all-ios/hidd/uikit/native_api.m:130`).
- `cocoa_readback(CocoaWindow*, void *out, int *w, int *h)` — **the
  unattended-verification hook**: read the last-presented drawable's texture back
  to a host buffer (`getBytes:bytesPerRow:fromRegion:…` for the CGBitmap fallback,
  a CPU-readable `MTLTexture` / blit-to-staging-buffer for Metal), so the agent
  can pixel-assert without `screencapture`. See the verification section.

**Pixel format:** the window is 32-bit chunky BGRA/RGBX (matching H7's RGBX 4-byte
layout, `hosted/display.c:61` and the `kCGImageAlphaNoneSkipLast |
kCGBitmapByteOrder32Big` of `host_present`). We advertise exactly one TrueColor
`vHidd_StdPixFmt_*` matching the layer's pixel format, like SDL maps `vfmt`.

### AROS side (the HIDD / bitmap class)

Two OOP subclasses, built at runtime via `CLID_HiddMeta` + `aMeta_SuperID`
exactly as `arch/all-hosted/hidd/sdl/startup.c` does:

- **Gfx class** `CocoaGfx`, `SuperID = CLID_Hidd_Gfx`. Overrides:
  - `moHidd_Gfx_CreateObject` — for a displayable bitmap (`aHidd_BitMap_ModeID`
    valid / `aHidd_BitMap_Displayable` TRUE), inject `aHidd_BitMap_ClassPtr =
    CocoaBMclass` and call the super; for offscreen bitmaps, either our class or
    delegate to the system `CLID_Hidd_ChunkyBM` (the headless driver does exactly
    this delegation: `rom/hidds/gfx/headless/headlessgfx_hiddclass.c`).
  - `moHidd_Gfx_Show` — on the framebuffer bitmap, call `cocoa_open_window(w, h,
    scale)` (lazily, first Show — same lazy-window pattern as SDL, whose window is
    created on first `SDL_SetVideoMode` inside the bitmap `New`/`Show`, not at
    init).
  - `New` builds the pixfmt + sync + mode taglists (one or a few fixed modes to
    start — `default_modes[]` in `sdlgfx_hiddclass.c:35` is the precedent for a
    static fallback list).
- **BitMap class** `CocoaBM`, `SuperID = CLID_Hidd_BitMap`. Instance data holds
  the AROS-side chunky pixel buffer + the `CocoaWindow*` + `is_onscreen`.
  Overrides:
  - `moHidd_BitMap_PutPixel` / `GetPixel` (mandatory) writing the chunky buffer.
  - `moHidd_BitMap_PutImage` / `GetImage` / `FillRect` / `Clear` (fast paths over
    the buffer; can defer to base fallbacks initially for correctness).
  - **`moHidd_BitMap_UpdateRect`** → if `is_onscreen`, call
    `cocoa_present(window, buf, bytesPerRow, x, y, w, h)`. This mirrors SDL's
    `SDLBitMap__Hidd_BitMap__UpdateRect → SDL_UpdateRect` gated on
    `bmdata->is_onscreen` (`sdlgfx_bitmapclass.c:352`).

**Framebuffer ownership decision:** unlike SDL (where pixels live *inside* the
host `SDL_Surface`), we keep the chunky framebuffer **AROS-side** — `AllocMem`'d
from the AROS heap, the H7/H5 model (`hosted/display.c:144` `fb = AllocMem(W*H*4,
MEMF_CLEAR)`) — and hand the host a *pointer* per present. This keeps the thesis
honest ("AROS draws into a framebuffer it allocates; the host presents it") and
makes the Metal texture-upload an explicit, observable DMA-like step. (A later
optimisation could share a `MTLBuffer`-backed texture to avoid the copy;
deferred.)

### The bridge (present path + input events back to AROS)

**Present path:** graphics.library draws into the bitmap → calls
`HIDD_BM_UpdateRect` → our `UpdateRect` calls `cocoa_present`, all under
`HostLib_Lock()`.

**The run-loop / main-thread problem (the host-specific crux).** Cocoa/AppKit
insists most UI work — and `CAMetalLayer.nextDrawable` / event pumping — happens
on the process **main thread**. In this project the macOS main thread *is* the
AROS boot task: H4 models "the macOS main thread as a low-priority 'boot' anchor
task" (NOTES.md:119). So we do **not** spin a second pthread for Cocoa; we drive
the run loop from an AROS task, taking `HostLib_Lock()` around each host call.
Precedent is exact and Apple-specific: the iOS UIKit driver's
`arch/all-ios/hidd/uikit/eventtask.c` runs `EventTask()` — an AROS task that
`AddIntServer(INTB_VERTB, …)`, `Wait()`s on the VBlank signal, then under
`HostLib_Lock()` calls `PollEvents()` → `CFRunLoopRunInMode(…, 0, TRUE)`. That
same file documents the host-ABI hazard we inherit on Apple Silicon: *"MacOS X
ABI says that stack must be aligned at 16 bytes boundary. Some functions crash …
deeply inside UIKit … `CFRunLoopRunInMode()` …"* (`eventtask.c:17–24`) — our
host-call shim (`hosted/abishim.S`, NOTES.md H3) already owns the AAPCS64↔Apple
ABI boundary, and 16-byte SP alignment is the relevant invariant here.

So: a single **`cocoa.hidd` event task**, VBlank-signalled (the SDL/UIKit model),
that under `HostLib_Lock()` (a) pumps NSEvents, (b) on the boot/main thread.
Present (`UpdateRect`) is called from the drawing task but also takes
`HostLib_Lock()`, so all Cocoa touches serialise onto one lock — matching how
every hosted HIDD funnels host calls.

**Input back to AROS:** the event task translates each NSEvent into the rawkey /
mouse-delta form and calls the kbd/mouse hardware-driver callback
(`HIDD_Input_AddHardwareDriver` registration; the `HandleEvent`→callback dispatch
of `sdl_kbdclass.c:58` / `sdl_mouseclass.c:57`). A macOS keycode→AROS-rawkey table
is the analogue of `arch/all-hosted/hidd/sdl/keymap.c` /
`arch/all-hosted/hidd/x11/mac-x11-keycode2rawkey.table` (the latter is a ready
macOS-keycode mapping we can adapt).

## The unattended-verification tension (and how we solve it)

The whole reason H7 stopped at a PNG: **verifying a live NSWindow on screen
requires `screencapture`, which requires macOS Screen-Recording (TCC) approval — a
manual click that breaks the unattended loop** (NOTES.md "H7"). A live window that
can only be eyeballed is, by this project's standing rule, unverifiable.

Resolution: **the live window and the verification channel are decoupled.** We
never screen-capture. Instead we read the *drawable itself* back to host memory and
pixel-assert it — the framebuffer is the ground truth, the window is just a second
consumer of the same pixels:

- For the CGBitmap fallback path the readback is trivial — the AROS chunky buffer
  *is* what we presented, so we pixel-assert it directly (identical to H7's
  `px_is` checks, `hosted/display.c:129`), and additionally assert that
  `CGBitmapContextCreateImage` succeeded and the layer accepted it.
- For the Metal path, `cocoa_readback` does a `MTLBlitCommandEncoder` copy of the
  presented texture to a CPU-readable staging `MTLBuffer` (or a
  `MTLStorageModeShared` texture) and `getBytes:`. We then pixel-assert that
  buffer. This proves the GPU actually received and laid out the pixels — a
  stronger check than asserting the source buffer alone.
- Because the readback is a host function returning bytes the agent reads (or we
  re-encode it to PNG via the proven H7 ImageIO path for human inspection), the
  loop stays exactly as unattended as H7: run the Mach-O, read a buffer/PNG, get
  one PASS/FAIL.

The window being visible to a human is a *bonus output*, never the verification
oracle. This is the explicit design tension and its resolution.

## Plan — spikes in the loop

Each marker is one binary, one PASS/FAIL verdict, grounded-then-built (the H1–H12
discipline). Ordered so each spike de-risks one unknown before the next.

- **[D1] Window + Metal layer + readback (host only).** Open an `NSWindow` with a
  `CAMetalLayer`, present a known test pattern (the H7 four-colour squares +
  "AROS" so it's recognisable and pixel-checkable), then `cocoa_readback` and
  pixel-assert the squares/colours. **PASS:** readback matches the pattern exactly
  (no `screencapture`). Proves the Cocoa/Metal call sequence on Apple Silicon, the
  retina `contentsScale`, and the readback oracle — the way `pngprobe` proved
  ImageIO before H7. (Start with the CGBitmap fallback present if Metal readback
  is fiddly; upgrade in D1b.)
- **[D2] Run-loop on the boot/main task.** Drive `cocoa_pump_events` +
  `presentDrawable` from a single VBlank-signalled AROS task under
  `HostLib_Lock()` (the `eventtask.c` model), proving no second pthread is needed
  and the 16-byte SP-alignment / ABI boundary holds (reuse `abishim.S`).
  **PASS:** N frames present + events pump for T seconds without a crash, readback
  stable each frame.
- **[D3] Wire the AROS bitmap present.** Stand up the `CocoaGfx`/`CocoaBM`
  subclasses (runtime `CLID_HiddMeta`), implement `CreateObject` (inject
  `aHidd_BitMap_ClassPtr`), `Show` (lazy `cocoa_open_window`), `PutPixel`/`PutImage`,
  and `UpdateRect`→`cocoa_present`. Draw the H7 scene *through the HIDD methods*.
  **PASS:** `cocoa_readback` after a sequence of `HIDD_BM_*` calls matches the
  expected scene; assert `UpdateRect` actually fired the present (counter, the way
  H8/H12 instrument an LVO via `SetFunction`).
- **[D4] Mode list + pixelformat advertised correctly.** Build the
  `aHidd_Gfx_PixFmtTags`/`SyncTags`/`ModeTags` and verify graphics.library can
  `GetMode`/select the mode and that `BytesPerRow`/depth round-trip. **PASS:** a
  mode query returns the advertised WxH/pixfmt; a displayable bitmap of that mode
  is created and shown.
- **[D5] Input HIDD — mouse then keyboard.** Register kbd/mouse hardware drivers
  (`HIDD_Input_AddHardwareDriver`), translate NSEvents to rawkey/mouse-delta in
  the event task, deliver via the IRQ callback. **PASS:** synthesised host events
  (posted via `CGEventPost` or injected into the pump) arrive as the expected AROS
  rawkeys / mouse deltas — assertable without a human (no on-screen interaction
  required).
- **[D6] (stretch) Real graphics.library / Wanderer in the window.** Register via
  `AddDisplayDriverA`, point the booting AROS at this monitor, and confirm the
  desktop renders — verified by reading the drawable back and asserting non-blank,
  title-bar-present, etc. **PASS:** readback shows a plausible Wanderer frame
  (structural pixel asserts, not a screenshot).

## How we verify it unattended

Same shape as the entire project: build the Mach-O, run it headless-to-the-agent,
read a file/buffer, emit one verdict block. Concretely:

- The oracle is **`cocoa_readback` → pixel asserts** (D1–D4) and **synthesised
  input → expected AROS events** (D5) — never `screencapture`, so **no TCC
  prompt, no manual step**.
- Each spike prints a unique marker `[D1]…[D6]` (the marker discipline,
  NOTES.md), exits clean (so PASS is fast), and is reaped by the bash watchdog on
  hang.
- For human inspection only, the presented drawable is *also* re-encoded to PNG
  via the already-proven H7 ImageIO path (`hosted/display.c host_present`) into
  `run/`, exactly like `run/aros-h7.png` — a courtesy artefact, not the verdict.
- The window does open on screen during the run (that's the feature); the point is
  the *judgement* never depends on a human seeing it.

## Risks & open questions

- **Metal has zero precedent in the AROS tree** — every Metal API here is
  **UNVERIFIED** against `aros-upstream` and grounded only against the live macOS
  SDK. Mitigation: D1 proves the exact call sequence first (the `pngprobe`/H7
  method), and the CGBitmap-into-`layer.contents` fallback is a fully grounded
  path (H7 + `native_api.m:119`) we can ship if Metal blocks. *Out-of-tree*, the
  exact pipeline (guest framebuffer → `MTLTexture` → drawable in an `NSWindow`)
  already ships on Apple Silicon in **vAmiga** (`GUI/Metal/`, GPL,
  <https://github.com/dirkwhoffmann/vAmiga>) — a reference for the call sequence,
  though GPL/Swift and not linkable.
- **"Why not just reuse the SDL HIDD?"** On macOS the in-tree SDL HIDD bottoms out
  in SDL1.2's `SDL_QuartzVideo.m` (NSWindow/NSView/CGContextRef) — Cocoa already,
  but built on APIs Apple *removed* by High Sierra (`CGDirectPaletteRef`;
  <https://github.com/libsdl-org/SDL-1.2/issues/739>), so it won't build for modern
  macOS. The SDL2-for-AROS effort is a *guest* lib, incomplete. Writing Cocoa
  directly is therefore the warranted path, not gold-plating around a working
  alternative.
- **Main-thread / run-loop ownership.** AppKit + `nextDrawable` want the main
  thread; AROS owns it as the boot task. The `eventtask.c` VBlank-task-under-
  HostLib_Lock model is the precedent, but the iOS driver's `UpdateRect` is a
  `/* TODO */` stub (`uikit_bitmapclass.c:191`) — so the *present* under this model
  is unproven in-tree and is exactly what D2/D3 must establish. Open question:
  does any Cocoa call we need *require* being the literal `NSApplicationMain`
  thread vs. merely the main pthread? (Probably fine since the boot task runs on
  the main pthread; confirm in D2.)
- **16-byte SP alignment / Apple ABI** at the Cocoa boundary (`eventtask.c:17`).
  Covered by the existing `hosted/abishim.S` shim (H3), but Objective-C message
  sends (`objc_msgSend` is variadic-shaped) must be checked under it.
- **Retina/HiDPI coordinate math.** Drawable pixels = points × `backingScaleFactor`;
  the AROS mode is in pixels. Need to decide whether AROS sees the backing
  (e.g. 2× = a 2560×1440 mode) or points with the layer upscaling. D1/D4 decide.
- **Present rate / tearing.** `presentDrawable` ties to the display refresh;
  `UpdateRect` can be called far more often than 60 Hz. Likely coalesce dirty
  rects and present once per VBlank tick (the event task already has the VBlank
  signal). Deferred past D3.
- **W^X is *not* a risk here** — the host shim dylib is ordinary signed code; we
  generate no executable memory (contrast the LoadSeg path, NOTES.md graft). Noted
  so we don't build a workaround for a non-problem (the H8 lesson).
- **Cursor.** `moHidd_Gfx_SetCursorShape/Pos/Visible` are optional; initially hide
  the macOS cursor over the view and let AROS draw its own (SDL does
  `SDL_ShowCursor(DISABLE)`, `sdl_init.c`). Hardware cursor deferred.

## References

In-project:
- `NOTES.md` — H3 (host-call ABI shim), H4 (macOS main thread = boot task), H7
  (render-to-PNG display + the TCC/Screen-Recording deferral reasoning), H8 (no
  W^X wall for data vectors).
- `hosted/display.c` — H7: AROS `AllocMem` framebuffer + ImageIO present
  (`host_present`, line 113) + `px_is` pixel asserts (line 129); the present
  fallback and verification-oracle shapes reused here.
- `hosted/abishim.S` — the AAPCS64↔Apple-arm64 host-call shim (the Cocoa boundary
  rides this).
- `graft/WORKFLOW.md`, `graft/UPSTREAM-NOTES.md`, `graft/bootrun.sh` — the
  boot/run harness this driver plugs into.

AROS upstream (`/Users/user/Source/aros-upstream/`):
- `rom/hidds/gfx/gfx.conf` — authoritative HIDD interface (methods, attrs, IIDs,
  stub macros).
- `rom/hidds/gfx/include/gfx.h` — `<hidd/gfx.h>`: `CLID_*`/`IID_*` aliases,
  `HIDDT_*` types, pixfmt/colormodel enums.
- `rom/hidds/gfx/gfx_hiddclass.c` — base graphics class (`CreateObject` at :1168,
  the `aHidd_BitMap_ClassPtr` contract at :1144, `Show` at :2502).
- `rom/hidds/gfx/gfx_bitmapclass.c` — base bitmap class (`PutPixel` mandatory note
  :1392, `UpdateRect` impl :5057 / contract :5007–5055).
- `rom/hidds/gfx/headless/headlessgfx_hiddclass.c` — minimal driver (CreateObject
  delegating to `CLID_Hidd_ChunkyBM`).
- `arch/all-hosted/hidd/sdl/` — the hosted-driver template: `startup.c`
  (class/Input/AddDisplayDriverA boot), `sdlgfx_hiddclass.c`
  (pixfmt/sync/mode taglists, CreateObject, Show), `sdlgfx_bitmapclass.c`
  (`UpdateRect` present gated on `is_onscreen`, :352), `sdl_hostlib.{c,h}`
  (hostlib runtime load + `S/SP/SV` lock macros), `event.c` / `sdl_kbdclass.c` /
  `sdl_mouseclass.c` / `keymap.c` (VBlank event task + input drivers).
- `arch/all-hosted/hidd/x11/` — second hosted display driver:
  `x11gfx_onbitmap.c` / `x11gfx_offbitmap.c` (on-screen vs offscreen storage
  selected per-instance on `aHidd_BitMap_FrameBuffer`), `x11_init.c` (host event
  task `create_x11task`/`x11task_entry`), `mac-x11-keycode2rawkey.table` (a
  macOS-keycode mapping to adapt).
- `arch/all-ios/hidd/uikit/` — the Apple-platform precedent: `eventtask.c`
  (`EventTask` = VBlank-signalled AROS task driving `CFRunLoopRunInMode` under
  `HostLib_Lock`; the SP-alignment/`CFRunLoopRunInMode`-crash note, :17–24),
  `native_api.m` (`CGBitmapContextCreate` at :119, `CFRunLoopRunInMode` at :130),
  `uikit_bitmapclass.c` (`UpdateRect` is `/* TODO */`, :191 — present never
  finished).
- `rom/graphics/adddisplaydrivera.c:28` — `AddDisplayDriverA` LVO.

External prior art (web, *not* in either tree):
- AROS ports page — Darwin/macOS hosted port "requires an X11 server for the
  display" (the X11-only history): <https://aros.sourceforge.io/introduction/ports.html>.
- vAmiga — native Apple-Silicon macOS Amiga emulator, GPL, Metal present path
  (`MTKView`/`MTLDevice`/`drawable`) over a guest framebuffer; the closest working
  analogue of our pipeline: <https://github.com/dirkwhoffmann/vAmiga> /
  <https://github.com/dirkwhoffmann/vAmiga/tree/master/GUI/Metal>.
- SDL1.2 Quartz video backend (`SDL_QuartzVideo.m`, NSWindow/NSView/CGContextRef)
  — what the SDL HIDD reduces to on macOS, and why it no longer builds for modern
  macOS (`CGDirectPaletteRef` removed): 
  <https://github.com/klange/SDL/blob/master/src/video/quartz/SDL_QuartzVideo.h>,
  <https://github.com/libsdl-org/SDL-1.2/issues/739>.
- Pavel "Sonic" Fedin — author of the AROS iOS UIKit HIDD / host-driver lineage:
  <https://github.com/Sonic-Amiga>.
