#!/bin/sh
#
# run-window.sh — boot hosted darwin-aarch64 AROS in a Cocoa/Metal WINDOW with
# keyboard + mouse input.
#
# Run this from a GUI session (Terminal.app, not over plain ssh): a window titled
# "AROS" opens showing the blue Workbench screen and the boot console. Click the
# window to give it keyboard focus, then type at the "1> " shell prompt.
#
#   Dir MacRO:     list the shared Mac folder (read-only volume)
#   Dir MacRW:     same folder, read/write — Copy/MakeDir here land on the Mac
#   Echo hello     prints "hello" (built-in, no file I/O)
#   Version        prints the kickstart version
#   <Ctrl-C>       in THIS terminal stops everything
#
# Two volumes are mapped to a Mac folder (default ~/AROS/Shared, or pass one as
# the first argument): MacRO: read-only and MacRW: read/write. A write to MacRO:
# is refused ("disk is write-protected"); a write to MacRW: appears in the folder.
#
# Most C: commands aren't built yet ("object not found"). Commands that do heavy
# host file I/O can still hit the threaded-mode mid-syscall bug — basic typing,
# echo and the shell built-ins are the safe path for now.
#
# The AROS build dir lives outside the repo (often under /private/tmp, ephemeral);
# this script discovers it, or you can set AROS_CTL_BOOTD. The dylib + entitlements
# travel with the repo.
set -e

# --- Relocatable paths -----------------------------------------------------
# Resolve this script's own dir (following symlinks) -> repo root, so the in-repo
# artifacts are found wherever the checkout lives. Each is overridable.
SELF="$0"
while [ -h "$SELF" ]; do
    dir="$(cd "$(dirname "$SELF")" && pwd)"
    SELF="$(readlink "$SELF")"; case "$SELF" in /*) ;; *) SELF="$dir/$SELF" ;; esac
done
HERE="$(cd "$(dirname "$SELF")" && pwd)"   # .../graft
ROOT="$(cd "$HERE/.." && pwd)"             # repo root

: "${AROS_HOST_CONF:=$HOME/Library/Application Support/AROS/aros-host.conf}"
export AROS_HOST_CONF
[ -f "$HERE/aros-host-conf.sh" ] && . "$HERE/aros-host-conf.sh"

# Default keyboard layout. Without a keymap loaded the keyboard.hidd emits raw
# keycodes (everything comes out uppercase). pc105_f is the France AZERTY map;
# the keymaps are built into DEVS:Keymaps and the Startup-Sequence SetKeyboard's
# this. Override per-run with AROS_CTL_KEYMAP=pc105_gb / pc104_usa / ... .
: "${AROS_CTL_KEYMAP:=pc105_f}"
export AROS_CTL_KEYMAP

# cocoametal host shim (built into the repo's build/) and the entitlements that
# travel beside this script. Override AROS_CTL_DYLIB / AROS_CTL_ENT if relocated.
DYLIB_SRC="${AROS_CTL_DYLIB:-$ROOT/build/cocoametal.dylib}"
ENT="${AROS_CTL_ENT:-$HERE/aros-host.entitlements.plist}"

# The AROS boot dir (AROSBootstrap + .conf) is a BUILD OUTPUT outside the repo, so
# discover it: $AROS_CTL_BOOTD wins, else the canonical build dir ($BUILD, default
# /tmp/arosbuild — see build-darwin-aarch64.sh), else an in-repo staging dir, else
# the newest matching scratchpad build (newest AROSBootstrap by mtime breaks ties).
find_bootd() {
    if [ -n "${AROS_CTL_BOOTD:-}" ]; then printf '%s\n' "$AROS_CTL_BOOTD"; return; fi
    best=""; bt=0
    for d in \
        "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64/AROS/boot/darwin" \
        "$ROOT/build/AROS/boot/darwin" \
        /private/tmp/claude-*/*/*/scratchpad/arosbuild/bin/darwin-aarch64/AROS/boot/darwin \
        /tmp/*/bin/darwin-aarch64/AROS/boot/darwin ; do
        [ -x "$d/AROSBootstrap" ] || continue
        t="$(stat -f %m "$d/AROSBootstrap" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}
BOOTD="$(find_bootd)"

if [ -z "$BOOTD" ] || [ ! -x "$BOOTD/AROSBootstrap" ]; then
    echo "run-window.sh: no AROSBootstrap found." >&2
    echo "  point AROS_CTL_BOOTD at <build>/bin/darwin-aarch64/AROS/boot/darwin," >&2
    echo "  or build one with graft/build-darwin-aarch64.sh (BUILD defaults to /tmp/arosbuild)." >&2
    exit 1
fi
echo ">> boot dir: $BOOTD"
AROS="$(cd "$BOOTD/../.." && pwd)"   # .../darwin-aarch64/AROS

artifact_line() {
    label="$1"
    path="$2"
    [ -e "$path" ] || return 0
    stamp="$(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$path" 2>/dev/null || echo '?')"
    hash="$(shasum -a 256 "$path" 2>/dev/null | awk '{print substr($1,1,12)}')"
    echo ">> $label ${hash:-????????????} $stamp"
}

ensure_desktop_payloads() {
    upstream="${AROS_UPSTREAM:-$ROOT/../aros-upstream}"
    fontsrc="$upstream/workbench/fonts"

    if [ -d "$fontsrc" ]; then
        mkdir -p "$AROS/Fonts"
        for d in fixed arial stop ttcourier XEN; do
            [ -d "$fontsrc/$d" ] || continue
            mkdir -p "$AROS/Fonts/$d"
            cp -p "$fontsrc/$d"/* "$AROS/Fonts/$d/" 2>/dev/null || true
        done
    fi

    # Stage the AROSDefault theme + the boot signature. The build steps that
    # normally produce these (distfiles for the theme, `make boot` for AROS.boot)
    # are not run when we boot the AROS/ build dir directly, so without them:
    #  - no theme  -> Startup-Sequence "Assign THEMES:" fails -> Wanderer pops a
    #    "Please insert volume THEMES" requester and the desktop is blocked;
    #  - no AROS.boot -> __dos_IsBootable() rejects the hosted volume -> "Display
    #    driver(s) failed to initialize. Entering emergency shell."
    themesrc="$upstream/images/Themes/AROSDefault"
    if [ -d "$themesrc" ] && [ ! -d "$AROS/Prefs/Presets/Themes/AROSDefault" ]; then
        mkdir -p "$AROS/Prefs/Presets/Themes"
        cp -Rp "$themesrc" "$AROS/Prefs/Presets/Themes/"
    fi
    [ -f "$AROS/AROS.boot" ] || printf 'aarch64\n' > "$AROS/AROS.boot"

    default_images="$AROS/Prefs/Presets/Themes/AROSDefault/images"
    if [ -d "$default_images" ]; then
        mkdir -p "$AROS/System/Images"
        cp -Rp "$default_images/." "$AROS/System/Images/"
    fi
}

write_startup_sequence() {
    startup="$AROS/S/Startup-Sequence"

    if [ -n "${AROS_CTL_STARTUP_FILE:-}" ]; then
        cp -f "$AROS_CTL_STARTUP_FILE" "$startup"
    else
        case "${AROS_CTL_STARTUP_MODE:-console}" in
        desktop|wanderer)
            # A compact desktop boot path. The stock Startup-Sequence does much
            # more, but these are the pieces Wanderer expects before it starts.
            {
                printf '%s\n' \
                    'Version' \
                    'FailAt 21' \
                    'If NOT EXISTS "RAM:Clipboards"' \
                    '    MakeDir "RAM:Clipboards"' \
                    'EndIf' \
                    'If NOT EXISTS "RAM:T"' \
                    '    MakeDir "RAM:T"' \
                    'EndIf' \
                    'If NOT EXISTS "RAM:ENV"' \
                    '    MakeDir "RAM:ENV"' \
                    '    Assign "ENV:" "RAM:ENV"' \
                    'EndIf' \
                    'Assign "T:" "RAM:T"' \
                    'Assign "CLIPS:" "SYS:clips"' \
                    'If EXISTS "DEVS:Keymaps"' \
                    '    Assign "KEYMAPS:" "DEVS:Keymaps"' \
                    'EndIf'
                # Seed the saved keymap from the host config on first boot; an
                # interactive `LoadKeymap <name>` overrides it (writes ENVARC:Keymap).
                if [ -n "${AROS_CTL_KEYMAP:-}" ] && [ ! -s "$AROS/Prefs/Env-Archive/Keymap" ]; then
                    mkdir -p "$AROS/Prefs/Env-Archive" 2>/dev/null
                    printf '%s' "$AROS_CTL_KEYMAP" > "$AROS/Prefs/Env-Archive/Keymap"
                fi
                printf '%s\n' \
                    'If EXISTS "C:LoadKeymap"' \
                    '    LoadKeymap RESTORE' \
                    'EndIf'
                printf '%s\n' \
                    'Assign "LOCALE:" "SYS:Locale"' \
                    'Assign "LIBS:" "SYS:Classes" ADD' \
                    'Assign "HELP:" "LOCALE:Help" DEFER' \
                    'Assign "IMAGES:" "SYS:System/Images" DEFER' \
                    'Assign "WANDERER:" "SYS:System/Wanderer" DEFER' \
                    'Assign "THEMES:" "SYS:Prefs/Presets/Themes" >NIL:' \
                    'Assign "THEME:" "THEMES:AROSDefault"' \
                    'If EXISTS "THEME:Images"' \
                    '    Assign "IMAGES:" "THEME:Images" PREPEND' \
                    'EndIf' \
                    'Path "C:" "SYS:System" "S:" "SYS:Prefs" QUIET' \
                    'If EXISTS "SYS:Fonts"' \
                    '    Assign "FONTS:" "SYS:Fonts"' \
                    'EndIf' \
                    'If EXISTS "SYS:Tools"' \
                    '    Path "SYS:Tools" QUIET ADD' \
                    'EndIf' \
                    'If EXISTS "SYS:Utilities"' \
                    '    Path "SYS:Utilities" QUIET ADD' \
                    'EndIf' \
                    'If EXISTS "C:AddDataTypes"' \
                    '    AddDataTypes REFRESH QUIET' \
                    'EndIf' \
                    'If EXISTS "C:AddAudioModes"' \
                    '    If EXISTS "DEVS:AudioModes/COREAUDIO"' \
                    '        Run <NIL: >NIL: QUIET AddAudioModes DEVS:AudioModes/COREAUDIO QUIET' \
                    '    EndIf' \
                    'EndIf' \
                    'If EXISTS "C:IPrefs"' \
                    '    IPrefs' \
                    'EndIf' \
                    'Run <NIL: >NIL: QUIET ConClip' \
                    'If EXISTS "WANDERER:Wanderer"' \
                    '    Run <NIL: >NIL: QUIET WANDERER:Wanderer' \
                    '    Wait 2' \
                    '    EndCLI' \
                    'EndIf'
            } > "$startup"
            ;;
        console)
            # Startup-sequence: print the version, then make clipboard.device
            # use a visible backing directory and start ConClip for console
            # copy/paste.
            {
                printf '%s\n' \
                    'Version' \
                    'If NOT EXISTS "RAM:T"' \
                    '    MakeDir "RAM:T"' \
                    'EndIf' \
                    'Assign "T:" "RAM:T"' \
                    'Assign CLIPS: SYS:clips' \
                    'If EXISTS "C:AddAudioModes"' \
                    '    If EXISTS "DEVS:AudioModes/COREAUDIO"' \
                    '        AddAudioModes DEVS:AudioModes/COREAUDIO QUIET' \
                    '    EndIf' \
                    'EndIf' \
                    'Run ConClip'
            } > "$startup"
            ;;
        *)
            echo "run-window.sh: unknown AROS_CTL_STARTUP_MODE=${AROS_CTL_STARTUP_MODE}" >&2
            exit 1
            ;;
        esac
    fi

    [ -n "${AROS_CTL_STARTUP_EXTRA:-}" ] && printf '%s\n' "$AROS_CTL_STARTUP_EXTRA" \
        >> "$startup"
    return 0
}

# Refuse to relaunch over the exact same boot tree while an older instance is
# still alive. This script rewrites the conf, copies Storage/Monitors/Cocoa into
# Devs/Monitors, and replaces/signs "$BOOTD/Daedalos"; doing that underneath a
# running instance makes the next launch look randomly brittle.
RUNNING_PIDS="$(ps -axo pid=,command= | awk -v conf="$BOOTD/AROSBootstrap.conf" '
    index($0, conf) &&
    (index($0, "/Daedalos ") || index($0, "./Daedalos ") ||
     index($0, " Daedalos ") || index($0, "/AROSBootstrap ") ||
     index($0, "./AROSBootstrap ") || index($0, " AROSBootstrap ")) {
        print $1
    }
')"
if [ -n "$RUNNING_PIDS" ]; then
    echo ">> stopping previous Daedalos instance(s) for this boot tree: $RUNNING_PIDS"
    for pid in $RUNNING_PIDS; do
        kill "$pid" 2>/dev/null || true
    done
    sleep 0.4
    for pid in $RUNNING_PIDS; do
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    done
fi

# Host shim: cocoametal.dylib must resolve by bare name under the hardened
# runtime, so keep it in ~/lib and point DYLD_FALLBACK_LIBRARY_PATH there.
mkdir -p "$HOME/lib"
[ -f "$DYLIB_SRC" ] && cp -f "$DYLIB_SRC" "$HOME/lib/cocoametal.dylib"
# Clipboard bridge host shim: the AROS clipboard sync task loads it by bare name
# via hostlib.resource, so it lives in ~/lib like cocoametal.dylib. Without it the
# bridge starts but silently no-ops (build it with: make pasteboard-dylib).
[ -f "$ROOT/build/libpasteboard.dylib" ] && cp -f "$ROOT/build/libpasteboard.dylib" "$HOME/lib/libpasteboard.dylib"
# CoreAudio/AHI host shim, deployed beside the other HostLib dylibs so the
# AROS-side audio driver can load it by bare name.
[ -f "$ROOT/build/libcoreaudio.dylib" ] && cp -f "$ROOT/build/libcoreaudio.dylib" "$HOME/lib/libcoreaudio.dylib"
# bsdsocket.library host pump shim, deployed beside the other HostLib dylibs so
# AROS-side networking can load it by bare name.
[ -f "$ROOT/build/libbsdsockhost.dylib" ] && cp -f "$ROOT/build/libbsdsockhost.dylib" "$HOME/lib/libbsdsockhost.dylib"
if [ ! -f "$HOME/lib/cocoametal.dylib" ]; then
    echo "cocoametal.dylib missing — build it:" >&2
    echo "    cd ~/Source/aros-aarch64 && make cocoametal-dylib" >&2
    exit 1
fi
echo ">> cocoametal.dylib $(shasum -a 256 "$HOME/lib/cocoametal.dylib" | awk '{print substr($1,1,12)}') from $HOME/lib"

# The Settings window is GENERATED from settings.json — deploy it beside the dylib
# (the dylib resolves it next to itself, and the launch env points at it too).
SCHEMA_SRC="$ROOT/hosted/cocoametal/settings.json"
[ -f "$SCHEMA_SRC" ] && cp -f "$SCHEMA_SRC" "$HOME/lib/settings.json"

# Cocoa as the sole display: deploy the driver to Devs/Monitors and drop the
# headless fallback (its teardown clashes with a real display registering).
mkdir -p "$AROS/Devs/Monitors"
[ -f "$AROS/Storage/Monitors/Cocoa" ] && cp -f "$AROS/Storage/Monitors/Cocoa" "$AROS/Devs/Monitors/Cocoa"
rm -f "$AROS/Devs/Monitors/headless"
mkdir -p "$AROS/clips"
ensure_desktop_payloads

# Clean conf (no boot narration) + ensure the full standard module set and the
# resident shell.resource are in the kickstart (same list as run-shell.sh —
# without shell.resource the Initial CLI never starts and the boot hangs).
grep -v '^arguments ' "$BOOTD/AROSBootstrap.conf" > "$BOOTD/AROSBootstrap.conf.tmp" 2>/dev/null \
    && mv "$BOOTD/AROSBootstrap.conf.tmp" "$BOOTD/AROSBootstrap.conf"
if [ -n "${AROS_HOST_MEMORY:-}" ]; then
    grep -v '^memory ' "$BOOTD/AROSBootstrap.conf" > "$BOOTD/AROSBootstrap.conf.tmp" 2>/dev/null \
        || cp "$BOOTD/AROSBootstrap.conf" "$BOOTD/AROSBootstrap.conf.tmp"
    printf 'memory %s\n' "$AROS_HOST_MEMORY" >> "$BOOTD/AROSBootstrap.conf.tmp"
    mv "$BOOTD/AROSBootstrap.conf.tmp" "$BOOTD/AROSBootstrap.conf"
fi
for M in \
    Devs/shell.resource Devs/task.resource \
    Devs/Drivers/hiddclass.hidd Devs/Drivers/gfx.hidd \
    Devs/Drivers/inputclass.hidd Devs/Drivers/keyboard.hidd Devs/Drivers/mouse.hidd \
    Devs/console.device Devs/input.device Devs/keyboard.device Devs/gameport.device \
    Devs/clipboard.device Devs/timer.device \
    Libs/keymap.library Libs/graphics.library Libs/layers.library \
    Libs/intuition.library Libs/gadtools.library Libs/iffparse.library \
    Libs/asl.library Libs/commodities.library Libs/cybergraphics.library \
    Libs/coolimages.library Libs/datatypes.library Libs/locale.library \
    Libs/muimaster.library Libs/rexxsyslib.library Libs/stdc.library \
    L/con-handler ; do
    if [ -e "$AROS/$M" ] && ! grep -q "/$M\$" "$BOOTD/AROSBootstrap.conf"; then
        echo "module $AROS/$M" >> "$BOOTD/AROSBootstrap.conf"
    fi
done

write_startup_sequence

# Share a Mac folder with AROS as TWO volumes, both mapped to the same folder:
#   MacRO:  read-only   (writes are refused with "disk is write-protected")
#   MacRW:  read/write  (changes appear in the Mac folder, and vice-versa)
# Override the folder with the first argument: run-window.sh ~/somewhere
# These are mounted by emul-handler itself from the AROS_HOST_VOLUME env var
# (one "<Vol>:<path>[;WRITE]" per line). ;WRITE is OUR keyword — it is delivered
# this way precisely so it never passes through (and is mangled by) AROS's Mount.
if [ -z "${AROS_HOST_VOLUME:-}" ]; then
    HOST_FOLDER="${1:-$HOME/AROS/Shared}"
    mkdir -p "$HOST_FOLDER"
    [ -e "$HOST_FOLDER/ReadMe" ] || \
        printf 'Files here show up in AROS as MacRO: (read-only) and MacRW: (read/write).\n' \
            > "$HOST_FOLDER/ReadMe"
    AROS_HOST_VOLUME="MacRO:$HOST_FOLDER
MacRW:$HOST_FOLDER;WRITE"
    export AROS_HOST_VOLUME
    echo ">> Sharing $HOST_FOLDER as MacRO: (read-only) and MacRW: (read/write)."
else
    export AROS_HOST_VOLUME
    echo ">> Sharing volumes from $AROS_HOST_CONF:"
    printf '%s\n' "$AROS_HOST_VOLUME" | sed 's/^/>>   /'
fi
artifact_line "AROSBootstrap" "$BOOTD/AROSBootstrap"
artifact_line "emul-handler" "$BOOTD/L/emul-handler"
artifact_line "Cocoa monitor" "$AROS/Devs/Monitors/Cocoa"
artifact_line "locale.library" "$AROS/Libs/locale.library"
artifact_line "timer.device" "$BOOTD/Devs/timer.device"
[ -e "$BOOTD/Devs/timer.device" ] || artifact_line "timer.device" "$AROS/Devs/timer.device"
artifact_line "task.resource" "$AROS/Devs/task.resource"
artifact_line "cybergraphics.library" "$AROS/Libs/cybergraphics.library"
artifact_line "coolimages.library" "$AROS/Libs/coolimages.library"
artifact_line "muimaster.library" "$AROS/Libs/muimaster.library"
artifact_line "stdc.library" "$AROS/Libs/stdc.library"
artifact_line "posixc.library" "$AROS/Libs/posixc.library"
artifact_line "png.library" "$AROS/Libs/png.library"
artifact_line "picture.datatype" "$AROS/Classes/DataTypes/picture.datatype"
artifact_line "png.datatype" "$AROS/Classes/DataTypes/png.datatype"
artifact_line "TestLib" "$AROS/C/TestLib"
artifact_line "LoadMatrix" "$AROS/C/LoadMatrix"
artifact_line "Wanderer" "$AROS/System/Wanderer/Wanderer"

# Present in the macOS menu bar / Dock as "Daedalos" (the host app), not the
# bootstrap binary's name: run a copy named Daedalos. macOS takes a non-bundled app's
# menu-bar name from the executable's filename, so the file itself must be Daedalos.
# (AROS is the OS it runs; Daedalos is the Mac app — the craftsman of Icaros's wings.)
cp -f "$BOOTD/AROSBootstrap" "$BOOTD/Daedalos"

# Sign with entitlements that allow DYLD_* env + loading the unsigned host shim.
[ -f "$ENT" ] && codesign -s - -f -o runtime --entitlements "$ENT" "$BOOTD/Daedalos" 2>/dev/null \
              || codesign -s - -f "$BOOTD/Daedalos" 2>/dev/null

echo ">> A 'Daedalos' window will open — click it for keyboard focus, then type."
mkdir -p "$ROOT/run/darwin-aarch64"   # File > screenshot/Record Movie land here
cd "$BOOTD"
if [ -n "${AROS_CM_CONTROL:-}" ]; then
    exec env AROS_DARWIN_THREADED=1 AROS_CM_CONTROL="$AROS_CM_CONTROL" \
        DYLD_FALLBACK_LIBRARY_PATH="$HOME/lib" \
        AROS_HOST_VOLUME="$AROS_HOST_VOLUME" \
        AROS_SETTINGS_SCHEMA="$HOME/lib/settings.json" \
        AROS_RUN_DIR="$ROOT/run/darwin-aarch64" \
        ./Daedalos -c "$BOOTD/AROSBootstrap.conf"
else
    exec env AROS_DARWIN_THREADED=1 DYLD_FALLBACK_LIBRARY_PATH="$HOME/lib" \
        AROS_HOST_VOLUME="$AROS_HOST_VOLUME" \
        AROS_SETTINGS_SCHEMA="$HOME/lib/settings.json" \
        AROS_RUN_DIR="$ROOT/run/darwin-aarch64" \
        ./Daedalos -c "$BOOTD/AROSBootstrap.conf"
fi
