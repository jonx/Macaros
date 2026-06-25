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
echo ">> done. assembled:"
ls -l "$HERE/bin/"
