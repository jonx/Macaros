#!/bin/bash
# build-ff1.sh -- [FF1] stage: cross-build a MINIMAL libavcodec + libavformat +
# libswscale (on top of libavutil) for darwin-aarch64 AROS, enough to decode a
# single video frame. Native software port (ARM code built for AROS), the codec
# step after [FF0]. Keeps build.sh (libavutil-only) untouched.
#
#   ./build-ff1.sh            configure (if needed) + make the libs + install
#   ./build-ff1.sh --reconf   force a fresh configure
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"

FFVER="${FFMPEG_VERSION:-8.1.2}"
OUT="$REPO/build/ffmpeg"
SRC="$OUT/ffmpeg-${FFVER}"
BLD="$OUT/build-ff1"               # separate from FF0's build-aros
SYSROOT="$OUT/sysroot-ff1"
mkdir -p "$OUT"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    for d in "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64"; do
        [ -e "$d/AROS/Developer/include/aros/posixc/stdio.h" ] \
            && [ -x "$d/tools/collect-aros" ] && { printf '%s\n' "$d"; return; }
    done
}
T="$(find_tree)"
[ -n "$T" ] || { echo "build-ff1: no AROS SDK tree (set AROS_BUILD)" >&2; exit 1; }
export AROS_BUILD="$T"
LLVMBIN=/opt/homebrew/opt/llvm/bin
echo "[ff1] AROS tree: $T"
[ -d "$SRC" ] || { echo "[ff1] ffmpeg source missing -- run build.sh first to fetch" >&2; exit 1; }

# --- configure: minimal codec/demuxer set for one video frame ----------------
if [ "${1:-}" = "--reconf" ] || [ ! -f "$BLD/config.h" ]; then
    echo "[ff1] configure (avcodec+avformat+swscale, mjpeg-only, single-threaded)"
    rm -rf "$BLD"; mkdir -p "$BLD"
    ( cd "$BLD" && "$SRC/configure" \
        --enable-cross-compile --arch=aarch64 --target-os=none \
        --cc="$DIR/aros-cc.sh" --ld="$DIR/aros-cc.sh" \
        --extra-cflags="-D_GNU_SOURCE" \
        --ar="$LLVMBIN/llvm-ar" --ranlib="$LLVMBIN/llvm-ranlib" --nm="$LLVMBIN/llvm-nm" \
        --enable-static --disable-shared --disable-asm --disable-autodetect \
        --disable-programs --disable-doc --disable-network --disable-pthreads \
        --disable-everything \
        --enable-avutil --enable-avcodec --enable-avformat --enable-swscale \
        --enable-decoder=mjpeg,bmp,png,rawvideo \
        --enable-parser=mjpeg,png \
        --enable-demuxer=image2,mjpeg,avi \
        --enable-protocol=file \
        --prefix="$SYSROOT" )
fi

echo "[ff1] make libs"
NCPU="$(sysctl -n hw.ncpu)"
make -C "$BLD" -j"$NCPU" \
    libavutil/libavutil.a libavcodec/libavcodec.a \
    libavformat/libavformat.a libswscale/libswscale.a
make -C "$BLD" install >/dev/null

for l in avutil avcodec avformat swscale; do
    f="$SYSROOT/lib/lib$l.a"
    [ -f "$f" ] && echo "[ff1] ok: $f" || { echo "FAIL: lib$l.a not produced" >&2; exit 1; }
done
echo "[ff1] done. sysroot: $SYSROOT"
