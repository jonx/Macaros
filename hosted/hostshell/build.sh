#!/bin/sh
# build.sh — self-contained build+run for the host app-shell POC.
#
# Deliberately standalone: it does NOT touch the project Makefile or any existing
# cocoametal source, so this POC can be developed in parallel and merged later
# (see README.md). Mirrors the cocoametal spike build flags (host clang, ARC,
# arm64, ad-hoc codesign).
#
#   ./build.sh             build, then run BOTH unattended verifiers → "[G] PASS" + "[GS] PASS"
#   ./build.sh --show      build, then run the app LIVE (menu bar; ⌘, opens Settings)
#   ./build.sh --show-settings   build, then show the generated Settings window live
#   ./build.sh --build     build only
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
OUT="$REPO/build/hostshell"
mkdir -p "$OUT"
MIN="${MACOSX_MIN:-12.0}"

CFLAGS="-fobjc-arc -fmodules -arch arm64 -mmacosx-version-min=$MIN -O0 -g -Wall -Wextra -Wno-unused-parameter"
FRAMEWORKS="-framework AppKit -framework Foundation -framework CoreFoundation -framework CoreGraphics"

build() {   # build <out-bin> <test.m...>
    out="$1"; shift
    echo "[build] clang -> $out"
    # shellcheck disable=SC2086
    clang $CFLAGS $FRAMEWORKS "$DIR/cmshell.m" "$DIR/cmsettings.m" "$@" -o "$out"
    codesign -s - -f "$out" >/dev/null 2>&1 || true
}

build "$OUT/shell_poc"    "$DIR/shell_test.m"
build "$OUT/settings_poc" "$DIR/cmsettings_test.m"

# the schema is a DATA FILE loaded at runtime; place it next to the binaries
# (resolved via NSBundle — the .app Contents/Resources path at merge).
cp -f "$DIR/settings.json" "$OUT/settings.json"

case "${1:-}" in
    --build)         echo "[build] ok: $OUT/{shell_poc,settings_poc}"; exit 0 ;;
    --show)          exec "$OUT/shell_poc" --show ;;
    --show-settings) exec "$OUT/settings_poc" --show ;;
    *) ;;
esac

rc=0
"$OUT/shell_poc"    || rc=$?
echo "---"
"$OUT/settings_poc" || rc=$?
exit $rc
