#!/bin/bash
# build.sh — [FF0] stage: cross-build libavutil for darwin-aarch64 AROS and install
# it into a local sysroot the C: smoke links against. Native software port: ARM code
# built *for* AROS by the crosstools, not a host bridge. See NOTES.md "[FF0]" and
# docs/features/ffmpeg-native/README.md.
#
# Pinned ffmpeg (FF0 is version-insensitive; matches the reference headers). Source
# is fetched into the gitignored build/, never vendored (LGPL/GPL: we build it).
#
#   ./build.sh            fetch (if needed) + configure + make libavutil + install
#   ./build.sh --reconf   force a fresh configure
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"

FFVER="${FFMPEG_VERSION:-8.1.2}"
# Pin a sha256 to verify the tarball; empty = print the computed one and continue.
FFSHA="${FFMPEG_SHA256:-}"
FFURL="https://ffmpeg.org/releases/ffmpeg-${FFVER}.tar.xz"

OUT="$REPO/build/ffmpeg"            # gitignored
SRC="$OUT/ffmpeg-${FFVER}"
BLD="$OUT/build-aros"
SYSROOT="$OUT/sysroot"             # where libavutil.a + headers install
TARBALL="$OUT/ffmpeg-${FFVER}.tar.xz"
mkdir -p "$OUT"

# --- discover a COMPLETE SDK tree (not just newest clang; see aros-cc.sh) -----
find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    best="" ; bt=0
    for d in \
        "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64" \
        /tmp/*/bin/darwin-aarch64 \
        /private/tmp/*/*/*/scratchpad/arosbuild/bin/darwin-aarch64 ; do
        [ -e "$d/AROS/Developer/include/aros/posixc/stdio.h" ] \
            && [ -e "$d/AROS/Developer/lib/libmui.a" ] \
            && [ -x "$d/tools/collect-aros" ] || continue
        t="$(stat -f %m "$d/AROS/Developer/lib/libmui.a" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}
T="$(find_tree)"
[ -n "$T" ] || {
    echo "build: no COMPLETE AROS SDK tree (need posixc/stdio.h + libmui.a + collect-aros; set AROS_BUILD)" >&2; exit 1; }
export AROS_BUILD="$T"             # pin it so aros-cc.sh doesn't re-glob per probe
# ar/nm/ranlib are arch-agnostic (they archive/index/list ELF); use host llvm-* so
# they do not depend on which crosstools the tree carries.
LLVMBIN=/opt/homebrew/opt/llvm/bin
echo "[ff0] AROS tree: $T"

# --- fetch + verify pinned source --------------------------------------------
if [ ! -d "$SRC" ]; then
    echo "[ff0] fetch ffmpeg $FFVER"
    [ -f "$TARBALL" ] || curl -fSL -o "$TARBALL" "$FFURL"
    got="$(shasum -a 256 "$TARBALL" | awk '{print $1}')"
    if [ -n "$FFSHA" ]; then
        [ "$got" = "$FFSHA" ] || { echo "FAIL: sha256 mismatch ($got != $FFSHA)" >&2; exit 1; }
        echo "[ff0] sha256 OK"
    else
        echo "[ff0] NOTE: pin FFMPEG_SHA256=$got to verify future fetches"
    fi
    tar -C "$OUT" -xf "$TARBALL"
fi

# --- configure: cross, libavutil only ----------------------------------------
if [ "${1:-}" = "--reconf" ] || [ ! -f "$BLD/config.h" ]; then
    echo "[ff0] configure (cross, --disable-everything, libavutil only, --disable-asm)"
    rm -rf "$BLD"; mkdir -p "$BLD"
    ( cd "$BLD" && "$SRC/configure" \
        --enable-cross-compile --arch=aarch64 --target-os=none \
        --cc="$DIR/aros-cc.sh" --ld="$DIR/aros-cc.sh" \
        --extra-cflags="-D_GNU_SOURCE" \
        --ar="$LLVMBIN/llvm-ar" --ranlib="$LLVMBIN/llvm-ranlib" --nm="$LLVMBIN/llvm-nm" \
        --enable-static --disable-shared --disable-asm --disable-autodetect \
        --disable-programs --disable-doc --disable-network \
        --disable-avcodec --disable-avformat --disable-avdevice --disable-avfilter \
        --disable-swscale --disable-swresample \
        --prefix="$SYSROOT" )
fi

# --- build + install libavutil into the sysroot ------------------------------
echo "[ff0] make libavutil"
make -C "$BLD" -j"$(sysctl -n hw.ncpu)" libavutil/libavutil.a
make -C "$BLD" install >/dev/null

LIB="$SYSROOT/lib/libavutil.a"
[ -f "$LIB" ] || { echo "FAIL: $LIB not produced" >&2; exit 1; }
echo "[ff0] installed: $LIB"
echo "[ff0]            $SYSROOT/include/libavutil/avutil.h"
echo "[ff0] every member aarch64 ELF?"
/opt/homebrew/opt/llvm/bin/llvm-objdump -f "$LIB" 2>/dev/null | grep -oE 'file format [a-z0-9-]+' | sort -u | sed 's/^/  /'
echo "[ff0] stage done. Link the smoke + run:  ../../graft/ffmpeg-smoke"
