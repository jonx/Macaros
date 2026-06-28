#!/bin/sh
# build.sh — self-contained build+run for the host print-to-PDF engine (the
# PDF-direct path of the printing bridge). Marker [PRPDF].
#
# Deliberately standalone: it does NOT touch the project Makefile, cocoametal, or any
# existing host bridge (the hosted/hostshell/build.sh convention). The dylib it
# produces (build/printing/libpdfgen.dylib) is the artifact the AROS side dlopens via
# hostlib.resource, and is the same module the Daedalos app can link/load directly.
#
#   ./build.sh          build, then run [PRPDF1]/[PRPDF2] (writes run/*.pdf, verifies)
#   ./build.sh --build  build only
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/printing"
mkdir -p "$OUT"
MIN="${MACOSX_MIN:-12.0}"

CFLAGS="-arch arm64 -mmacosx-version-min=$MIN -O2 -g -Wall -Wextra -Wno-unused-parameter"
FRAMEWORKS="-framework CoreFoundation -framework CoreGraphics -framework CoreText"

echo "[build] clang -> $OUT/libpdfgen.dylib"
clang $CFLAGS $FRAMEWORKS -dynamiclib -install_name @rpath/libpdfgen.dylib \
    -exported_symbols_list "$DIR/pdfgen.exports" \
    "$DIR/pdfgen.c" -o "$OUT/libpdfgen.dylib"
codesign -s - -f "$OUT/libpdfgen.dylib" >/dev/null 2>&1 || true

echo "[build] clang -> $OUT/pr_test"
clang $CFLAGS $FRAMEWORKS "$DIR/pr_test.c" "$DIR/pdfgen.c" -o "$OUT/pr_test"
codesign -s - -f "$OUT/pr_test" >/dev/null 2>&1 || true

[ "${1:-}" = "--build" ] && { echo "[build] ok: $OUT/{libpdfgen.dylib,pr_test}"; exit 0; }

echo "--- [PRPDF] print-to-PDF engine ---"
exec "$OUT/pr_test"
