#!/bin/bash
# deploy.sh -- build FFView (the image/video viewer) + the FF3 AVIO demo against
# the curated video sysroot and copy them into the AROS SDK C:. Run build.sh +
# build-video.sh first. FFView is ~29MB and is deployed on demand, not baked into
# the boot image. Put media on MacRW: (= ~/AROS/Shared) and run "FFView MacRW:clip.avi".
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/ffmpeg"
SYS="$OUT/sysroot-video"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    for d in "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64"; do
        [ -e "$d/AROS/Developer/include/aros/posixc/stdio.h" ] \
            && [ -x "$d/tools/collect-aros" ] && { printf '%s\n' "$d"; return; }
    done
}
T="$(find_tree)"
[ -n "$T" ] || { echo "deploy: no AROS SDK tree (set AROS_BUILD)" >&2; exit 1; }
export AROS_BUILD="$T"
[ -d "$SYS/lib" ] || { echo "deploy: $SYS missing -- run build-video.sh first" >&2; exit 1; }
CDEST="$T/AROS/C"

LIBS="-lavformat -lavcodec -lswscale -lswresample -lavutil -lpthread"
mkdir -p "$OUT"

build_one() {  # <output-name> <src...>
    local name="$1"; shift
    echo "[deploy] $name"
    "$DIR/aros-cc.sh" -O2 -I"$SYS/include" -I"$DIR" "$@" -L"$SYS/lib" $LIBS -o "$OUT/$name"
    cp -f "$OUT/$name" "$CDEST/$name" && chmod +x "$CDEST/$name"
}

build_one FFView  "$DIR/ffview.c"   "$DIR/aros_avio.c"
build_one FF3Avio "$DIR/ff3_avio.c" "$DIR/aros_avio.c"

echo "[deploy] done -> $CDEST/{FFView,FF3Avio} ($(du -h "$OUT/FFView" | cut -f1) each)"
