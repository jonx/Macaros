#!/bin/bash
# build-full.sh -- port ffmpeg "completely" for darwin-aarch64 AROS: cross-build
# the full native set of decoders + demuxers + parsers + bitstream filters +
# protocols + libswscale + libswresample (decode-only: no encoders/muxers/
# network/programs/asm). The proper I/O is a custom AVIOContext backed by AROS
# dos (aros_avio.c), so libavformat's demuxers (mp4/avi/mkv/mov/...) read through
# dos rather than ffmpeg's posixc file protocol (which returns empty reads here).
#
#   ./build-full.sh            configure (if needed) + make the libs + install
#   ./build-full.sh --reconf   force a fresh configure
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"

FFVER="${FFMPEG_VERSION:-8.1.2}"
OUT="$REPO/build/ffmpeg"
SRC="$OUT/ffmpeg-${FFVER}"
BLD="$OUT/build-full"
SYSROOT="$OUT/sysroot-full"
mkdir -p "$OUT"

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    for d in "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64"; do
        [ -e "$d/AROS/Developer/include/aros/posixc/stdio.h" ] \
            && [ -x "$d/tools/collect-aros" ] && { printf '%s\n' "$d"; return; }
    done
}
T="$(find_tree)"
[ -n "$T" ] || { echo "build-full: no AROS SDK tree (set AROS_BUILD)" >&2; exit 1; }
export AROS_BUILD="$T"
LLVMBIN=/opt/homebrew/opt/llvm/bin
echo "[full] AROS tree: $T"
[ -d "$SRC" ] || { echo "[full] ffmpeg source missing -- run build.sh first to fetch" >&2; exit 1; }

# --- configure: full default decode set, minus the parts AROS can't host -------
if [ "${1:-}" = "--reconf" ] || [ ! -f "$BLD/config.h" ]; then
    echo "[full] configure (all native decoders/demuxers/parsers, decode-only)"
    rm -rf "$BLD"; mkdir -p "$BLD"
    ( cd "$BLD" && "$SRC/configure" \
        --enable-cross-compile --arch=aarch64 --target-os=none \
        --cc="$DIR/aros-cc.sh" --ld="$DIR/aros-cc.sh" \
        --extra-cflags="-D_GNU_SOURCE" \
        --ar="$LLVMBIN/llvm-ar" --ranlib="$LLVMBIN/llvm-ranlib" --nm="$LLVMBIN/llvm-nm" \
        --enable-static --disable-shared --disable-asm --disable-autodetect \
        --disable-programs --disable-doc --disable-network --disable-pthreads \
        --disable-encoders --disable-muxers \
        --enable-avutil --enable-avcodec --enable-avformat \
        --enable-swscale --enable-swresample \
        --prefix="$SYSROOT" )
fi

echo "[full] make libs"
NCPU="$(sysctl -n hw.ncpu)"
make -C "$BLD" -j"$NCPU" \
    libavutil/libavutil.a libavcodec/libavcodec.a \
    libavformat/libavformat.a libswscale/libswscale.a libswresample/libswresample.a
make -C "$BLD" install >/dev/null

for l in avutil avcodec avformat swscale swresample; do
    f="$SYSROOT/lib/lib$l.a"
    [ -f "$f" ] && echo "[full] ok: $f ($(du -h "$f" | cut -f1))" || { echo "FAIL: lib$l.a not produced" >&2; exit 1; }
done
echo "[full] done. sysroot: $SYSROOT"
