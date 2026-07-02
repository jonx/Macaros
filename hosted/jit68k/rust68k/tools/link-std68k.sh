#!/bin/bash
# link-std68k.sh — stage 2 for the 68k std probe: strip the staticlib, do a FULL
# GNU ld link (merged sections, --emit-relocs, image base 0, _start first), convert
# to a single-hunk executable with elfexec2hunk.py, and run it under BOTH engines.
#
# Why this pipeline and not vlink like the no_std corpus: LLVM emits 16-bit
# PC-relative references that must be RESOLVED at link time by merging sections
# (vlink's -mtype crashes on std-sized input, and unmerged PC16 cannot be
# represented in hunk format). A fully linked single-hunk image keeps every
# pc-relative fixup valid at any load address; only the absolute 32-bit relocs
# become HUNK_RELOC32 entries. See tools/elfexec2hunk.py.
#
# Usage: link-std68k.sh [toolchain]   (default: nightly-2026-06-27;
#        use m68k-ccr-fixed once tools/build-patched-rustc.sh has produced it)
set -euo pipefail
cd "$(dirname "$0")/../std68k-probe"

TC="${1:-nightly-2026-06-27}"
AR=/usr/local/bin/m68k-elf-ar
OBJCOPY=/usr/local/bin/m68k-elf-objcopy
LD=/usr/local/bin/m68k-elf-ld
RUN68K=../../../../build/run68k

cargo "+$TC" build --release -Zjson-target-spec \
    -Zbuild-std=std,panic_abort \
    --target ../../../rust/m68k-unknown-aros.json

rm -rf .obj && mkdir .obj
( cd .obj \
  && $AR x ../target/m68k-unknown-aros/release/libstd68k_probe.a \
  && for o in *.o; do $OBJCOPY -R .comment -R .note.GNU-stack -R .eh_frame "$o"; done \
  && $AR rcs stripped.a *.o && rm -f *.o )

cat > link68k.ld <<EOF
ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { ../libc68k/lvo.o(.text) *(.text) *(.text.*) }
  .rodata : { *(.rodata) *(.rodata.*) *(.data.rel.ro*) }
  .data : { *(.data) *(.data.*) }
  .bss : { *(.bss) *(.bss.*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.debug*) }
}
EOF
$LD -q --gc-sections -e _start -T link68k.ld -o std68k.elf \
    ../libc68k/lvo.o ../libc68k/libc68k.o .obj/stripped.a 2>&1 | grep -v 'RWX permissions' || true
python3 ../tools/elfexec2hunk.py std68k.elf std68k.exe

echo "== JIT:"
JIT68K_STEP_CAP=200000000 $RUN68K std68k.exe; echo "exit=$?"
echo "== interpreter:"
JIT68K_STEP_CAP=200000000 $RUN68K --interp std68k.exe; echo "exit=$?"
