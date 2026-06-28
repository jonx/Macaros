# Printing — AROS printer.device bridged to macOS

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

Give the hosted AROS real printing on the Mac. AROS already ships the whole
generic printing stack — `printer.device` + the `PRT:` text spooler, the graphics
dump path, the per-driver plugin model, and a working **PostScript** driver. What
is missing is the last hop: getting that spool *off* AROS and onto a real macOS
print queue (or a PDF on disk). macOS owns the printers via **CUPS**; AROS reaches
them through standard exec I/O — the printing face of the standing project thesis.

The cheapest first win reuses the in-tree precedent that already redirects printer
output to an arbitrary consumer (`printtotool` opens `PIPE:*` and `SystemTags()`es a
C: tool): set the printer's spool device to a tiny **`cups.device`** host shim that
drops the PostScript/text on the **host volume** (already implemented) or hands it to
`cupsfilter`/`lp` — no print dialog, no TCC prompt, fully unattended. The deeper path
is a real `DEVS:Printers/CUPS` driver whose backend bridges to **libcups** via a
`libcups_shim.dylib` (loaded through `hostlib.resource`, peer of `libcoreaudio.dylib`),
to enumerate real macOS printers, submit jobs, and read queue status — the latter
feeding the **printer-STATUS gadget** the project owner called out as the gap (a
Zune/MUI panel modelled on the in-tree printer prefs editor and SysMon).

Verification is headless throughout: print a known document and assert the produced
PDF on disk (page count, extracted text, byte/size). Spike markers are **`[PR1]`…**.

See **[design.md](design.md)** for the why, the grounded AROS + CUPS contracts, the
design, the spike plan, and the risks; **[spec.md](spec.md)** for the implementation
spec (provenance-tagged, per [../CLEANROOM.md](../CLEANROOM.md)).

## The one real blocker to flag up front

`compiler/include/aros/printertag.h` defines `AROS_PRINTER_MAGIC` for m68k / i386 /
x86_64 / arm (32-bit) / ppc / riscv but has **no `__aarch64__` branch** — it hits
`#error AROS_PRINTER_MAGIC is not defined for your architecture`
(`printertag.h:16–30`). **No printer driver compiles for aarch64 today** until that
one-line branch is added. `[PR0]` in the plan.
