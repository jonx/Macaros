#!/bin/bash
# std-build.sh -- stage 2 for the RS3 std probe: link libstd_probe.a (Rust built
# with -Zbuild-std=std for aarch64-unknown-aros) + rs3_main.c into a loadable AROS
# C: command and deploy it. Mirrors aros-build.sh (the proven [RS0]/[RS1] recipe);
# the only differences are the staticlib (std, not no_std aros-rt) and the harness.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
RSLIB="$DIR/std-probe/target/aarch64-unknown-aros/release/libstd_probe.a"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    local best="" bt=0 d t
    for d in \
        "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64" \
        /tmp/*/bin/darwin-aarch64 ; do
        [ -x "$d/tools/collect-aros" ] && [ -d "$d/gen/include" ] || continue
        t="$(stat -f %m "$d/tools/collect-aros" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}

[ -f "$RSLIB" ] || { echo "FAIL: $RSLIB missing -- build the std probe first" >&2; exit 1; }
T="$(find_tree)"
[ -n "$T" ] && [ -x "$T/tools/collect-aros" ] || { echo "no AROS build tree" >&2; exit 2; }
echo "[stage2] AROS tree: $T"

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

echo "[stage2] compile harness rs3_main.c + glues"
"$CC" "${CFLAGS[@]}" -c "$DIR/rs3_main.c" -o "$OUT/rs3_main.o"
# the probe staticlib also contains aros_rust_net_test (one crate object), which
# references the bsdsocket glue -- link it here too so RustStd resolves.
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_net_glue.c" -o "$OUT/aros_net_glue.o"
# the fs metadata glue needs the posixc include for <sys/stat.h> (resolves to AROS's
# own header, not the macOS SDK).
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$DIR/aros_fs_glue.c" -o "$OUT/aros_fs_glue.o"
# the process glue (dos SystemTagList) needs no special include.
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_process_glue.c" -o "$OUT/aros_process_glue.o"
# the env glue (walk pr_LocalVars for std::env::vars); AROS headers only, no include.
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_env_glue.c" -o "$OUT/aros_env_glue.o"
# thread glue (pthread spawn/join, header-clean) + sync glue (pthread mutex/cond,
# needs the posixc include for <pthread.h>).
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_thread_glue.c" -o "$OUT/aros_thread_glue.o"
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$DIR/aros_sync_glue.c" -o "$OUT/aros_sync_glue.o"

echo "[stage2] link RustStd (collect-aros -> ET_REL AROS program)"
COMPILER_PATH="$XTBIN" "$COLLECT" \
    --eh-frame-hdr --allow-multiple-definition \
    -L"$LIBDIR" -L"$XTLIB" -o "$OUT/RustStd" \
    "$LIBDIR/startup.o" "$OUT/rs3_main.o" "$OUT/aros_net_glue.o" "$OUT/aros_fs_glue.o" "$OUT/aros_process_glue.o" "$OUT/aros_env_glue.o" "$OUT/aros_thread_glue.o" "$OUT/aros_sync_glue.o" "$RSLIB" \
    -\( "${AUTOLIB[@]}" "${STDLIBS[@]}" -\)
echo "[stage2] built: $OUT/RustStd ($(stat -f%z "$OUT/RustStd" 2>/dev/null) bytes)"

cp -f "$OUT/RustStd" "$CDIR/RustStd"; chmod +x "$CDIR/RustStd"
echo "[stage2] deployed -> $CDIR/RustStd"
