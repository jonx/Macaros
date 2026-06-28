# Fonts (CoreText) — the Mac's fonts inside AROS

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

Make the Mac's installed fonts usable from AROS. macOS ships hundreds of high-quality
TrueType/OpenType faces under `/System/Library/Fonts`, `/Library/Fonts`, and
`~/Library/Fonts`; AROS already has a full scalable-font stack
(`diskfont.library` → `bullet.library` → an outline *engine* → glyph bitmaps that
`graphics.library/Text()` blits). What's missing is the bridge that lets AROS *see*
and *render* the Mac's faces. This is the typography face of the project thesis —
*"macOS owns the drivers; AROS reaches them via standard exec I/O"* — applied to the
font surface.

Two layers, cheapest first:

- **MVP — font *provisioning* (recommended first win).** Enumerate the host's fonts
  via CoreText, expose the `.ttf`/`.otf` files into `FONTS:` (over the already-built
  [host-volume](../host-volume/README.md) bridge), and run the **in-tree** `ftmanager`
  + `freetype2.library` path so AROS loads them natively. Almost entirely reuses
  existing AROS code; the only new host code is a thin CoreText *enumeration* shim
  (or even just reading the font directories). Lowest risk.
- **Deeper — a CoreText-backed outline *engine*.** A new AROS engine library
  (`coretext.library`) that satisfies the **same** `OpenEngine`/`SetInfoA`/`ObtainInfoA`
  glyph contract `freetype2.library` implements, rasterizing glyphs through CoreText/
  CoreGraphics via a `hostlib.resource` shim — so AROS text matches macOS pixel-for-pixel.
  The `.otag`'s `OT_Engine` tag already names the engine library by string, and
  `diskfont` loads it by name (verified seam), so this slots in without touching
  `diskfont`/`bullet`.

## Verification (unattended)

No human, no TCC. After the bridge runs, the loop asserts numerically: `diskfont`
`AvailFonts` enumerates the host faces; `OpenDiskFont` opens one; and a known glyph
rasterized through AROS matches a reference (FreeType rasterization of the same
`.ttf`, by pixel-coverage / bbox / checksum). Markers are **[FN1]…[FN7]**, one host
binary per marker, greppable — same discipline as the `[H*]`/`[A*]`/`[V*]` milestones.

## Status

Planned. No font code exists in this repo yet (only the `Assign FONTS: SYS:Fonts`
line in `graft/run-window.sh`). The AROS-side stack and the host-side reuse targets
are all present and grounded — see the design.

## Links

- [design.md](design.md) — the why, the grounded AROS/CoreText contracts, the two
  layers, the `[FN*]` spike plan, honest risks.
- [spec.md](spec.md) — implementation spec for a fresh implementer (provenance-tagged).
- Process: [../CLEANROOM.md](../CLEANROOM.md) · Conventions: [../README.md](../README.md)
- Reuses: [host-volume](../host-volume/README.md) (font-file delivery into `FONTS:`),
  the `hostlib.resource` shim precedents
  ([coreaudio](../coreaudio-audio/README.md), bsdsocket, clipboard), the `aros-ctl`
  [control harness](../control-harness/README.md) (drive the shell, assert output).
