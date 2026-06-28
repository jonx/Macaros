# Implementation spec — CoreText fonts (host fonts in AROS)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent,
driver, or otherwise — was read, searched, or consulted in producing it, and any
resemblance to existing implementations is coincidental.** Implement only from this
spec + the approved sources cited by tag: `[PUB]` Apple CoreText/CoreGraphics docs,
the OpenType/TrueType spec, POSIX; `[AROS]` in-tree AROS headers and the
`diskfont`/`bullet`/`freetype2`/`ftmanager`/`graphics` source (paths given; APL/LGPL —
ours); `[OURS]` this project's spikes (the H-series, `hosted/*`, `graft/*`).
`[DERIVED]` items are independently-derived requirements flagged for extra
verification; each stands solely on its cited `[PUB]`/`[AROS]`/`[OURS]` justification —
implement from that justification, never from any reference. No identifier name, call
sequence, file layout, or glyph-handling algorithm in this spec derives from any
third-party implementation.

## Scope

**In.** Expose the Mac's installed fonts to AROS, in **two layers**:

- **Layer A (MVP, ship first).** A host CoreText *enumeration* shim + a
  *provisioning* step that exposes the host `.ttf`/`.otf`/`.ttc` into `FONTS:` (over
  the built [host-volume](../host-volume/spec.md) bridge) and runs the **in-tree**
  `ftmanager` + `freetype2.library` path, so `diskfont` enumerates and `OpenDiskFont`
  opens the host faces with **no new AROS rendering code**.
- **Layer B (deeper).** A new AROS engine library `coretext.library` that implements
  the **same** `OpenEngine`/`SetInfoA`/`ObtainInfoA` glyph contract `freetype2.library`
  implements, rasterizing glyphs through CoreText/CoreGraphics via a `hostlib.resource`
  shim. Selected by the `.otag`'s `OT_Engine` tag (a verified seam) so `diskfont`,
  `bullet`, and `graphics` are untouched.

**Decision (recommended order): Layer A first.** It produces an asserted end-to-end win
([FN4]) with only new *host* code (enumeration) — all rendering is the existing,
working freetype path; the only failure surface is provisioning. Layer A also builds the
fixtures Layer B reuses. Rationale in full in [design.md](design.md).

**Out (non-goals, this spec).** Editing `diskfont.library`/`bullet.library`/
`graphics.library` (the MVP reuses them verbatim; Layer B plugs into the existing seam
without changing them); a font *installer* GUI; full Unicode coverage / complex-script
shaping / ligatures / kerning beyond what the engine contract already carries; `.dfont`
suitcase and bitmap-only host faces (first cut filters to `.ttf`/`.otf`/`.ttc`); writing
fonts *from* AROS *to* the Mac; live font-change notification (re-provision instead);
matching FreeType output byte-for-byte from CoreText (impossible by design — see
Verification).

## Architecture

Layer A and Layer B share the host-shim seam and the verification harness; they differ
in where AROS does the rasterization.

```
LAYER A (MVP) — CoreText catalogs, AROS (freetype) renders
┌──────────────────────────────────────┐          ┌────────────────────────────┐
│ AROS (reused verbatim)  [AROS]        │          │ libfonts.dylib  [OURS]     │
│  diskfont.library  AvailFonts/Open    │          │  · CTFontManager enumerate │
│     │ reads FONTS:<name>.otag/.font   │  host-   │    (fn_enumerate/fn_get)   │
│     ▼  OT_Engine="freetype2"          │  volume  └────────────────────────────┘
│  freetype2.library  -> GlyphMap       │  + ftmanager                │ lists .ttf paths
│     (renders the HOST .ttf)           │ ◄───────── host .ttf/.otf exposed in FONTS:
└──────────────────────────────────────┘

LAYER B (deeper) — CoreText renders; AROS text == macOS text
┌──────────────────────────────────────┐          ┌────────────────────────────┐
│ diskfont.library (unchanged)  [AROS]  │          │ libfonts.dylib  [OURS]     │
│   bullet.c: OpenLibrary(OT_Engine     │ hostlib +│  · ct_open/ct_set_face     │
│             +".library"); OpenEngine  │ HostLib_ │  · ct_render_glyph         │
│        │  OT_Engine="coretext"        │ Lock     │    (CGBitmapContext +      │
│        ▼                              │ ───────► │     CTFontDrawGlyphs)      │
│  coretext.library  [AROS-shaped, OURS]│ ct_* ABI │  · 8-bit coverage out      │
│   · OpenEngine/SetInfoA/ObtainInfoA   │ ◄─────── └────────────────────────────┘
│   · fills struct GlyphMap             │  glyph bitmap + bbox + advance
└──────────────────────────────────────┘
```

- **Host shim** `[OURS]` — native arm64 (`.c`/`.m`), built with **host** clang (NOT
  AROS crosstools), peer of `hosted/coreaudio/coreaudio_shim.c`. It owns every CoreText/
  CoreGraphics object and exposes the flat `fn_*` (enum) + `ct_*` (render) C ABI below.
  It pulls **no** AROS headers. Reached via `hostlib.resource` (`HostLib_Open` of the
  dylib + `HostLib_GetPointer` per symbol from a frozen `fonts.exports`) `[AROS]`,
  exactly as the coreaudio/bsdsocket/clipboard shims resolve theirs `[OURS]`.
- **Layer A AROS side** — **none new in the engine**: `diskfont` + `freetype2` reused
  verbatim. New AROS-visible artifact is a *provisioning script/helper* that runs
  `ftmanager` per host face.
- **Layer B AROS side** `[OURS]` — `coretext.library`, modelled on `freetype2.library`;
  the only AROS file naming CoreText is the shim. Selected by the `.otag` string.
- Spike-phase paths: shim in `hosted/fonts/`; at graft, Layer B's library lands as a new
  `workbench/libs/coretext/` modelled on `workbench/libs/freetype2/`.

## The C ABI (`fonts_shim.h`)

Hand-authored, neutral. Two verb families. Enumeration (`fn_*`) backs Layer A;
rasterization (`ct_*`) backs Layer B. `[PUB]` CoreText/CoreGraphics under the hood; the
ABI shapes are `[OURS]`, mirroring the *role* of the coreaudio shim's opaque handle API
(`ca_open`/`ca_set_format`/…) `[OURS]`. The shim is the only owner of the CoreText state.

```c
/* ---- Layer A: enumeration ---- */
struct fn_face {
    char     family[128];   /* CoreText family name, UTF-8                       */
    char     style[64];     /* "Regular"/"Bold"/"Italic"/...                     */
    char     path[1024];    /* filesystem path to the backing .ttf/.otf/.ttc     */
    int      face_index;    /* index within a .ttc collection (0 for single)     */
    int      weight;        /* 0..1000 normalized (400=regular, 700=bold)        */
    int      is_italic;     /* 0/1                                               */
    int      is_fixed;      /* 0/1 monospaced                                    */
};
int  fn_enumerate(void);                       /* build internal list; return count, <0 err */
int  fn_get(int i, struct fn_face *out);       /* fill out for face i; 0 ok, <0 err          */
void fn_free(void);

/* ---- Layer B: rasterization ---- */
typedef struct CTGlyphCtx CTGlyphCtx;
struct ct_glyph {                              /* one rasterized glyph, host memory */
    int      bm_modulo;     /* bytes per row of coverage (= width, 8-bit)        */
    int      bm_rows;       /* rows                                              */
    int      black_left, black_top, black_width, black_height;  /* tight ink bbox */
    int      advance_16_16; /* advance width, 16.16 fixed                        */
    unsigned char *coverage; /* 8-bit coverage, top-down, bm_rows*bm_modulo bytes; valid until next call/close */
};
CTGlyphCtx *ct_open(void);                     /* NULL on failure                  */
int  ct_set_face(CTGlyphCtx *, const char *path, int face_index);  /* 0 ok, <0 err */
/* render one glyph for `codepoint` at `px_size` pixels; `synth` carries optional
   synthetic bold/italic transform (0 = none). Returns 0 ok, OTERR-style <0 on miss. */
int  ct_render_glyph(CTGlyphCtx *, unsigned int codepoint, float px_size,
                     int synth_bold, int synth_italic, struct ct_glyph *out);
void ct_close(CTGlyphCtx *);
```

The header is shared source, hand-written, independent work. The shim must not include
AROS headers; the AROS side must not include CoreText headers. The ABI is the only
contact surface.

## Layer A (MVP) — provisioning, requirement by requirement

### R-ENUM — host font enumeration `[PUB]` + `[OURS]`

**Requirement.** `fn_enumerate`/`fn_get` return the Mac's file-backed faces. Use
`CTFontManagerCopyAvailableFontURLs` (or `CTFontManagerCopyAvailableFontFamilyNames` +
per-family `CTFontDescriptor`s) to list faces; resolve each to a filesystem path under
`/System/Library/Fonts`, `/Library/Fonts`, `~/Library/Fonts` `[PUB]`. Populate
`fn_face` (family/style/weight/italic/fixed from the `CTFontDescriptor` traits;
`face_index` from the URL fragment for `.ttc`). **Filter** to existing `.ttf`/`.otf`/
`.ttc`; skip `.dfont` and bitmap-only faces in the first cut (out of scope). *Justification:*
public CoreText API + macOS font-location convention; the filter is `[OURS]` first-cut
policy. **Fallback (`[DERIVED]`, restated `[PUB]`):** the same list can be produced with
no CoreText at all by `readdir` over the three dirs for `*.ttf|*.otf|*.ttc` — cheaper,
loses correct family/style metadata; allowed for a first cut, default to CoreText for
correctness. We independently determined enumeration is the only new host primitive
needed for Layer A.

### R-DELIVER — expose host font files into `FONTS:` `[OURS]` + `[AROS]`

**Requirement.** The host `.ttf`/`.otf`/`.ttc` must be reachable from AROS as files so
`ftmanager`/`diskfont` can open them. **Reuse the host-volume bridge** (built &
verified): mount a host font directory as an AROS volume via
`AROS_HOST_VOLUME="MacFonts:/System/Library/Fonts"` (read-only) — or stage a curated
subset into the existing `MacRW:` shared folder — so the files appear under an AROS path
(e.g. `MacFonts:` or `FONTS:TrueType/`). *Justification:* `[OURS]` the host-volume spec
delivers exactly this (`graft/run-window.sh` `MacRO:`/`MacRW:`, `AROS_HOST_VOLUME`);
`[AROS]` `FONTS:`/`FONTS:TrueType` is the conventional outline-font location
(`ftmanager` default, `cli.c:46-48,227`). No new bridge code.

### R-REGISTER — write the `.otag`/`.font` descriptors via `ftmanager` `[AROS]`

**Requirement.** For each enumerated face, register it so `diskfont` lists/opens it.
**Run the in-tree `ftmanager` CLI** (`workbench/system/ftmanager/cli.c:15`, template
`COPY/S,TTFFONT/A,CODEPAGE/K,TO/K,FONTDIR/K,OUTFONT/K`) with `TTFFONT=<mounted .ttf>`,
`FONTDIR=FONTS:`, `OUTFONT=<name>`; it writes `FONTS:<name>.otag` + `FONTS:<name>.font`
`[AROS]`. The `.otag` keeps `OT_Engine="freetype2"` (`fontinfo_class.c` ~`:585`) so the
existing rasterizer handles it. For `.ttc`, pass the face index so `OT_Spec6_FaceNum` is
written. *Justification:* `[AROS]` `ftmanager` already produces the exact descriptor
pair `diskfont` reads (`.otag` writer `fontinfo_class.c:fiWriteFiles` ~`:741`; `.font`
stub 4 bytes `0x0f 0x03 0x00 0x00` = `OFCH_ID`).

- **The descriptor format (grounded, for the fallback helper) `[AROS]`.** If running
  `ftmanager` per-face proves impractical, a small provisioning helper may write the
  pair directly: `.otag` = two big-endian ULONGs (`OT_FileIdent`, total byte size) then
  big-endian `{ti_Tag, ti_Data}` pairs (`OT_Engine`→"freetype2", `OT_Family`,
  `OT_Spec1_FontFile`→the `.ttf` path, `OT_StemWeight`/`OT_SlantStyle` from traits,
  `OT_AvailSizes`), indirect data (strings) packed in a trailing section referenced by
  offset, `TAG_END` terminator (`fontinfo_class.c:fiWriteFiles`); `.font` = the 4-byte
  `OFCH_ID` stub. **Decision:** prefer `ftmanager` (no new AROS binary); the direct
  writer is the documented fallback. **UNVERIFIED:** that `ftmanager`'s CLI builds/runs
  headless on `aarch64-darwin`.

### R-AVAIL / R-OPEN — enumeration + open light up (consumer contract) `[AROS]`

**Requirement.** After R-REGISTER, the host faces must appear in `diskfont`
`AvailFonts` and open via `OpenDiskFont`/`OpenFont` with **no new AROS code**:

- `AvailFonts(buffer, bytes, AFF_DISK|AFF_SCALED)` (`availfonts.c:36`) must list each
  registered face as a `struct AvailFonts` (`diskfont.h:78`) with the right
  `af_Attr.ta_Name`/`ta_YSize`/`ta_Style` and `AFF_DISK` `[AROS]`.
- `OpenDiskFont(&ta)` (`opendiskfont.c:22`) must return a non-NULL `TextFont`; the
  outline branch (`bullet.c:898` `OTAG_ReadOutlineFont`) reads the `.otag`, opens
  `freetype2.library` via the `OT_Engine` string (`:911-920`), renders the strike, and
  `AddFont`s it (`diskfontfunc.c:1270`) so `OpenFont` and `graphics.library/Text` then
  work `[AROS]`. *Justification:* this is the existing, working path; the requirement is
  that provisioning lands valid descriptors so it triggers — asserted by [FN3]/[FN4].

## Layer B (deeper) — the CoreText engine, requirement by requirement

### R-SEAM — the engine is selected by the `.otag` string (the load-bearing seam) `[AROS]`

**Requirement (a fact we depend on, restated from source).** `diskfont`'s outline
loader reads the engine name from the `.otag` and `OpenLibrary`s `"<name>.library"`,
then calls the engine's `OpenEngine`/`SetInfoA`/`ObtainInfoA`:

```c
enginename = GetTagData(OT_Engine, NULL, otag->tags);            /* bullet.c:911  */
strcpy(buf, enginename); strcat(buf, ".library");                /* bullet.c:917-918 */
BulletBase = OpenLibrary(buf, 0);                                /* bullet.c:920  */
ge = OpenEngine();                                               /* bullet.c:925  */
OTAG_SetupFontEngine(... ge ...); OTAG_GetGlyphMaps(ge, gm, ...);/* bullet.c:933,982 */
```

So a `.otag` with `OT_Engine="coretext"` makes `diskfont` load `coretext.library` and
drive it through the identical five LVOs `[AROS]`. **No change to `diskfont` or
`bullet`.** **Verified** at the code lines cited; **UNVERIFIED** is whether
`OTAG_SetupFontEngine`/`OTAG_GetGlyphMaps` pass only documented tags or assume
freetype-specific behaviour — R-ENGINE-TAGS covers this.

### R-ENGINE-LVO — `coretext.library` implements the glyph contract `[AROS]` + `[OURS]`

**Requirement.** A new library `coretext.library` (basename to be assigned; modelled on
`freetype2.library`) exporting, at the bullet LVO offsets (`bullet.conf`):

- **`OpenEngine`** (LVO 5) → alloc a `CTGlyphEngine` (our analogue of
  `FT_GlyphEngine`, `ftglyphengine.h`), set `gle_Library`=this base,
  `gle_Name`="coretext" (`glyph.h:24`); `ct_open()` the shim handle. Returns the
  `GlyphEngine *` `[AROS]`.
- **`SetInfoA(ge, taglist)`** → scan the tags freetype scans (`freetype2/setinfoa.c`):
  `OT_Spec1_FontFile` (+ `OT_Spec6_FaceNum`) → `ct_set_face(path, faceIndex)`;
  `OT_PointHeight` (16.16 fixed) + `OT_DeviceDPI` → pixel size; `OT_ShearSin/Cos` →
  synthetic italic; `OT_EmboldenX/Y` → synthetic bold; `OT_GlyphCode` → requested
  character; `OT_OTagList` → recurse. Return `OTERR_Success`/`OTERR_*`
  (`oterrors.h`) `[AROS]`.
- **`ObtainInfoA(ge, taglist)`** → on `OT_GlyphMap8Bits` (8-bit) / `OT_GlyphMap`
  (1-bit), `ct_render_glyph(...)` and fill the caller's `struct GlyphMap` (R-GLYPHMAP).
  Return `OTERR_Success`/`OTERR_NoGlyph` `[AROS]`.
- **`CloseEngine`/`ReleaseInfoA`** → free per-engine state / `ct_close`.

*Justification:* `[AROS]` the contract is `bullet.conf` + `freetype2/*` as the worked
example; `[OURS]` the implementation is ours, written from that contract, not from any
reference.

### R-GLYPHMAP — fill `struct GlyphMap` correctly from CoreText output `[AROS]` + `[PUB]`

**Requirement.** `ct_render_glyph` returns an 8-bit coverage buffer + metrics; the
engine packs them into `struct GlyphMap` (`glyph.h:32-48`) with these exact semantics
(reverse-derived from `freetype2/glyph.c:173` `RenderGlyph`, the only worked example):

| GlyphMap field | from `ct_glyph` | note |
|----------------|-----------------|------|
| `glm_BMModulo` | `bm_modulo` | bytes per coverage row |
| `glm_BMRows`   | `bm_rows`   | rows |
| `glm_BlackLeft/Top/Width/Height` | `black_*` | tight ink bbox |
| `glm_Width`    | `advance_16_16` | advance, 16.16 `FIXED` |
| `glm_BitMap`   | `coverage` (copied into AROS memory) | 8-bit coverage |
| `glm_X0/Y0/X1/Y1` | derived | positioning — **match freetype's convention** |

**Coordinate mapping (`[DERIVED]`, restated `[PUB]`/`[AROS]`).** CoreText/CoreGraphics
draw y-up, origin bottom-left `[PUB]`; the `GlyphMap`/AROS bitmap is **top-down**. The
shim (or engine) must **flip Y** so row 0 is the top of the ink box. The advance comes
from `CTFontGetAdvancesForGlyphs` `[PUB]`, converted to 16.16. The exact meaning of
`glm_X0/Y0/X1/Y1` vs `glm_Black*` and the baseline origin **must be matched to
`freetype2/glyph.c` + `OTAG_MakeCharData` (`bullet.c`)** `[AROS]` — getting the Y-flip
or advance wrong renders glyphs that *open* but *misposition*. *Justification:* the
struct is `[AROS]` (`glyph.h`); the y-up/top-down conversion is `[PUB]` CoreGraphics
geometry; we independently determined the flip + advance-conversion are required and
must be matched against the freetype example, the only in-tree filler. **UNVERIFIED**
until [FN6]/[FN7] match.

### R-ENGINE-TAGS — honour every tag freetype consumes `[AROS]`

**Requirement.** Before declaring the seam clean, read `OTAG_SetupFontEngine` and
`OTAG_GetGlyphMaps` (`bullet.c`) **in full** and `freetype2/setinfoa.c`/`obtaininfoa.c`,
and replicate every tag the freetype engine consumes — not just the obvious ones.
Candidates that must be handled or explicitly defaulted: `OT_Spec2_CodePage` (the
256-entry codepage map, `ftglyphengine.h`), `OT_Spec4_Metric` (which metric defines the
box), `OT_Spec7_BMSize`, `OT_DeviceDPI`. *Justification:* `[AROS]` correctness against
the real caller; this requirement exists because R-SEAM is verified only for the
*dispatch*, not for the *tag set* — flagged as the headline Layer-B risk.

### R-CHARSET — AROS char code → Unicode → CoreText glyph `[AROS]` + `[PUB]`

**Requirement.** AROS font requests carry an 8-bit character code (codepage/Latin-1
indexed, `OT_GlyphCode`, `OT_Spec2_CodePage`); CoreText is Unicode `[AROS]`/`[PUB]`. The
engine maps AROS code → Unicode code point (identity for ASCII; via the codepage table
for Latin-1/`OT_Spec2_CodePage`), then `CTFontGetGlyphsForCharacters` `[PUB]`. Full
Unicode/complex scripts are out of scope (first cut: ASCII + Latin-1). *Justification:*
`[AROS]` the existing codepage handling; `[PUB]` CoreText is Unicode. **UNVERIFIED**
how tightly `diskfont`'s `OT_Spec2_CodePage` path constrains a non-freetype engine.

## The bridge — host-call discipline `[AROS]` + `[OURS]`

Both layers cross the AROS→native-dylib boundary the existing shims use; **no
RT/second-thread hazard** (font rasterization is synchronous pull, unlike CoreAudio).

- **R-HOSTLIB.** Resolve the `fn_*`/`ct_*` symbols once (at engine open / first use) via
  `HostLib_Open("libfonts.dylib")` + `HostLib_GetPointer` from a frozen
  `fonts.exports`; errcount must be 0 or refuse to load — the coreaudio/bsdsocket
  pattern `[OURS]`/`[AROS]`.
- **R-LOCK.** Bracket each host call `HostLib_Lock(); … ; AROS_HOST_BARRIER;
  HostLib_Unlock();` (the host-volume overlay precedent) `[AROS]`. Calls are fixed-arg —
  the H3 variadic `abishim.S` is **not** needed `[OURS]`.
- **R-PULL (`[DERIVED]`, restated `[AROS]`/`[OURS]`).** Glyph rendering is pull-only and
  amortized: `diskfont` renders a whole strike at `OpenDiskFont` time
  (`OTAG_GetGlyphMaps`, `bullet.c:982`) then `graphics.library/Text` blits from
  `tf_CharData` locally `[AROS]`, so the per-glyph host round-trip happens **once per
  (face,size)**, not per draw. No background thread, no signal pump (contrast CoreAudio).
  *Justification:* the strike-at-open behaviour is `[AROS]` (`bullet.c`); we
  independently determined this removes any latency/threading concern.

## Verification (unattended — `[OURS]` H7/H11 discipline)

No human looks at glyphs; **no TCC** — listing font files and rasterizing into an
in-memory `CGBitmapContext` need no Screen-Recording/Automation entitlement (ordinary
process I/O + drawing). The oracle is **rasterize-to-bitmap + numeric assertion**
against a deterministic reference computed in a pure-host binary.

**The assertions** (every marker asserts *values*, never "it didn't crash"):

- **Enumeration**: host shim / `AvailFonts` count ≥ a known floor; the named test face
  present with the right size/style/`AFF_DISK`; every reported path `stat`s as a real
  `.ttf`/`.otf`/`.ttc`.
- **Open**: `OpenDiskFont` non-NULL; engine returns `OTERR_Success` (no `NoFace`/
  `NoGlyph`).
- **Rasterization match**: compare the AROS-rendered glyph bitmap to a reference
  rasterization of the **same `.ttf`** by **ink bbox** (`glm_Black*` vs reference —
  catches wrong size/face), **coverage sum/mean** (total 8-bit alpha within tolerance —
  catches blank/garbage), and a **checksum / per-pixel diff** where the engines match.
  - *Cross-engine* ([FN5]: CoreText vs FreeType) → **loose coverage tolerance** (hinting/
    AA differ by design — never demand equality).
  - *Same-engine* ([FN7]: AROS-CoreText vs host-CoreText) → **tight tolerance** (same
    rasterizer, same code path).

**Markers** (one host binary per marker, `[FN?]` PASS/FAIL via `harness/run-hosted.sh`,
clean-exit on PASS). 1-4 = Layer A; 5-7 = Layer B.

- **[FN1] host enumerates the Mac's fonts (pure host).** `fn_enumerate`/`fn_get` print
  the count + sample `(family, style, path)`. PASS = count ≥ floor (e.g. ≥ 20) and every
  path `stat`s as `.ttf`/`.otf`/`.ttc`. Grounds the host catalog, like H7's `pngprobe`.
  `[FN1]`.
- **[FN2] FreeType reference rasterization, asserted (pure host).** Rasterize a known
  glyph of a chosen `.ttf` at a fixed px size with FreeType; assert coverage sum / ink
  bbox vs a stored golden. The **reference oracle** for [FN4]. `[FN2]`.
- **[FN3] provisioned host font appears in `AvailFonts` (booted AROS).** Provision one
  host face (R-DELIVER + R-REGISTER), boot, drive `AvailFonts` from the Startup-Sequence
  (small client or `C:` lister), read the output. PASS = the host face is listed with
  the right size/style/`AFF_DISK`. The MVP enumeration win. `[FN3]`.
- **[FN4] `OpenDiskFont` + AROS glyph matches the FreeType reference (booted AROS).**
  Same face; `OpenDiskFont`, render the [FN2] glyph through `diskfont`→`freetype2`→
  `GlyphMap`, compare to the [FN2] golden (coverage/bbox/checksum within tolerance).
  PASS = AROS rendered the **host file** and the bitmap matches. **Full MVP thesis
  end-to-end.** `[FN4]`.
- **[FN5] CoreText rasterizer shim, asserted (pure host).** `ct_render_glyph` draws the
  known glyph into a `CGBitmapContext`; assert non-empty coverage / ink bbox within a
  *loose* tolerance of the [FN2] FreeType reference. Proves the deeper host path, no
  AROS. `[FN5]`.
- **[FN6] `coretext.library` satisfies the engine contract (booted AROS).** Build the
  engine; rewrite a host face's `.otag` `OT_Engine="coretext"`; `OpenDiskFont` it.
  PASS = opens (no `OTERR_NoFace`/`NoGlyph`), `diskfont` accepts the `GlyphMap`s,
  `AvailFonts` still lists it. Proves the seam took the new engine (R-SEAM/R-ENGINE-TAGS).
  `[FN6]`.
- **[FN7] AROS-via-CoreText glyph == host-CoreText reference (booted AROS).** Render the
  glyph through the AROS CoreText engine; compare to the [FN5] host-CoreText reference
  (same rasterizer) within **tight** tolerance. PASS = bitmaps match. The deeper thesis:
  AROS text == macOS text. `[FN7]`.

Build/run in the existing harness style (`make hosted-fonts`/`fonts-abi` → `[FN?]`
markers; a `graft/fonts-smoke` end-to-end script modelled on `graft/audio-smoke`).

## Build / integration

- Shim `libfonts.dylib` links `CoreText, CoreGraphics, CoreFoundation` (and `FreeType`
  only for the [FN2] reference host binary, not the AROS path); built with host clang
  `-arch arm64`, codesigned (ad-hoc fine for spikes — confirm vs. the existing signing
  path, **UNVERIFIED**), loaded via `hostlib.resource`. Peer of `build/libcoreaudio.dylib`;
  deployed to `~/lib/` by `graft/run-window.sh`/`aros-ctl` and bundled into
  `Daedalos.app/Contents/Frameworks/` by `graft/make-aros-app.sh` (the coreaudio pattern).
- Spikes compile to Mach-O via the existing `Makefile` pattern (`make hosted-fonts` →
  `build/host-fonts*`; `make fonts-abi` → dylib + ABI test; `harness/run-hosted.sh
  '[FN?] PASS'`) `[OURS]`.
- Layer A adds **no** AROS binary (it runs in-tree `ftmanager`); Layer B adds
  `workbench/libs/coretext/` (a new library) at graft, built by AROS crosstools.
- The C ABI header is shared, hand-written, independent work. The shim must not link or
  include AROS headers; the AROS side must not include CoreText headers.

## Open questions / UNVERIFIED

- That `ftmanager`'s CLI (`cli.c`) builds and runs **headless** on `aarch64-darwin`
  (it's a GUI app with a CLI entry) — fallback is the direct `.otag`/`.font` writer
  (format grounded in R-REGISTER).
- Whether `OTAG_SetupFontEngine`/`OTAG_GetGlyphMaps` pass **only** documented tags or
  assume freetype-specific tag behaviour a CoreText engine must also honour
  (R-ENGINE-TAGS) — the headline Layer-B risk; read the source before pixel work.
- Exact `glm_X0/Y0/X1/Y1` vs `glm_Black*` semantics and the baseline/Y-origin
  convention — must be matched to `freetype2/glyph.c` + `OTAG_MakeCharData` (R-GLYPHMAP).
- Tolerances for the cross-engine ([FN5]) vs same-engine ([FN7]) compares — set
  empirically; coverage-sum tolerance for cross-engine, tight checksum for same-engine.
- `.dfont`/bitmap-only/`.ttc` coverage by FreeType (MVP) and CoreText (deeper) — first
  cut filters to single-face `.ttf`/`.otf`; `.ttc` via `OT_Spec6_FaceNum`.
- How tightly `diskfont`'s `OT_Spec2_CodePage` path constrains a non-freetype engine
  (R-CHARSET) — first cut ASCII + Latin-1.
- A `coretext.library` basename + LVO layout that mirrors `bullet.conf`/`freetype2.conf`
  exactly — confirm at graft so `diskfont`'s `OpenEngine()` call resolves.
- Codesign / entitlements for a `dlopen`'d CoreText dylib in the hosted process —
  confirm vs. the existing signing path.

## Provenance summary

`[PUB]` Apple CoreText (`CTFontManagerCopyAvailableFontURLs`/`…FontFamilyNames`,
`CTFontCollection`, `CTFontDescriptor` traits, `CTFontCreateWithName`,
`CTFontGetGlyphsForCharacters`, `CTFontDrawGlyphs`, `CTFontGetAdvancesForGlyphs`) +
CoreGraphics (`CGBitmapContext` 8-bit grayscale glyph rasterization, y-up/bottom-left
geometry); the OpenType/TrueType container (`.ttf`/`.otf`/`.ttc`, face index, glyph IDs);
POSIX (`readdir`/`stat`, the macOS font-dir locations); Unicode code points. ·
`[AROS]` `compiler/include/graphics/text.h` (`TextFont`/`TextAttr`/`TTextAttr`,
`FPF_*`/`FSF_*`), `rom/graphics/{openfont.c,text.c,addfont.c}` (consumer + render path);
`workbench/libs/diskfont/` (`opendiskfont.c:22`, `availfonts.c:36` `AFF_*`,
`af_fontdescr_io.c`, `diskfont_io.c`, `diskfontfunc.c:1270` `AddFont`,
**`bullet.c:898-982`** the `OT_Engine` seam — `:911,920,925,982`), `compiler/include/
diskfont/{diskfont.h,diskfonttag.h,glyph.h,oterrors.h}` (`fch_FileID`/`OFCH_ID`,
`OT_*` tags, `GlyphEngine`/`GlyphMap`, `OTERR_*`); `workbench/libs/bullet/`
(`bullet.conf` LVOs, stub bodies), `workbench/libs/freetype2/` (`openengine.c`,
`setinfoa.c`, `obtaininfoa.c`, `glyph.c:173` `RenderGlyph`, `ftglyphengine.h`,
`freetype2.conf`); `workbench/system/ftmanager/` (`cli.c:15` template + `FONTS:`/
`FONTS:TrueType` defaults, `fontinfo_class.c:fiWriteFiles` `.otag`/`.font` writer);
`workbench/fonts/truetype/*` (the `.ttf`+`.otag`+`.font` layout); `arch/all-unix/
bootstrap/hostlib.h` + `arch/all-hosted/hostlib/` (`HostLib_Open`/`GetPointer`/`Lock`). ·
`[OURS]` `hosted/coreaudio/{coreaudio_shim.h,coreaudio.exports,abi_test.c}`,
`hosted/bsdsocket/`, `hosted/clipboard/` (shim ABI + `.exports` + `dlopen` precedent);
the H3 host-call boundary, H7 render-to-file unattended-verify stance; the
host-volume bridge (`AROS_HOST_VOLUME`, `graft/run-window.sh`, `graft/hostvol-smoke`);
`harness/run-hosted.sh` marker harness; `graft/audio-smoke`/`aros-ctl` (drive the
booted shell, assert output). ·
`[DERIVED]` independently-derived points flagged for extra verification:
(a) enumeration is the only new host primitive Layer A needs (R-ENUM), (b) the
y-up→top-down flip + advance-to-16.16 conversion are required and must be matched to the
freetype filler (R-GLYPHMAP), and (c) glyph rendering is amortized pull with no
threading hazard (R-PULL) — each restated above from `[PUB]` CoreGraphics geometry +
`[AROS]` `glyph.c`/`bullet.c` behaviour + the `[OURS]` shim precedents. No third-party
code, identifiers, or call sequence used.
