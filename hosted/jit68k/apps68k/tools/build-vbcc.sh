#!/bin/sh
# build-vbcc.sh — fetch + build the 68k-AmigaOS C cross-compiler on THIS Mac:
# vbcc (Volker Barthelmann's portable C compiler, same author as vasm) + vlink
# (Frank Wille's linker), both from source, targeting m68k / AmigaOS hunk output.
#
# The pipeline this produces:
#     vbcc (C -> vasm-mot asm)  ->  vasm (asm -> vobj)  ->  vlink (vobj -> hunk .exe)
# vasm is already built by tools/build-vasm.sh; this adds the C front end + linker.
#
# Output (gitignored, like vasm, under .toolchain/):
#     .toolchain/vbccm68k   — the m68k/ColdFire code generator (the actual compiler)
#     .toolchain/vc         — the vbcc driver front end (optional convenience)
#     .toolchain/vlink      — the linker that emits the AmigaOS hunk executable
#
# Verified on this Mac (macOS 26.x, Apple clang/arm64). The ONE non-obvious step:
# vbcc's `dtgen` (the target-datatype generator) is INTERACTIVE — it asks "Are you
# building a cross-compiler?".
#
# [J5t] WE NOW ANSWER cross=y (was cross=n through [J5m]).  Rationale: with cross=n,
# dtgen wires the host's NATIVE datatypes into vbcc, which is correct for INTEGER constant
# folding (the m68k codegen reaches integer constants only through arithmetic functions,
# so host endianness is irrelevant — doc/interface.texi) but WRONG for FLOAT constants: a
# little-endian host (arm64) then emits FP immediates BYTE-SWAPPED (e.g. 1000.0 ->
# 0x0000000000408f40 instead of the correct big-endian 0x408f400000000000).  cross=y makes
# dtgen use vbcc's portable IEEE-conversion datatypes (machine.dt declares S64BIEEEBE — the
# host need only be IEEE, which arm64 is), so FP constants come out in BIG-ENDIAN target byte
# order.  This is what the [J5t] HARDWARE-FP capstone (-fpu=68881) needs.  cross=y produces
# BYTE-IDENTICAL integer code to cross=n (verified by diffing j5m.s), so it is a strict
# improvement: same integer output, plus correct FP constants.  dtgen with cross=y prompts
# per-type ("Does your system support a type implemented as X? [y]" + "Enter that type [T]");
# every prompt's default is correct for clang/arm64, so we feed a stream of BLANK LINES (each
# accepts the default) instead of `n` — fully non-interactive.  (Historical note: cross=n's
# "are you building a cross-compiler?" refers to whether the CC that BUILDS vbcc is itself a
# cross-compiler — not our case — but the FP-constant byte order is the part that actually
# matters here, and it needs cross=y.)
#
# (Historical, pre-[J5t]: the j5m integer capstone was integer-only (-no-fp); the milestone
# explicitly scoped FP out, and the build piped `n` to dtgen.  The [J5t] FP capstone needs
# hardware FP with correct big-endian constants, hence cross=y now.)
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"      # .../apps68k
DEST="$HERE/.toolchain"
VBCC_URL="http://www.ibaug.de/vbcc/vbcc.tar.gz"
VLINK_URL="http://sun.hasenbraten.de/vlink/release/vlink.tar.gz"

mkdir -p "$DEST"
if [ -x "$DEST/vbccm68k" ] && [ -x "$DEST/vlink" ]; then
    echo ">> vbcc+vlink already built:"
    "$DEST/vbccm68k" 2>&1 | grep -i 'code-generator' | head -1 || true
    "$DEST/vlink" 2>&1 | head -1 || true
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---- vbcc (the C compiler) -------------------------------------------------------
echo ">> fetching vbcc source from $VBCC_URL"
curl -fsSL -o "$WORK/vbcc.tar.gz" "$VBCC_URL"
# vbcc's tarball carries self-referential hardlinks for shared machine files; the
# warnings are benign (tar still extracts the real file content).
tar xzf "$WORK/vbcc.tar.gz" -C "$WORK" 2>/dev/null || true
VBCCSRC="$WORK/vbcc"
[ -d "$VBCCSRC" ] || { echo "!! vbcc source not extracted"; exit 1; }

echo ">> generating m68k target datatypes (dtgen, answering cross=y) + building vbccm68k"
mkdir -p "$VBCCSRC/objects/m68k"
# Build the host tools (vc, dtgen) first, then the m68k datatypes, then the compiler.
make -C "$VBCCSRC" bin/dtgen bin/vc >/dev/null 2>"$WORK/vbcc1.log" || {
    echo "!! vbcc host-tools (dtgen/vc) build failed:"; tail -25 "$WORK/vbcc1.log"; exit 1; }
# dtgen is interactive: "Are you building a cross-compiler?" then a per-type probe
# ("Does your system support a type implemented as X? [y]" / "Enter that type [T]").
# We answer cross=y (BIG-ENDIAN IEEE FP constants — see header) and accept every default
# by feeding a stream of BLANK LINES (each accepts the bracketed default). It writes
# objects/m68k/dt.{h,c}.
yes '' | head -200 | "$VBCCSRC/bin/dtgen" "$VBCCSRC/machines/m68k/machine.dt" \
    "$VBCCSRC/objects/m68k/dt.h" "$VBCCSRC/objects/m68k/dt.c" >/dev/null 2>&1
[ -s "$VBCCSRC/objects/m68k/dt.h" ] || { echo "!! dtgen produced empty dt.h"; exit 1; }
# Sanity: cross=y must wire zfloat/zdouble to the host float/double (IEEE conversion path).
grep -q 'zfloat' "$VBCCSRC/objects/m68k/dt.h" || { echo "!! dtgen dt.h missing zfloat"; exit 1; }
make -C "$VBCCSRC" TARGET=m68k bin/vbccm68k >/dev/null 2>"$WORK/vbcc2.log" || {
    echo "!! vbccm68k build failed:"; tail -25 "$WORK/vbcc2.log"; exit 1; }

cp "$VBCCSRC/bin/vbccm68k" "$DEST/vbccm68k"
cp "$VBCCSRC/bin/vc"       "$DEST/vc" 2>/dev/null || true
chmod +x "$DEST/vbccm68k" "$DEST/vc" 2>/dev/null || true
# Keep provenance: vbcc's license is in doc/vbcc.texi (same family as vasm — free for
# non-commercial AND with an explicit commercial exception for M68k/AmigaOS targets,
# which is exactly our use). Extract the "@subsection Distribution"/copyright prose.
if [ -f "$VBCCSRC/doc/vbcc.texi" ]; then
    awk '/vbcc is copyright/{p=1} p{print} /In all other cases you need my written consent/{exit}' \
        "$VBCCSRC/doc/vbcc.texi" > "$DEST/VBCC-LICENSE.txt" 2>/dev/null || true
fi

# ---- vlink (the linker) ----------------------------------------------------------
echo ">> fetching vlink source from $VLINK_URL"
curl -fsSL -o "$WORK/vlink.tar.gz" "$VLINK_URL"
tar xzf "$WORK/vlink.tar.gz" -C "$WORK" 2>/dev/null || true
VLINKSRC="$WORK/vlink"
[ -d "$VLINKSRC" ] || { echo "!! vlink source not extracted"; exit 1; }
echo ">> building vlink (-bamigahunk target included)"
mkdir -p "$VLINKSRC/objects"
make -C "$VLINKSRC" >/dev/null 2>"$WORK/vlink.log" || {
    echo "!! vlink build failed:"; tail -25 "$WORK/vlink.log"; exit 1; }
cp "$VLINKSRC/vlink" "$DEST/vlink"
chmod +x "$DEST/vlink"
# vlink provenance (Frank Wille, freeware; copyright header in main.c).
{ echo "vlink — portable linker, (c) 1997-2025 Frank Wille (freeware)."; \
  echo "Source: $VLINK_URL"; \
  sed -n '1,8p' "$VLINKSRC/main.c" 2>/dev/null; } > "$DEST/VLINK-LICENSE.txt" 2>/dev/null || true

echo ">> built:"
echo "   $DEST/vbccm68k"; "$DEST/vbccm68k" 2>&1 | grep -i 'code-generator' | head -1 || true
echo "   $DEST/vlink";    "$DEST/vlink"    2>&1 | head -1 || true
