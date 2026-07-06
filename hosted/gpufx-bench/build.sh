#!/bin/bash
# Build C:GpuFxBench -- the software-vs-3D-shim video benchmark. Mirrors
# hosted/feraille/build.sh: cargo builds the Rust staticlib for
# aarch64-unknown-aros, then collect-aros links it with the C harness
# (bench_main.c, which host-binds the shim's cm_gpu_*) into an AROS C: command.
#
#   hosted/gpufx-bench/build.sh                 # build + deploy C:GpuFxBench
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
RUSTDIR="$REPO/hosted/rust"
RSLIB="$DIR/target/aarch64-unknown-aros/release/libgpufx_bench.a"

T="${AROS_BUILD:-$HOME/aros-build}/bin/darwin-aarch64"
XTOOLS="${AROS_CROSSTOOLS:-$HOME/aros-crosstools}"
GEN="$T/gen"; DEV="$T/AROS/Developer"; LIBDIR="$DEV/lib"
CDIR="$T/AROS/C"; COLLECT="$T/tools/collect-aros"
CC="${AROS_CC:-clang}"
OUT="$DIR/build"; mkdir -p "$OUT"

[ -x "$COLLECT" ] || { echo "FAIL: no collect-aros at $COLLECT" >&2; exit 2; }
[ -f "$GEN/include/aros/hostbind.h" ] || {
    echo "FAIL: <aros/hostbind.h> not staged -- 'make includes' first" >&2; exit 2; }

echo "[gpufx-bench] cargo build (aarch64-unknown-aros, release)"
( cd "$DIR" && cargo +nightly-2026-06-27 build --release \
      -Zjson-target-spec -Zbuild-std=std,panic_abort \
      --target "$RUSTDIR/aarch64-unknown-aros.json" )
[ -f "$RSLIB" ] || { echo "FAIL: $RSLIB missing" >&2; exit 1; }

# collect-aros resolves `ld` via COMPILER_PATH; the AROS-patched lld lives in
# the crosstools dir (same shim as hosted/feraille / feraille-aros-app).
XTBIN="$OUT/toolshim"; mkdir -p "$XTBIN"
ln -sf "$XTOOLS/bin/ld.lld" "$XTBIN/ld"

CFLAGS=(--target=aarch64-unknown-none-elf -mcmodel=large -ffixed-x18 -D__arm64__
        -O2 -Wall -Wno-pointer-sign -I"$GEN/include" -I"$DEV/include"
        -I"$REPO/hosted/cocoametal") # cocoametal.h: CmGpu*Req structs

echo "[gpufx-bench] compile harness + hostbind glue"
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -I"$GEN/include/aros/stdc" \
    -c "$DIR/c/bench_main.c" -o "$OUT/bench_main.o"

# rust-aros std C glues (same set hosted/feraille links; the benchmark uses
# std time/thread/args/fs-for-redirect).
for g in aros_net_glue aros_process_glue aros_thread_glue; do
    "$CC" "${CFLAGS[@]}" -c "$RUSTDIR/$g.c" -o "$OUT/$g.o"
done
for g in aros_fs_glue aros_sync_glue; do
    "$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$RUSTDIR/$g.c" -o "$OUT/$g.o"
done

AUTOLIB=(-lamiga -larossupport -lamiga -laros -lgraphics -lintuition
         -llayers -lcybergraphics -ldos -lutility -loop -llibinit -lautoinit)
STDLIBS=(-lposixc -lstdcio -lstdc -lexec -lpthread)

echo "[gpufx-bench] link C:GpuFxBench"
COMPILER_PATH="$XTBIN" "$COLLECT" \
    --eh-frame-hdr --allow-multiple-definition \
    -L"$LIBDIR" -o "$OUT/GpuFxBench" \
    "$LIBDIR/startup.o" "$OUT/bench_main.o" \
    "$OUT/aros_net_glue.o" "$OUT/aros_fs_glue.o" "$OUT/aros_process_glue.o" \
    "$OUT/aros_thread_glue.o" "$OUT/aros_sync_glue.o" "$RSLIB" \
    -\( "${AUTOLIB[@]}" "${STDLIBS[@]}" -\)

"$XTOOLS/bin/llvm-strip" --strip-debug "$OUT/GpuFxBench"
echo "[gpufx-bench] built + stripped: $OUT/GpuFxBench ($(stat -f%z "$OUT/GpuFxBench") bytes)"

cp -f "$OUT/GpuFxBench" "$CDIR/GpuFxBench"; chmod +x "$CDIR/GpuFxBench"
echo "[gpufx-bench] deployed -> $CDIR/GpuFxBench"
