#!/bin/bash
# build.sh -- build + link the hosted Zed editor-core into C:SockProbe.
# Stage 1 (Rust): cargo -Zbuild-std staticlib for aarch64-unknown-aros, with
#   the cc-rs C recipe from aros-env.sh (sqlite/tree-sitter/ring).
# Stage 2 (link): collect-aros, startup.o, the C shim, the std pal glue objects
#   from hosted/rust, the native .a's cargo built (sqlite/ring/tree-sitter/
#   gpui_aros glue), and the AROS libraries. Same recipe as hosted/feraille.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
RUSTDIR="$REPO/hosted/rust"
WS="$REPO/../zed-aros"                       # the vendored Zed workspace
[ -d "$WS" ] || WS="$HOME/Source/zed-aros"
RSLIB="$WS/target/aarch64-unknown-aros/release/libsockprobe.a"

# The cc-rs C recipe (CC_/AR_/CFLAGS_aarch64_unknown_aros).
. "$DIR/aros-env.sh"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD/bin/darwin-aarch64"; return; fi
    local best="" bt=0 d t
    for d in "$HOME/aros-build/bin/darwin-aarch64" /tmp/*/bin/darwin-aarch64 ; do
        [ -x "$d/tools/collect-aros" ] && [ -d "$d/gen/include" ] || continue
        t="$(stat -f %m "$d/tools/collect-aros" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}

echo "[sockprobe] stage 1: cargo build (aarch64-unknown-aros, release)"
( cd "$WS" && cargo +nightly-2026-06-27 build --release -p sockprobe \
      -Zjson-target-spec -Zbuild-std=std,panic_abort \
      --target "$RUSTDIR/aarch64-unknown-aros.json" )
[ -f "$RSLIB" ] || { echo "FAIL: $RSLIB missing" >&2; exit 1; }

T="$(find_tree)"
[ -n "$T" ] && [ -x "$T/tools/collect-aros" ] || { echo "no AROS build tree" >&2; exit 2; }
echo "[sockprobe] stage 2: AROS tree: $T"

GEN="$T/gen"; DEV="$T/AROS/Developer"; LIBDIR="$DEV/lib"
XTBIN="$T/tools/crosstools/bin"; XTLIB="$T/tools/crosstools/lib/generic"
CDIR="$T/AROS/C"; COLLECT="$T/tools/collect-aros"
CC="${AROS_CC:-clang}"
OUT="$REPO/build/zed-aros"; mkdir -p "$OUT"

# The pal glue + C shim are compiled with the bare-metal recipe the std pal
# uses (matches hosted/rust std-build.sh); the cc-rs recipe in aros-env.sh is
# only for cargo's own C deps.
CFLAGS=(--target=aarch64-unknown-none-elf -mcmodel=large -ffixed-x18 -D__arm64__ -O2
        -Wno-pointer-sign -Wno-int-conversion -Wno-implicit-function-declaration
        -I"$GEN/include" -I"$DEV/include")
AUTOLIB=(-lmui -lamiga -larossupport -lamiga -lcodesets -lkeymap -lexpansion
         -lcommodities -ldiskfont -lasl -lmuimaster -ldatatypes -lcybergraphics
         -lworkbench -licon -lintuition -lgadtools -llayers -laros -lpartition
         -liffparse -lgraphics -llocale -ldos -lutility -loop -llibinit -lautoinit)
STDLIBS=(-lposixc -lstdcio -lstdc -lexec -lpthread)

echo "[sockprobe] compile C shim + mmap leaves"
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -I"$GEN/include/aros/stdc" \
    -c "$DIR/sockprobe_main.c" -o "$OUT/sockprobe_main.o"
"$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -I"$GEN/include/aros/stdc" \
    -c "$DIR/aros_mman_stub.c" -o "$OUT/aros_mman_stub.o"

echo "[sockprobe] compile std pal glue"
for g in aros_net_glue aros_process_glue aros_thread_glue aros_fd_shim; do
    "$CC" "${CFLAGS[@]}" -c "$RUSTDIR/$g.c" -o "$OUT/$g.o"
done
for g in aros_fs_glue aros_sync_glue; do
    "$CC" "${CFLAGS[@]}" -I"$GEN/include/aros/posixc" -c "$RUSTDIR/$g.c" -o "$OUT/$g.o"
done

# Native .a's from build scripts (sqlite, ring, tree-sitter, gpui_aros glue)
# are not folded into the staticlib; collect-aros picks them up explicitly.
NATIVE_A=()
while IFS= read -r a; do NATIVE_A+=("$a"); done < <(
    find "$WS/target/aarch64-unknown-aros/release/build" -name "*.a" | sort)
echo "[sockprobe] native libs: ${#NATIVE_A[@]}"

echo "[sockprobe] link C:SockProbe"
COMPILER_PATH="$XTBIN" "$COLLECT" \
    --eh-frame-hdr --allow-multiple-definition \
    -L"$LIBDIR" -L"$XTLIB" -o "$OUT/SockProbe" \
    "$LIBDIR/startup.o" "$OUT/sockprobe_main.o" "$OUT/aros_mman_stub.o" \
    "$OUT/aros_fd_shim.o" \
    "$OUT/aros_net_glue.o" "$OUT/aros_fs_glue.o" "$OUT/aros_process_glue.o" \
    "$OUT/aros_thread_glue.o" "$OUT/aros_sync_glue.o" \
    "$RSLIB" "${NATIVE_A[@]}" \
    -\( "${AUTOLIB[@]}" "${STDLIBS[@]}" -\)
echo "[sockprobe] built: $OUT/SockProbe ($(stat -f%z "$OUT/SockProbe") bytes)"

# Strip debug info: -Zbuild-std over the whole editor graph produces ~1 GB of
# .debug_*/.rela.debug_* sections. AROS loads the file into RAM, so that is
# ~900 MB of pure debug bloat resident at boot. Stripping it takes the binary
# from ~1 GB to ~140 MB with no behavior change (symtab kept for crash traces).
for STRIP in "$AROS_CROSSTOOLS/bin/llvm-strip" "$XTBIN/llvm-strip" "$(command -v llvm-strip 2>/dev/null)"; do
    [ -x "$STRIP" ] || continue
    "$STRIP" --strip-debug "$OUT/SockProbe"
    echo "[sockprobe] stripped: $OUT/SockProbe ($(stat -f%z "$OUT/SockProbe") bytes)"
    break
done

cp -f "$OUT/SockProbe" "$CDIR/SockProbe"; chmod +x "$CDIR/SockProbe"
echo "[sockprobe] deployed -> $CDIR/SockProbe"
