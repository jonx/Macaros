#!/bin/bash
# stdnet-build.sh -- link the std probe + the bsdsocket net glue + rs_stdnet_main.c
# into C:RustStdNet (the RSN std::net::TcpStream round-trip). Same recipe as
# net-build.sh; the only change is the harness (rs_stdnet_main.c) and the output
# name. The net pal (sys/net/connection/aros.rs) lives inside libstd_probe.a and
# resolves its aros_np_* symbols against aros_net_glue.o at this final link.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
RSLIB="$DIR/std-probe/target/aarch64-unknown-aros/release/libstd_probe.a"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    local best="" bt=0 d t
    for d in "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64" /tmp/*/bin/darwin-aarch64 ; do
        [ -x "$d/tools/collect-aros" ] && [ -d "$d/gen/include" ] || continue
        t="$(stat -f %m "$d/tools/collect-aros" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}

[ -f "$RSLIB" ] || { echo "FAIL: $RSLIB missing -- build the std probe first" >&2; exit 1; }
T="$(find_tree)"
[ -n "$T" ] && [ -x "$T/tools/collect-aros" ] || { echo "no AROS build tree" >&2; exit 2; }
echo "[stdnet] AROS tree: $T"

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

echo "[stdnet] compile glue + harness (-ffixed-x18)"
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_net_glue.c"  -o "$OUT/aros_net_glue.o"
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$DIR/aros_fs_glue.c" -o "$OUT/aros_fs_glue.o"
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_process_glue.c" -o "$OUT/aros_process_glue.o"
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_thread_glue.c" -o "$OUT/aros_thread_glue.o"
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$DIR/aros_sync_glue.c" -o "$OUT/aros_sync_glue.o"
"$CC" "${CFLAGS[@]}" -c "$DIR/rs_stdnet_main.c" -o "$OUT/rs_stdnet_main.o"

echo "[stdnet] link RustStdNet"
COMPILER_PATH="$XTBIN" "$COLLECT" \
    --eh-frame-hdr --allow-multiple-definition \
    -L"$LIBDIR" -L"$XTLIB" -o "$OUT/RustStdNet" \
    "$LIBDIR/startup.o" "$OUT/rs_stdnet_main.o" "$OUT/aros_net_glue.o" "$OUT/aros_fs_glue.o" "$OUT/aros_process_glue.o" "$OUT/aros_thread_glue.o" "$OUT/aros_sync_glue.o" "$RSLIB" \
    -\( "${AUTOLIB[@]}" "${STDLIBS[@]}" -\)
echo "[stdnet] built: $OUT/RustStdNet ($(stat -f%z "$OUT/RustStdNet" 2>/dev/null) bytes)"
cp -f "$OUT/RustStdNet" "$CDIR/RustStdNet"; chmod +x "$CDIR/RustStdNet"
echo "[stdnet] deployed -> $CDIR/RustStdNet"
