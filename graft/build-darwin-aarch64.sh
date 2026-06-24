#!/usr/bin/env bash
# Reproducible recipe: build AROS for the (new) darwin-aarch64 hosted target on
# Apple Silicon, as far as the trail has been blazed. Run from anywhere; edit the
# paths at the top. This is the live-grounded companion to GRAFT.md — every step
# here was actually run on an M-series Mac (macOS 26, Xcode/clang 21).
#
# Status reached: configure succeeds, all host tools build, a native-LLVM
# aarch64-ELF cross-toolchain works, the compiler test passes, and the build
# COMPILES real AROS source (compiler/alib) for darwin-aarch64 — until it needs
# AROS's *patched* clang for spec-flags like -noposixc (see GRAFT.md "current wall").
set -euo pipefail

AROS_SRC="${AROS_SRC:-/Users/user/Source/aros-upstream}"   # checkout w/ the aarch64-darwin-graft branch
BUILD="${BUILD:-/tmp/arosbuild}"
XTOOLS="${XTOOLS:-/tmp/aros-xtools}"                       # thin LLVM crosstools
LLVM="${LLVM:-/opt/homebrew/opt/llvm/bin}"
SHIM="${SHIM:-/tmp/graft-tools}"                          # objcopy shim dir

# 1) Host build prerequisites (the chain configure walks, found one error at a time)
brew install gawk automake autoconf bison flex netpbm libpng gnu-sed >/dev/null || true
python3 -m pip install mako --break-system-packages -q || true
mkdir -p "$SHIM"; ln -sf "$LLVM/llvm-objcopy" "$SHIM/objcopy"   # macOS lacks objcopy

# 2) A thin "crosstools": native LLVM, but clang/clang++ retargeted to aarch64 ELF
#    (not Mach-O) and forced to use lld (Apple ld can't link ELF). The other tools
#    are arch-agnostic, so symlink them. AROS wants them under <install>/bin with
#    plain names (target_tool_prefix is empty for the llvm toolchain).
rm -rf "$XTOOLS"; mkdir -p "$XTOOLS/bin"
printf '#!/bin/sh\nexec %s/clang --target=aarch64-unknown-none-elf -fuse-ld=lld "$@"\n'   "$LLVM" > "$XTOOLS/bin/clang"
printf '#!/bin/sh\nexec %s/clang++ --target=aarch64-unknown-none-elf -fuse-ld=lld "$@"\n' "$LLVM" > "$XTOOLS/bin/clang++"
printf '#!/bin/sh\nexec %s/clang-cpp --target=aarch64-unknown-none-elf "$@"\n'            "$LLVM" > "$XTOOLS/bin/clang-cpp"
chmod +x "$XTOOLS"/bin/clang "$XTOOLS"/bin/clang++ "$XTOOLS"/bin/clang-cpp
for t in ld.lld llvm-as llvm-ar llvm-ranlib llvm-nm llvm-strip llvm-objcopy llvm-objdump llvm-mc; do
    ln -sf "$LLVM/$t" "$XTOOLS/bin/$t"
done

export PATH="$SHIM:/opt/homebrew/bin:$PATH"

# 3) Configure. NOTE the AROS-style target order is <arch>-<cpu> => darwin-aarch64
#    (NOT aarch64-darwin — that parses as arch=aarch64, which is rejected).
rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"
"$AROS_SRC/configure" \
    --target=darwin-aarch64 \
    --with-toolchain=llvm \
    --with-aros-toolchain-install="$XTOOLS"

# 4) Build a core metatarget (kernel-exec = exec.library). The default 'make'
#    target tries to download+build LLVM 11, libcxx, compiler-rt and ACPICA from
#    source; we don't need that (we supplied a toolchain), and the branch already
#    drops the ACPICA dependency that otherwise gates everything.
make kernel-exec
# (Currently stops at compiler/alib with `clang: error: unknown argument
#  '-noposixc'` — see GRAFT.md. Next step: AROS's patched-clang crosstools, or
#  replicate the AROS specs in the clang wrapper.)
