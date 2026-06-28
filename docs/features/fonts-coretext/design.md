# Fonts (CoreText) — the Mac's installed fonts inside AROS

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

Give the hosted AROS access to the Mac's installed fonts. macOS ships a deep library
of TrueType/OpenType faces (`/System/Library/Fonts`, `/Library/Fonts`,
`~/Library/Fonts`); AROS already has a complete *scalable*-font stack — what's missing
is the bridge that lets AROS enumerate and render those faces. This is the typography
instance of the standing thesis: *"macOS owns the drivers; AROS reaches them via
standard exec I/O."* The Mac owns the font library and (optionally) the rasterizer;
AROS reaches them through the existing `diskfont`/`bullet` glyph-engine contract and a
`hostlib.resource` shim.

The feature earns a doc because it has **two genuinely different shapes**, and the
honest call is that the *cheap* one is the real first win:

- A **font-provisioning bridge** (MVP) that re-uses almost all of the in-tree outline
  path — CoreText just tells us *which files* to register, and the already-built
  [host-volume](../host-volume/design.md) bridge carries them into `FONTS:`. Risk:
  low, because AROS does the rendering with code that already works.
- A **CoreText-backed outline engine** (deeper) that rasterizes glyphs *through*
  CoreText so AROS text is pixel-identical to macOS. Risk: higher, but it slots into a
  **verified clean seam** (the `.otag`'s `OT_Engine` tag names the engine library by
  string; `diskfont` `OpenLibrary`s it and calls the same five LVOs `freetype2.library`
  exports — confirmed below).

Both verify the same unattended way: enumerate, open, rasterize a known glyph, and
**numerically compare** the AROS-rendered bitmap to a reference. No human, no TCC.

## Does it already exist?

**Partly — the AROS rendering machinery is all present; the host bridge is not.**

- This repo: **no font code at all** beyond a single boot line
  (`graft/run-window.sh:126` writes `If EXISTS "SYS:Fonts" / Assign "FONTS:" "SYS:Fonts"`
  into the Startup-Sequence). `grep -ri 'coretext|diskfont|bullet|freetype|OpenFont|
  AvailFonts|FONTS:'` over `/Users/user/Source/aros-aarch64` returns only that line. So
  there is **no CoreText shim, no host font enumeration, no engine bridge** today.
- Upstream `/Users/user/Source/aros-upstream` ships **every AROS-side piece we reuse**:
  - `graphics.library` — the consumer API (`OpenFont`/`AskFont`/`Text`,
    `rom/graphics/`).
  - `diskfont.library` (`workbench/libs/diskfont/`, basename **Diskfont** v50.3) —
    `OpenDiskFont`, `AvailFonts`, `NewFontContents`; the `.font`/`.otag` loaders.
  - `bullet.library` (`workbench/libs/bullet/`, basename **Bullet** v41.0) — the
    **engine *interface*** (`OpenEngine`/`CloseEngine`/`SetInfoA`/`ObtainInfoA`/
    `ReleaseInfoA`); the bodies are empty stubs ("This interface is implemented in
    freetype2.library" — `openengine.c:37`).
  - `freetype2.library` (`workbench/libs/freetype2/`, basename **FreeType2** v6.9) —
    the **actual** in-tree TrueType/OTF rasterizer that implements that interface
    (`glyph.c`, `setinfoa.c`, `obtaininfoa.c`).
  - `ftmanager` (`workbench/system/ftmanager/`) — the tool that registers a `.ttf`/
    `.otf` into `FONTS:` by writing the `.otag` + `.font` pair.
  - The bundled outline fonts under `workbench/fonts/truetype/*` (e.g. Bitstream Vera),
    each a `.ttf` + `.otag` + `.font` triple — the exact layout the MVP mirrors.

So the work is: **(MVP)** a small host font-enumeration shim + a provisioning step that
copies/exposes host `.ttf`/`.otf` into `FONTS:` and runs the existing `ftmanager`/
freetype path; **(deeper)** one new `coretext.library` engine + its CoreText shim. The
`diskfont`/`bullet`/`graphics` machinery is reused unchanged.

**External prior art (web-grounded, *not* in the AROS tree).** The upstream AROS-darwin
ports (i386/x86_64/ppc) are X11-bound and use freetype for outlines; **no CoreText font
backend exists anywhere** — this is new ground for `darwin-aarch64`, but the *engine
contract* it plugs into is unchanged from the proven freetype path. (To be reconfirmed
against upstream HEAD at spec time; **UNVERIFIED** that no CoreText engine has appeared
out-of-tree.)

## Background: AROS font contracts (grounded)

AROS has a **three-tier** font stack. The consumer never names an engine; the engine is
selected by data in the `.otag`. Getting the tiers right is what makes the deeper layer
a drop-in.

### Tier 1 — `graphics.library`: the consumer API (NOT a hook point, but the proof surface)

The public face every program uses:

- `struct TextFont` / `struct TextAttr` / `struct TTextAttr` —
  `compiler/include/graphics/text.h` (`TextFont` :22, `TextAttr` :72, `TTextAttr` :80).
  A loaded font is a `TextFont` with a glyph bitmap in `tf_CharData` (modulo
  `tf_Modulo`), per-char offsets in `tf_CharLoc` (low 16 bits = width, high 16 = bit
  position — `rom/graphics/text.c:195-198`), and metrics `tf_YSize`/`tf_Baseline`/
  `tf_XSize`.
- `OpenFont` (`rom/graphics/openfont.c:19`) scans `GfxBase->TextFonts`
  (`compiler/include/graphics/gfxbase.h:53`) by name/style/size via `WeighTAMatch`. It
  finds only fonts **already in memory** (ROM fonts + disk fonts that were `AddFont`'d).
- Disk fonts enter that list because `diskfont` calls `AddFont(tf)` after loading
  (`workbench/libs/diskfont/diskfontfunc.c:1270-1282`), setting `FPF_DISKFONT`.
- Font flags `FPF_*` (`text.h:107-123`): `FPB_ROMFONT`=0, `FPB_DISKFONT`=1,
  `FPB_PROPORTIONAL`=5, `FPB_DESIGNED`=6; style `FSF_*` (`text.h:92-105`): `FSB_BOLD`=1,
  `FSB_ITALIC`=2, `FSB_TAGGED`=7 (font carries a `TTextAttr` tag list).
- `Text()` (`rom/graphics/text.c:76`) blits the glyph bitmap. It has an **antialiased**
  path (`BltTemplateAlphaBasedText`, `text.c:351`, for `CT_ANTIALIAS` fonts) — relevant
  because outline-derived fonts can carry 8-bit coverage.

**Verdict:** `graphics.library` is unchanged. It is where the *final* assertion lands
(open a host font, render a glyph, compare pixels), not where we hook.

### Tier 2 — `diskfont.library`: enumeration, `.font`/`.otag` loading, engine selection (the hook for the MVP)

`diskfont.library` (`workbench/libs/diskfont/`) is the disk side: it scans `FONTS:`,
parses the on-disk descriptors, and decides bitmap vs. outline.

- **`AvailFonts(buffer, bufBytes, flags)`** (`availfonts.c:36`) enumerates **memory**
  fonts (`MF_Iterator*`) and **disk** fonts (`DF_Iterator*`, scanning `FONTS:` =
  `FONTSDIR`, `diskfont_intern.h`) into `struct AvailFonts` records
  (`compiler/include/diskfont/diskfont.h:78`), tagged `AFF_MEMORY`(0x1)/`AFF_DISK`(0x2)/
  `AFF_SCALED`(0x4)/`AFF_TAGGED`(0x10000). **This is the enumeration the MVP must light
  up** — after provisioning, the host faces must appear here.
- **`OpenDiskFont(textAttr)`** (`opendiskfont.c:22`) — tries `OpenFont` first (memory),
  else iterates `FONTS:` for the best `WeighTAMatch`, then `DF_IteratorRememberOpen`
  loads it and `AddFont`s it.
- **The `.font` / `.otag` descriptors** (`diskfont.h`): `FontContentsHeader.fch_FileID`
  (`:45`) is `FCH_ID`(0x0f00, bitmap), `TFCH_ID`(0x0f02, tagged), or `OFCH_ID`(0x0f03,
  **outline**). `NewFontContents`/`ReadFontDescr` (`newfontcontents.c:45`,
  `af_fontdescr_io.c:28`) parse them; the outline branch reads the `.otag`.
- **Bitmap strikes**: a `.font` points at numbered strike files (`Topaz.font` +
  `Topaz/8`, `/9`, `/11`); each strike is a seglist with a `struct DiskFontHeader`
  (`diskfont.h:54`, `dfh_FileID`=`DFH_ID` 0x0f80) loaded by `ConvDiskFont`
  (`diskfont_io.c:79`).
- **The outline branch — the seam to the engine.** For `OFCH_ID`,
  `OTAG_ReadOutlineFont` (`bullet.c:898`) does, *verbatim*:
  ```c
  enginename = GetTagData(OT_Engine, NULL, otag->tags);   /* bullet.c:911 */
  strcpy(enginenamebuf, enginename); strcat(enginenamebuf, ".library"); /* :917-918 */
  BulletBase = OpenLibrary(enginenamebuf, 0);             /* :920  */
  ge = OpenEngine();                                      /* :925  (from that lib) */
  OTAG_SetupFontEngine(... ge ...);                       /* :933  -> SetInfoA */
  OTAG_GetGlyphMaps(ge, gm, ...);                         /* :982  -> ObtainInfoA */
  ```
  **The engine library is named by a string in the `.otag`.** Today every bundled
  `.otag` says `OT_Engine = "freetype2"`, so `diskfont` opens `freetype2.library`. A
  `.otag` that said `OT_Engine = "coretext"` would make `diskfont` open
  `coretext.library` and call the identical five LVOs — **this is the verified clean
  seam the deeper layer uses**, with zero change to `diskfont` or `bullet`.

### Tier 3 — the glyph engine: `bullet.library`'s interface, `freetype2.library`'s body (the hook for the deeper layer)

The engine contract is five LVOs over a `struct GlyphEngine` (`diskfont/glyph.h:24`):

```
struct GlyphEngine *OpenEngine(void)                         (bullet.conf, LVO 5)
void   CloseEngine(struct GlyphEngine *)                     (A0)
ULONG  SetInfoA  (struct GlyphEngine *, struct TagItem *)    (A0, A1)
ULONG  ObtainInfoA(struct GlyphEngine *, struct TagItem *)   (A0, A1)
ULONG  ReleaseInfoA(struct GlyphEngine *, struct TagItem *)  (A0, A1)
```

`bullet.library`'s bodies are **empty** (the `.conf` declares the LVOs; the `.c` files
are stubs — `openengine.c:37`, `setinfoa.c`, `obtaininfoa.c`). The real engine is
whatever library the `.otag` names; `freetype2.library` is that engine today and is the
template:

- **`OpenEngine`** (`freetype2/openengine.c:15`) `AllocGE()`s an `FT_GlyphEngine`
  (`ftglyphengine.h`), sets `gle_Name = "freetype2"`.
- **`SetInfoA`** (`freetype2/setinfoa.c`) `scantags()` the request: `OT_Spec1_FontFile`
  → the `.ttf`/`.otf` path (`FT_New_Face`), `OT_PointHeight` (size, 16.16 fixed),
  `OT_DeviceDPI`, `OT_ShearSin/Cos` (italic), `OT_EmboldenX/Y` (bold), `OT_GlyphCode`
  (the character).
- **`ObtainInfoA`** (`freetype2/obtaininfoa.c`) services `OT_GlyphMap8Bits` (8-bit AA)
  / `OT_GlyphMap` (1-bit): `GetGlyph` → `SetInstance` (`FT_Set_Char_Size`) →
  `UnicodeToGlyphIndex` (`FT_Get_Char_Index`) → `RenderGlyph` (`glyph.c:173`:
  `FT_Load_Glyph` + `FT_Outline_Render` into an 8-bit `FT_PIXEL_MODE_GRAY` buffer),
  then fills a **`struct GlyphMap`** (`glyph.h:32`).
- **The output struct — `struct GlyphMap`** (`glyph.h:32-48`), the byte-exact contract
  the deeper layer must fill:

  | field | meaning |
  |-------|---------|
  | `glm_BMModulo` | bytes per row of the coverage bitmap |
  | `glm_BMRows`   | rows |
  | `glm_BlackLeft/Top/Width/Height` | tight ink bounding box |
  | `glm_X0/Y0/X1/Y1` | advance/positioning metrics (pixels) |
  | `glm_Width`    | advance width (16.16 `FIXED`) |
  | `glm_BitMap`   | pointer to the 8-bit (or 1-bit) coverage buffer |

  `diskfont`'s `OTAG_GetGlyphMaps`/`OTAG_MakeCharData` (`bullet.c`) then pack these
  `GlyphMap`s into the `TextFont`'s `tf_CharData`. **A CoreText engine must produce a
  `GlyphMap` with this exact field semantics from a `CTFontDrawGlyphs` rasterization.**

- **OT tags** (`compiler/include/diskfont/diskfonttag.h`, values in `glyph.h` /
  `ftglyphengine.h`): `OT_Engine`, `OT_Spec1_FontFile`, `OT_PointHeight`,
  `OT_DeviceDPI`, `OT_GlyphCode`, `OT_GlyphMap8Bits`, `OT_Spec6_FaceNum` (index into a
  `.ttc`), `OT_Spec4_Metric` (which metric to use for the box). Errors:
  `diskfont/oterrors.h` (`OTERR_Success`=0, `OTERR_NoGlyph`=7, `OTERR_NoFace`=5, …).

### `ftmanager` — the existing registration path the MVP rides

`ftmanager` (`workbench/system/ftmanager/`) is the tool that turns a `.ttf`/`.otf` into
an AROS-loadable font, by writing the descriptor pair `diskfont` reads:

- **CLI** (`cli.c:15`): `COPY/S, TTFFONT/A, CODEPAGE/K, TO/K, FONTDIR/K, OUTFONT/K`.
  Default `FONTDIR` = `FONTS:`; default `TO` (the `.ttf` copy target) =
  `FONTS:TrueType` (`cli.c:46-48,227`). So a registered face becomes
  `FONTS:<name>.otag` + `FONTS:<name>.font`, with the `.ttf` under `FONTS:TrueType/`.
- **The `.otag` writer** (`fontinfo_class.c:fiWriteFiles`, ~`:741`): two big-endian
  ULONGs (`OT_FileIdent`, total size) then big-endian `{ti_Tag, ti_Data}` pairs, with
  indirect data (strings: engine name `"freetype2"`, family, the `.ttf` path) packed in
  a trailing section and referenced by offset; `OT_AvailSizes` appended. The companion
  **`.font` stub** is 4 bytes `0x0f 0x03 0x00 0x00` (= `OFCH_ID`) — a marker that says
  "this is an outline font, read the `.otag`".
- **Where the `OT_Engine` string is set** (`fontinfo_class.c`, ~`:585`):
  `OT_Engine = "freetype2"`. **The MVP keeps `"freetype2"`; the deeper layer flips it to
  `"coretext"`** to retarget the same machinery.

`ftmanager` is a GUI app but has the `cli.c` non-interactive entry, so it scriptable
from the Startup-Sequence — the MVP drives that CLI.

### Reference points already de-risked in this repo

- **Host-symbol resolution**: `hostlib.resource` (`HostLib_Open`/`HostLib_GetPointer`,
  `arch/all-unix/bootstrap/hostlib.h`, impl `arch/all-hosted/hostlib/`). Proven by
  the coreaudio/bsdsocket/clipboard shims, which each `dlopen` a native arm64 `.dylib`
  and fill a function-pointer table from a frozen `.exports` list. The CoreText shim is
  the same shape.
- **Host shim ABI convention** (`hosted/coreaudio/`, `hosted/bsdsocket/`,
  `hosted/clipboard/`): a flat C ABI in a `*_shim.h`, a native `.c`/`.m` body, a
  `*.exports` symbol allowlist, an `abi_test.c` that links through the dylib and asserts
  a `[X] PASS` marker via `harness/run-hosted.sh`. We mirror it exactly.
- **Font-file delivery into AROS**: the **[host-volume](../host-volume/design.md)**
  bridge is built and verified — `AROS_HOST_VOLUME="Name:/host/path[;WRITE]"` mounts a
  Mac folder as an AROS volume (`graft/run-window.sh` sets up `MacRO:`/`MacRW:`). This
  is the MVP's cheapest delivery path: mount the host font dir (or a staged copy) so the
  `.ttf`/`.otf` files appear under `FONTS:TrueType/` without copying.
- **Driving the booted AROS**: `aros-ctl` (the [control harness](../control-harness/design.md))
  + Startup-Sequence drive the shell headlessly and screenshot/log — used to run
  `AvailFonts`/`ftmanager` and read the result.

## Design

Two layers. Recommended order: **MVP first** (it produces a real, asserted win with
almost no new code), then the deeper engine if pixel-identical rendering is wanted.

### Layer A (MVP) — font provisioning: CoreText says *which*, AROS renders

The Mac is the *catalog*; AROS is the *renderer* (its existing freetype path). The only
new host code is enumeration.

**Host side — a tiny CoreText enumeration shim** (`hosted/fonts/`, native arm64,
peer of `hosted/coreaudio/`; reached via `hostlib.resource`). Verbs (snake_case +
prefix, like `ca_*`):

```
fn_enumerate()                    -> int count   (build an internal list of host faces)
fn_get(int i, struct fn_face *)   -> int         (path, family, style, weight, isTTC, faceIndex)
fn_free()
```

Implementation: `CTFontManagerCopyAvailableFontURLs` /
`CTFontManagerCopyAvailableFontFamilyNames`, or a `CTFontCollection`, to list faces and
their backing file URLs; resolve each URL to a filesystem path under
`/System/Library/Fonts`, `/Library/Fonts`, `~/Library/Fonts`. (Fallback that needs **no
CoreText at all**: just `readdir` those three dirs for `*.ttf`/`*.otf`/`*.ttc` — even
cheaper, and a useful first cut. CoreText buys correct family/style/weight metadata and
de-duplication.) Filter to file-backed `.ttf`/`.otf`/`.ttc` (skip `.dfont`/system
bitmap-only oddities for the first cut).

**Delivery into `FONTS:`** — reuse host-volume:
1. Mount the host font directory(ies) as an AROS volume (e.g.
   `AROS_HOST_VOLUME="MacFonts:/System/Library/Fonts"`, read-only) so the `.ttf`/`.otf`
   appear in AROS without copying; or stage a curated subset into the existing
   `MacRW:` shared folder.
2. For each face, run **the existing `ftmanager` CLI** to write `FONTS:<name>.otag` +
   `FONTS:<name>.font`, pointing `OT_Spec1_FontFile` at the mounted `.ttf` and keeping
   `OT_Engine = "freetype2"`. (`.ttc` collections use `OT_Spec6_FaceNum` for the
   sub-face — `ftmanager` already has the tag.) This is a Startup-Sequence loop or a
   small provisioning helper that shells `ftmanager` per face.
3. `diskfont` now enumerates and opens them with **zero new AROS code** — it's the same
   path the bundled Vera fonts use.

**Why this is the real first win:** every rendering byte is produced by code that
already works (`freetype2.library`), so the only thing that can be wrong is the
*provisioning* (did the right files land in `FONTS:`, did `ftmanager` write a valid
`.otag`). That is exactly what the loop can assert cheaply. The MVP also doubles as the
test fixture for the deeper layer (same faces, same glyphs, a freetype reference to
compare CoreText against).

**MVP open question (verify, don't assume):** whether running `ftmanager` per-face is
the lightest provisioning, or whether a small dedicated helper that writes the `.otag`/
`.font` pair directly (the format is fully grounded above) is cleaner for a bulk import.
Lean toward `ftmanager` first (no new AROS binary), profile later.

### Layer B (deeper) — a CoreText-backed engine: AROS text == macOS text

A new AROS engine library `coretext.library` that implements the **same five-LVO glyph
contract** `freetype2.library` does, but rasterizes via CoreText/CoreGraphics through a
host shim. Because `diskfont` selects the engine by the `.otag`'s `OT_Engine` string
(verified, `bullet.c:911-920`), this is a drop-in: flip `OT_Engine` to `"coretext"` and
`diskfont`/`bullet`/`graphics` are untouched.

**AROS side — `coretext.library`** (modelled on `freetype2.library`, host-agnostic AROS
code except for the shim calls):

- `OpenEngine` → alloc a `CTGlyphEngine` (our analogue of `FT_GlyphEngine`), `gle_Name
  = "coretext"`; `ct_open()` the host shim handle.
- `SetInfoA` → `scantags()`: `OT_Spec1_FontFile` (+ `OT_Spec6_FaceNum`) →
  `ct_set_face(path, faceIndex)`; `OT_PointHeight`/`OT_DeviceDPI` → size in px;
  `OT_ShearSin/Cos`/`OT_EmboldenX/Y` → a synthetic transform passed to the shim;
  `OT_GlyphCode` → the requested character. Same tag set freetype reads.
- `ObtainInfoA` → on `OT_GlyphMap8Bits`: `ct_render_glyph(char, size, &out)` and pack
  the returned 8-bit coverage buffer + metrics into a `struct GlyphMap` with the
  field semantics above (`glm_BMModulo`/`glm_BMRows`/`glm_Black*`/`glm_Width`/
  `glm_BitMap`).

**Host side — the CoreText rasterizer shim** (`hosted/fonts/`, extends Layer A's shim):

```
ct_open()                         -> APTR handle
ct_set_face(h, path, faceIndex)   -> int     (CTFontCreateWithName / from file URL)
ct_render_glyph(h, uint32 codepoint, float pxSize, struct ct_glyph *out) -> int
ct_close(h)
```

`ct_render_glyph`: get the glyph for the code point (`CTFontGetGlyphsForCharacters`),
size a `CGBitmapContext` (8-bit grayscale, one component), `CTFontDrawGlyphs` (or
`CGContextShowGlyphsAtPositions`) into it, return the coverage buffer + tight bbox +
advance. This is plain CoreGraphics drawing into a memory bitmap — **no window, no TCC**.

**Format / coordinate mapping (the load-bearing correctness work):** CoreText's origin
is bottom-left, y-up; the `GlyphMap`/AROS bitmap is top-down. CoreText coverage is
8-bit grayscale (premultiplied alpha against a cleared context); `GlyphMap` `glm_BitMap`
is 8-bit coverage with `glm_BMModulo` stride. The engine must flip Y, set
`glm_Black*` to CoreText's ink bbox, and convert the advance (`CTFontGetAdvancesForGlyphs`)
to `glm_Width` as 16.16 `FIXED`. Hinting/antialiasing differences mean CoreText output
will **not** be bit-identical to FreeType — verification compares *coverage/bbox within
tolerance*, not exact bytes (see verification).

### The bridge (host-call discipline)

Both layers cross the same boundary the other shims use: AROS-built code (AAPCS64)
calling a native arm64 `.dylib` via `hostlib.resource`. Discipline, grounded in the
existing shims:

- Resolve the `fn_*`/`ct_*` symbols once at engine open via `HostLib_GetPointer` from
  the frozen `fonts.exports` list (errcount must be 0 or the engine refuses to load) —
  exactly the coreaudio/bsdsocket pattern.
- Serialize host calls under `HostLib_Lock`/`Unlock` with the `AROS_HOST_BARRIER` fence
  after return (the host-volume overlay precedent). Font calls are fixed-arg (no
  variadic), so the H3 `abishim.S` variadic shim is **not** needed.
- **No RT/second-thread problem** (unlike CoreAudio): font rasterization is **pull** —
  AROS asks, the shim rasterizes synchronously and returns. CoreText drawing into a
  `CGBitmapContext` runs on the calling thread. This is the simple host-call shape (like
  the host-volume `Do*` calls), not the audio RT-callback shape.
- **Caching**: `diskfont` renders a whole strike (all glyphs for one size) at
  `OpenDiskFont` time, then `Text()` blits from `tf_CharData` — so the per-glyph host
  call happens **once per (face,size)**, not per draw. Performance is a non-issue for
  the deeper layer; the host round-trips are amortized at font-open.

## Plan — spikes in the loop

One standalone host binary per marker (the `hosted/*` style), single PASS/FAIL the
agent greps. Markers **[FN1]…[FN7]**. Markers 1-4 are the MVP; 5-7 are the deeper
engine.

- **[FN1] host enumerates the Mac's fonts (pure host probe).** `hosted/fonts/` builds
  `fn_enumerate`/`fn_get` and prints the count + a few `(family, style, path)`. PASS =
  count ≥ a known floor (e.g. ≥ 20 file-backed faces) and every reported path `stat`s as
  an existing `.ttf`/`.otf`/`.ttc`. Proves the CoreText (or `readdir`) catalog before any
  AROS is involved. *(grounds the host side, like H7's `pngprobe`.)*
- **[FN2] freetype reference rasterization, asserted (pure host).** Rasterize a known
  glyph (e.g. `'A'` of a chosen bundled `.ttf` at a fixed px size) with **FreeType** in
  a host binary; assert coverage sum / ink-bbox against a stored golden. This is the
  **reference oracle** [FN4]/[FN7] compare AROS against. *(no AROS; pins the ground
  truth.)*
- **[FN3] provisioned host font appears in `AvailFonts` (booted AROS).** Provision one
  host face into `FONTS:` (mount via host-volume + run `ftmanager`), boot AROS, drive
  `AvailFonts` (a small client, or `C:` font lister) from the Startup-Sequence. PASS =
  the host face's name appears in the `AvailFonts` output with the right
  size/style/`AFF_DISK`. This is the MVP's enumeration win.
- **[FN4] `OpenDiskFont` + glyph rasterized through AROS matches FreeType reference.**
  Same provisioned face; `OpenDiskFont` it, render `'A'` at the [FN2] size through the
  AROS path (`diskfont`→`freetype2`→`GlyphMap`), and compare the resulting glyph bitmap
  to the [FN2] golden — **pixel-coverage / bbox / checksum** within tolerance. PASS =
  AROS rendered the *host file* and the bitmap matches the reference. **This is the full
  MVP thesis end-to-end** (a Mac font, rendered by AROS, proven correct).
- **[FN5] CoreText rasterizer shim, asserted (pure host).** `ct_render_glyph` draws a
  known glyph into a `CGBitmapContext`; assert coverage sum / ink-bbox is non-empty and
  within a *loose* tolerance of the [FN2] FreeType reference (they differ by
  hinting/AA — tolerance, not equality). Proves the deeper host path with no AROS.
- **[FN6] `coretext.library` satisfies the engine contract (booted AROS).** Build the
  new engine; a host face's `.otag` rewritten with `OT_Engine = "coretext"`;
  `OpenDiskFont` it. PASS = it opens (no `OTERR_NoFace`/`NoGlyph`), `diskfont` accepts
  the `GlyphMap`s, and `AvailFonts` still lists it. Proves the seam took the new engine.
- **[FN7] AROS-via-CoreText glyph == macOS CoreText reference.** Render `'A'` through the
  AROS CoreText engine and compare to the [FN5] host CoreText reference (the *same*
  rasterizer) — this should be **near-exact** (same engine, same path), unlike the
  loose FreeType compare. PASS = AROS-rendered bitmap matches the host CoreText
  reference within tight tolerance. This is the deeper thesis: AROS text == macOS text.

Build/run them in the existing harness (`make hosted-fonts` / a `fonts-smoke` script →
`[FN?]` markers), clean-exit on PASS.

## How we verify it unattended

No human looks at glyphs; no TCC (rasterizing into an in-memory `CGBitmapContext` and
listing font files need **no** Screen-Recording/Automation entitlement — it's ordinary
process I/O and drawing). The oracle is **rasterize-to-bitmap + numeric assertion**:

1. **Enumeration** ([FN1]/[FN3]): `AvailFonts` (and the host shim) must list the host
   faces; assert count ≥ floor and the named face is present with the right
   size/style/flags.
2. **Open** ([FN4]/[FN6]): `OpenDiskFont` returns non-NULL and the engine returns
   `OTERR_Success` (no `NoFace`/`NoGlyph`).
3. **Rasterization match** ([FN2] reference vs [FN4]/[FN7]): compare the AROS-rendered
   glyph bitmap to a reference rasterization of the **same `.ttf`** by:
   - **ink bounding box** (`glm_Black*` vs reference bbox) — catches wrong size/face;
   - **coverage sum / mean** (total 8-bit alpha) within tolerance — catches blank/garbage;
   - **checksum / per-pixel diff** for the *same-engine* compare ([FN7], CoreText vs
     CoreText) where near-equality is expected; **loose coverage tolerance** for the
     *cross-engine* compare ([FN5], CoreText vs FreeType) where hinting/AA differ.
   A single known glyph at a fixed size pins face + size + "real glyph, not blank".

The references ([FN2] FreeType, [FN5] CoreText) are computed in pure-host binaries with
no AROS in the loop, so the AROS-side assertion is always against a deterministic
golden. Every marker prints `[FN?] PASS …`/`[FN?] FAIL …`; a hung boot is reaped by the
existing bash watchdog; markers are unique per spike so a regression localizes.

## Risks & open questions

- **Does the `bullet`/`OT_Engine` seam *cleanly* accept a non-freetype backend? —
  partly verified, the headline risk.** `diskfont` selecting the engine by the `.otag`
  string and calling `OpenEngine`/`SetInfoA`/`ObtainInfoA` is **confirmed**
  (`bullet.c:911-933,982`). What is **UNVERIFIED**: whether `OTAG_SetupFontEngine`/
  `OTAG_GetGlyphMaps` (`bullet.c`) pass *only* the documented tags, or whether they
  assume freetype-specific tag behaviour (e.g. `OT_Spec2_CodePage`, the codepage table
  in `ftglyphengine.h`) that a CoreText engine must also honour. The deeper layer must
  read `OTAG_SetupFontEngine`/`OTAG_GetGlyphMaps` in full and replicate every tag
  freetype consumes, not just the obvious ones. Mitigation: [FN6] exists precisely to
  prove the seam took the new engine before any pixel compare.
- **`GlyphMap` field semantics under non-freetype rendering.** `freetype2/glyph.c:173`
  is the only worked example of filling `GlyphMap` (modulo, rows, black-box, 16.16
  width, Y orientation). The exact meaning of `glm_X0/Y0/X1/Y1` vs `glm_Black*` and the
  Y-origin convention must be reverse-derived from `glyph.c` + `OTAG_MakeCharData`
  (`bullet.c`) — **UNVERIFIED** until matched. Get the Y-flip or advance wrong and
  glyphs render but misposition (caught by the bbox assertion, not by "it opened").
- **CoreText ≠ FreeType pixels (by design).** Different hinting and AA mean the deeper
  engine will not byte-match the bundled freetype output. Verification accounts for this:
  cross-engine compares ([FN5]) use *loose coverage tolerance*; only the same-engine
  compare ([FN7], AROS-CoreText vs host-CoreText) expects near-equality. Don't write a
  test that demands FreeType==CoreText.
- **`.dfont` / system bitmap faces / `.ttc` collections.** Some macOS faces are
  `.dfont` (datafork suitcase) or bitmap-only or multi-face `.ttc`. First cut filters to
  single-face `.ttf`/`.otf`; `.ttc` via `OT_Spec6_FaceNum`. Whether freetype (MVP) and
  CoreText (deeper) handle every host file is **UNVERIFIED** — enumerate broadly, register
  conservatively, expand once [FN4] is green.
- **`ftmanager` CLI completeness on this target.** The MVP leans on `ftmanager`'s
  non-interactive `cli.c` path writing a valid `.otag`/`.font`. That it builds and runs
  headless on `aarch64-darwin` (it's a GUI app with a CLI entry) is **UNVERIFIED**;
  fallback is a tiny provisioning helper that writes the (fully-grounded) `.otag`/`.font`
  format directly.
- **Charset / encoding.** AROS fonts are historically codepage/Latin-1 indexed
  (`OT_Spec2_CodePage`, the 256-entry `codepage[]` in `ftglyphengine.h`); CoreText is
  Unicode. The engine maps an AROS character code → Unicode code point → CoreText glyph.
  For ASCII/Latin-1 this is the identity-ish map freetype already uses; full Unicode is
  out of scope for the first cut. **UNVERIFIED** whether the existing `OT_Spec2_CodePage`
  handling in `diskfont` constrains the deeper engine.
- **The graft, not a spike.** [FN3]/[FN4]/[FN6]/[FN7] need a booted AROS with
  `diskfont`/`graphics`/`freetype2` up — the same crosstools+`mmake` integration the
  boot milestone began, and the `FONTS:` assign actually resolving. [FN1]/[FN2]/[FN5]
  are session-sized pure-host spikes that stand alone; the AROS-side markers ride the
  graft.
- **Host-call cost is amortized (low risk).** Glyph rendering for the deeper layer is
  pull-only and happens once per (face,size) at `OpenDiskFont` (`diskfont` renders the
  whole strike then `Text()` blits locally), so even hundreds of host round-trips per
  font-open are fine. No RT thread, no second-thread/scheduler hazard.

## References

AROS upstream (`/Users/user/Source/aros-upstream`):
- Consumer API: `compiler/include/graphics/text.h` (`TextFont` :22, `TextAttr` :72,
  `TTextAttr` :80, `FPF_*` :107, `FSF_*` :92), `compiler/include/graphics/gfxbase.h:53`
  (`TextFonts` list); `rom/graphics/openfont.c:19`, `rom/graphics/text.c` (render :76,
  AA :351, glyph layout :195-198), `addfont.c`, `askfont.c`, `setfont.c`.
- `diskfont.library`: `workbench/libs/diskfont/` — `opendiskfont.c:22`, `availfonts.c:36`
  (`AFF_*`, `DF_Iterator*`), `newfontcontents.c:45`, `af_fontdescr_io.c:28`,
  `diskfont_io.c:79` (`ConvDiskFont`), `diskfontfunc.c:1270` (`AddFont`),
  `bullet.c:898-982` (**the `OT_Engine` seam** — `:911` read, `:920` OpenLibrary, `:925`
  OpenEngine, `:982` GetGlyphMaps), `diskfont.conf` (basename Diskfont v50.3).
- Headers: `compiler/include/diskfont/diskfont.h` (`FontContentsHeader.fch_FileID` :45 —
  `FCH_ID`/`TFCH_ID`/`OFCH_ID`; `DiskFontHeader` :54; `AvailFonts` :78),
  `diskfont/diskfonttag.h` (`OT_*` tags), `diskfont/glyph.h` (`GlyphEngine` :24,
  `GlyphMap` :32-48), `diskfont/oterrors.h` (`OTERR_*`).
- Engine: `workbench/libs/bullet/` (`bullet.conf` LVOs/basename Bullet v41.0;
  `openengine.c:37` stub — "implemented in freetype2.library"),
  `workbench/libs/freetype2/` (`openengine.c:15`, `setinfoa.c` `scantags`,
  `obtaininfoa.c` `GetGlyph`, `glyph.c:173` `RenderGlyph`, `ftglyphengine.h`
  `FT_GlyphEngine`/`codepage[]`, `freetype2.conf` basename FreeType2 v6.9).
- Registration: `workbench/system/ftmanager/` (`cli.c:15` template, `:46-48,227,337`
  `FONTS:`/`FONTS:TrueType` defaults; `fontinfo_class.c` `fiWriteFiles` ~`:741` `.otag`
  writer + `.font` stub `0x0f030000`, `OT_Engine="freetype2"` ~`:585`).
- Bundled outline fonts (the layout the MVP mirrors): `workbench/fonts/truetype/*`
  (e.g. `bitstream/Vera Sans.{ttf,otag,font}`).
- Host-symbol mechanism: `arch/all-unix/bootstrap/hostlib.h` (`Host_HostLib_Open`/
  `GetPointer`), `arch/all-hosted/hostlib/`.

This repo (`/Users/user/Source/aros-aarch64`):
- Shim ABI precedents: `hosted/coreaudio/{coreaudio_shim.h,coreaudio.exports,abi_test.c}`,
  `hosted/bsdsocket/`, `hosted/clipboard/` (flat C ABI + `.exports` + `dlopen` via
  `hostlib.resource`); `hosted/abishim.S` (variadic shim — *not* needed here).
- Font-file delivery: `docs/features/host-volume/` + `graft/run-window.sh`
  (`AROS_HOST_VOLUME`, `MacRO:`/`MacRW:`), `graft/hostvol-smoke`.
- Harness: `harness/run-hosted.sh` (`[X] PASS` marker grep), `Makefile`
  (`hosted-coreaudio`/`coreaudio-abi` pattern), `graft/audio-smoke` (end-to-end smoke
  template); `graft/aros-ctl` + `docs/features/control-harness/` (drive the shell).
- Boot grounding: `graft/run-window.sh:126` (`Assign FONTS: SYS:Fonts`).

External prior art (web, not in the AROS tree):
- Apple CoreText: `CTFontManagerCopyAvailableFontURLs`,
  `CTFontManagerCopyAvailableFontFamilyNames`, `CTFontCollection`,
  `CTFontCreateWithName`, `CTFontGetGlyphsForCharacters`, `CTFontDrawGlyphs`,
  `CTFontGetAdvancesForGlyphs`; CoreGraphics `CGBitmapContext` (8-bit grayscale glyph
  rasterization). macOS font locations: `/System/Library/Fonts`, `/Library/Fonts`,
  `~/Library/Fonts`.
- OpenType/TrueType spec (the `.ttf`/`.otf`/`.ttc` container — face index, glyph IDs).
- AROS-darwin upstream ports (i386/x86_64/ppc, X11, freetype outlines, no CoreText
  backend) — to be reconfirmed against upstream HEAD at spec time (**UNVERIFIED**).
