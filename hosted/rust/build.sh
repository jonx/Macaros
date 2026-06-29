#!/bin/sh
# build.sh — stage 1 of the no_std Rust-on-AROS build: cross-compile the runtime
# crate. Markers [RS0] (codegen+link+startup) and [RS1] (allocator+collections)
# from docs/features/rust-aros/README.md. Both are PROVEN LIVE on AROS.
#
#   STAGE 1 (here): cross-compile aros-rt to the custom target with stock nightly +
#     -Zbuild-std=core,alloc. Needs NO AROS crosstools and NO sysroot (a no_std
#     staticlib links nothing), so it runs anywhere and PROVES: the target spec
#     parses, codegen for aarch64-AROS works, and the #[global_allocator] + alloc
#     collections compile. Then it dumps the ABI boundary (exported Rust symbols +
#     the four imported flat-C symbols).
#
#   STAGE 2 (./aros-build.sh): compile the C glue/harness with the AROS crosstools,
#     collect-aros-link the staticlib into a real ET_REL AROS C: command (with
#     startup.o), and deploy. graft/rust-smoke = stage1 + stage2 + boot + assert.
#
#   ./build.sh           stage 1, with ABI dump
#   ./build.sh --build   stage 1 only, no symbol dump
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TARGET_JSON="$DIR/aarch64-unknown-aros.json"
CRATE="$DIR/aros-rt"
TARGET_NAME="aarch64-unknown-aros"
# Default: let rust-toolchain.toml pick the pinned nightly (no +toolchain override).
# RUST_NIGHTLY=<name> forces a specific toolchain instead.
TC="${RUST_NIGHTLY:+ +$RUST_NIGHTLY}"

# ---- STAGE 1: cargo build-std (real, no AROS toolchain needed) --------------
echo "[RS0/RS1] cargo${TC:- (rust-toolchain.toml pin)} build-std core,alloc -> target $TARGET_NAME"
( cd "$CRATE" && cargo$TC build --release \
    -Z build-std=core,alloc \
    -Z json-target-spec \
    --target "$TARGET_JSON" )

LIB="$CRATE/target/$TARGET_NAME/release/libaros_rt.a"
[ -f "$LIB" ] || { echo "FAIL: $LIB not produced"; exit 1; }
echo "[RS1] ok: $LIB"

[ "${1:-}" = "--build" ] && { echo "[build] stage-1 ok (staticlib built)"; exit 0; }

# ---- ABI boundary dump (grounds the contract in the actual object) ----------
NM=""
for c in llvm-nm /opt/homebrew/opt/llvm/bin/llvm-nm; do
    command -v "$c" >/dev/null 2>&1 && { NM="$c"; break; }
done
if [ -n "$NM" ]; then
    echo "--- Rust EXPORTS the C harness calls (T = defined text) ---"
    "$NM" "$LIB" 2>/dev/null | grep -E ' T (aros_rust_selftest|aros_rust_alloc_checksum)$' || true
    echo "--- Rust IMPORTS from aros_rt_glue.c (U = undefined, the flat-C boundary) ---"
    "$NM" "$LIB" 2>/dev/null | grep -E ' U (aros_exec_allocvec|aros_exec_freevec|aros_rt_puts|aros_rt_abort)$' | sort -u || true
else
    echo "(no llvm-nm on PATH — skipping ABI symbol dump; staticlib still built)"
fi

# ---- STAGE 2: AROS-side link + deploy lives in aros-build.sh -----------------
# (compile glue+harness with the crosstools, collect-aros-link into an ET_REL AROS
# C: command with the proper startup.o, deploy). It auto-discovers the AROS build
# tree; run it directly, or graft/rust-smoke for the full build->deploy->boot->assert.
echo "[build] stage-1 done. AROS-side link + deploy:  ./aros-build.sh"
echo "[build] full proof on booted AROS:              ../../graft/rust-smoke"
