#!/bin/bash
# build.sh -- build + link the Feraille stage-1 probe into C:FerailleProbe.
# Stage 1 (Rust): cargo -Zbuild-std staticlib for aarch64-unknown-aros.
# Stage 2 (link): the proven std-build.sh recipe -- collect-aros, startup.o,
# the std pal glue objects from hosted/rust, posixc/stdc/pthread.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
RUSTDIR="$REPO/hosted/rust"
RSLIB="$DIR/core-probe/target/aarch64-unknown-aros/release/libferaille_core_probe.a"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    local best="" bt=0 d t
    for d in "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64" /tmp/*/bin/darwin-aarch64 ; do
        [ -x "$d/tools/collect-aros" ] && [ -d "$d/gen/include" ] || continue
        t="$(stat -f %m "$d/tools/collect-aros" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}

echo "[feraille-probe] stage 1: cargo build (aarch64-unknown-aros)"
( cd "$DIR/core-probe" && cargo +nightly-2026-06-27 build --release \
      -Zjson-target-spec -Zbuild-std=std,panic_abort \
      --target "$RUSTDIR/aarch64-unknown-aros.json" )
[ -f "$RSLIB" ] || { echo "FAIL: $RSLIB missing" >&2; exit 1; }

T="$(find_tree)"
[ -n "$T" ] && [ -x "$T/tools/collect-aros" ] || { echo "no AROS build tree" >&2; exit 2; }
echo "[feraille-probe] stage 2: AROS tree: $T"

GEN="$T/gen"; DEV="$T/AROS/Developer"; LIBDIR="$DEV/lib"
XTBIN="$T/tools/crosstools/bin"; XTLIB="$T/tools/crosstools/lib/generic"
CDIR="$T/AROS/C"; COLLECT="$T/tools/collect-aros"
CC="${AROS_CC:-clang}"
OUT="$REPO/build/feraille-aros"; mkdir -p "$OUT"

CFLAGS=(--target=aarch64-unknown-none-elf -mcmodel=large -ffixed-x18 -D__arm64__ -O2
        -Wno-pointer-sign -Wno-int-conversion -Wno-implicit-function-declaration
        -I"$GEN/include" -I"$DEV/include")
AUTOLIB=(-lmui -lamiga -larossupport -lamiga -lcodesets -lkeymap -lexpansion
         -lcommodities -ldiskfont -lasl -lmuimaster -ldatatypes -lcybergraphics
         -lworkbench -licon -lintuition -lgadtools -llayers -laros -lpartition
         -liffparse -lgraphics -llocale -ldos -lutility -loop -llibinit -lautoinit)
STDLIBS=(-lposixc -lstdcio -lstdc -lexec -lpthread)

echo "[feraille-probe] compile harness"
# printf needs the posixc/stdc headers (the raw -I set has no <stdio.h>).
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -I"$GEN/include/aros/stdc" \
    -c "$DIR/probe_main.c" -o "$OUT/probe_main.o"

# The std pal references the same C glue std-build.sh compiles; build them
# here too (idempotent) so this script stands alone.
for g in aros_net_glue aros_process_glue aros_thread_glue; do
    "$CC" "${CFLAGS[@]}" -c "$RUSTDIR/$g.c" -o "$OUT/$g.o"
done
for g in aros_fs_glue aros_sync_glue; do
    "$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$RUSTDIR/$g.c" -o "$OUT/$g.o"
done

# Native .a's produced by build scripts (SQLite amalgamation, blake3 NEON)
# are NOT folded into the Rust staticlib; cargo links them only at final-bin
# time. Our final link is collect-aros, so pick them up explicitly.
NATIVE_A=()
while IFS= read -r a; do NATIVE_A+=("$a"); done < <(
    find "$DIR/core-probe/target/aarch64-unknown-aros/release/build" -name "*.a" | sort)
echo "[feraille-probe] native libs: ${NATIVE_A[*]:-none}"

echo "[feraille-probe] link C:FerailleProbe"
COMPILER_PATH="$XTBIN" "$COLLECT" \
    --eh-frame-hdr --allow-multiple-definition \
    -L"$LIBDIR" -L"$XTLIB" -o "$OUT/FerailleProbe" \
    "$LIBDIR/startup.o" "$OUT/probe_main.o" \
    "$OUT/aros_net_glue.o" "$OUT/aros_fs_glue.o" "$OUT/aros_process_glue.o" \
    "$OUT/aros_thread_glue.o" "$OUT/aros_sync_glue.o" \
    "$RSLIB" "${NATIVE_A[@]}" \
    -\( "${AUTOLIB[@]}" "${STDLIBS[@]}" -\)
echo "[feraille-probe] built: $OUT/FerailleProbe ($(stat -f%z "$OUT/FerailleProbe") bytes)"

cp -f "$OUT/FerailleProbe" "$CDIR/FerailleProbe"; chmod +x "$CDIR/FerailleProbe"
echo "[feraille-probe] deployed -> $CDIR/FerailleProbe"
