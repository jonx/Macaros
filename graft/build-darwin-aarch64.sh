#!/usr/bin/env bash
# Build hosted AROS for the darwin-aarch64 target on Apple Silicon.
#
# This is the real, working recipe (it boots to a Workbench desktop). It is the
# runnable companion to docs/features/build/README.md — read that doc for the
# full rationale, the desktop-userland set, and the symptom->cause table.
#
# The four rules that save you days (all baked in below):
#   1. build in a STABLE dir (not a session scratchpad that gets GC'd mid-build)
#   2. REUSE the cross-toolchain — never rebuild the ~1-2h LLVM from scratch
#   3. never a bare `make` (it tries to rebuild LLVM and breaks on darwin modules)
#   4. build module METATARGETS, not the `kernel`/`workbench-libs` aggregates
#
# Usage:
#   graft/build-darwin-aarch64.sh                 # configure + build the boot set
#   AROS_SRC=../aros-upstream graft/build-darwin-aarch64.sh
#   PRESERVE_TOOLCHAIN=1 graft/build-darwin-aarch64.sh   # copy crosstools aside after first build
set -euo pipefail

# --- Paths (override via env) ------------------------------------------------
AROS_SRC="${AROS_SRC:-../aros-upstream}"   # checkout on branch aarch64-darwin-graft
BUILD="${BUILD:-/tmp/arosbuild}"           # STABLE build dir (rule 1)
XT="${XT:-/tmp/aros-crosstools}"           # preserved, reusable crosstools (rule 2)
LLVM="${LLVM:-/opt/homebrew/opt/llvm/bin}"
SHIM="${SHIM:-/tmp/graft-tools}"           # objcopy shim dir

AROS_SRC="$(cd "$AROS_SRC" && pwd)"        # absolutise (configure is picky about relative)

# --- 1) Host prerequisites ---------------------------------------------------
# One-time; harmless to re-run. `mako` is the genmodule template engine.
brew install llvm cmake zstd gawk automake autoconf bison flex netpbm libpng gnu-sed >/dev/null || true
python3 -m pip install mako --break-system-packages -q || true

# macOS has no objcopy; AROS needs one. Expose llvm-objcopy under that name.
mkdir -p "$SHIM"; ln -sf "$LLVM/llvm-objcopy" "$SHIM/objcopy"
export PATH="$SHIM:/opt/homebrew/bin:$PATH"

# --- 2) Configure ------------------------------------------------------------
# If a preserved toolchain exists, REUSE it (skips the ~1-2h LLVM build).
# Otherwise configure builds the AROS-patched clang 20.1.0 from source the first
# time. --without-x: the display is Cocoa, not X11.
REUSE_ARGS=()
if [ -x "$XT/bin/clang" ]; then
    echo "### reusing preserved crosstools at $XT (no LLVM rebuild)"
    REUSE_ARGS=(--with-aros-toolchain=yes --with-aros-toolchain-install="$XT")
else
    echo "### no preserved crosstools at $XT — the first build compiles clang 20.1.0 (~1-2h, one time)"
fi

rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"
# NOTE the AROS target order is <arch>-<cpu> => darwin-aarch64 (NOT aarch64-darwin).
"$AROS_SRC/configure" \
    --target=darwin-aarch64 \
    --with-toolchain=llvm \
    --without-x \
    "${REUSE_ARGS[@]}"

# Preserve the freshly-built toolchain for next time (rule 2).
if [ "${PRESERVE_TOOLCHAIN:-0}" = 1 ] && [ ! -x "$XT/bin/clang" ]; then
    TOOLS="$BUILD/bin/darwin-aarch64/tools/crosstools"
    if [ -x "$TOOLS/bin/clang" ]; then
        echo "### preserving crosstools -> $XT"
        cp -a "$TOOLS" "$XT"
    fi
fi

# --- 3) Build the boot module set (rules 3 + 4) ------------------------------
# Explicit metatargets, ONE PER `make` CALL (a mid-run mmake rebuild otherwise
# staleness the metatarget DB -> "Nothing known about project kernel-kernel").
# This set boots to a CLI. The full Wanderer desktop needs more userland — see
# docs/features/build/README.md section 3b.
TARGETS=(
    kernel-exec kernel-kernel kernel-dos kernel-dosboot kernel-utility
    kernel-intuition kernel-graphics kernel-layers kernel-keymap
    kernel-console kernel-input kernel-keyboard kernel-clipboard
    kernel-hidd kernel-hidd-cocoa kernel-bootstrap-hosted
    workbench-libs-stdc workbench-libs-cybergraphics
)
for t in "${TARGETS[@]}"; do
    echo "### make $t"
    make "$t"
done

echo
echo "### done. Boot it from the repo:"
echo "###   make cocoametal-dylib pasteboard-dylib coreaudio-dylib bsdsock-dylib"
echo "###   graft/aros-ctl deploy && AROS_CTL_STARTUP_MODE=desktop graft/run-window.sh"
