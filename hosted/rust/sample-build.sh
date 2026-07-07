#!/bin/bash
# sample-build.sh -- build the RustHello std sample and deploy it two ways:
#   1. C:RustHello  in the booted AROS tree (bundled with the release)
#   2. <share>/RustHello  on the Mac folder that mounts as MacRW: (run it as
#      `MacRW:RustHello` from the AROS shell -- a Rust program off the mounted share)
#
# Mirrors std-build.sh's collect-aros link recipe. It defaults to ~/aros-build (the
# tree graft/run-window.sh boots); override with AROS_BUILD=<tree>/bin/darwin-aarch64.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
RSLIB="$DIR/sample/target/aarch64-unknown-aros/release/librust_hello_aros.a"
SHARE="${AROS_CTL_HOST_FOLDER:-$HOME/AROS/Shared}"
NIGHTLY="${RUST_NIGHTLY:-nightly-2026-06-27}"

# Prefer ~/aros-build (what run-window.sh boots); fall back to newest /tmp tree.
find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    local best="" bt=0 d t
    for d in \
        "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64" \
        /tmp/*/bin/darwin-aarch64 ; do
        [ -x "$d/tools/collect-aros" ] && [ -d "$d/gen/include" ] || continue
        # first complete tree wins if it is ~/aros-build; else newest collect-aros
        if [ "$d" = "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64" ]; then best="$d"; break; fi
        t="$(stat -f %m "$d/tools/collect-aros" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}

echo "[sample] build the Rust staticlib (std from ../rust-aros via -Zbuild-std)"
( cd "$DIR/sample" && cargo "+$NIGHTLY" build --release \
    -Zjson-target-spec -Zbuild-std=std,panic_abort \
    --target ../aarch64-unknown-aros.json )
[ -f "$RSLIB" ] || { echo "FAIL: $RSLIB missing" >&2; exit 1; }

T="$(find_tree)"
[ -n "$T" ] && [ -x "$T/tools/collect-aros" ] || { echo "no AROS build tree" >&2; exit 2; }
echo "[sample] AROS tree: $T"

GEN="$T/gen"; DEV="$T/AROS/Developer"; LIBDIR="$DEV/lib"
XTBIN="$T/tools/crosstools/bin"; XTLIB="$T/tools/crosstools/lib/generic"
CDIR="$T/AROS/C"; COLLECT="$T/tools/collect-aros"
CC="${AROS_CC:-clang}"
OUT="$REPO/build/rust-aros"; mkdir -p "$OUT"

CFLAGS=(--target=aarch64-unknown-none-elf -mcmodel=large -ffixed-x18 -D__arm64__ -O2
        -Wno-pointer-sign -Wno-int-conversion -Wno-implicit-function-declaration
        -I"$GEN/include" -I"$DEV/include")
AUTOLIB=(-lmui -lamiga -larossupport -lamiga -lcodesets -lkeymap -lexpansion
         -lcommodities -ldiskfont -lasl -lmuimaster -ldatatypes -lcybergraphics
         -lworkbench -licon -lintuition -lgadtools -llayers -laros -lpartition
         -liffparse -lgraphics -llocale -ldos -lutility -loop -llibinit -lautoinit)
STDLIBS=(-lposixc -lstdcio -lstdc -lexec -lpthread)

echo "[sample] compile hello_main.c + the std pal glues"
"$CC" "${CFLAGS[@]}" -c "$DIR/hello_main.c" -o "$OUT/hello_main.o"
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_net_glue.c" -o "$OUT/aros_net_glue.o"
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$DIR/aros_fs_glue.c" -o "$OUT/aros_fs_glue.o"
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_process_glue.c" -o "$OUT/aros_process_glue.o"
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_env_glue.c" -o "$OUT/aros_env_glue.o"
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_thread_glue.c" -o "$OUT/aros_thread_glue.o"
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$DIR/aros_sync_glue.c" -o "$OUT/aros_sync_glue.o"

echo "[sample] link RustHello (collect-aros -> ET_REL AROS program)"
COMPILER_PATH="$XTBIN" "$COLLECT" \
    --eh-frame-hdr --allow-multiple-definition \
    -L"$LIBDIR" -L"$XTLIB" -o "$OUT/RustHello" \
    "$LIBDIR/startup.o" "$OUT/hello_main.o" "$OUT/aros_net_glue.o" "$OUT/aros_fs_glue.o" "$OUT/aros_process_glue.o" "$OUT/aros_env_glue.o" "$OUT/aros_thread_glue.o" "$OUT/aros_sync_glue.o" "$RSLIB" \
    -\( "${AUTOLIB[@]}" "${STDLIBS[@]}" -\)
echo "[sample] built: $OUT/RustHello ($(stat -f%z "$OUT/RustHello" 2>/dev/null) bytes)"

# 1. bundle it into the AROS tree as a C: command
cp -f "$OUT/RustHello" "$CDIR/RustHello"; chmod +x "$CDIR/RustHello"
echo "[sample] deployed -> $CDIR/RustHello   (run in AROS as: RustHello)"

# 2. drop it on the Mac share so it also runs as MacRW:RustHello
if [ -d "$SHARE" ]; then
    cp -f "$OUT/RustHello" "$SHARE/RustHello"
    echo "[sample] copied  -> $SHARE/RustHello  (run in AROS as: MacRW:RustHello)"
else
    echo "[sample] note: share $SHARE not found; skipped MacRW: copy" >&2
fi
