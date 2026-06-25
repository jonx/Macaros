# cocoametal — the frozen AROS↔host interface (single source of truth)

> Status: **v1 — proposed, freezing in progress** · Drafted 2026-06-25
> Both sides build against *this* file. The host shim (`hosted/cocoametal/`, Apple
> clang, John's clean-room implementation) and the AROS driver
> (`arch/all-darwin/hidd/cocoa/`, AROS crosstools, mine) meet at exactly one seam:
> **`cocoametal.dylib` + the flat-C `cm_*` ABI in `cocoametal.h`.** Nothing else
> crosses. Any change here is a change to *both* sides and a version bump (§7).

This document freezes three things the design.md left open so D3 (wiring the HIDD
to the shim) becomes pure plumbing:

1. **The symbol list + interface-struct ordering** — the exact load contract.
2. **The pixel / geometry / scale contract.**
3. **The threading + call contract.**

Plus the input ABI (§5) and the readback-oracle rule (§6).

---

## 1. The load sequence (how the AROS side opens the shim)

The AROS driver loads the shim the same way unixio loads libc and the SDL HIDD
loads `libSDL.dylib` — via `hostlib.resource`. Grounded against
`arch/all-unix/hidd/unixio/unixio_class.c:1150-1156`:

```c
HostLibBase = OpenResource("hostlib.resource");          /* once, at driver init */
handle = HostLib_Open("cocoametal.dylib", &errstr);       /* dlopen the shim      */
CMIFace = HostLib_GetInterface(handle, cocoametal_symbols, &errcount);
/* errcount MUST be 0: it counts symbols that failed to resolve. */
```

`HostLib_GetInterface(handle, names[], &errcount)` resolves each name in the
NULL-terminated `names[]` array and returns a struct of function pointers **in the
same order as the array**. This ordering is the contract (see `struct
LibCInterface` mirroring `libc_symbols[]` in `unixio.h:50`). So the array and the
struct below are locked together — append only, never reorder.

### 1a. The frozen symbol array (shared)

Order = `cocoametal.h` declaration order. **Append-only. Never reorder, never
remove.**

```c
static const char *cocoametal_symbols[] =
{
    "cm_open",                     /* 0 */
    "cm_close",                    /* 1 */
    "cm_upload_rect",              /* 2 */
    "cm_present",                  /* 3 */
    "cm_set_effect",               /* 4 */
    "cm_pump_events",              /* 5 */
    "cm_readback",                 /* 6 */
    "cm_target_size",              /* 7 */
    "cm_render_effect_readback",   /* 8 */
    "cm_abi_version",              /* 9 — appended per §7 (append-only) */
    "cm_set_option",               /* 10 — appended at v2 per §9 (append-only) */
    "cm_get_option",               /* 11 — appended at v2 per §9 (append-only) */
    "cm_open_settings",            /* 12 — appended at v2 per §9 (append-only) */
    NULL
};
```

### 1b. The AROS-side interface struct (mine — must mirror 1a exactly)

```c
struct CMInterface
{
    struct CMContext *(*cm_open)(int w, int h, const struct CMPixelDesc *fmt, const char *title);
    void              (*cm_close)(struct CMContext *);
    void              (*cm_upload_rect)(struct CMContext *, const void *src, int srcStride, int x, int y, int w, int h);
    void              (*cm_present)(struct CMContext *);
    int               (*cm_set_effect)(struct CMContext *, int effect);
    int               (*cm_pump_events)(struct CMContext *, struct CMEvent *out, int maxEvents);
    int               (*cm_readback)(struct CMContext *, void *dst, int dstStride, int w, int h);
    int               (*cm_target_size)(struct CMContext *, int *outW, int *outH, int *outScale);
    int               (*cm_render_effect_readback)(struct CMContext *, int effect, void *dst, int dstStride, int w, int h);
    int               (*cm_abi_version)(void);   /* entry 9 — appended per §7 */
    int               (*cm_set_option)(struct CMContext *, int key, long value);   /* entry 10 — v2 §9 */
    int               (*cm_get_option)(struct CMContext *, int key, long *value);  /* entry 11 — v2 §9 */
    int               (*cm_open_settings)(struct CMContext *);                     /* entry 12 — v2 §9 */
};
```

### 1c. Host-side build obligation

- Emit a **`cocoametal.dylib`** (not just the D1 test executable).
- Every `cm_*` name above is exported with **default visibility** and **not
  stripped** (`HostLib_GetInterface` resolves them by `dlsym`).
- The dylib pulls **no AROS headers**; the AROS side pulls **no Cocoa headers**.
  `cocoametal.h` is the only shared text (it already holds to this).

---

## 2. Pixel & geometry contract — FROZEN

| Property | Value |
|---|---|
| Pixel format | **BGRA8** — blue=byte 0, green 1, red 2, alpha 3 (little-endian AArch64) |
| Bytes/pixel | 4 |
| Origin | top-left |
| Framebuffer owner | **AROS** — `AllocMem(W*H*4, MEMF_CLEAR)`, the H7 model |
| Hand-off | AROS passes a **pointer into the whole framebuffer + `srcStride`** per present; the shim copies the dirty rect (`cm_upload_rect`) |
| What AROS sees | **LOGICAL pixels.** The AROS mode list is in logical WxH. The shim upscales to the backing store via `layer.contentsScale = backingScaleFactor`. |
| Retina/scale | The shim owns scale. `cm_target_size` reports `(backingW, backingH, scale)` **for the readback oracle only** — AROS never reasons in backing pixels. |
| Coords in `cm_*` | **logical (pre-scale)**, top-left origin — applies to `cm_upload_rect`, `cm_readback`, and `CMEvent.x/y`. |

`CMPixelDesc` (from `cocoametal.h`) is passed at `cm_open` and pins the above; the
AROS gfx class advertises exactly one matching TrueColor `vHidd_StdPixFmt_*`.

**Why logical, not backing:** keeps the AROS sync/mode taglists trivial (one mode =
one logical resolution) and keeps the thesis honest — AROS draws a framebuffer it
allocates; the host scales on present. A future `MTLBuffer`-shared zero-copy path
can revisit this without changing the ABI.

---

## 2a. D3 pixel hand-off — the exact `CMPixelDesc` + call mapping — FROZEN

This pins, to the field, what the AROS side passes at `cm_open` for BGRA8 and how
each HIDD method maps onto the `cm_*` ABI, so D3 (rows A–C) is pure plumbing. The
host side has proven this contract end-to-end: `make cocoametal-hiddsim` →
**`[HIDDSIM] PASS`** (the HIDD-shaped behavioral harness — pinned desc, lazy open,
a dirty-rect stream that composes byte-exact vs an independent reference, and a
pixfmt round-trip that catches any swizzle — see §8).

### 2a-1. The exact `CMPixelDesc` (BGRA8 == `MTLPixelFormatBGRA8Unorm`, LE AArch64)

The AROS side fills `cm_open`'s `fmt` with **exactly** these values (any other
value is a contract violation — the host asserts them via the §8 round-trip):

```c
CMPixelDesc fmt = {
    .bytesPerPixel = 4,
    .blueShift  = 0,           .greenShift = 8,           .redShift  = 16,          .alphaShift = 24,
    .blueMask   = 0x000000FF,  .greenMask  = 0x0000FF00,  .redMask   = 0x00FF0000,  .alphaMask  = 0xFF000000,
};
/* In-memory byte order: blue @ byte 0, green @ byte 1, red @ byte 2, alpha @ byte 3. */
```

| Field | Value | Meaning |
|---|---|---|
| `bytesPerPixel` | `4` | chunky 32-bit |
| `blueShift,greenShift,redShift,alphaShift` | `0, 8, 16, 24` | bit position of each channel in the LE 32-bit word |
| `blueMask,greenMask,redMask,alphaMask` | `0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000` | the matching masks |
| Origin | **top-left** | rows top-to-bottom |
| Dimensions | **logical `W×H`** | the AROS mode-list resolution (§2) |

**The AROS-side requirement (THEIR action):** advertise the TrueColor
`vHidd_StdPixFmt_*` whose **in-memory byte order is B, G, R, A** (blue lowest
address) so neither side swizzles — and fill the `CMPixelDesc` above from the
*same* constants. **The AROS side confirms the exact `vHidd_StdPixFmt_*` constant
against `rom/hidds/gfx/include/gfx.h`** (this host spec does not name an AROS enum
it cannot verify). Register that format in the gfx-class `aHidd_Gfx_PixFmtTags`
(`vHidd_ColorModel_TrueColor`, `BytesPerPixel=4`, `BitMapType=…_Chunky`, the
matching `*Shift`/`*Mask`) per spec.md "Pixel format mapping".

### 2a-2. The call-mapping table (HIDD method → `cm_*`)

| AROS HIDD action | `cm_*` call | Notes |
|---|---|---|
| `moHidd_Gfx_Show` (first, displayable bitmap front) | **lazy `cm_open(w,h,&fmt,title)`** | open on the FIRST Show, not at class `New` (the SDL lazy-window pattern, §4). Subsequent Shows just select the front bitmap + request a full present. |
| `moHidd_Gfx_CreateObject` (displayable bitmap) | inject **`aHidd_BitMap_ClassPtr`** | hand the base class our bitmap class (mandatory, `gfx_hiddclass.c`). No `cm_*` here — the framebuffer is `AllocMem(W*H*4, MEMF_CLEAR)` in the bitmap class. |
| `moHidd_BitMap_UpdateRect(x,y,w,h)` | **`cm_upload_rect(fb, stride, x,y,w,h)` then `cm_present()`** | the present hook. Pass a pointer to the WHOLE framebuffer + `srcStride`; the shim copies just the dirty sub-rect (§2 hand-off). Many small UpdateRects is the norm — proven to compose byte-exact in `[HIDDSIM]`. |
| VBlank-signalled event task | **`cm_pump_events(out, maxEvents)`** | non-blocking drain (pull, §3/§5); feed the returned `CMEvent[]` into the input subsystem. |
| Unattended verification | **`cm_readback`** (joint acceptance) | the §6 oracle — both sides assert pixels against it, no TCC. |

### 2a-3. The one-time AppKit init (boot task, ONCE — D2t RESOLVED)

Before the first `cm_open`, the display-server / boot task does exactly this pair
(initialization only — **no** run loop, **no** `[NSApp run]`, **no**
`NSApplicationMain`; §3):

```objc
[NSApplication sharedApplication];                              /* create NSApp */
[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
```

Run-loop servicing is by hand:
`CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true)` (bounded, non-blocking). The
shim's `cm_try_window` performs this same pair idempotently, so omitting it still
works — but the boot task should do it explicitly so the activation policy is set
before any window.

---

## 3. Threading & call contract — FROZEN

This is the design.md "main-thread crux", pinned so D2 proves the *real* model.

- **Single caller.** Every `cm_*` is called from the **one AROS scheduler thread**
  (`[OURS]` H6), serialized under **`HostLib_Lock()` / `HostLib_Unlock()`**, across
  the **`abishim.S`** AAPCS64↔Apple-arm64 boundary (16-byte SP alignment — H3). No
  `cm_*` is ever called from two threads.
- **No callbacks into AROS.** The shim **never** calls an AROS LVO and **never**
  raises an AROS `Signal`. Input flows *only* by the AROS side calling
  `cm_pump_events` and reading the returned `CMEvent[]` (pull, not push). This is
  why cocoametal needs **no** `host-wake` seam (contrast the socket/clipboard/audio
  drivers).
- **No `NSApplicationMain` / no `[NSApp run]`.** The AROS boot task *is* the host
  main pthread (H4); it drives `CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true)`
  by hand. **D2 must open the window, present, and pump under this exact model** —
  not a harness that spins `NSApp`. If any call needs the app run loop, we find out
  at D2.

  **D2 RESOLVED (`[D2t]` green — `hosted/cocoametal/d2t_test.m`, this Mac: M5 /
  macOS 26.5).** Under exactly this model (main pthread, hand-pumped
  `CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true)`, never `[NSApp run]`):
  - **The minimal AppKit init the boot task must do ONCE, before the first
    `cm_open`:**
    ```objc
    [NSApplication sharedApplication];                 /* create NSApp */
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    ```
    This is *initialization only* — it does **not** start a run loop (`[NSApp
    isRunning]` stays 0). No `finishLaunching`, no `run`, no `NSApplicationMain`.
    (The shim's `cm_try_window` already performs this same pair idempotently, so if
    the boot task omits it the first `cm_open` still establishes it — but the boot
    task should do it explicitly so the activation policy is set before any window.)
  - **`cm_open` (NSWindow + CAMetalLayer creation): WORKS** under the hand-pumped
    model — the window is created and `cm_target_size` reports the real backing
    scale (2× here).
  - **`cm_present` (`[layer nextDrawable]`): WORKS** — every present acquired a
    live `CAMetalDrawable` (3/3 at both 320×200 and 640×512). The hand-pumped
    `CFRunLoopRunInMode(...,0,true)` between presents is sufficient for the window
    server; **no app run loop is required** for `nextDrawable` to yield.
  - **The offscreen oracle (`cm_readback`) PASSES** at both resolutions regardless
    — it never depends on the window, so even a future headless context (no window
    server → `cm_open` gives a context with `scale=1` and the present pass is
    skipped) keeps the contract.
  - **Caveat:** verified in a GUI login session (a window server was reachable). A
    truly headless launch context (e.g. a bare `launchd` daemon with no session)
    degrades `cm_open` to the headless `scale=1` path; the oracle still holds. The
    graft's display-server task runs in the user's session, matching this.
- **No `cm_*` may block or spin.** `cm_pump_events` is a non-blocking drain;
  `cm_present` is synchronous but bounded (commit + `waitUntilCompleted` on the
  oracle pass only). A `cm_*` that could block must be documented here first.

---

## 4. Function semantics (the parts AROS depends on)

- `cm_open(w, h, fmt, title)` → `CMContext*` or NULL. Builds device/queue,
  framebuffer + offscreen oracle textures, best-effort live window. Called lazily
  on first `moHidd_Gfx_Show` (the SDL lazy-window pattern).
- `cm_upload_rect(ctx, src, srcStride, x, y, w, h)` — copy a logical sub-rect from
  the AROS framebuffer into the GPU texture. Driven by `moHidd_BitMap_UpdateRect`.
- `cm_present(ctx)` — render framebuffer texture → offscreen oracle (pass-through)
  then best-effort present to the drawable. Applies the selected `CMEffect` to the
  *presented* image only; the oracle target stays pass-through.
- `cm_pump_events(ctx, out, maxEvents)` → count written. Drains pending NSEvents.
  Driven by the VBlank-signalled event task. **REAL since D4/D5** (was the D1 stub) —
  non-blocking `nextEventMatchingMask:untilDate:distantPast dequeue:YES` loop bounded by
  `maxEvents`; mapping in §5.
- `cm_readback(ctx, dst, dstStride, w, h)` → 0 on success. Copies the last-presented
  **oracle** target as BGRA8 at logical w×h. **The unattended oracle (§6).**
- `cm_target_size`, `cm_set_effect`, `cm_render_effect_readback` — oracle/effect
  introspection; not on the boot-critical path.

---

## 5. Input ABI — FROZEN

`CMEvent` (from `cocoametal.h`): `{ type, x, y, code, pressed, mods }`.

- `type` ∈ `CM_EV_{NONE,MOUSEMOVE,MOUSEBTN,KEY,CLOSE,RESIZE,SETTING}`.
  `CM_EV_SETTING` is appended at v2 (after `CM_EV_RESIZE`, append-only) — a
  user-changed AROS-facing option, surfaced for pull. Packing in §9.
- `x, y` — **logical** pixel coords, top-left origin. (For `CM_EV_SETTING`,
  `x`/`y` instead carry the option value(s) — see §9.)
- `code` — for `CM_EV_KEY`: the **macOS virtual keycode**; for `CM_EV_MOUSEBTN`:
  button index.
- `pressed` — 1=down, 0=up.
- `mods` — `CM_MOD_{SHIFT,CONTROL,ALT,CMD}` bitmask from `NSEvent.modifierFlags`.

> **`cm_pump_events` is now REAL (2026-06-25, John — D4/D5 green).** It is no longer the
> D1 stub. Implemented in `hosted/cocoametal/cocoametal_window.m`
> (`cm__pump_events_appkit`, the strong override of a weak stub in `cocoametal.m` — the
> same weak/strong split as `cm_try_window`, so the AppKit-free translation unit pulls no
> AppKit headers). Runs on the **single AROS/main thread** (§3, the only caller). It drains
> NON-BLOCKING in a loop bounded by `maxEvents`:
> `[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES]`
> (`distantPast` ⇒ returns at once when the queue is empty — never blocks/spins).
> **Mapping as implemented:**
> - `MouseMoved`/`{Left,Right,Other}MouseDragged` → `CM_EV_MOUSEMOVE`.
> - `{Left,Right,Other}MouseDown/Up` → `CM_EV_MOUSEBTN`, `code` 0=left/1=right/2=middle,
>   `pressed` 1/0.
> - **Coords (all logical, top-left, §2):** `locationInWindow` is points (= logical) with a
>   **bottom-left** origin ⇒ `x = locationInWindow.x`, `y = contentHeightPoints −
>   locationInWindow.y` (content height in points == logical H — the `contentsScale` lives
>   below the layer, so **no `/scale`**), then clamp to `[0,w)×[0,h)`.
> - `KeyDown`/`KeyUp` → `CM_EV_KEY`, `code = keyCode` (macOS VK), `pressed` 1/0, `mods`.
> - `FlagsChanged` → `CM_EV_KEY` for the modifier transition: `code = keyCode`, `pressed`
>   from whether that modifier's flag is set in `modifierFlags`.
> - `mods`: `NSEventModifierFlag{Shift,Control,Option,Command}` → `CM_MOD_{SHIFT,CONTROL,ALT,CMD}`.
> - Window close → `CM_EV_CLOSE`; live resize → `CM_EV_RESIZE` (best-effort) — surfaced by a
>   tiny `NSWindowDelegate` (`windowShouldClose:` returns **NO** — report, don't tear down;
>   AROS owns the lifecycle — `windowDidResize:`) setting one-shot context flags the pump
>   drains; these are delegate callbacks, not dequeuable `NSEvent`s.
> - Untranslated (window-management / system) events are forwarded to `[NSApp sendEvent:]`
>   so the window still behaves (drag/close), then draining continues — they never
>   accumulate. (Translated key events are NOT forwarded — AROS owns the keyboard; AppKit
>   would beep on an unhandled key with no responder.)
>
> **Verified unattended, NO TCC** (`hosted/cocoametal/input_test.m`, `make cocoametal-input`
> → `[D4D5] PASS`): synthesise `NSEvent`s with `+[NSEvent mouse/keyEventWithType:…]` and
> inject **in-process** with `[NSApp postEvent:ev atStart:NO]` (rides `NSApp`'s own queue
> that the pump drains — needs no accessibility grant, unlike `CGEventPost`). Asserts
> `[D4]` mouse-move exact logical `x,y` (Y-flip) + LMB down/up (`code=0`, `pressed` 1/0) and
> `[D5]` keyDown/Up (`code==keyCode`, `pressed` 1/0, `mods & CM_MOD_SHIFT`). **Measured
> caveat:** posted mouse `locationInWindow` is not bit-exact on a 2× Retina display (the
> queue re-resolves it through backing geometry — a stable affine `got≈1.019·p−3`; lossless
> `convertPointToScreen` confirms it is the queue's pixel-snapping, not the shim's math);
> the test calibrates that affine at runtime and pre-inverts the posted point so the shim
> receives the integer target, then asserts the Y-flip/clamp **exactly**. A genuine hardware
> mouse event is window-local and exact, so the shim's translation is correct as-is.

**Keycode → AROS rawkey:** my kbd hardware-driver class needs the
macOS-VK→AROS-rawkey map. Source to adapt:
`/Users/user/Source/aros-upstream/arch/all-hosted/hidd/x11/mac-x11-keycode2rawkey.table`.
**Action (John):** drop an adapted copy as
`docs/features/cocoa-metal-display/keycode2rawkey.table` so my input class includes
one agreed table. Mouse deltas/buttons map directly to
`vHidd_Mouse_Motion/Press/Release`.

> **Note — table now exists (2026-06-25, John).** The adapted map is committed at
> `docs/features/cocoa-metal-display/keycode2rawkey.table`. It is an includable C
> fragment (a `{ macVK, rawkey }` initializer list — drop it inside
> `static const struct { unsigned char macVK, rawkey; } cm_kc2rk[] = { #include ... };`).
> **Left side is exactly `CMEvent.code`** for a `CM_EV_KEY` event (the bare macOS
> virtual keycode = Apple `kVK_*`); **right side is an AROS `RAWKEY_*`** value grounded
> against `compiler/include/devices/rawkeycodes.h`. It was adapted directly from the
> in-tree x11 mac table cited above: that file is a 256-byte array indexed by the
> **XQuartz X11 keycode (= macOS VK + 8)**, so I decoded it byte-for-byte and shifted
> the index back by 8 — the rawkey values are copied verbatim, only the index is
> de-offset so it lands on `CMEvent.code` with no +8. **Coverage (102 entries):**
> letters A–Z, digits 0–9, F1–F13, arrows, all modifiers (shift/ctrl/alt-option/cmd,
> caps lock, fn), space/return/escape/tab/delete(=backspace)/fwd-delete, common
> punctuation, and the full numeric keypad. **Gaps (unmapped, as in the source):**
> F14–F20/F16/F17, Volume/Mute, ContextualMenu, Help, all `kVK_JIS_*`; no Mac key
> exists for PrintScreen/ScrollLock/Pause/Insert. **UNVERIFIED:** two keypad keys
> (`kVK_ANSI_KeypadClear`→0x5A, `kVK_ANSI_KeypadEquals`→0x5B) carry the source's byte
> but hit rawkey codes with no named `RAWKEY_*` macro — tagged inline. The fragment
> compiles clean under `clang -Wall -Wextra -Wcomment` (102 rows, no duplicate VKs).
> **Coords confirmation:** `cocoametal.h` documents `CMEvent.x/y` as *logical
> (pre-scale) pixel coords, top-left origin* (line 47), matching §2 — so when the
> D4/D5 input spike fills `cm_pump_events`, it returns logical, pre-scale coords (the
> current D1 stub returns 0 events, so nothing yet violates this).

---

## 6. The readback oracle is the verification contract — FROZEN

`cm_readback` is how **both** sides verify unattended — no `screencapture`, no TCC
prompt. Rules:

- The **offscreen oracle target is always pass-through/nearest**; `CMEffect`
  changes only the *presented* drawable, never the oracle.
- Every new shim behaviour ships with a `cm_readback` (or
  `cm_render_effect_readback`) assertion in a host test.
- When I wire D3, the same oracle re-verifies the scene *after a sequence of real
  `HIDD_BM_*` calls* — so the contract carries from host spike to AROS path
  unchanged.

---

## 7. Versioning — proposed (needs a one-line header change)

So a stale dylib can't silently mismatch the loader, `cocoametal.h` carries:

```c
#define CM_ABI_VERSION 2          /* bump on ANY breaking change to this contract */
int cm_abi_version(void);          /* returns CM_ABI_VERSION; symbol-list entry 9 */
```

The AROS loader checks `CMIFace->cm_abi_version() == CM_ABI_VERSION` right after
`HostLib_GetInterface` and refuses to register the driver on mismatch. Append-only
rule (§1a) means adding `cm_abi_version` goes at the **end** of the array/struct.

**v1 → v2 (2026-06-25, John — `[SET]` green).** The settings/options ABI (§9) was
**appended**: `cm_set_option`/`cm_get_option`/`cm_open_settings` (symbol entries
10/11/12, after `cm_abi_version` at 9), the `CMOption`/`CMScaleMode`/`CMFilter`
enums, and the `CM_EV_SETTING` event (after `CM_EV_RESIZE`). **All append-only —
no existing symbol or enum member moved.** The bump is conservative (it is purely
additive, so a v1 loader against a v2 dylib would still resolve all v1 symbols),
but the version gate is the contract: an AROS side compiled for one version must
not bind a dylib built for the other without an explicit decision. `[ABI]` now
asserts `cm_abi_version() == 2` with 13 names resolving, errcount 0.

---

## 8. Split & status checklist

| # | Deliverable | Owner | Status |
|---|---|---|---|
| 1 | `cocoametal.dylib` (dlopen-loadable, `cm_*` exported) | John | ✅ `make cocoametal-dylib` → `build/cocoametal.dylib`; 13 `cm_*` exported default-visibility (via `cocoametal.exports`; +3 at v2: `cm_set_option`/`cm_get_option`/`cm_open_settings`), unstripped, ad-hoc signed; test `.m` files NOT in it; no AROS headers |
| 2 | **dlopen-based ABI conformance test** (open→upload→present→readback→pump→close, asserts oracle) | John | ✅ `make cocoametal-abi` → `[ABI] PASS` (errcount=0, all 13 dlsym'd, `cm_abi_version()==2`, §6 oracle quadrants+marker exact, option round-trip + open_settings sane) — `hosted/cocoametal/abi_test.c` |
| 3 | D2 under the **real threading model** (main pthread, manual CFRunLoop, no NSApplicationMain) | John | ✅ `make cocoametal-d2t` → `[D2t] PASS`; window + `nextDrawable` WORK hand-pumped, oracle exact at 320×200 + 640×512 — finding in §3 — `hosted/cocoametal/d2t_test.m` |
| 4 | Pixel/geometry/scale frozen (§2) | both | ✅ frozen here (BGRA8, logical px, scale via contentsScale) |
| 5 | `keycode2rawkey.table` adapted into the feature folder | John | ✅ — `docs/features/cocoa-metal-display/keycode2rawkey.table` (102 rows; left = `CMEvent.code` macOS VK, right = `RAWKEY_*`; see §5 note) |
| 6 | `CM_ABI_VERSION` + `cm_abi_version()` in the header (§7) | John | ✅ `#define CM_ABI_VERSION 2` (v1→v2 at the settings ABI) + `int cm_abi_version(void)` in `cocoametal.h`; implemented in `cocoametal.m`; symbol-array entry 9 (append-only) |
| 7 | **Settings panel + key/value option ABI (§9)** — `cm_set_option`/`cm_get_option`/`cm_open_settings` (entries 10/11/12), `CM_EV_SETTING`, host-acted vs AROS-pull split, `NSUserDefaults` persistence | John | ✅ `make cocoametal-settings` → `[SET] PASS`; host-owned option acts on the live present (oracle unchanged), AROS-facing key surfaces as `CM_EV_SETTING`, `NSUserDefaults` round-trips — `hosted/cocoametal/cocoametal_settings.m` + `settings_test.m`. ABI v2; `[ABI] PASS` now 13 symbols / `cm_abi_version()==2` |
| 8 | **D3 host-support: pixfmt pinned (§2a) + HIDD-shaped harness green** | John | ✅ §2a pins the exact `CMPixelDesc` + call-mapping table; `make cocoametal-hiddsim` → `[HIDDSIM] PASS` — dlopens `build/cocoametal.dylib` (real boundary), AROS-owned framebuffer, lazy `cm_open` on first Show, a dirty-rect stream of partial/overlapping `cm_upload_rect`+`cm_present` composes **byte-exact** vs an independent host reference (256000 B), and a known-pixel round-trip proves B@0/G@1/R@2/A@3 with no swizzle — `hosted/cocoametal/hiddsim_test.c`. Additive (no ABI change). |
| A | `arch/all-darwin/hidd/cocoa/` — `CocoaGfx`/`CocoaBM`, `HostLib_Open`+`GetInterface`, `UpdateRect→cm_present`, `Show→cm_open`, `CreateObject` injects `aHidd_BitMap_ClassPtr` | me | ☐ — starts after the CLI prompt is banked. Host reference: §2a call-mapping table + `[HIDDSIM]` |
| B | Input kbd/mouse hardware drivers consuming `CMEvent` | me | ☐ — also the interactivity hedge for the console. **Host side ready:** `cm_pump_events` is now REAL (`make cocoametal-input` → `[D4D5] PASS`; mouse + keyboard CMEvents asserted field-exact, in-process synthetic injection, no TCC — §5 note) |
| C | `AddDisplayDriverA` + pixfmt/sync/mode taglists; kickstart/monitor integration | me | ☐ |

D1/D2 host shim already proven green (`run/d1-`, `d2-cocoametal.png`). **The seam
items 1–3 (+ 6, 7) are now green** (`[ABI] PASS` at v2, `[D2t] PASS`, `[SET] PASS`,
`build/cocoametal.dylib` exports all 13 `cm_*`): the dylib loads via `dlopen`+`dlsym`
with `errcount==0`, the ABI version handshakes, the §6 oracle is exact, and
`cm_open`/`cm_present`/`cm_pump_events`/`cm_open_settings` run under the real
main-pthread/manual-CFRunLoop model (no `NSApplicationMain`). D3 (rows A–C) is now
pure wiring against this contract.

---

## 9. Settings & options — FROZEN (added at v2)

A native host **settings panel** plus a **key/value option ABI**. The split is the
contract: the **host** owns presentation (effect/scale/fullscreen/filter) and acts
on those immediately; **AROS** owns everything functional (mode, keymap, volume),
which the panel can only *request* — surfaced to the AROS side as a pull event,
never acted on by the host (keeps §3's no-callbacks-into-AROS rule).

### 9a. The three new calls (entries 10/11/12, append-only)

- `int cm_set_option(CMContext*, int key, long value)` — set an option.
  **Host-acted keys** take effect immediately on the **live present only** (never
  the offscreen oracle — the oracle stays pass-through/nearest per §6).
  **AROS-facing keys** are **not acted on**: the shim records the requested value
  (so `cm_get_option`/the panel can show it) and **enqueues a `CM_EV_SETTING`**.
  Returns 0 on success, nonzero for an unknown key / out-of-range value.
- `int cm_get_option(CMContext*, int key, long *value)` — read the current
  host-side value (host-acted keys = the live value; AROS-facing keys = the
  last-requested value). 0 / nonzero(unknown key).
- `int cm_open_settings(CMContext*)` — open the native AppKit panel, best-effort,
  on the display-server task under the manual `CFRunLoop` (like `cm_open`'s
  window). Idempotent (re-fronts if already up); degrades to a **no-op returning
  nonzero** with no window server. Non-blocking.

### 9b. The `CMOption` keys (numbered with gaps so each group grows independently)

| Key | Value | Group | Host behaviour |
|---|---|---|---|
| `CM_OPT_EFFECT` = 0x00 | a `CMEffect` (`CM_FX_*`) | **host-acted** | sets the present-time effect (== `cm_set_effect`) |
| `CM_OPT_SCALE_MODE` = 0x01 | a `CMScaleMode` | **host-acted** | live-present viewport: `FIT` (stretch) / `INTEGER_NEAREST` (largest integer multiple, centred) / `PIXEL_PERFECT` (1:1, centred) |
| `CM_OPT_FULLSCREEN` = 0x02 | 0/1 | **host-acted** | recorded for the live present (the present path consults it) |
| `CM_OPT_FILTER` = 0x03 | a `CMFilter` | **host-acted** | live-present sampler: `NEAREST` / `LINEAR` (oracle stays nearest) |
| 0x04–0x0F | — | reserved (host) | future host-acted keys |
| `CM_OPT_REQUEST_MODE_W` = 0x10 | width px | **AROS-facing (stub)** | NOT acted on → `CM_EV_SETTING` |
| `CM_OPT_REQUEST_MODE_H` = 0x11 | height px | **AROS-facing (stub)** | NOT acted on → `CM_EV_SETTING` (paired with W) |
| `CM_OPT_KEYMAP` = 0x12 | keymap id | **AROS-facing (stub)** | NOT acted on → `CM_EV_SETTING` |
| `CM_OPT_AUDIO_VOLUME` = 0x13 | 0..100 | **AROS-facing (stub)** | NOT acted on → `CM_EV_SETTING` |
| 0x14+ | — | reserved (AROS) | future AROS-facing keys |

### 9c. The `CM_EV_SETTING` contract (pull surface; §5 append)

Appended to `CMEventType` **after `CM_EV_RESIZE`** (append-only). Emitted by
`cm_pump_events` when the user changed an AROS-facing setting (panel or a direct
`cm_set_option` of an AROS-facing key). **Packing:**

- `code` = the `CMOption` key,
- `x` = the new value (e.g. `CM_OPT_REQUEST_MODE_W` → width),
- `y` = a 2nd value when the key carries a pair (e.g. `CM_OPT_REQUEST_MODE_H`
  carries height in `x` **and the partner width in `y`** so the AROS side gets a
  complete W×H), else 0,
- `pressed` = 0, `mods` = 0 (unused).

**Pull-only:** the shim never calls into AROS — it enqueues, the AROS side drains
via `cm_pump_events` exactly like input (§5). Settings events are drained ahead of
NSEvents in the same `cm_pump_events` call, in the AppKit-free TU, so a **headless**
build (no window server) still delivers them.

### 9d. The panel + persistence

`hosted/cocoametal/cocoametal_settings.m` — a native `NSWindow` with controls for
the host-owned settings (scanline effect on/off, scale-mode popup, fullscreen
checkbox, filter popup) wired to `cm_set_option`, plus an AROS-owned resolution
popup that routes through `cm_set_option(CM_OPT_REQUEST_MODE_W/H)` (i.e. only ever
enqueues `CM_EV_SETTING` — never changes resolution itself). Host-owned state is
**persisted via `NSUserDefaults`** under `cocoametal.*` keys: **loaded + applied at
`cm_open`** (`cm__apply_persisted_options`, a strong override; weak no-op headless)
and **saved on every change** in the control actions. The AROS-facing requests are
transient and **not** persisted. The panel lives on the single display-server /
main thread (§3), under the same hand-pumped `CFRunLoop` proven at D2t, and is a
strong override of a weak stub so the AppKit-free TU pulls no AppKit headers.

### 9e. Verification — `[SET] PASS` (`make cocoametal-settings`)

`hosted/cocoametal/settings_test.m`, under the D2t threading model, asserts:
- **host-acted**: `cm_set_option(CM_OPT_EFFECT, CM_FX_SCANLINE)` → `cm_present` →
  the **presented** path reflects the effect (odd target rows darker, via
  `cm_render_effect_readback`) while the **offscreen oracle** (`cm_readback`) is
  byte-for-byte pass-through unchanged; `cm_get_option` reflects it.
- **AROS-pull**: `cm_set_option(CM_OPT_REQUEST_MODE_W=640, _H=512)` → host did NOT
  act (`cm_target_size` unchanged) → `cm_pump_events` returns two `CM_EV_SETTING`
  events carrying the keys/values (H event carries the paired W in `y`).
- **persistence**: seed `NSUserDefaults`, simulate a reopen (fresh `cm_open`
  re-reads + applies the defaults), assert the four host options restored; cleans
  the test keys afterward.

**UNVERIFIED / deviations:** `CM_OPT_FULLSCREEN` is **recorded** but the live
present does not yet enter a real AppKit fullscreen mode (no
`toggleFullScreen:`/`NSWindowStyleMaskFullScreen` plumbing under the manual-
CFRunLoop model — left for the D3 wiring); the option round-trips and persists, but
visually it is a stored flag, not yet an entered fullscreen state. The panel
degrades to a no-op (returns nonzero) in a headless launch context, same caveat as
`cm_open`'s window (§3).
