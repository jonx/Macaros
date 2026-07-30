#!/bin/bash
# kqprobe-build.sh -- build C:KqProbe, the host-kqueue directory-watch check.
#
# Pure C (no Rust): proves the hostlib kqueue mechanism the editor's file
# watcher will sit on. See kqprobe_main.c.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"

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

T="$(find_tree)"
[ -n "$T" ] && [ -x "$T/tools/collect-aros" ] || { echo "no AROS build tree" >&2; exit 2; }

GEN="$T/gen"; DEV="$T/AROS/Developer"; LIBDIR="$DEV/lib"
XTBIN="$T/tools/crosstools/bin"; XTLIB="$T/tools/crosstools/lib/generic"
CDIR="$T/AROS/C"; COLLECT="$T/tools/collect-aros"
CC="${AROS_CC:-clang}"
OUT="$REPO/build/kqprobe"; mkdir -p "$OUT"

CFLAGS=(--target=aarch64-unknown-none-elf -mcmodel=large -ffixed-x18 -D__arm64__ -O2
        -Wno-pointer-sign -Wno-int-conversion -Wno-implicit-function-declaration
        -I"$GEN/include" -I"$DEV/include"
        -I"$GEN/include/aros/posixc" -I"$GEN/include/aros/stdc")
AUTOLIB=(-lamiga -larossupport -laros -lgraphics -llocale -ldos -lutility -loop
         -llibinit -lautoinit)
STDLIBS=(-lposixc -lstdcio -lstdc -lexec)

echo "[kqprobe] compile"
"$CC" "${CFLAGS[@]}" -c "$DIR/kqprobe_main.c" -o "$OUT/kqprobe_main.o"

echo "[kqprobe] link"
COMPILER_PATH="$XTBIN" "$COLLECT" \
    --eh-frame-hdr --allow-multiple-definition \
    -L"$LIBDIR" -L"$XTLIB" -o "$OUT/KqProbe" \
    "$LIBDIR/startup.o" "$OUT/kqprobe_main.o" \
    -\( "${AUTOLIB[@]}" "${STDLIBS[@]}" -\)

cp -f "$OUT/KqProbe" "$CDIR/KqProbe"; chmod +x "$CDIR/KqProbe"
echo "[kqprobe] deployed -> $CDIR/KqProbe ($(stat -f%z "$OUT/KqProbe") bytes)"
