#!/bin/sh
# compile-args.sh — compile the SELF-CONTAINED C program args.c to a REAL big-endian
# AmigaOS hunk executable using the from-source 68k cross-toolchain:
#     vbcc (C -> vasm-mot asm)  ->  vasm (asm -> vobj)  ->  vlink (vobj -> hunk .exe)
# Output: bin/args.exe  (committed, like the other corpus *.exe).
#
# This is the args analog of compile-j5m.sh: it links crt0_ARGS.s (NOT crt0.s) FIRST, so
# _start is the entry (first byte of the CODE hunk, per the [J4] loader / engine entry
# contract).  crt0_args.s reads the AmigaDOS CLI argument string (A0 = string, D0 = length
# incl '\n', the convention run68k now honours), splits it into argv[], and calls
# main(argc, argv).  -cpu=68020 lets vbcc lower 32-bit mul/div to native opcodes (so the
# program is self-contained — only _putch in crt0_args.s needs resolving); -no-fp:
# integer-only.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # .../apps68k
TC="$HERE/.toolchain"
VBCC="$TC/vbccm68k"; VASM="$TC/vasmm68k_mot"; VLINK="$TC/vlink"
for t in "$VBCC" "$VASM" "$VLINK"; do
    [ -x "$t" ] || { echo "!! missing $t — run tools/build-vbcc.sh (and build-vasm.sh)"; exit 1; }
done
mkdir -p "$HERE/bin"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

echo ">> vbcc: args.c -> args.s (m68020, integer-only, full opt)"
"$VBCC" "$HERE/args.c" -o="$WORK/args.s" -quiet -O=255 -cpu=68020

echo ">> vasm: crt0_args.s + args.s -> vobj"
"$VASM" -Fvobj -quiet -no-opt -m68020 "$HERE/crt0_args.s" -o "$WORK/crt0.o" 2>&1 | grep -vE '^vasm |^$' || true
"$VASM" -Fvobj -quiet -no-opt -m68020 "$WORK/args.s"      -o "$WORK/args.o" 2>&1 | grep -vE '^vasm |^$' || true

echo ">> vlink: crt0.o + args.o -> bin/args.exe (amigahunk, crt0_args FIRST = entry)"
"$VLINK" -bamigahunk -s -o "$HERE/bin/args.exe" "$WORK/crt0.o" "$WORK/args.o"

echo ">> done:"
ls -l "$HERE/bin/args.exe"
# Sanity: confirm the hunk magic.
od -An -tx1 -N4 "$HERE/bin/args.exe" | tr -d ' \n' | grep -q "000003f3" \
    && echo "   hunk magic 0x000003f3 OK" || { echo "!! not a hunk executable"; exit 1; }
