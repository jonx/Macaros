#!/bin/bash
# build-video.sh -- the viewer's ffmpeg: a curated-but-broad decoder + demuxer
# set covering the common image and video formats, small enough to link into a
# binary that actually loads on AROS (the full build's all-decoder registry makes
# a ~90MB binary that does not). The complete build still exists (build-full.sh,
# sysroot-full) as proof ffmpeg ports in full; the viewer links this subset.
#
# I/O is the dos-backed custom AVIOContext (aros_avio.c), so every demuxer here
# reads through AROS dos, not ffmpeg's posixc file protocol.
#
#   ./build-video.sh            configure (if needed) + make + install
#   ./build-video.sh --reconf   force a fresh configure
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"

FFVER="${FFMPEG_VERSION:-8.1.2}"
OUT="$REPO/build/ffmpeg"
SRC="$OUT/ffmpeg-${FFVER}"
BLD="$OUT/build-video"
SYSROOT="$OUT/sysroot-video"
mkdir -p "$OUT"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    for d in "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64"; do
        [ -e "$d/AROS/Developer/include/aros/posixc/stdio.h" ] \
            && [ -x "$d/tools/collect-aros" ] && { printf '%s\n' "$d"; return; }
    done
}
T="$(find_tree)"
[ -n "$T" ] || { echo "build-video: no AROS SDK tree (set AROS_BUILD)" >&2; exit 1; }
export AROS_BUILD="$T"
LLVMBIN=/opt/homebrew/opt/llvm/bin
echo "[video] AROS tree: $T"
[ -d "$SRC" ] || { echo "[video] ffmpeg source missing -- run build.sh first to fetch" >&2; exit 1; }

# Kept deliberately small: the AROS loader relocates the WHOLE binary before the
# program runs, and the relocation count scales with code size. A broad set
# (h264/hevc/vp8/vp9/...) pushed FFView to ~416K relocs and a ~25s load freeze;
# this set keeps it near FF1Decode's ~100K (a few seconds). h264 etc. live in the
# complete build (build-full.sh) for when a longer load is acceptable.
DECODERS=mjpeg,png,bmp,gif,rawvideo,mpeg1video,mpeg2video,mpeg4
PARSERS=mjpeg,png,mpeg4video,mpegvideo
DEMUXERS=image2,mjpeg,avi,mov,m4v,gif,mpegps,mpegts
BSFS=mpeg4_unpack_bframes

if [ "${1:-}" = "--reconf" ] || [ ! -f "$BLD/config.h" ]; then
    echo "[video] configure (common image + video codecs)"
    rm -rf "$BLD"; mkdir -p "$BLD"
    ( cd "$BLD" && "$SRC/configure" \
        --enable-cross-compile --arch=aarch64 --target-os=none \
        --cc="$DIR/aros-cc.sh" --ld="$DIR/aros-cc.sh" \
        --extra-cflags="-D_GNU_SOURCE" \
        --ar="$LLVMBIN/llvm-ar" --ranlib="$LLVMBIN/llvm-ranlib" --nm="$LLVMBIN/llvm-nm" \
        --enable-static --disable-shared --disable-asm --disable-autodetect \
        --disable-programs --disable-doc --disable-network --disable-pthreads \
        --disable-everything \
        --enable-avutil --enable-avcodec --enable-avformat --enable-swscale --enable-swresample \
        --enable-decoder="$DECODERS" \
        --enable-parser="$PARSERS" \
        --enable-demuxer="$DEMUXERS" \
        --enable-bsf="$BSFS" \
        --enable-protocol=file \
        --prefix="$SYSROOT" )
fi

echo "[video] make libs"
NCPU="$(sysctl -n hw.ncpu)"
make -C "$BLD" -j"$NCPU" \
    libavutil/libavutil.a libavcodec/libavcodec.a \
    libavformat/libavformat.a libswscale/libswscale.a libswresample/libswresample.a
make -C "$BLD" install >/dev/null

for l in avutil avcodec avformat swscale swresample; do
    f="$SYSROOT/lib/lib$l.a"
    [ -f "$f" ] && echo "[video] ok: $f ($(du -h "$f" | cut -f1))" || { echo "FAIL: lib$l.a not produced" >&2; exit 1; }
done
echo "[video] done. sysroot: $SYSROOT"
