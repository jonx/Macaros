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
    for d in "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64"; do
        [ -e "$d/AROS/Developer/include/aros/posixc/stdio.h" ] \
            && [ -x "$d/tools/collect-aros" ] && { printf '%s\n' "$d"; return; }
    done
}
T="$(find_tree)"
[ -n "$T" ] || { echo "deploy: no AROS SDK tree (set AROS_BUILD)" >&2; exit 1; }
export AROS_BUILD="$T"
[ -d "$SYS/lib" ] || { echo "deploy: $SYS missing -- run build-video.sh first" >&2; exit 1; }
# FFView opens gpufx.library for GPU colour-convert/scale, so ffview.c includes
# <proto/gpufx.h>. That header is installed by the gpufx.library build; fail
# early with a clear message rather than a cryptic mid-compile error.
[ -f "$T/gen/include/proto/gpufx.h" ] || {
    echo "deploy: $T is missing the gpufx headers (proto/gpufx.h)." >&2
    echo "        Build gpufx.library first:  (cd ~/aros-build && make hostlibs-gpufx)" >&2
    exit 1; }
CDEST="$T/AROS/C"

LIBS="-lavformat -lavcodec -lswscale -lswresample -lavutil -lpthread"
mkdir -p "$OUT"
STRIP="${LLVMBIN:-/opt/homebrew/opt/llvm/bin}/llvm-strip"

build_one() {  # <output-name> <src...>
    local name="$1"; shift
    echo "[deploy] $name"
    "$DIR/aros-cc.sh" -O2 -I"$SYS/include" -I"$DIR" "$@" -L"$SYS/lib" $LIBS -o "$OUT/$name"
    # Strip the symbol table the relocations don't need: a static ffmpeg link
    # carries ~24MB of symbols, and loading a 30MB file through LoadSeg is what
    # made FFView seem to hang. --strip-unneeded is reloc-safe (keeps symbols the
    # AROS large-model MOVW/ABS relocs reference) and cuts ~30MB -> ~8MB.
    [ -x "$STRIP" ] && "$STRIP" --strip-unneeded "$OUT/$name" 2>/dev/null || true
    cp -f "$OUT/$name" "$CDEST/$name" && chmod +x "$CDEST/$name"
}

build_one FFView  "$DIR/ffview.c"   "$DIR/aros_avio.c"
build_one FF3Avio "$DIR/ff3_avio.c" "$DIR/aros_avio.c"

# FFViewX: same viewer linked against the broad "tricky formats" set (build-videox.sh)
# if it's been built. Adds mpeg1/2, the mkv/flv/mov/asf/ogg containers, etc. Loads
# just as fast (stripping, not codec count, drives load time). NOTE: the h264/hevc
# decoders currently CRASH on this target (a complex pure-C decoder bug, same class
# as the libswscale yuv2rgb one) -- the other formats work; h264 is WIP.
SYSX="$OUT/sysroot-videox"
if [ -d "$SYSX/lib" ]; then
    echo "[deploy] FFViewX (broad codecs)"
    "$DIR/aros-cc.sh" -O2 -I"$SYSX/include" -I"$DIR" "$DIR/ffview.c" "$DIR/aros_avio.c" \
        -L"$SYSX/lib" $LIBS -o "$OUT/FFViewX"
    [ -x "$STRIP" ] && "$STRIP" --strip-unneeded "$OUT/FFViewX" 2>/dev/null || true
    cp -f "$OUT/FFViewX" "$CDEST/FFViewX" && chmod +x "$CDEST/FFViewX"
fi

echo "[deploy] done -> $CDEST/{FFView,FF3Avio${SYSX:+,FFViewX}} ($(du -h "$OUT/FFView" | cut -f1) FFView)"
