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
# building a cross-compiler?". Per vbcc's own doc/interface.texi: answer `n` when you
# are running a NATIVE host compiler (clang here) to build a vbcc binary that runs on
# the host — even though that vbcc TARGETS m68k. (Their "cross-compiler=y" means the
# CC that BUILDS vbcc is itself a cross-compiler, which is not our case.) Answering `n`
# makes dtgen use the host's native integer types; the m68k code generator accesses
# integer constants only through its arithmetic functions, so host endianness does not
# matter for INTEGER code (doc/interface.texi). FP constant folding WOULD need
# big-endian host floats, so the j5m program is integer-only (-no-fp); the milestone
# explicitly scopes FP out. We pipe `n` to dtgen so the build is non-interactive.
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

echo ">> generating m68k target datatypes (dtgen, answering cross=n) + building vbccm68k"
mkdir -p "$VBCCSRC/objects/m68k"
# Build the host tools (vc, dtgen) first, then the m68k datatypes, then the compiler.
make -C "$VBCCSRC" bin/dtgen bin/vc >/dev/null 2>"$WORK/vbcc1.log" || {
    echo "!! vbcc host-tools (dtgen/vc) build failed:"; tail -25 "$WORK/vbcc1.log"; exit 1; }
# dtgen is interactive ("Are you building a cross-compiler?"); pipe `n` (host-native
# datatypes — see header). It writes objects/m68k/dt.{h,c}.
printf 'n\n' | "$VBCCSRC/bin/dtgen" "$VBCCSRC/machines/m68k/machine.dt" \
    "$VBCCSRC/objects/m68k/dt.h" "$VBCCSRC/objects/m68k/dt.c" >/dev/null
[ -s "$VBCCSRC/objects/m68k/dt.h" ] || { echo "!! dtgen produced empty dt.h"; exit 1; }
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
