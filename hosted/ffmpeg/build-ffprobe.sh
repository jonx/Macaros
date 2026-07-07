#!/bin/bash
# build-ffprobe.sh -- build C:FFProbe (the standalone media inspector) against
# the broad "videox" ffmpeg libs. Unlike the FFView viewers (which use only the
# installed public headers), ffprobe reuses ffmpeg's own fftools sources, so it
# compiles from the SOURCE tree + the generated build config, then links the
# already-built static libs. Read-only: it demuxes + decodes to REPORT stream/
# format info -- no encoders, no muxers, no threads.
#
# Prereq: build.sh + build-videox.sh (the videox sysroot + build dir). Run this
# after them; deploy.sh calls it automatically when the videox build exists.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/ffmpeg"
SRC="$OUT/ffmpeg-8.1.2"
BLD="$OUT/build-videox"            # generated config.h / ffversion.h live here
SYSX="$OUT/sysroot-videox"        # the installed broad-codec libs
: "${AROS_BUILD:=$HOME/aros-build/bin/darwin-aarch64}"; export AROS_BUILD

[ -d "$SYSX/lib" ] || { echo "build-ffprobe: $SYSX missing -- run build-videox.sh first" >&2; exit 1; }
[ -f "$BLD/config.h" ] || { echo "build-ffprobe: $BLD/config.h missing -- run build-videox.sh first" >&2; exit 1; }

OBJ="$OUT/ffprobe-obj"; rm -rf "$OBJ"; mkdir -p "$OBJ"

# ffprobe's object set (fftools/Makefile OBJS-ffprobe + the DOFFTOOL shared
# cmdutils/opt_common): the CLI plumbing + the text-format writers.
SRCS="
fftools/cmdutils.c fftools/opt_common.c fftools/ffprobe.c
fftools/textformat/avtextformat.c fftools/textformat/tf_compact.c
fftools/textformat/tf_default.c fftools/textformat/tf_flat.c
fftools/textformat/tf_ini.c fftools/textformat/tf_json.c
fftools/textformat/tf_mermaid.c fftools/textformat/tf_xml.c
fftools/textformat/tw_avio.c fftools/textformat/tw_buffer.c
fftools/textformat/tw_stdout.c
"

echo "[ffprobe] compile fftools (source tree + generated config)"
OBJS=()
for s in $SRCS; do
    o="$OBJ/$(basename "$s" .c).o"
    "$DIR/aros-cc.sh" -O2 -D_GNU_SOURCE -I"$SRC" -I"$BLD" -c "$SRC/$s" -o "$o"
    OBJS+=("$o")
done

# Stubs for the avfilter/avdevice symbols the shared fftools reference for the
# -filters/-sources/-sinks options (not built here, never hit on the probe path).
"$DIR/aros-cc.sh" -O2 -c "$DIR/ffprobe_stubs.c" -o "$OBJ/ffprobe_stubs.o"
OBJS+=("$OBJ/ffprobe_stubs.o")

echo "[ffprobe] link C:FFProbe"
LIBS="-lavformat -lavcodec -lswscale -lswresample -lavutil -lpthread"
"$DIR/aros-cc.sh" -O2 "${OBJS[@]}" -L"$SYSX/lib" $LIBS -o "$OUT/FFProbe"

STRIP="${LLVMBIN:-/opt/homebrew/opt/llvm/bin}/llvm-strip"
[ -x "$STRIP" ] && "$STRIP" --strip-unneeded "$OUT/FFProbe" 2>/dev/null || true

CDEST="$AROS_BUILD/AROS/C"
cp -f "$OUT/FFProbe" "$CDEST/FFProbe" && chmod +x "$CDEST/FFProbe"
echo "[ffprobe] deployed -> $CDEST/FFProbe ($(du -h "$OUT/FFProbe" | cut -f1))"
