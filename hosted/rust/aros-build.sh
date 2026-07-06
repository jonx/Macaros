#!/bin/bash
# aros-build.sh — stage 2: build the no_std Rust staticlib into a real, loadable
# AROS C: command and deploy it. This is the PROVEN [RS0]/[RS1] recipe — it built
# RustHello, which runs on booted AROS and prints "RUST-AROS: ALL PASS"
# (graft/rust-smoke drives the run).
#
# Needs an AROS build tree (the crosstools, the SDK headers/libs, collect-aros).
# It is auto-discovered the same way graft/deploy-check finds the boot dir; set
# $AROS_BUILD to the <build>/bin/darwin-aarch64 dir to override.
#
# Why it looks the way it does (each line is a real lesson — see ../../docs/
# features/rust-aros/README.md and graft/UPSTREAM-NOTES.md):
#   -mcmodel=large   AROS loads GOT-less; large model => absolute movw, no GOT
#                    (make.cfg item; same model every AROS C: command uses)
#   -D__arm64__      darwin backend host headers dispatch on __arm64__ (item 16)
#   startup.o        the REAL program startup (defines __startup_main + the
#                    PROGRAM_ENTRIES set). NOT elf-startup.o — that has none of it,
#                    and a binary missing it loads as "filesystem action type
#                    unknown". This was the one bug between "links" and "runs".
#   collect-aros     the AROS linker driver: relocatable (ET_REL) link + AROS
#                    symbol-set generation. COMPILER_PATH lets it find the real ld.
#   --allow-multiple-definition   the AROS_LIBREQ duplicate marker (item 18)
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
RSLIB="$DIR/aros-rt/target/aarch64-unknown-aros/release/libaros_rt.a"

# --- discover the AROS build tree (.../bin/darwin-aarch64) ---
find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    local best="" bt=0 d t
    for d in \
        "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64" \
        "$REPO/build/AROS"/.. \
        /private/tmp/*/*/*/scratchpad/arosbuild/bin/darwin-aarch64 \
        /tmp/*/bin/darwin-aarch64 ; do
        [ -x "$d/tools/collect-aros" ] && [ -d "$d/gen/include" ] || continue
        t="$(stat -f %m "$d/tools/collect-aros" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}

[ -f "$RSLIB" ] || { echo "FAIL: $RSLIB missing — run ./build.sh --build first" >&2; exit 1; }
T="$(find_tree)"
[ -n "$T" ] && [ -x "$T/tools/collect-aros" ] || {
    echo "aros-build: no AROS build tree found (need .../bin/darwin-aarch64 with" >&2
    echo "  tools/collect-aros + gen/include). Set \$AROS_BUILD. Skipping stage 2." >&2
    exit 2; }
echo "[stage2] AROS tree: $T"

GEN="$T/gen"; DEV="$T/AROS/Developer"; LIBDIR="$DEV/lib"
# The crosstools install: prefer an in-tree copy, else the canonical stable
# toolchain ($HOME/aros-crosstools). $T/tools holds the HOST tools (collect-aros
# etc.), NOT the cross toolchain -- pointing XTLIB there silently loses
# -lclang_rt.builtins-aarch64 the moment no scratchpad tree matches.
XT="${AROS_CROSSTOOLS:-$T/tools/crosstools}"
[ -x "$XT/bin/clang" ] || XT="$HOME/aros-crosstools"
[ -x "$XT/bin/clang" ] || { echo "aros-build: no crosstools (set AROS_CROSSTOOLS)" >&2; exit 2; }
XTBIN="$XT/bin"; XTLIB="$XT/lib/generic"
CDIR="$T/AROS/C"; COLLECT="$T/tools/collect-aros"
CC="${AROS_CC:-clang}"; CCTARGET="--target=aarch64-unknown-none-elf"
OUT="$REPO/build/rust-aros"; mkdir -p "$OUT"

CFLAGS=($CCTARGET -mcmodel=large -D__arm64__ -O2 -Wno-pointer-sign
        -Wno-int-conversion -Wno-implicit-function-declaration
        -I"$GEN/include" -I"$DEV/include")
# AROS ELF program default lib group (config/elf-specs.in autolib + lib).
AUTOLIB=(-lmui -lamiga -larossupport -lamiga -lcodesets -lkeymap -lexpansion
         -lcommodities -ldiskfont -lasl -lmuimaster -ldatatypes -lcybergraphics
         -lworkbench -licon -lintuition -lgadtools -llayers -laros -lpartition
         -liffparse -lgraphics -llocale -ldos -lutility -loop -llibinit -lautoinit)
STDLIBS=(-lposixc -lstdcio -lstdc -lexec)

echo "[stage2] compile glue + harness (crosstools cc, real AROS headers)"
"$CC" "${CFLAGS[@]}" -c "$DIR/aros_rt_glue.c" -o "$OUT/aros_rt_glue.o"
"$CC" "${CFLAGS[@]}" -c "$DIR/rs0_main.c"     -o "$OUT/rs0_main.o"

echo "[stage2] link RustHello (collect-aros -> ET_REL AROS program)"
COMPILER_PATH="$XTBIN" "$COLLECT" \
    --eh-frame-hdr --allow-multiple-definition \
    -L"$LIBDIR" -L"$XTLIB" -o "$OUT/RustHello" \
    "$LIBDIR/startup.o" "$OUT/rs0_main.o" "$OUT/aros_rt_glue.o" "$RSLIB" \
    -\( "${AUTOLIB[@]}" "${STDLIBS[@]}" -\) -lclang_rt.builtins-aarch64
echo "[stage2] built: $OUT/RustHello ($(stat -f%z "$OUT/RustHello") bytes, ET_REL)"

echo "[stage2] deploy -> $CDIR/RustHello"
cp -f "$OUT/RustHello" "$CDIR/RustHello"; chmod +x "$CDIR/RustHello"
echo "[stage2] done. Run on booted AROS:  graft/rust-smoke   (or: graft/bench-run C:RustHello)"
