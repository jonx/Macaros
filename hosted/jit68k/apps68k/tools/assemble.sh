#!/bin/sh
# assemble.sh — (re)assemble the apps68k *.s sources into REAL big-endian AmigaOS
# hunk executables in bin/, using the vasm built by tools/build-vasm.sh.
#
# vasm flags:
#   -Fhunkexe     emit an AmigaOS hunk EXECUTABLE (HUNK_HEADER..HUNK_END, BE32)
#   -nosym        strip the debug symbol/line hunks (stripped = production form;
#                 the [J4] loader also skips HUNK_SYMBOL/HUNK_DEBUG, but stripped
#                 binaries keep the committed artifacts minimal)
#   -kick1hunks   emit classic HUNK_RELOC32 (1004) long-form relocations instead of
#                 the compact HUNK_DREL32 (1015) short form, so the [J4] loader's
#                 RELOC32 relocator (grounded on internalloadseg_aos.c:292-332)
#                 applies them. Only matters for programs with a relocated section
#                 (arraysum: lea nums,a0).
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # .../apps68k
VASM="$HERE/.toolchain/vasmm68k_mot"
[ -x "$VASM" ] || { echo "!! vasm not built; run tools/build-vasm.sh first"; exit 1; }
mkdir -p "$HERE/bin"

asm() {   # asm <name> [extra vasm flags]
    name="$1"; shift
    echo ">> $name.s -> bin/$name.exe"
    "$VASM" -Fhunkexe -nosym "$@" -o "$HERE/bin/$name.exe" "$HERE/$name.s" \
        2>&1 | grep -vE '^vasm |^$' || true
}

asm mul
asm fact
asm arraysum -kick1hunks      # has a relocated DATA section
asm libcall
asm sumsq -kick1hunks         # subroutines: nested bsr/jsr/rts + computed jsr(a0) ([J5f])
asm bubsort -kick1hunks -no-opt  # [J5g] bubble sort (indexed EA) + checksum (shifts/imm/misc);
                                 # -no-opt keeps the exact opcodes (vasm would fold addi->addq etc.)
asm mp64    -no-opt              # [J5h] 64-bit add/negate via add.l+addx.l / neg.l+negx.l
                                 # -no-opt keeps the exact X-chain opcodes (no fold to addq etc.)
asm j5i     -kick1hunks -no-opt  # [J5i] exception/SR model: trap #1 / divu.w #0 / illegal;
                                 # -kick1hunks for the jmp-finish RELOC32; -no-opt keeps the
                                 # exact opcodes (no addq fold of the exit packer)
asm mandel  -no-opt              # [J5j] CAPSTONE: fixed-point Mandelbrot ASCII renderer.
                                 # -no-opt keeps the exact opcodes (no fold of move.l #imm->
                                 # moveq, addi->addq, etc.) so the JIT + oracle decode the
                                 # precise muls.w/asr/(d16,a5)-EA/Bcc/cmpi set. No relocation
                                 # (PC-relative branches + abs sandbox scratch addr only).
asm j5l     -no-opt              # [J5l] movem save/restore: a compiler-style non-leaf sub
                                 # (movem.l d2-d7/a2-a6,-(sp) prologue + movem.l (sp)+,...
                                 # epilogue) around a clobbering body, plus the control/(d16,An)
                                 # /.w movem forms. -no-opt keeps the exact movem encodings.
                                 # PC-relative bsr only, no relocation.
echo ">> done. assembled:"
ls -l "$HERE/bin/"
