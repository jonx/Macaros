#!/bin/bash
# build-patched-rustc.sh — build a rustc whose LLVM carries the M68k CCR fix
# (llvm/llvm-project#152816, dansalvato's copyPhysReg workaround branch), because
# the stock backend miscompiles flag-consuming branches after register copies and
# that blocks Rust std on m68k outright (see ../UPSTREAM-LLVM-CCR-BUG.md and
# hosted/rust/STD68K-PLAN.md).
#
# Produces a rustup toolchain named `m68k-ccr-fixed`. Long build (LLVM + stage1
# std: roughly 1-2 h on an M-series). Everything lands under /tmp/rust-m68k.
#
# After it finishes:
#   cd hosted/jit68k/rust68k/std68k-probe
#   cargo +m68k-ccr-fixed build --release -Zjson-target-spec \
#       -Zbuild-std=std,panic_abort -Zbuild-std-features=compiler-builtins-mem \
#       --target ../../../rust/m68k-unknown-aros.json
#   ../tools/link-std68k.sh   # relink + two-engine run
set -euo pipefail

# This Mac has BOTH Homebrew prefixes; the Intel one (/usr/local) leaks an x86_64
# liblzma into bootstrap's link. Force lzma-sys to build its bundled copy.
export LZMA_API_STATIC=1

WORK=/tmp/rust-m68k
NIGHTLY=nightly-2026-06-27
PATCH_URL="https://github.com/llvm/llvm-project/compare/main...dansalvato:llvm-project:m68k-fix-ccr-kill.patch"

COMMIT=$(rustc +$NIGHTLY -vV | awk '/commit-hash/ {print $2}')
echo "== rust commit for $NIGHTLY: $COMMIT"

if [ ! -d "$WORK/.git" ]; then
    mkdir -p "$WORK"
    git -C "$WORK" init -q
    git -C "$WORK" remote add origin https://github.com/rust-lang/rust
    git -C "$WORK" fetch --depth 1 origin "$COMMIT"
    git -C "$WORK" checkout -q FETCH_HEAD
fi
cd "$WORK"
git submodule update --init --depth 1 src/llvm-project

echo "== applying the CCR workaround (minimal port, vendored patch)"
# Neither dansalvato's 6-commit series nor his whole M68k directory fit this
# nightly's LLVM snapshot (API drift in BOTH directions). The vendored
# m68k-ccr-minimal.patch is the surgical port: copyPhysReg saves/restores a
# live CCR around data-register copies via a free data register (the memory
# push/pop last resort is dropped — this snapshot's tablegen lacks those
# opcodes, and with eight data registers the spill path is theoretical).
# Generated with `git diff` against this exact snapshot, so it always applies.
PATCH_DIR="$(cd "$(dirname "$0")" && pwd)"
if ! grep -q 'EmitCopyPreservingCCR' src/llvm-project/llvm/lib/Target/M68k/M68kInstrInfo.cpp; then
    git -C src/llvm-project apply "$PATCH_DIR/m68k-ccr-minimal.patch"
fi
grep -c 'EmitCopyPreservingCCR' src/llvm-project/llvm/lib/Target/M68k/M68kInstrInfo.cpp

cat > bootstrap.toml <<'EOF'
profile = "library"
[llvm]
experimental-targets = "M68k"
download-ci-llvm = false
[rust]
incremental = false
EOF
# older checkouts name it config.toml; keep both in sync
cp bootstrap.toml config.toml 2>/dev/null || true

echo "== building stage1 rustc + std (long)"
./x build library --stage 1

# CRITICAL when iterating on the LLVM patch: x links rustc against the INSTALL
# prefix (build/<host>/llvm/lib), which only updates via `ninja install` — a bare
# `ninja` in llvm/build leaves rustc linking the stale archive. After any change
# to the M68k files: ninja install in llvm/build, delete the driver artifacts
# (rustc_llvm / rustc-main / librustc_driver) under stage1-rustc, rerun x.

rustup toolchain link m68k-ccr-fixed "$WORK/build/host/stage1" || \
rustup toolchain link m68k-ccr-fixed "$WORK"/build/*/stage1
echo "== done: toolchain m68k-ccr-fixed linked"
