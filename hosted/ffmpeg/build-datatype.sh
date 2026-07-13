#!/bin/bash
# build-datatype.sh -- build + deploy ffmpeg.datatype, the libavcodec-backed
# picture datatype (decodes the first video frame of any container libavcodec
# handles, so MultiView / the desktop can preview media). The class + its
# FFmpeg descriptor live in the OS tree (aros-upstream/workbench/classes/
# datatypes/ffmpeg + workbench/devs/datatypes/FFmpeg.dtd), but the class links
# the out-of-tree ffmpeg sysroot this repo builds -- like FFView -- so the OS
# mmakefile takes FFMPEG_SYSROOT and this host script points it at
# build/ffmpeg/sysroot-video (run build.sh + build-video.sh first).
#
#   hosted/ffmpeg/build-datatype.sh
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
SYS="$REPO/build/ffmpeg/sysroot-video"

BUILD="${AROS_BUILD:-$HOME/aros-build}"
XT="${AROS_CROSSTOOLS:-$HOME/aros-crosstools}"
[ -e "$XT/bin/objcopy" ] || ln -sf "$XT/bin/llvm-objcopy" "$XT/bin/objcopy"
export PATH="$XT/bin:/opt/homebrew/bin:$PATH"

[ -d "$SYS/lib" ] || { echo "build-datatype: $SYS missing -- run build-video.sh first" >&2; exit 1; }
[ -f "$BUILD/Makefile" ] || { echo "build-datatype: $BUILD not configured (see docs/features/build)" >&2; exit 1; }

echo "== build ffmpeg.datatype (class) =="
( cd "$BUILD" && make workbench-datatypes-ffmpeg FFMPEG_SYSROOT="$SYS" )

echo "== build the FFmpeg DataTypes descriptor =="
( cd "$BUILD" && make workbench-devs-datatypes-quick )

CLASS="$BUILD/bin/darwin-aarch64/AROS/Classes/DataTypes/ffmpeg.datatype"
DESC="$BUILD/bin/darwin-aarch64/AROS/Devs/DataTypes/FFmpeg"
[ -f "$CLASS" ] && echo "   class: $CLASS ($(du -h "$CLASS" | cut -f1))" || { echo "FAIL: no class" >&2; exit 1; }
[ -f "$DESC" ]  && echo "   descriptor: $DESC" || { echo "FAIL: no descriptor" >&2; exit 1; }
echo "build-datatype: done. AddDataTypes REFRESH (desktop boot) registers it; MultiView then previews video first frames."
