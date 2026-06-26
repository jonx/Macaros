#!/bin/sh
# compile-j5t.sh — compile the SELF-CONTAINED FP C program j5t.c to a REAL big-endian
# AmigaOS hunk executable with HARDWARE 68881 FP, using the from-source 68k cross-toolchain:
#     vbcc (C -> vasm-mot asm)  ->  vasm (asm -> vobj)  ->  vlink (vobj -> hunk .exe)
# Output: bin/j5t.exe  (committed, like the other corpus *.exe).  This is the FP analog of
# compile-j5m.sh (the integer capstone) — the [J5t] FP capstone.
#
# THE FP CODEGEN CONFIG (the whole point of this milestone):
#   -fpu=68881   makes vbcc lower `float`/`double` arithmetic to HARDWARE line-F FP
#                instructions (FMOVE/FADD/FSUB/FMUL/FDIV/FCMP/FBcc/FMOVEM/fintrz/...),
#                NOT soft-float library calls (__ieeeaddd etc.).  Without -fpu, vbcc
#                defaults to FPU=0 (no FPU) even with -cpu=68040, and emits soft-float.
#   -cpu=68020   the integer base (native mulu.l/divu.l, indexed EAs); the FPU is the
#                68881 coprocessor on top.
# PREREQUISITE: the toolchain's vbcc must be built with dtgen cross=y (tools/build-vbcc.sh)
# so FP CONSTANTS are emitted in BIG-ENDIAN IEEE-754 target byte order.  A cross=n vbcc
# byte-SWAPS FP constants (e.g. 1000.0 -> 0x0000000000408f40 instead of 0x408f400000000000)
# because it uses host-native (little-endian) datatypes — correct for integer constant
# folding (j5m), WRONG for FP.  build-vbcc.sh now answers dtgen cross=y for this reason;
# this script verifies the produced hunk actually contains line-F FP opcodes.
#
# crt0_fp.s is linked FIRST so _start is the entry; it also provides the three HARDWARE-FP
# transcendental shims (_sqrt->FSQRT, _sin->FSIN, _exp->FETOX) the C calls, plus the integer
# PutChar LVO shim (the program's output is integer-ized; see j5t.c).
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # .../apps68k
TC="$HERE/.toolchain"
VBCC="$TC/vbccm68k"; VASM="$TC/vasmm68k_mot"; VLINK="$TC/vlink"
for t in "$VBCC" "$VASM" "$VLINK"; do
    [ -x "$t" ] || { echo "!! missing $t — run tools/build-vbcc.sh (and build-vasm.sh)"; exit 1; }
done
mkdir -p "$HERE/bin"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

echo ">> vbcc: j5t.c -> j5t.s (m68020 + HARDWARE 68881 FP, full opt)"
"$VBCC" "$HERE/j5t.c" -o="$WORK/j5t.s" -quiet -O=255 -cpu=68020 -fpu=68881

# VERIFY the compiler actually emitted line-F FP opcodes (not soft-float library calls).
if grep -qiE '__ieee(add|sub|mul|div)d' "$WORK/j5t.s"; then
    echo "!! vbcc emitted SOFT-FLOAT (__ieee*d) — hardware FP not engaged"; exit 1
fi
NFP=$(grep -icE '\b(fmove|fadd|fsub|fmul|fdiv|fcmp|fsqrt|fabs|fneg|fmovem|fb[a-z]+|fint)' "$WORK/j5t.s")
echo "   hardware FP opcodes in j5t.s: $NFP line-F mnemonics (fmove/fadd/fmul/fdiv/fcmp/fbcc/...)"
[ "$NFP" -gt 0 ] || { echo "!! no line-F FP opcodes found — hardware FP not engaged"; exit 1; }

echo ">> vasm: crt0_fp.s + j5t.s -> vobj (-m68882 for the FP instruction set)"
"$VASM" -Fvobj -quiet -no-opt -m68020 -m68882 "$HERE/crt0_fp.s" -o "$WORK/crt0.o" 2>&1 | grep -vE '^vasm |^$' || true
"$VASM" -Fvobj -quiet -no-opt -m68020 -m68882 "$WORK/j5t.s"    -o "$WORK/j5t.o"  2>&1 | grep -vE '^vasm |^$' || true

echo ">> vlink: crt0.o + j5t.o -> bin/j5t.exe (amigahunk, crt0 FIRST = entry)"
"$VLINK" -bamigahunk -s -o "$HERE/bin/j5t.exe" "$WORK/crt0.o" "$WORK/j5t.o"

echo ">> done:"
ls -l "$HERE/bin/j5t.exe"
# Sanity: confirm the hunk magic.
od -An -tx1 -N4 "$HERE/bin/j5t.exe" | tr -d ' \n' | grep -q "000003f3" \
    && echo "   hunk magic 0x000003f3 OK" || { echo "!! not a hunk executable"; exit 1; }
