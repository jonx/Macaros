#!/bin/bash
# build-rust68k.sh — rebuild the committed rust68k corpus binaries (bin/*.exe) from
# src/lib.rs.  The regression (make hosted-jit68k-rust) runs the COMMITTED binaries
# and needs none of this; run this only to regenerate them after editing the corpus
# or to pick up a new rustc.
#
# Requirements (all already present on this machine, see README.md):
#   - rustup nightly with the rust-src component (rustc's LLVM ships the experimental
#     M68k backend; m68k-unknown-none-elf is a Tier 3 target built via -Zbuild-std)
#   - Homebrew m68k-elf binutils: m68k-elf-as / -ar / -objcopy
#   - vlink from ../apps68k/.toolchain (build once: ../apps68k/tools/build-vbcc.sh)
#
# Pipeline per program:
#   cargo (staticlib, pinned profile)  ->  strip .comment/.eh_frame per object
#   ->  m68k-elf-as entry stub  ->  vlink -bamigahunk -s -gc-all -mtype  ->  bin/*.exe
# (-gc-all prunes the compiler_builtins staticlib roots; -mtype merges rustc's
#  per-function sections into one hunk per type; .comment must go because its odd
#  payload size trips run68k's strict hunk-size check.)
set -euo pipefail
cd "$(dirname "$0")/.."

VLINK=../apps68k/.toolchain/vlink
AS=m68k-elf-as
AR=m68k-elf-ar
OBJCOPY=m68k-elf-objcopy
TARGET=m68k-unknown-none-elf

[ -x "$VLINK" ] || { echo "rust68k: $VLINK missing — run ../apps68k/tools/build-vbcc.sh first" >&2; exit 1; }
command -v $AS >/dev/null || { echo "rust68k: $AS not found (brew install m68k-elf-binutils)" >&2; exit 1; }
rustup +nightly component list 2>/dev/null | grep -q '^rust-src (installed)' \
    || { echo "rust68k: rust-src missing (rustup +nightly component add rust-src)" >&2; exit 1; }

# Two crates (see the notes in each Cargo.toml):
#  - the corpus crate (this dir): the alloc programs, lto=true so they fit the sandbox;
#  - fibcore/: fib ALONE, no LTO — with the upstream MOVE-clobbers-CCR miscompile open,
#    correct loop scheduling is register-allocation luck, and fibcore/'s exact crate
#    shape + profile is the verified-correct combination on the pinned nightly.
# A regenerated binary is only trusted after `make hosted-jit68k-rust` re-passes: with
# the compiler bug open, ANY profile/source change can flip a loop from correct to
# miscompiled (that is exactly what the two-engine check catches).
cargo +nightly build --release --target $TARGET -Zbuild-std=core,alloc
( cd fibcore && cargo +nightly build --release --target $TARGET -Zbuild-std=core )
( cd hello   && cargo +nightly build --release --target $TARGET -Zbuild-std=core )

strip_pack() {  # $1 = staticlib path, $2 = output archive name (inside .obj/)
    ( cd .obj \
      && rm -f *.o && $AR x "$1" \
      && for o in *.o; do $OBJCOPY -R .comment -R .note.GNU-stack -R .eh_frame "$o"; done \
      && $AR rcs "$2" *.o && rm -f *.o )
}

rm -rf .obj && mkdir .obj
strip_pack "../target/$TARGET/release/librust68k_corpus.a"          stripped.a
strip_pack "../fibcore/target/$TARGET/release/libfibcore.a"         stripped-fib.a
strip_pack "../hello/target/$TARGET/release/libhello68k.a"          stripped-hello.a

mkdir -p bin
for prog in fib allocprobe vecsum vecsum_inclusive hello; do
    lib=.obj/stripped.a
    [ "$prog" = fib ]   && lib=.obj/stripped-fib.a
    [ "$prog" = hello ] && lib=.obj/stripped-hello.a
    $AS -m68000 entry_$prog.s -o .obj/entry_$prog.o
    $VLINK -bamigahunk -s -gc-all -mtype -e _start \
        -o bin/$prog.exe .obj/entry_$prog.o $lib
    echo "rust68k: built bin/$prog.exe ($(stat -f%z bin/$prog.exe) bytes)"
done
rm -rf .obj
