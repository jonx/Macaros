#!/bin/sh
# make-aros-app.sh — wrap hosted AROS in a first-class macOS .app bundle.
#
# ADDITIVE + non-colliding: a sibling to run-window.sh (which keeps its relocatable
# bare-binary model). This packages the same pieces as a double-clickable Daedalus.app:
#   Contents/MacOS/Daedalus              launcher script (CFBundleExecutable) — sets env
#   Contents/MacOS/AROSBootstrap     the hosted-AROS binary (signed w/ entitlements)
#   Contents/MacOS/AROSBootstrap.conf + aros-host-conf.sh
#   Contents/Frameworks/cocoametal.dylib + settings.json   (dladdr-relative schema)
#   Contents/Resources/settings.json (AROS_SETTINGS_SCHEMA)
#   Contents/Info.plist
# The launcher exports AROS_DARWIN_THREADED, points the dylib at the bundled schema,
# and sources aros-host-conf.sh so the GUI's aros-host.conf drives the boot.
#
#   ./make-aros-app.sh            build build/Daedalus.app from the discovered build
#   ./make-aros-app.sh --check    unattended structural self-check (no AROS build needed)
#
# Note (v1 limitation): AROSBootstrap.conf's module paths point at the EXTERNAL AROS
# install (same as run-window.sh) — the bundle wraps the launcher, it does not embed
# the whole AROS tree yet. The first-class app surface (menu/About/icon/settings) is
# delivered by the dylib regardless of packaging.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DYLIB="${AROS_CTL_DYLIB:-$ROOT/build/cocoametal.dylib}"
SCHEMA="$ROOT/hosted/cocoametal/settings.json"
ENT="${AROS_CTL_ENT:-$HERE/aros-host.entitlements.plist}"
APP="${AROS_APP:-$ROOT/build/Daedalus.app}"

INFO_PLIST='<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Daedalus</string>
  <key>CFBundleDisplayName</key><string>Daedalus</string>
  <key>CFBundleIdentifier</key><string>org.aros.hosted</string>
  <key>CFBundleExecutable</key><string>Daedalus</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
</dict></plist>'

LAUNCHER='#!/bin/sh
D="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "$D/../.." && pwd)"
export AROS_DARWIN_THREADED=1
export DYLD_FALLBACK_LIBRARY_PATH="$APP/Contents/Frameworks"
export AROS_SETTINGS_SCHEMA="$APP/Contents/Resources/settings.json"
: "${AROS_HOST_CONF:=$HOME/Library/Application Support/AROS/aros-host.conf}"
export AROS_HOST_CONF
[ -f "$D/aros-host-conf.sh" ] && . "$D/aros-host-conf.sh"
if [ -n "${AROS_HOST_MEMORY:-}" ] && [ -f "$D/AROSBootstrap.conf" ]; then
    grep -v "^memory " "$D/AROSBootstrap.conf" > "$D/.c.tmp" 2>/dev/null || cp "$D/AROSBootstrap.conf" "$D/.c.tmp"
    printf "memory %s\n" "$AROS_HOST_MEMORY" >> "$D/.c.tmp"; mv "$D/.c.tmp" "$D/AROSBootstrap.conf"
fi
exec "$D/AROSBootstrap" -c "$D/AROSBootstrap.conf"'

assemble() {   # assemble <bootd> <app>
    _bd="$1"; _app="$2"
    rm -rf "$_app"
    mkdir -p "$_app/Contents/MacOS" "$_app/Contents/Frameworks" "$_app/Contents/Resources"
    cp "$_bd/AROSBootstrap" "$_app/Contents/MacOS/AROSBootstrap"
    [ -f "$_bd/AROSBootstrap.conf" ] && cp "$_bd/AROSBootstrap.conf" "$_app/Contents/MacOS/AROSBootstrap.conf"
    cp "$DYLIB"  "$_app/Contents/Frameworks/cocoametal.dylib"
    cp "$SCHEMA" "$_app/Contents/Frameworks/settings.json"
    cp "$SCHEMA" "$_app/Contents/Resources/settings.json"
    cp "$HERE/aros-host-conf.sh" "$_app/Contents/MacOS/aros-host-conf.sh"
    printf '%s' "$LAUNCHER"   > "$_app/Contents/MacOS/Daedalus"; chmod +x "$_app/Contents/MacOS/Daedalus"
    printf '%s' "$INFO_PLIST" > "$_app/Contents/Info.plist"
}

# --- discover the AROS boot dir (same approach as run-window.sh) ---
find_bootd() {
    if [ -n "${AROS_CTL_BOOTD:-}" ]; then printf '%s\n' "$AROS_CTL_BOOTD"; return; fi
    _best=""; _bt=0
    for d in \
        "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64/AROS/boot/darwin" \
        "$ROOT/build/AROS/boot/darwin" \
        /private/tmp/claude-*/*/*/scratchpad/arosbuild/bin/darwin-aarch64/AROS/boot/darwin ; do
        [ -x "$d/AROSBootstrap" ] || continue
        _t="$(stat -f %m "$d/AROSBootstrap" 2>/dev/null || echo 0)"
        [ "$_t" -ge "$_bt" ] && { _bt="$_t"; _best="$d"; }
    done
    printf '%s\n' "$_best"
}

# --- unattended structural self-check (synthesizes a dummy bootd; no AROS build) ---
if [ "${1:-}" = "--check" ]; then
    fail=0; ck() { if [ "$1" = 0 ]; then printf '    ok: %s\n' "$2"; else printf '    FAIL: %s\n' "$2"; fail=1; fi; }
    echo "[APP] make-aros-app structural self-check"
    [ -f "$DYLIB" ]  || { echo "[APP] SKIP: build/cocoametal.dylib missing (run: make cocoametal-dylib)"; exit 0; }
    [ -f "$SCHEMA" ] || { echo "[APP] FAIL: settings.json missing"; exit 1; }
    TMP="$(mktemp -d)"; BD="$TMP/bootd"; mkdir -p "$BD"
    printf '#!/bin/sh\nexit 0\n' > "$BD/AROSBootstrap"; chmod +x "$BD/AROSBootstrap"
    printf 'module /x/exec.library\nmemory 64\n' > "$BD/AROSBootstrap.conf"
    assemble "$BD" "$TMP/Daedalus.app"
    A="$TMP/Daedalus.app/Contents"
    plutil -lint "$A/Info.plist" >/dev/null 2>&1; ck $? "Info.plist is valid"
    [ -x "$A/MacOS/Daedalus" ];                       ck $? "MacOS/Daedalus launcher present + executable"
    [ -f "$A/MacOS/AROSBootstrap" ];              ck $? "MacOS/AROSBootstrap present"
    [ -f "$A/Frameworks/cocoametal.dylib" ];      ck $? "Frameworks/cocoametal.dylib present"
    [ -f "$A/Frameworks/settings.json" ];         ck $? "Frameworks/settings.json (dladdr-relative)"
    [ -f "$A/Resources/settings.json" ];          ck $? "Resources/settings.json (AROS_SETTINGS_SCHEMA)"
    [ -f "$A/MacOS/aros-host-conf.sh" ];          ck $? "MacOS/aros-host-conf.sh present"
    grep -q 'AROS_DARWIN_THREADED=1' "$A/MacOS/Daedalus";       ck $? "launcher sets AROS_DARWIN_THREADED"
    grep -q 'AROS_SETTINGS_SCHEMA' "$A/MacOS/Daedalus";         ck $? "launcher points the dylib at the bundled schema"
    grep -q 'aros-host-conf.sh' "$A/MacOS/Daedalus";            ck $? "launcher sources aros-host-conf.sh"
    codesign --verify "$A/Frameworks/cocoametal.dylib" 2>/dev/null; ck $? "bundled dylib codesign verifies"
    rm -rf "$TMP"
    [ "$fail" = 0 ] && { echo "[APP] PASS structural self-check"; exit 0; } || { echo "[APP] FAIL"; exit 1; }
fi

# --- real build ---
BOOTD="$(find_bootd)"
[ -n "$BOOTD" ] && [ -x "$BOOTD/AROSBootstrap" ] || {
    echo "make-aros-app.sh: no AROSBootstrap found." >&2
    echo "  point AROS_CTL_BOOTD at <build>/bin/darwin-aarch64/AROS/boot/darwin, or build one." >&2
    exit 1; }
[ -f "$DYLIB" ] || { echo "missing $DYLIB — run: make cocoametal-dylib" >&2; exit 1; }
echo ">> boot dir: $BOOTD"
assemble "$BOOTD" "$APP"

# Sign inside-out: dylib ad-hoc, AROSBootstrap with the hardened-runtime entitlements
# (allow-jit / dyld-env / library-validation — see aros-host.entitlements.plist), then
# the bundle. The launcher script execs AROSBootstrap, which carries its own signature.
codesign -s - -f "$APP/Contents/Frameworks/cocoametal.dylib" 2>/dev/null || true
if [ -f "$ENT" ]; then
    codesign -s - -f -o runtime --entitlements "$ENT" "$APP/Contents/MacOS/AROSBootstrap" 2>/dev/null || true
else
    codesign -s - -f "$APP/Contents/MacOS/AROSBootstrap" 2>/dev/null || true
fi
codesign -s - -f "$APP" 2>/dev/null || true

echo ">> built $APP"
echo ">> open it:  open '$APP'      (or: '$APP/Contents/MacOS/Daedalus' to see stdout)"
