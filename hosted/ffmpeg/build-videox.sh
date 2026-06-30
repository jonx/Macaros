#!/bin/bash
# build-videox.sh -- the BROAD "tricky formats" codec set for FFViewX: h264, hevc,
# vp8, vp9, mpeg1/2/4, wmv/vc1, h263, theora, cinepak + the common containers
# (mp4/mov, mkv/webm, avi, flv, mpeg-ts/ps, asf, ogg). This links a bigger binary
# that takes longer to LOAD (AROS relocates the whole thing at LoadSeg; relocations
# scale with code size), but decodes natively at full speed once up. The small,
# fast-loading set is build-video.sh (FFView); the complete set is build-full.sh.
#
#   ./build-videox.sh [--reconf]
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
FFVER="${FFMPEG_VERSION:-8.1.2}"
OUT="$REPO/build/ffmpeg"
SRC="$OUT/ffmpeg-${FFVER}"
BLD="$OUT/build-videox"
SYSROOT="$OUT/sysroot-videox"
mkdir -p "$OUT"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    for d in "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64"; do
        [ -e "$d/AROS/Developer/include/aros/posixc/stdio.h" ] \
            && [ -x "$d/tools/collect-aros" ] && { printf '%s\n' "$d"; return; }
    done
}
T="$(find_tree)"; [ -n "$T" ] || { echo "build-videox: no AROS SDK tree (set AROS_BUILD)" >&2; exit 1; }
export AROS_BUILD="$T"
LLVMBIN=/opt/homebrew/opt/llvm/bin
echo "[videox] AROS tree: $T"
[ -d "$SRC" ] || { echo "[videox] ffmpeg source missing -- run build.sh first" >&2; exit 1; }

DECODERS=mjpeg,png,bmp,gif,tiff,webp,rawvideo,\
h264,hevc,mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,vc1,\
vp8,vp9,h263,h263p,theora,cinepak,svq1,flv
PARSERS=mjpeg,png,h264,hevc,mpeg4video,mpegvideo,vp8,vp9,vc1,h263
# NB: no raw h264/hevc demuxers -- their fuzzy ES probe mis-claims other raw
# streams (e.g. a .m4v mpeg4 ES) and then scans the whole file for NAL units via
# the custom AVIO, which stalls. Container h264/hevc come through mov/matroska.
DEMUXERS=image2,mjpeg,avi,mov,matroska,flv,mpegts,mpegps,asf,ogg,m4v,gif
BSFS=h264_mp4toannexb,hevc_mp4toannexb,vp9_superframe,mpeg4_unpack_bframes

if [ "${1:-}" = "--reconf" ] || [ ! -f "$BLD/config.h" ]; then
    echo "[videox] configure (broad image + video codecs)"
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
        --enable-decoder="$DECODERS" --enable-parser="$PARSERS" \
        --enable-demuxer="$DEMUXERS" --enable-bsf="$BSFS" --enable-protocol=file \
        --prefix="$SYSROOT" )
fi

echo "[videox] make libs"
make -C "$BLD" -j"$(sysctl -n hw.ncpu)" \
    libavutil/libavutil.a libavcodec/libavcodec.a \
    libavformat/libavformat.a libswscale/libswscale.a libswresample/libswresample.a
make -C "$BLD" install >/dev/null
for l in avutil avcodec avformat swscale swresample; do
    f="$SYSROOT/lib/lib$l.a"; [ -f "$f" ] && echo "[videox] ok: $f ($(du -h "$f"|cut -f1))" || { echo "FAIL lib$l.a" >&2; exit 1; }
done
echo "[videox] done. sysroot: $SYSROOT"
