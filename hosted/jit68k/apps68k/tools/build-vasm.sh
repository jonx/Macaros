#!/bin/sh
# build-vasm.sh — fetch + build vasm (M68k/Motorola syntax, Amiga hunk output),
# the toolchain that produces the REAL big-endian AmigaOS hunk executables in
# this directory. vasm is small, dependency-free C under a permissive
# "free for non-commercial AND in/with vasm" license (see the downloaded
# COPYRIGHT); we build it from source rather than committing a 650 KB binary.
#
# Output: ./.toolchain/vasmm68k_mot  (the assembler the Makefile invokes to
# regenerate bin/*.exe from the *.s sources).
#
# Verified on this Mac: macOS 26.x, Apple clang. vasm 2.0e builds with warnings
# only (assignment-as-condition style warnings in vasm's own code), no errors.
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"      # .../apps68k
DEST="$HERE/.toolchain"
URL="http://sun.hasenbraten.de/vasm/release/vasm.tar.gz"

mkdir -p "$DEST"
if [ -x "$DEST/vasmm68k_mot" ]; then
    echo ">> vasm already built: $DEST/vasmm68k_mot"
    "$DEST/vasmm68k_mot" 2>&1 | head -1
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
echo ">> fetching vasm source from $URL"
curl -fsSL -o "$WORK/vasm.tar.gz" "$URL"
tar xzf "$WORK/vasm.tar.gz" -C "$WORK"

echo ">> building vasmm68k_mot (CPU=m68k SYNTAX=mot)"
make -C "$WORK/vasm" CPU=m68k SYNTAX=mot >/dev/null 2>"$WORK/build.log" || {
    echo "!! vasm build failed; see log:"; tail -20 "$WORK/build.log"; exit 1; }

cp "$WORK/vasm/vasmm68k_mot" "$DEST/vasmm68k_mot"
# Keep the license text alongside for provenance.
cp "$WORK/vasm/COPYRIGHT" "$DEST/COPYRIGHT" 2>/dev/null || true
chmod +x "$DEST/vasmm68k_mot"
echo ">> built $DEST/vasmm68k_mot"
"$DEST/vasmm68k_mot" 2>&1 | head -1
