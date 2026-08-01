#!/bin/sh
# build-dhry.sh — reproduce bin/dhry.exe: the freestanding 68k Dhrystone 2.1
# (the jit68k benchmark program; see docs/features/benchmarks/README.md).
#
# The Dhrystone SOURCES are third-party (Reinhold Weicker's benchmark, fetched
# from netlib) and are NOT vendored in this repo — this script fetches them,
# applies the committed freestanding patch (static records instead of malloc,
# fixed DHRY_RUNS, no timing, printf -> the putch mini-printf in dhry_glue.c),
# and compiles with the apps68k vbcc/vasm/vlink cross-toolchain.
#
#   tools/build-vasm.sh + tools/build-vbcc.sh   # (in ../apps68k) build the toolchain
#   ./build-dhry.sh                             # -> src/ (untracked), bin/dhry.exe
#   DHRY_SHAR=/path/dhry-c ./build-dhry.sh      # use a pre-fetched netlib shar
#
# Verify (deterministic at the fixed DHRY_RUNS):
#   ../../../build/run68k bin/dhry.exe | cmp - dhry-reference.txt && echo BYTE-EXACT
#   (exit code 0 = the canonical integrity values held; timing is external:
#    JIT68K_NO_DIAG=1 for the chained hot path, see the benchmarks doc.)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TC="$HERE/../apps68k/.toolchain"
URL="https://netlib.org/benchmark/dhry-c"

VBCC="$TC/vbccm68k"; VASM="$TC/vasmm68k_mot"; VLINK="$TC/vlink"
for t in "$VBCC" "$VASM" "$VLINK"; do
    [ -x "$t" ] || { echo "!! missing $t — run ../apps68k/tools/build-vasm.sh + build-vbcc.sh"; exit 1; }
done

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

if [ -n "${DHRY_SHAR:-}" ]; then
    echo ">> using local dhry shar $DHRY_SHAR"
    cp "$DHRY_SHAR" "$WORK/dhry.shar"
else
    echo ">> fetching Dhrystone 2.1 from $URL"
    curl -fsSL -o "$WORK/dhry.shar" "$URL"
fi
( cd "$WORK" && sh dhry.shar >/dev/null 2>&1 )
for f in dhry.h dhry_1.c dhry_2.c; do
    [ -f "$WORK/$f" ] || { echo "!! $f missing from the shar"; exit 1; }
done

echo ">> applying the freestanding patch"
mkdir -p "$HERE/src"
cp "$WORK/dhry.h" "$WORK/dhry_1.c" "$WORK/dhry_2.c" "$HERE/src/"
patch -s -N -p1 -d "$HERE" < "$HERE/dhry-freestanding.patch"

echo ">> compiling (vbcc -cpu=68020, integer only) + linking (amigahunk)"
"$VBCC" "$HERE/src/dhry_1.c" -o="$WORK/dhry_1.s" -quiet -O=255 -cpu=68020 -I"$HERE/src" 2>/dev/null
"$VBCC" "$HERE/src/dhry_2.c" -o="$WORK/dhry_2.s" -quiet -O=255 -cpu=68020 -I"$HERE/src" 2>/dev/null
"$VBCC" "$HERE/dhry_glue.c"  -o="$WORK/glue.s"   -quiet -O=255 -cpu=68020 2>/dev/null
for s in "$HERE/../apps68k/crt0.s" "$WORK/dhry_1.s" "$WORK/dhry_2.s" "$WORK/glue.s"; do
    o="$WORK/$(basename "$s" .s).o"
    "$VASM" -Fvobj -quiet -no-opt -m68020 "$s" -o "$o" 2>&1 | grep -vE '^vasm|^$' || true
done
mkdir -p "$HERE/bin"
"$VLINK" -bamigahunk -s -o "$HERE/bin/dhry.exe" \
    "$WORK/crt0.o" "$WORK/dhry_1.o" "$WORK/dhry_2.o" "$WORK/glue.o" 2>&1 \
    | grep -v 'imported symbol <_strcpy> was not referenced' || true

od -An -tx1 -N4 "$HERE/bin/dhry.exe" | tr -d ' \n' | grep -q 000003f3 \
    && echo ">> built $HERE/bin/dhry.exe (hunk magic OK)" \
    || { echo "!! not a hunk executable"; exit 1; }
