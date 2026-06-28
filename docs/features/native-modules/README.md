# Native modules — disk-loadable AArch64 AROS code under W^X

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

The ability to **build and `LoadSeg` a natively-compiled AArch64 AROS module from
disk** — a `DEVS:Printers/*` printer driver, a `DEVS:Midi/*` CAMD driver, an AHI
sub-driver, a `LIBS:*.library` — and run its code, mapping the loaded sections into
**Apple-Silicon W^X executable memory**. Hosted AROS already loads its own *native*
modules baked into the kickstart; this feature is the *disk-loadable* path: `dos.library`
`LoadSeg` → the native ELF loader (`rom/dos/internalloadseg_elf.c`) → exec/host
executable memory → a relocated, I-cache-correct, executable segment.

It is a **load-bearing foundation**, not a leaf feature: it is the real dependency
under the printer, MIDI, fonts and audio-sub-driver work — the thing that lets those
modules actually *run*, not just compile.

## Which dependent features this unblocks

- **[Printing](../printing/design.md)** — `[PR0]` adds the `__aarch64__`
  `AROS_PRINTER_MAGIC`; that magic is the **first instruction executed** in a loaded
  driver, so it depends on the loaded code being executable (this foundation).
- **[MIDI / CoreMIDI](../midi-coremidi/design.md)** — explicitly gated on
  "native-`LoadSeg` driver discovery": camd `LoadSeg`s `DEVS:Midi/<name>` and scans the
  seglist. `[MD5]` rides this.
- **AHI sub-drivers** ([CoreAudio audio](../coreaudio-audio/README.md) `[A5]`) and any
  **`LIBS:*.library`** loaded from disk rather than baked into the kickstart.

## Relationship to the 68k JIT (do not confuse them)

This covers the **native (non-translated)** path: native AArch64 ELF objects, executed
directly. The [68k JIT](../68k-jit/design.md) covers the **translated** path: 68k hunk
code recompiled to AArch64. **They share one substrate — W^X executable memory** (the
68k-JIT `[J1]` `jit_region.{h,c}` `MAP_JIT` layer). The JIT loads *translated* code into
it; native `LoadSeg` loads *native aarch64* code into it. The shared seam is frozen in
[../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md); this doc cross-references it and does
**not** restate or change it. Where scope overlaps `[J1]`, we defer to the JIT doc.

## Links

- [design.md](design.md) — the why, the grounded contracts (the ELF loader's section
  allocation + AArch64 relocations + the R/W→R/X flip), the two W^X strategies in the
  tree today, the ROM-resident sidestep, the `[NL*]` spike plan.
- [spec.md](spec.md) — the implementation spec (the allocator contract, the toggle
  discipline, the cache-maintenance requirement, the build/codesign/entitlement story),
  with per-requirement provenance.
- Shared foundation: [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2
  (`jit_region` host-call routing), [../68k-jit/design.md](../68k-jit/design.md) `[J1]`.
- Gap map: [../darwin-aarch64-port-inventory.md](../darwin-aarch64-port-inventory.md)
  (§2 "W^X / `MAP_JIT` executable-memory policy", §9 "MAP_JIT/W^X allocation layer").
