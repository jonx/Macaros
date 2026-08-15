#!/bin/sh
# make-aros-release.sh — build a SELF-CONTAINED, relocatable Macaros.app that boots
# the full AROS Wanderer desktop on a clean Mac (no ~/aros-build, no ../aros-upstream).
#
# Sibling to make-aros-app.sh (the dev wrapper). The difference is self-containment:
# this EMBEDS a release-filtered AROS volume (fonts + AROSDefault theme + Cocoa
# monitor + AROS.boot + the full C:/Libs: set incl. FFViewX, FFView, gpufx.library,
# RustHello) inside Contents/Resources/AROS, and ships a launcher that regenerates a
# bundle-relative AROSBootstrap.conf into a per-user writable dir at each launch (the
# bundle itself stays read-only so the notarization seal holds).
#
# NO DUPLICATION of the deployment: the source tree is the one graft/run-window.sh
# has already prepared in ~/aros-build (desktop payloads staged, media + rust
# artifacts installed). We copy its runtime payload, normalise the conf paths, and
# bake the desktop Startup-Sequence. Developer files, test disks, logs, caches,
# and crash reports are deliberately outside the release boundary.
#
#   ./make-aros-release.sh                 build build/Macaros.app (unsigned)
#   ./make-aros-release.sh --dmg           …then wrap it in build/Macaros.dmg
#   ./make-aros-release.sh --dmg-only      wrap an existing signed app without rebuilding it
#   ./make-aros-release.sh --check         static self-containment audit of an existing build
#   AROS_APP=/path ./make-aros-release.sh  build elsewhere
#
# Signing/notarization is a SEPARATE step; see RELEASE.md and
# graft/sign-macaros-release.sh.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# --- inputs (override via env) ---------------------------------------------
DYLIB="${AROS_CTL_DYLIB:-$ROOT/build/cocoametal.dylib}"
PASTEBOARD="${AROS_CTL_PASTEBOARD_DYLIB:-$ROOT/build/libpasteboard.dylib}"
COREAUDIO="${AROS_CTL_COREAUDIO_DYLIB:-$ROOT/build/libcoreaudio.dylib}"
BSDSOCK="${AROS_CTL_BSDSOCK_DYLIB:-$ROOT/build/libbsdsockhost.dylib}"
EMU68K="${AROS_CTL_EMU68K_DYLIB:-$ROOT/build/libemu68k.dylib}"
SCHEMA="$ROOT/hosted/cocoametal/settings.json"
ICON="${AROS_CTL_ICON:-$ROOT/hosted/cocoametal/Macaros.icns}"
APP="${AROS_APP:-$ROOT/build/Macaros.app}"
COMPAT="$HERE/macaros-compatibility.sh"
RELEASE_README="$HERE/release-README.md"
NOTICES="$ROOT/notices"
REPORT_ENV="$HERE/report-env.sh"
VERSION_FILE="${MACAROS_VERSION_FILE:-$ROOT/VERSION}"
[ -r "$VERSION_FILE" ] || { echo "missing release version file: $VERSION_FILE" >&2; exit 1; }
. "$VERSION_FILE"
VERSION="${MACAROS_VERSION:-}"
BUILD_NUMBER="${MACAROS_BUILD_NUMBER:-}"
INCLUDE_MOONSTONE="${MACAROS_INCLUDE_MOONSTONE:-0}"
AROS_SOURCE_TREE="${MACAROS_AROS_SOURCE:-$ROOT/../aros-upstream}"
ZED_SOURCE_TREE="${MACAROS_ZED_SOURCE:-$ROOT/../zed-aros}"
FERAIL_SOURCE_TREE="${MACAROS_FERAIL_SOURCE:-$ROOT/../Ferail}"
MOONSTONE_SOURCE_TREE="${MACAROS_MOONSTONE_SOURCE:-$ROOT/../MoonstoneCS}"
# The editor is the memory-hungry payload; 512 MB boots the desktop but cannot
# hold Zed.
MEMORY="${AROS_HOST_MEMORY:-1280}"

case "$VERSION" in *[!0-9.]*|'') echo "invalid MACAROS_VERSION: $VERSION" >&2; exit 2 ;; esac
case "$BUILD_NUMBER" in *[!0-9]*|'') echo "invalid MACAROS_BUILD_NUMBER: $BUILD_NUMBER" >&2; exit 2 ;; esac
case "$INCLUDE_MOONSTONE" in 0|1) ;; *) echo "MACAROS_INCLUDE_MOONSTONE must be 0 or 1" >&2; exit 2 ;; esac

# Developer probes and superseded application builds must not ship.
EXCLUDE_C="AEdit AHISmoke BevelProbe BrkProbe CRT64Probe CatalogProbe ClockTest \
DeviceProbe EXFATFailpointProbe EXFATFormatProbe EXFATGeometryProbe \
EXFATMediaChangeProbe EXFATNameProbe EXFATReadonlyProbe EXFATSparseProbe \
EXFATWriteProbe FDSK64Probe FF3Avio FFProbe FFThumb Feraille GpuFxBench \
GpuFxTest GpuiSmoke KqProbe PrefsProbe ProcProbe RustAlloc RustBulk RustPath \
RustProc RustShell RustStack RustStd RustStream SockProbe TestLib TimerTest \
ZedAros nettest reactortest socktest"

# Only these top-level items cross from the mutable developer boot tree into the
# product. In particular, never copy crash/, DiskImages/, Developer/, T/, hidden
# user state, symbols, or loose 68k test fixtures.
RUNTIME_ROOT_ITEMS="AROS.boot C Classes Devs Extras Fonts L Libs Locale Prefs S \
Storage System Tools Utilities boot clips Devs.info Fonts.info Libs.info \
Locale.info Prefs.info Storage.info System.info Tools.info Utilities.info"

# The game reads its assets from one root, music included: the soundtrack is
# 15 MB of pre-rendered tunes the AHI backend streams.
MOONSTONE_SRC="${MOONSTONE_SRC:-$HOME/AROS/Shared/Moonstone}"

INFO_PLIST='<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Macaros</string>
  <key>CFBundleDisplayName</key><string>Macaros</string>
  <key>CFBundleIdentifier</key><string>org.aros.hosted</string>
  <key>CFBundleExecutable</key><string>Macaros</string>
  <key>CFBundleIconFile</key><string>Macaros</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>@MACAROS_VERSION@</string>
  <key>CFBundleVersion</key><string>@MACAROS_BUILD_NUMBER@</string>
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
AROS_RUN_DIR="$STATE"; export AROS_RUN_DIR
[ -f "$APP/Resources/report-env.sh" ] && . "$APP/Resources/report-env.sh"
MACAROS_VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Info.plist" 2>/dev/null || true)"
MACAROS_BUILD_NUMBER="$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$APP/Info.plist" 2>/dev/null || true)"
export MACAROS_VERSION MACAROS_BUILD_NUMBER

# The desktop remains usable without the optional legacy engine, but do not let
# a damaged installation make 68k programs fail without an explanation.
if [ ! -f "$APP/Frameworks/libemu68k.dylib" ]; then
    /usr/bin/osascript -e "display alert \"Legacy 68k support is unavailable\" message \"The Macaros 68k engine is missing. Reinstall Macaros from the release disk image. Native AROS programs are unaffected.\" as warning" >/dev/null 2>&1 || true
fi

# Bundle-relative conf: expand @AROSROOT@ from the read-only template into the
# writable state dir (never write inside the .app — it would break the seal).
sed "s|@AROSROOT@|$AROSROOT|g" "$BOOTD/AROSBootstrap.conf.tmpl" > "$STATE/AROSBootstrap.conf"

# Host share: two volumes onto one Mac folder (read-only MacRO:, read/write MacRW:).
: "${AROS_HOST_CONF:=$STATE/aros-host.conf}"; export AROS_HOST_CONF
[ -f "$APP/Resources/aros-host-conf.sh" ] && . "$APP/Resources/aros-host-conf.sh"
if [ -z "${AROS_HOST_VOLUME:-}" ]; then
    SHARE="${AROS_SHARE:-$HOME/AROS/Shared}"; mkdir -p "$SHARE"
    [ -e "$SHARE/ReadMe" ] || printf "Files here appear in AROS as MacRO: (read-only) and MacRW: (read/write).\n" > "$SHARE/ReadMe"
    AROS_HOST_VOLUME="MacRO:$SHARE
MacRW:$SHARE;WRITE"
fi
export AROS_HOST_VOLUME

# Native file watching maps AROS paths back to host paths, and only the launcher
# knows where the share landed. The baked Startup-Sequence cannot be edited (the
# bundle is sealed), so hand the mapping over through the share itself.
SHARE="${SHARE:-$(printf "%s\\n" "$AROS_HOST_VOLUME" | sed -n "s/^MacRW:\\([^;]*\\).*$/\\1/p" | head -1)}"
if [ -n "$SHARE" ] && [ -d "$SHARE" ]; then
    printf "SetEnv AROS_FSW_ROOTS \"MacRW:=%s\"\\n" "$SHARE" > "$SHARE/.macaros-boot" 2>/dev/null || true
fi

cd "$BOOTD"
exec env AROS_DARWIN_THREADED=1 \
    DYLD_FALLBACK_LIBRARY_PATH="$APP/Frameworks" \
    AROS_SETTINGS_SCHEMA="$APP/Resources/settings.json" \
    AROS_HOST_VOLUME="$AROS_HOST_VOLUME" \
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
Assign "CLIPS:" "RAM:Clipboards"
If EXISTS "C:SetClock"
    SetClock LOAD
EndIf
If EXISTS "DEVS:DOSDrivers/PIPE"
    Mount DEVS:DOSDrivers/PIPE
EndIf
If EXISTS "DEVS:Keymaps"
    Assign "KEYMAPS:" "DEVS:Keymaps"
EndIf
If EXISTS "C:LoadKeymap"
    LoadKeymap RESTORE
EndIf
If EXISTS "C:KeymapWatch"
    Run <NIL: >NIL: C:KeymapWatch
EndIf
SetEnv HOME "MacRW:"
SetEnv MOONSTONE_ROOT "SYS:Moonstone"
If EXISTS "MacRW:.macaros-boot"
    Execute "MacRW:.macaros-boot"
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
L/con-handler L/pipe-handler"

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

write_build_manifest() {
    manifest="$APP/Contents/Resources/Documentation/BUILD-MANIFEST.txt"
    require_clean="${MACAROS_REQUIRE_CLEAN_SOURCES:-0}"
    manifest_failed=0
    {
        echo "format=macaros-build-manifest-v1"
        echo "product=Macaros"
        echo "version=$VERSION"
        echo "build=$BUILD_NUMBER"
        echo "generated_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        source_specs="Macaros|$ROOT|origin AROS|$AROS_SOURCE_TREE|fork Zed|$ZED_SOURCE_TREE|jonx Ferail|$FERAIL_SOURCE_TREE|origin"
        [ "$INCLUDE_MOONSTONE" = 1 ] && source_specs="$source_specs Moonstone|$MOONSTONE_SOURCE_TREE|origin"
        for source_spec in $source_specs; do
            source_name=${source_spec%%|*}
            source_rest=${source_spec#*|}
            source_dir=${source_rest%|*}
            source_remote=${source_rest##*|}
            if git -C "$source_dir" rev-parse --git-dir >/dev/null 2>&1; then
                source_rev=$(git -C "$source_dir" rev-parse HEAD)
                source_branch=$(git -C "$source_dir" branch --show-current 2>/dev/null || echo detached)
                [ -n "$source_branch" ] || source_branch=detached
                if [ -n "$(git -C "$source_dir" status --porcelain 2>/dev/null)" ]; then
                    source_state=dirty
                    [ "$require_clean" = 1 ] && manifest_failed=1
                else
                    source_state=clean
                fi
                source_url=$(git -C "$source_dir" remote get-url "$source_remote" 2>/dev/null || \
                    git -C "$source_dir" remote get-url origin 2>/dev/null || echo unknown)
                source_upstream=$(git -C "$source_dir" rev-parse --abbrev-ref '@{upstream}' 2>/dev/null || echo none)
                if [ "$source_upstream" != none ]; then
                    source_counts=$(git -C "$source_dir" rev-list --left-right --count "$source_upstream...HEAD" 2>/dev/null || echo "unknown unknown")
                    source_behind=$(printf '%s\n' "$source_counts" | awk '{print $1}')
                    source_ahead=$(printf '%s\n' "$source_counts" | awk '{print $2}')
                else
                    source_ahead=unknown
                    source_behind=unknown
                fi
                printf 'source=%s\trevision=%s\tstate=%s\tbranch=%s\tupstream=%s\tahead=%s\tbehind=%s\turl=%s\n' \
                    "$source_name" "$source_rev" "$source_state" "$source_branch" \
                    "$source_upstream" "$source_ahead" "$source_behind" "$source_url"
            else
                printf 'source=%s\trevision=unknown\tstate=missing\turl=unknown\n' "$source_name"
                [ "$require_clean" = 1 ] && manifest_failed=1
            fi
        done
        for artifact in \
            "$DST/C/Zed" "$DST/C/Ferail" "$DST/C/Moonstone" \
            "$APP/Contents/Frameworks/cocoametal.dylib" \
            "$APP/Contents/Frameworks/libpasteboard.dylib" \
            "$APP/Contents/Frameworks/libcoreaudio.dylib" \
            "$APP/Contents/Frameworks/libbsdsockhost.dylib" \
            "$APP/Contents/Frameworks/libemu68k.dylib"; do
            [ -f "$artifact" ] || continue
            artifact_rel=${artifact#"$APP/"}
            artifact_hash=$(shasum -a 256 "$artifact" | awk '{print $1}')
            printf 'artifact=%s\tsha256=%s\n' "$artifact_rel" "$artifact_hash"
        done
    } > "$manifest"
    if [ "$manifest_failed" = 1 ]; then
        echo "make-aros-release.sh: source manifest contains dirty or missing inputs" >&2
        echo "unset MACAROS_REQUIRE_CLEAN_SOURCES for an internal candidate" >&2
        exit 1
    fi
}

require_clean_sources() {
    [ "${MACAROS_REQUIRE_CLEAN_SOURCES:-0}" = 1 ] || return 0
    source_failure=0
    source_specs="Macaros|$ROOT AROS|$AROS_SOURCE_TREE Zed|$ZED_SOURCE_TREE Ferail|$FERAIL_SOURCE_TREE"
    [ "$INCLUDE_MOONSTONE" = 1 ] && source_specs="$source_specs Moonstone|$MOONSTONE_SOURCE_TREE"
    for source_spec in $source_specs; do
        source_name=${source_spec%%|*}
        source_dir=${source_spec#*|}
        if ! git -C "$source_dir" rev-parse --git-dir >/dev/null 2>&1; then
            echo "release source missing: $source_name ($source_dir)" >&2
            source_failure=1
        elif [ -n "$(git -C "$source_dir" status --porcelain 2>/dev/null)" ]; then
            echo "release source is dirty: $source_name ($source_dir)" >&2
            source_failure=1
        fi
    done
    [ "$source_failure" = 0 ] || exit 1
}

make_dmg() {
    DMG="${AROS_DMG:-$ROOT/build/Macaros.dmg}"
    STAGE="${AROS_DMG_STAGE:-$ROOT/build/dmg-root}"
    [ -d "$APP" ] || { echo "make-aros-release.sh: missing app for DMG: $APP" >&2; exit 1; }
    [ -x "$COMPAT" ] || { echo "make-aros-release.sh: missing compatibility checker: $COMPAT" >&2; exit 1; }
    rm -f "$DMG"; rm -rf "$STAGE"; mkdir -p "$STAGE"
    echo ">> staging disk image contents ..."
    /usr/bin/ditto "$APP" "$STAGE/$(basename "$APP")"
    cp -f "$RELEASE_README" "$STAGE/README.md"
    cp -f "$ROOT/RELEASE-NOTES.md" "$STAGE/RELEASE-NOTES.md"
    cp -f "$COMPAT" "$STAGE/Check Macaros Compatibility.command"
    chmod +x "$STAGE/Check Macaros Compatibility.command"
    if [ -n "${MACAROS_SIGN_IDENTITY:-}" ]; then
        codesign --force --options runtime --timestamp \
            --sign "$MACAROS_SIGN_IDENTITY" "$STAGE/Check Macaros Compatibility.command"
    else
        codesign --force --sign - "$STAGE/Check Macaros Compatibility.command"
    fi
    cp -f "$ROOT/LICENSE" "$STAGE/LICENSE"
    cp -f "$ROOT/THIRD-PARTY-NOTICES.md" "$STAGE/THIRD-PARTY-NOTICES.md"
    [ -d "$NOTICES" ] || { echo "make-aros-release.sh: missing notices directory: $NOTICES" >&2; exit 1; }
    /usr/bin/ditto "$NOTICES" "$STAGE/Licenses"
    ln -s /Applications "$STAGE/Applications"
    echo ">> building $DMG ..."
    hdiutil create -quiet -volname "Macaros $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
    rm -rf "$STAGE"
    echo ">> built $DMG ($(du -sh "$DMG" | awk '{print $1}'))"
}

# --- static self-containment audit -----------------------------------------
if [ "${1:-}" = "--check" ]; then
    # Audit every invariant and report the full set instead of stopping at the
    # first failed test under the script's normal `set -e` behavior.
    set +e
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
    [ -e "$A/Resources/AROS/C/Zed" ];                   ck $? "Zed embedded (C:Zed)"
    [ -e "$A/Resources/AROS/C/Ferail" ];                ck $? "Ferail embedded (C:Ferail)"
    [ -e "$A/Resources/AROS/Libs/emu68k.library" ];     ck $? "legacy 68k execution library embedded"
    [ -e "$A/Resources/AROS/L/exfat-handler" ];         ck $? "exFAT filesystem handler embedded"
    [ -e "$A/Resources/AROS/Devs/DOSDrivers/EXFAT0" ];  ck $? "exFAT DOSDriver embedded"
    [ -f "$A/Resources/AROS/C/Zed.info" ];              ck $? "Zed icon"
    [ -f "$A/Resources/AROS/C/Ferail.info" ];           ck $? "Ferail icon"
    grep -q '^:C/Zed$'       "$A/Resources/AROS/.backdrop" 2>/dev/null; ck $? "Zed on the desktop (.backdrop)"
    grep -q '^:C/Ferail$'    "$A/Resources/AROS/.backdrop" 2>/dev/null; ck $? "Ferail on the desktop (.backdrop)"
    if grep -q '^Moonstone$' "$A/Resources/Documentation/COMPONENTS.txt" 2>/dev/null; then
        [ -e "$A/Resources/AROS/C/Moonstone" ];             ck $? "optional Moonstone embedded"
        [ -d "$A/Resources/AROS/Moonstone/extracted/moonahdk" ]; ck $? "Moonstone game assets"
        [ -d "$A/Resources/AROS/Moonstone/assets/data" ];   ck $? "Moonstone data tables"
        [ -d "$A/Resources/AROS/Moonstone/assets/music" ];  ck $? "Moonstone soundtrack"
        [ -f "$A/Resources/AROS/C/Moonstone.info" ];        ck $? "Moonstone icon"
        grep -q '^:C/Moonstone$' "$A/Resources/AROS/.backdrop" 2>/dev/null; ck $? "Moonstone on the desktop"
    else
        [ ! -e "$A/Resources/AROS/C/Moonstone" ];           ck $? "Moonstone binary excluded by default"
        [ ! -e "$A/Resources/AROS/Moonstone" ];             ck $? "Moonstone assets excluded by default"
        ! grep -q '^:C/Moonstone$' "$A/Resources/AROS/.backdrop" 2>/dev/null; ck $? "Moonstone absent from desktop"
    fi
    for x in $EXCLUDE_C; do
        [ ! -e "$A/Resources/AROS/C/$x" ];              ck $? "dev-only C:$x not shipped"
    done
    for x in crash DiskImages Developer T .cache .config .local symbols.out; do
        [ ! -e "$A/Resources/AROS/$x" ];                ck $? "private/developer AROS:$x not shipped"
    done
    if find "$A/Resources/AROS" -type f \( -name '*.snapshot' -o -name '*.ips' -o -name '*.core' \) -print -quit 2>/dev/null | grep -q .; then
        ck 1 "no crash snapshots or host crash reports embedded"
    else
        ck 0 "no crash snapshots or host crash reports embedded"
    fi
    [ -f "$A/Resources/AROS/S/Startup-Sequence" ];      ck $? "desktop Startup-Sequence baked"
    grep -q '^Assign "CLIPS:" "RAM:Clipboards"$' "$A/Resources/AROS/S/Startup-Sequence" 2>/dev/null; ck $? "clipboard uses writable RAM:Clipboards"
    ! grep -q '^Assign "CLIPS:" "SYS:clips"$' "$A/Resources/AROS/S/Startup-Sequence" 2>/dev/null; ck $? "clipboard does not target the sealed system volume"
    [ -f "$A/Resources/AROS/boot/darwin/AROSBootstrap.conf.tmpl" ]; ck $? "conf template present"
    # THE self-containment invariant: no path in the template escapes the bundle.
    if grep -qE '/(Users|private|tmp|Volumes)/' "$A/Resources/AROS/boot/darwin/AROSBootstrap.conf.tmpl" 2>/dev/null; then
        echo "    FAIL: conf template still has host-absolute paths:"; grep -nE '/(Users|private|tmp|Volumes)/' "$A/Resources/AROS/boot/darwin/AROSBootstrap.conf.tmpl" | head; fail=1
    else ck 0 "conf template is bundle-relative (@AROSROOT@ only)"; fi
    for m in cocoametal libpasteboard libcoreaudio libbsdsockhost libemu68k; do
        [ -f "$A/Frameworks/$m.dylib" ]; ck $? "Frameworks/$m.dylib"
    done
    [ -f "$A/Resources/report-env.sh" ];                  ck $? "bounded report environment helper"
    grep -q 'report-env.sh' "$A/MacOS/Macaros" 2>/dev/null; ck $? "launcher configures local Reports folder"
    grep -q 'export MACAROS_VERSION MACAROS_BUILD_NUMBER' "$A/MacOS/Macaros" 2>/dev/null; ck $? "launcher passes bundle identity to About"
    [ -x "$A/MacOS/Macaros" ];                          ck $? "launcher executable"
    [ -f "$A/Resources/Documentation/README.md" ];      ck $? "release README embedded"
    [ -f "$A/Resources/Documentation/RELEASE-NOTES.md" ]; ck $? "release notes embedded"
    [ -f "$A/Resources/Documentation/LICENSE" ];        ck $? "AROS licence embedded"
    [ -f "$A/Resources/Documentation/THIRD-PARTY-NOTICES.md" ]; ck $? "third-party notices embedded"
    [ -f "$A/Resources/Documentation/Licenses/ZED-LICENSE-GPL" ]; ck $? "Zed GPL licence embedded"
    [ -f "$A/Resources/Documentation/Licenses/FERAIL-THIRD-PARTY-NOTICES.md" ]; ck $? "Ferail notices embedded"
    [ -f "$A/Resources/Documentation/Licenses/EMU68-NOTICE" ]; ck $? "Emu68 notice embedded"
    [ -f "$A/Resources/Documentation/BUILD-MANIFEST.txt" ]; ck $? "build manifest embedded"
    grep -q '@AROSROOT@' "$A/Resources/AROS/boot/darwin/AROSBootstrap.conf.tmpl"; ck $? "template uses @AROSROOT@ placeholder"
    plutil -lint "$A/Info.plist" >/dev/null 2>&1;       ck $? "Info.plist valid"
    [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$A/Info.plist" 2>/dev/null)" = "$VERSION" ]; ck $? "bundle version matches VERSION"
    [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$A/Info.plist" 2>/dev/null)" = "$BUILD_NUMBER" ]; ck $? "bundle build number matches VERSION"
    [ "$fail" = 0 ] && { echo "[REL] PASS"; exit 0; } || { echo "[REL] FAIL"; exit 1; }
fi

if [ "${1:-}" = "--dmg-only" ]; then
    "$0" --check
    make_dmg
    exit 0
fi

case "${1:-}" in ""|--dmg) ;; *) echo "usage: $0 [--check|--dmg|--dmg-only]" >&2; exit 2 ;; esac

# --- real build ------------------------------------------------------------
BOOTD="$(find_bootd)"
[ -n "$BOOTD" ] && [ -x "$BOOTD/AROSBootstrap" ] || { echo "make-aros-release.sh: no AROSBootstrap (set AROS_CTL_BOOTD)" >&2; exit 1; }
SRC="$(cd "$BOOTD/../.." && pwd)"   # .../darwin-aarch64/AROS
[ -f "$DYLIB" ] || { echo "missing $DYLIB (make cocoametal-dylib, or set AROS_CTL_DYLIB)" >&2; exit 1; }
[ -f "$EMU68K" ] || { echo "missing $EMU68K (make emu68k-dylib, or set AROS_CTL_EMU68K_DYLIB)" >&2; exit 1; }
[ -x "$COMPAT" ] || { echo "missing executable compatibility checker: $COMPAT" >&2; exit 1; }
[ -f "$RELEASE_README" ] || { echo "missing release README: $RELEASE_README" >&2; exit 1; }
[ -f "$ROOT/RELEASE-NOTES.md" ] || { echo "missing release notes: $ROOT/RELEASE-NOTES.md" >&2; exit 1; }
[ -d "$NOTICES" ] || { echo "missing third-party notices directory: $NOTICES" >&2; exit 1; }
[ -f "$REPORT_ENV" ] || { echo "missing report environment helper: $REPORT_ENV" >&2; exit 1; }

# require the prepared desktop payloads — this script does NOT re-stage them
for p in AROS.boot Fonts Prefs/Presets/Themes/AROSDefault System/Wanderer/Wanderer \
    Devs/Monitors/Cocoa C/Zed C/Ferail Libs/emu68k.library L/exfat-handler \
    Devs/DOSDrivers/EXFAT0; do
    [ -e "$SRC/$p" ] || { echo "make-aros-release.sh: $SRC missing $p — boot desktop once via run-window.sh first" >&2; exit 1; }
done
if [ "$INCLUDE_MOONSTONE" = 1 ]; then
    [ -e "$SRC/C/Moonstone" ] || { echo "make-aros-release.sh: optional C:Moonstone is missing" >&2; exit 1; }
    for p in extracted/moonahdk assets/data assets/music; do
        [ -d "$MOONSTONE_SRC/$p" ] || { echo "make-aros-release.sh: $MOONSTONE_SRC missing $p (set MOONSTONE_SRC)" >&2; exit 1; }
    done
fi
require_clean_sources

echo ">> source AROS tree: $SRC ($(du -sh "$SRC" | awk '{print $1}'))"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"

echo ">> embedding release-filtered AROS volume (read-only) ..."
DST="$APP/Contents/Resources/AROS"
mkdir -p "$DST"
for item in $RUNTIME_ROOT_ITEMS; do
    [ -e "$SRC/$item" ] || continue
    /usr/bin/ditto "$SRC/$item" "$DST/$item"
done
rm -f "$DST/Devs/Monitors/headless" 2>/dev/null || true
for x in $EXCLUDE_C; do rm -f "$DST/C/$x" "$DST/C/$x.info"; done
[ "$INCLUDE_MOONSTONE" = 1 ] || rm -f "$DST/C/Moonstone" "$DST/C/Moonstone.info"

# Desktop icons: Wanderer draws a backdrop icon for each `:path` line in the
# volume's .backdrop, so the two apps land on the desktop without moving them
# off the command path. The icons themselves ship with this script (made by
# make-aros-icon.py from each project's own artwork).
cp -f "$HERE/icons/Zed.info"       "$DST/C/Zed.info"
cp -f "$HERE/icons/Ferail.info"    "$DST/C/Ferail.info"
printf ':C/Zed\n:C/Ferail\n' > "$DST/.backdrop"

if [ "$INCLUDE_MOONSTONE" = 1 ]; then
    cp -f "$HERE/icons/Moonstone.info" "$DST/C/Moonstone.info"
    printf ':C/Moonstone\n' >> "$DST/.backdrop"
    echo ">> embedding optional Moonstone assets ..."
    mkdir -p "$DST/Moonstone/assets"
    /usr/bin/ditto "$MOONSTONE_SRC/extracted"   "$DST/Moonstone/extracted"
    /usr/bin/ditto "$MOONSTONE_SRC/assets/data" "$DST/Moonstone/assets/data"
    /usr/bin/ditto "$MOONSTONE_SRC/assets/music" "$DST/Moonstone/assets/music"
    cp -f "$MOONSTONE_SRC/CH.PIV" "$DST/Moonstone/CH.PIV" 2>/dev/null || true
    : > "$DST/Moonstone/.moonstone-root"
fi

# HOME points at the writable host share, not at SYS: — the embedded volume is
# inside the signed bundle and must stay untouched.
mkdir -p "$DST/Prefs/Env-Archive"
printf 'MacRW:' > "$DST/Prefs/Env-Archive/HOME"
rm -f "$DST/Prefs/Env-Archive/AROS_FSW_ROOTS"   # dev paths; the launcher writes the real one

# The menu-bar/app binary must be named Macaros; keep it inside the boot dir so
# cwd-relative host resolution matches run-window.sh.
cp -f "$DST/boot/darwin/AROSBootstrap" "$DST/boot/darwin/Macaros"

# Bake the desktop Startup-Sequence.
mkdir -p "$DST/S"
if [ "$INCLUDE_MOONSTONE" = 1 ]; then
    printf '%s\n' "$STARTUP_SEQUENCE" > "$DST/S/Startup-Sequence"
else
    printf '%s\n' "$STARTUP_SEQUENCE" | grep -v '^SetEnv MOONSTONE_ROOT ' > "$DST/S/Startup-Sequence"
fi

# Build the bundle-relative conf TEMPLATE: every module path -> @AROSROOT@/<rel>.
# Take the base conf's module lines (normalising both relative and host-absolute
# forms to a tree-relative path), then add the extra module set if the file exists.
TMPL="$DST/boot/darwin/AROSBootstrap.conf.tmpl"
{
    echo "# Macaros self-contained boot config (generated by make-aros-release.sh)."
    echo "# @AROSROOT@ is expanded to the bundle's embedded AROS tree at launch."
    printf 'memory %s\n' "$MEMORY"
    # A crashing application should cost the user that application, not the
    # machine: contained traps remove the offending task and leave the rest
    # running (docs/features/crash-handling/design.md).
    printf 'arguments containment\n'
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
cp "$EMU68K" "$APP/Contents/Frameworks/libemu68k.dylib"
cp "$SCHEMA" "$APP/Contents/Resources/settings.json"
[ -f "$ICON" ] && cp "$ICON" "$APP/Contents/Resources/Macaros.icns"
[ -f "$HERE/aros-host-conf.sh" ] && cp "$HERE/aros-host-conf.sh" "$APP/Contents/Resources/aros-host-conf.sh"
cp "$REPORT_ENV" "$APP/Contents/Resources/report-env.sh"
mkdir -p "$APP/Contents/Resources/Documentation"
cp "$RELEASE_README" "$APP/Contents/Resources/Documentation/README.md"
cp "$ROOT/RELEASE-NOTES.md" "$APP/Contents/Resources/Documentation/RELEASE-NOTES.md"
cp "$ROOT/LICENSE" "$APP/Contents/Resources/Documentation/LICENSE"
cp "$ROOT/THIRD-PARTY-NOTICES.md" "$APP/Contents/Resources/Documentation/THIRD-PARTY-NOTICES.md"
/usr/bin/ditto "$NOTICES" "$APP/Contents/Resources/Documentation/Licenses"
printf 'Zed\nFerail\n' > "$APP/Contents/Resources/Documentation/COMPONENTS.txt"
if [ "$INCLUDE_MOONSTONE" = 1 ]; then
    printf 'Moonstone\n' >> "$APP/Contents/Resources/Documentation/COMPONENTS.txt"
fi
printf '%s' "$LAUNCHER"   > "$APP/Contents/MacOS/Macaros"; chmod +x "$APP/Contents/MacOS/Macaros"
printf '%s' "$INFO_PLIST" | sed \
    -e "s|@MACAROS_VERSION@|$VERSION|g" \
    -e "s|@MACAROS_BUILD_NUMBER@|$BUILD_NUMBER|g" \
    > "$APP/Contents/Info.plist"
write_build_manifest

echo ">> built $APP ($(du -sh "$APP" | awk '{print $1}'))"
"$0" --check || { echo ">> self-containment audit FAILED"; exit 1; }

if [ "${1:-}" = "--dmg" ]; then
    make_dmg
fi

echo ">> next: test-boot (relocated), then Developer-ID sign + notarize + staple."
echo ">>   open '$APP'      # or Contents/MacOS/Macaros for stdout"
