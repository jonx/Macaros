#!/bin/sh
# compile-j5m.sh — compile the SELF-CONTAINED C program j5m.c to a REAL big-endian
# AmigaOS hunk executable using the from-source 68k cross-toolchain:
#     vbcc (C -> vasm-mot asm)  ->  vasm (asm -> vobj)  ->  vlink (vobj -> hunk .exe)
# Output: bin/j5m.exe  (committed, like the other corpus *.exe).
#
# crt0.s is linked FIRST so _start is the entry (first byte of the CODE hunk), per the
# [J4] loader / engine entry contract.  -cpu=68020 lets vbcc lower 32-bit multiply/divide
# to NATIVE mulu.l/muls.l/divu.l/divul.l/divsl.l (the 68000 would call __mulu/__divu
# library helpers we do not have), making the program fully self-contained — only _putch
# (in crt0.s) needs resolving.  -no-fp: integer-only (the toolchain was built with
# host-native datatypes; see build-vbcc.sh).
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # .../apps68k
TC="$HERE/.toolchain"
VBCC="$TC/vbccm68k"; VASM="$TC/vasmm68k_mot"; VLINK="$TC/vlink"
for t in "$VBCC" "$VASM" "$VLINK"; do
    [ -x "$t" ] || { echo "!! missing $t — run tools/build-vbcc.sh (and build-vasm.sh)"; exit 1; }
done
mkdir -p "$HERE/bin"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

echo ">> vbcc: j5m.c -> j5m.s (m68020, integer-only, full opt)"
"$VBCC" "$HERE/j5m.c" -o="$WORK/j5m.s" -quiet -O=255 -cpu=68020

echo ">> vasm: crt0.s + j5m.s -> vobj"
"$VASM" -Fvobj -quiet -no-opt -m68020 "$HERE/crt0.s" -o "$WORK/crt0.o" 2>&1 | grep -vE '^vasm |^$' || true
"$VASM" -Fvobj -quiet -no-opt -m68020 "$WORK/j5m.s" -o "$WORK/j5m.o"   2>&1 | grep -vE '^vasm |^$' || true

echo ">> vlink: crt0.o + j5m.o -> bin/j5m.exe (amigahunk, crt0 FIRST = entry)"
"$VLINK" -bamigahunk -s -o "$HERE/bin/j5m.exe" "$WORK/crt0.o" "$WORK/j5m.o"

echo ">> done:"
ls -l "$HERE/bin/j5m.exe"
# Sanity: confirm the hunk magic.
od -An -tx1 -N4 "$HERE/bin/j5m.exe" | tr -d ' \n' | grep -q "000003f3" \
    && echo "   hunk magic 0x000003f3 OK" || { echo "!! not a hunk executable"; exit 1; }
