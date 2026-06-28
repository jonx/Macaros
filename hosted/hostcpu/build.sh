#!/bin/sh
# build.sh — self-contained build+run for the host CPU-facts shim (processor.resource
# darwin backend, host side). Markers [CP1]/[CP2] (pure probe) and [CP3] (dylib ABI).
#
# Deliberately standalone: it does NOT touch the project Makefile, cocoametal, or any
# existing host bridge — this prep is developed in parallel and merged later (the
# hosted/hostshell/build.sh convention). The dylib it produces (build/hostcpu/
# hostcpu.dylib) is the artifact the AROS side dlopens via hostlib.resource, and is
# the same module the Daedalos app can link/load directly (see README.md).
#
#   ./build.sh          build, then run [CP1]/[CP2] (probe) and [CP3] (dylib ABI)
#   ./build.sh --build  build only
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/hostcpu"
mkdir -p "$OUT"
MIN="${MACOSX_MIN:-12.0}"

CFLAGS="-arch arm64 -mmacosx-version-min=$MIN -O2 -g -Wall -Wextra -Wno-unused-parameter"

echo "[build] clang -> $OUT/hostcpu.dylib"
clang $CFLAGS -dynamiclib -install_name @rpath/hostcpu.dylib \
    -exported_symbols_list "$DIR/hostcpu.exports" \
    "$DIR/hostcpu_shim.c" -o "$OUT/hostcpu.dylib"
codesign -s - -f "$OUT/hostcpu.dylib" >/dev/null 2>&1 || true

echo "[build] clang -> $OUT/cp_test"
clang $CFLAGS "$DIR/cp_test.c" "$DIR/hostcpu_shim.c" -o "$OUT/cp_test"
codesign -s - -f "$OUT/cp_test" >/dev/null 2>&1 || true

echo "[build] clang -> $OUT/cp_abi_test"
clang $CFLAGS "$DIR/cp_abi_test.c" -o "$OUT/cp_abi_test"
codesign -s - -f "$OUT/cp_abi_test" >/dev/null 2>&1 || true

[ "${1:-}" = "--build" ] && { echo "[build] ok: $OUT/{hostcpu.dylib,cp_test,cp_abi_test}"; exit 0; }

rc=0
echo "--- [CP1]/[CP2] pure host probe ---"
"$OUT/cp_test" || rc=$?
echo "--- [CP3] dylib (dlopen) boundary ---"
"$OUT/cp_abi_test" "$OUT/hostcpu.dylib" || rc=$?
exit $rc
