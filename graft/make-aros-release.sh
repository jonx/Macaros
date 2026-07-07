#!/bin/sh
# make-aros-release.sh — build a SELF-CONTAINED, relocatable Macaros.app that boots
# the full AROS Wanderer desktop on a clean Mac (no ~/aros-build, no ../aros-upstream).
#
# Sibling to make-aros-app.sh (the dev wrapper). The difference is self-containment:
# this EMBEDS the whole prepared AROS volume (fonts + AROSDefault theme + Cocoa
# monitor + AROS.boot + the full C:/Libs: set incl. FFViewX, FFView, gpufx.library,
# RustHello) inside Contents/Resources/AROS, and ships a launcher that regenerates a
# bundle-relative AROSBootstrap.conf into a per-user writable dir at each launch (the
# bundle itself stays read-only so the notarization seal holds).
#
# NO DUPLICATION of the deployment: the source tree is the one graft/run-window.sh
# has already prepared in ~/aros-build (desktop payloads staged, media + rust
# artifacts installed). We copy it, normalise the conf paths, and bake the desktop
# Startup-Sequence — nothing is re-deployed.
#
#   ./make-aros-release.sh                 build build/Macaros.app (unsigned)
#   ./make-aros-release.sh --dmg           …then wrap it in build/Macaros.dmg
#   ./make-aros-release.sh --check         static self-containment audit of an existing build
#   AROS_APP=/path ./make-aros-release.sh  build elsewhere
#
# Signing/notarization is a SEPARATE step (Developer ID, see ~/Source/apple-codesigning.md):
#   codesign inside-out + xcrun notarytool submit + stapler — run once this boots clean.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# --- inputs (override via env) ---------------------------------------------
DYLIB="${AROS_CTL_DYLIB:-$ROOT/build/cocoametal.dylib}"
PASTEBOARD="${AROS_CTL_PASTEBOARD_DYLIB:-$ROOT/build/libpasteboard.dylib}"
COREAUDIO="${AROS_CTL_COREAUDIO_DYLIB:-$ROOT/build/libcoreaudio.dylib}"
BSDSOCK="${AROS_CTL_BSDSOCK_DYLIB:-$ROOT/build/libbsdsockhost.dylib}"
SCHEMA="$ROOT/hosted/cocoametal/settings.json"
ICON="${AROS_CTL_ICON:-$ROOT/hosted/cocoametal/Macaros.icns}"
APP="${AROS_APP:-$ROOT/build/Macaros.app}"
MEMORY="${AROS_HOST_MEMORY:-512}"

INFO_PLIST='<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Macaros</string>
  <key>CFBundleDisplayName</key><string>Macaros</string>
  <key>CFBundleIdentifier</key><string>org.aros.hosted</string>
  <key>CFBundleExecutable</key><string>Macaros</string>
  <key>CFBundleIconFile</key><string>Macaros</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>NSHumanReadableCopyright</key><string>Copyright © 2026 John Knipper. AROS under the AROS Public License.</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
</dict></plist>'

# The launcher (CFBundleExecutable). At runtime it expands the conf template into a
# per-user writable dir, points DYLD + the settings schema inside the bundle, and
# boots the embedded AROS from a read/write host share (~/AROS/Shared by default).
LAUNCHER='#!/bin/sh
set -eu
D="$(cd "$(dirname "$0")" && pwd)"              # Contents/MacOS
APP="$(cd "$D/.." && pwd)"                       # Contents
AROSROOT="$APP/Resources/AROS"
BOOTD="$AROSROOT/boot/darwin"
STATE="${AROS_STATE_DIR:-$HOME/Library/Application Support/AROS}"
mkdir -p "$STATE"

# Bundle-relative conf: expand @AROSROOT@ from the read-only template into the
# writable state dir (never write inside the .app — it would break the seal).
sed "s|@AROSROOT@|$AROSROOT|g" "$BOOTD/AROSBootstrap.conf.tmpl" > "$STATE/AROSBootstrap.conf"

# Host share: two volumes onto one Mac folder (read-only MacRO:, read/write MacRW:).
: "${AROS_HOST_CONF:=$STATE/aros-host.conf}"; export AROS_HOST_CONF
[ -f "$D/aros-host-conf.sh" ] && . "$D/aros-host-conf.sh"
if [ -z "${AROS_HOST_VOLUME:-}" ]; then
    SHARE="${AROS_SHARE:-$HOME/AROS/Shared}"; mkdir -p "$SHARE"
    [ -e "$SHARE/ReadMe" ] || printf "Files here appear in AROS as MacRO: (read-only) and MacRW: (read/write).\n" > "$SHARE/ReadMe"
    AROS_HOST_VOLUME="MacRO:$SHARE
MacRW:$SHARE;WRITE"
fi
export AROS_HOST_VOLUME

cd "$BOOTD"
exec env AROS_DARWIN_THREADED=1 \
    DYLD_FALLBACK_LIBRARY_PATH="$APP/Frameworks" \
    AROS_SETTINGS_SCHEMA="$APP/Frameworks/settings.json" \
    AROS_HOST_VOLUME="$AROS_HOST_VOLUME" \
    AROS_RUN_DIR="$STATE" \
    ./Macaros -c "$STATE/AROSBootstrap.conf"'

# Desktop Startup-Sequence — the exact set graft/run-window.sh writes for
# AROS_CTL_STARTUP_MODE=desktop (kept in sync with that recipe). Baked into the
# embedded tree at build time; it needs no runtime values.
STARTUP_SEQUENCE='Version
FailAt 21
If NOT EXISTS "RAM:Clipboards"
    MakeDir "RAM:Clipboards"
EndIf
If NOT EXISTS "RAM:T"
    MakeDir "RAM:T"
EndIf
If NOT EXISTS "RAM:ENV"
    MakeDir "RAM:ENV"
    Assign "ENV:" "RAM:ENV"
EndIf
Assign "T:" "RAM:T"
Assign "CLIPS:" "SYS:clips"
If EXISTS "DEVS:Keymaps"
    Assign "KEYMAPS:" "DEVS:Keymaps"
EndIf
If EXISTS "C:LoadKeymap"
    LoadKeymap RESTORE
EndIf
Assign "LOCALE:" "SYS:Locale"
Assign "LIBS:" "SYS:Classes" ADD
Assign "HELP:" "LOCALE:Help" DEFER
Assign "IMAGES:" "SYS:System/Images" DEFER
Assign "WANDERER:" "SYS:System/Wanderer" DEFER
Assign "THEMES:" "SYS:Prefs/Presets/Themes" >NIL:
Assign "THEME:" "THEMES:AROSDefault"
If EXISTS "THEME:Images"
    Assign "IMAGES:" "THEME:Images" PREPEND
EndIf
Path "C:" "SYS:System" "S:" "SYS:Prefs" QUIET
If EXISTS "SYS:Fonts"
    Assign "FONTS:" "SYS:Fonts"
EndIf
If EXISTS "SYS:Tools"
    Path "SYS:Tools" QUIET ADD
EndIf
If EXISTS "SYS:Utilities"
    Path "SYS:Utilities" QUIET ADD
EndIf
If EXISTS "C:AddDataTypes"
    AddDataTypes REFRESH QUIET
EndIf
If EXISTS "C:AddAudioModes"
    If EXISTS "DEVS:AudioModes/COREAUDIO"
        Run <NIL: >NIL: QUIET AddAudioModes DEVS:AudioModes/COREAUDIO QUIET
    EndIf
EndIf
If EXISTS "C:IPrefs"
    IPrefs
EndIf
Run <NIL: >NIL: QUIET ConClip
If EXISTS "WANDERER:Wanderer"
    Run <NIL: >NIL: QUIET WANDERER:Wanderer
    Wait 2
    EndCLI
EndIf'

# The module set run-window.sh ensures on top of the base conf (resident
# shell.resource + the full driver/library set the desktop needs).
EXTRA_MODULES="Devs/shell.resource Devs/task.resource \
Devs/Drivers/hiddclass.hidd Devs/Drivers/gfx.hidd \
Devs/Drivers/inputclass.hidd Devs/Drivers/keyboard.hidd Devs/Drivers/mouse.hidd \
Devs/console.device Devs/input.device Devs/keyboard.device Devs/gameport.device \
Devs/clipboard.device Devs/timer.device \
Libs/keymap.library Libs/graphics.library Libs/layers.library \
Libs/intuition.library Libs/gadtools.library Libs/iffparse.library \
Libs/asl.library Libs/commodities.library Libs/cybergraphics.library \
Libs/coolimages.library Libs/datatypes.library Libs/locale.library \
Libs/muimaster.library Libs/rexxsyslib.library Libs/stdc.library \
L/con-handler"

# ---------------------------------------------------------------------------
find_bootd() {
    if [ -n "${AROS_CTL_BOOTD:-}" ]; then printf '%s\n' "$AROS_CTL_BOOTD"; return; fi
    for d in \
        "${BUILD:-$HOME/aros-build}/bin/darwin-aarch64/AROS/boot/darwin" \
        "$HOME/aros-build/bin/darwin-aarch64/AROS/boot/darwin" ; do
        [ -x "$d/AROSBootstrap" ] && { printf '%s\n' "$d"; return; }
    done
    printf '%s\n' ""
}

# --- static self-containment audit -----------------------------------------
if [ "${1:-}" = "--check" ]; then
    fail=0; ck() { if [ "$1" = 0 ]; then printf '    ok: %s\n' "$2"; else printf '    FAIL: %s\n' "$2"; fail=1; fi; }
    echo "[REL] self-containment audit of $APP"
    A="$APP/Contents"
    [ -d "$A/Resources/AROS" ];                         ck $? "embedded AROS tree present"
    [ -f "$A/Resources/AROS/AROS.boot" ];               ck $? "AROS.boot embedded"
    [ -d "$A/Resources/AROS/Prefs/Presets/Themes/AROSDefault" ]; ck $? "AROSDefault theme embedded"
    [ -e "$A/Resources/AROS/System/Wanderer/Wanderer" ]; ck $? "Wanderer embedded"
    [ -e "$A/Resources/AROS/C/FFViewX" ];               ck $? "FFViewX embedded (C:FFViewX)"
    [ -e "$A/Resources/AROS/Libs/gpufx.library" ];      ck $? "gpufx.library embedded (LIBS:)"
    [ -e "$A/Resources/AROS/C/RustHello" ];             ck $? "RustHello embedded (C:RustHello)"
    [ -f "$A/Resources/AROS/S/Startup-Sequence" ];      ck $? "desktop Startup-Sequence baked"
    [ -f "$A/Resources/AROS/boot/darwin/AROSBootstrap.conf.tmpl" ]; ck $? "conf template present"
    # THE self-containment invariant: no path in the template escapes the bundle.
    if grep -qE '/(Users|private|tmp|Volumes)/' "$A/Resources/AROS/boot/darwin/AROSBootstrap.conf.tmpl" 2>/dev/null; then
        echo "    FAIL: conf template still has host-absolute paths:"; grep -nE '/(Users|private|tmp|Volumes)/' "$A/Resources/AROS/boot/darwin/AROSBootstrap.conf.tmpl" | head; fail=1
    else ck 0 "conf template is bundle-relative (@AROSROOT@ only)"; fi
    for m in cocoametal libpasteboard libcoreaudio libbsdsockhost; do
        [ -f "$A/Frameworks/$m.dylib" ]; ck $? "Frameworks/$m.dylib"
    done
    [ -x "$A/MacOS/Macaros" ];                          ck $? "launcher executable"
    grep -q '@AROSROOT@' "$A/Resources/AROS/boot/darwin/AROSBootstrap.conf.tmpl"; ck $? "template uses @AROSROOT@ placeholder"
    plutil -lint "$A/Info.plist" >/dev/null 2>&1;       ck $? "Info.plist valid"
    [ "$fail" = 0 ] && { echo "[REL] PASS"; exit 0; } || { echo "[REL] FAIL"; exit 1; }
fi

# --- real build ------------------------------------------------------------
BOOTD="$(find_bootd)"
[ -n "$BOOTD" ] && [ -x "$BOOTD/AROSBootstrap" ] || { echo "make-aros-release.sh: no AROSBootstrap (set AROS_CTL_BOOTD)" >&2; exit 1; }
SRC="$(cd "$BOOTD/../.." && pwd)"   # .../darwin-aarch64/AROS
[ -f "$DYLIB" ] || { echo "missing $DYLIB (make cocoametal-dylib, or set AROS_CTL_DYLIB)" >&2; exit 1; }

# require the prepared desktop payloads — this script does NOT re-stage them
for p in AROS.boot Fonts Prefs/Presets/Themes/AROSDefault System/Wanderer/Wanderer Devs/Monitors/Cocoa; do
    [ -e "$SRC/$p" ] || { echo "make-aros-release.sh: $SRC missing $p — boot desktop once via run-window.sh first" >&2; exit 1; }
done

echo ">> source AROS tree: $SRC ($(du -sh "$SRC" | awk '{print $1}'))"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"

echo ">> embedding AROS volume (read-only) ..."
# Copy the prepared tree; drop host-side scratch that must not ship.
/usr/bin/ditto "$SRC" "$APP/Contents/Resources/AROS"
DST="$APP/Contents/Resources/AROS"
rm -f "$DST/Devs/Monitors/headless" 2>/dev/null || true

# The menu-bar/app binary must be named Macaros; keep it inside the boot dir so
# cwd-relative host resolution matches run-window.sh.
cp -f "$DST/boot/darwin/AROSBootstrap" "$DST/boot/darwin/Macaros"

# Bake the desktop Startup-Sequence.
mkdir -p "$DST/S"; printf '%s\n' "$STARTUP_SEQUENCE" > "$DST/S/Startup-Sequence"

# Build the bundle-relative conf TEMPLATE: every module path -> @AROSROOT@/<rel>.
# Take the base conf's module lines (normalising both relative and host-absolute
# forms to a tree-relative path), then add the extra module set if the file exists.
TMPL="$DST/boot/darwin/AROSBootstrap.conf.tmpl"
{
    echo "# Macaros self-contained boot config (generated by make-aros-release.sh)."
    echo "# @AROSROOT@ is expanded to the bundle's embedded AROS tree at launch."
    printf 'memory %s\n' "$MEMORY"
    # base modules from the prepared conf, normalised
    grep '^module ' "$BOOTD/AROSBootstrap.conf" | while read -r _kw _path; do
        case "$_path" in
            /*)  rel="${_path##*/AROS/}" ;;      # host-absolute -> strip up to /AROS/
            *)   rel="$_path" ;;                  # already tree-relative
        esac
        printf 'module @AROSROOT@/%s\n' "$rel"
    done
    # ensure the desktop module set (skip dups already emitted above)
    for M in $EXTRA_MODULES; do
        [ -e "$DST/$M" ] || continue
        grep -q "@AROSROOT@/$M\$" "$TMPL.pre" 2>/dev/null && continue
        printf 'module @AROSROOT@/%s\n' "$M"
    done
} > "$TMPL.pre"
# de-duplicate, keep order
awk '!seen[$0]++' "$TMPL.pre" > "$TMPL"; rm -f "$TMPL.pre"
# the live conf that AROSBootstrap reads is regenerated at launch; drop the baked one
rm -f "$DST/boot/darwin/AROSBootstrap.conf"

# Frameworks + resources
cp "$DYLIB" "$APP/Contents/Frameworks/cocoametal.dylib"
[ -f "$PASTEBOARD" ] && cp "$PASTEBOARD" "$APP/Contents/Frameworks/libpasteboard.dylib"
[ -f "$COREAUDIO" ]  && cp "$COREAUDIO"  "$APP/Contents/Frameworks/libcoreaudio.dylib"
[ -f "$BSDSOCK" ]    && cp "$BSDSOCK"    "$APP/Contents/Frameworks/libbsdsockhost.dylib"
cp "$SCHEMA" "$APP/Contents/Frameworks/settings.json"
cp "$SCHEMA" "$APP/Contents/Resources/settings.json"
[ -f "$ICON" ] && cp "$ICON" "$APP/Contents/Resources/Macaros.icns"
[ -f "$HERE/aros-host-conf.sh" ] && cp "$HERE/aros-host-conf.sh" "$APP/Contents/MacOS/aros-host-conf.sh"
printf '%s' "$LAUNCHER"   > "$APP/Contents/MacOS/Macaros"; chmod +x "$APP/Contents/MacOS/Macaros"
printf '%s' "$INFO_PLIST" > "$APP/Contents/Info.plist"

echo ">> built $APP ($(du -sh "$APP" | awk '{print $1}'))"
"$0" --check || { echo ">> self-containment audit FAILED"; exit 1; }

if [ "${1:-}" = "--dmg" ]; then
    DMG="${AROS_DMG:-$ROOT/build/Macaros.dmg}"
    rm -f "$DMG"
    echo ">> building $DMG ..."
    hdiutil create -quiet -volname "Macaros" -srcfolder "$APP" -ov -format UDZO "$DMG"
    echo ">> built $DMG ($(du -sh "$DMG" | awk '{print $1}'))"
fi

echo ">> next: test-boot (relocated), then Developer-ID sign + notarize + staple."
echo ">>   open '$APP'      # or Contents/MacOS/Macaros for stdout"
