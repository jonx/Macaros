#!/bin/sh
# build.sh — self-contained build for the no_std Rust-on-AROS runtime shim.
# Markers [RS0] (codegen+link+startup) and [RS1] (allocator+collections) from
# docs/features/rust-aros/README.md.
#
# Two stages, deliberately split by what the toolchain situation can do TODAY:
#
#   STAGE 1 (always, real): cross-compile aros-rt to the custom target with stock
#     nightly + -Zbuild-std=core,alloc. This needs NO AROS crosstools and NO
#     sysroot (a no_std staticlib links nothing), so it runs in the loop right now
#     and PROVES: the target spec parses, codegen for aarch64-AROS works, and the
#     #[global_allocator] + alloc collections compile. Then it dumps the ABI
#     boundary (exported Rust symbols + the four imported flat-C symbols).
#
#   STAGE 2 (gated on $AROS_CC, the AROS-side link): compile aros_rt_glue.c +
#     rs0_main.c with the AROS crosstools cc and link the staticlib into an AROS
#     ELF, then (optionally) run it on booted AROS via aros-ctl for the real
#     PASS/FAIL. Skipped with a clear note until the AROS C SDK/compiler is up
#     (graft/build-darwin-aarch64.sh "current wall") — same standalone convention
#     as the other hosted/ shims: this prep is developed in parallel, merged later.
#
#   ./build.sh           stage 1 (+ stage 2 if $AROS_CC set), report
#   ./build.sh --build   stage 1 only, no symbol dump
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TARGET_JSON="$DIR/aarch64-unknown-aros.json"
CRATE="$DIR/aros-rt"
TARGET_NAME="aarch64-unknown-aros"
TOOLCHAIN="${RUST_NIGHTLY:-nightly}"

# ---- STAGE 1: cargo build-std (real, no AROS toolchain needed) --------------
echo "[RS0/RS1] cargo +$TOOLCHAIN build-std core,alloc -> target $TARGET_NAME"
( cd "$CRATE" && cargo "+$TOOLCHAIN" build --release \
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

# ---- STAGE 2: AROS-side compile + link (gated) ------------------------------
if [ -n "${AROS_CC:-}" ] && command -v "$AROS_CC" >/dev/null 2>&1; then
    OUT="$REPO/build/rust-aros"; mkdir -p "$OUT"
    echo "[RS0] AROS link via \$AROS_CC=$AROS_CC -> $OUT/rs0_test"
    "$AROS_CC" -O2 -c "$DIR/aros_rt_glue.c" -o "$OUT/aros_rt_glue.o"
    "$AROS_CC" -O2 -c "$DIR/rs0_main.c"     -o "$OUT/rs0_main.o"
    "$AROS_CC" "$OUT/rs0_main.o" "$OUT/aros_rt_glue.o" "$LIB" -o "$OUT/rs0_test"
    echo "[RS0] linked: $OUT/rs0_test  (run on booted AROS via aros-ctl for PASS/FAIL)"
else
    echo "[RS0] AROS link SKIPPED: set \$AROS_CC to the AROS crosstools cc to build"
    echo "      rs0_test (needs the AROS C SDK/proto headers). Stage 1 above is the"
    echo "      real proof for now; stage 2 runs once the AROS C compiler is up."
fi
echo "[build] done."
