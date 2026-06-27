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
# The build dir lives under /private/tmp and is ephemeral (cleared on reboot);
# if it's gone, rebuild and re-point BOOTD below.
set -e

BOOTD="/private/tmp/claude-501/-Users-user-Source-aros-aarch64/a7d73cfa-b608-4959-b0ec-f7720da95bb3/scratchpad/arosbuild/bin/darwin-aarch64/AROS/boot/darwin"
ENT="/private/tmp/claude-501/-Users-user-Source-aros-aarch64/a7d73cfa-b608-4959-b0ec-f7720da95bb3/scratchpad/scratchpad/cocoa-test-ent.plist"
DYLIB_SRC="/Users/user/Source/aros-aarch64/build/cocoametal.dylib"

if [ ! -x "$BOOTD/AROSBootstrap" ]; then
    echo "AROSBootstrap not found at:  $BOOTD" >&2
    echo "(the /private/tmp build dir was probably cleared — rebuild it)" >&2
    exit 1
fi
AROS="$(cd "$BOOTD/../.." && pwd)"   # .../darwin-aarch64/AROS

# Host shim: cocoametal.dylib must resolve by bare name under the hardened
# runtime, so keep it in ~/lib and point DYLD_FALLBACK_LIBRARY_PATH there.
mkdir -p "$HOME/lib"
[ -f "$DYLIB_SRC" ] && cp -f "$DYLIB_SRC" "$HOME/lib/cocoametal.dylib"
if [ ! -f "$HOME/lib/cocoametal.dylib" ]; then
    echo "cocoametal.dylib missing — build it:" >&2
    echo "    cd ~/Source/aros-aarch64 && make cocoametal-dylib" >&2
    exit 1
fi
echo ">> cocoametal.dylib $(shasum -a 256 "$HOME/lib/cocoametal.dylib" | awk '{print substr($1,1,12)}') from $HOME/lib"

# Cocoa as the sole display: deploy the driver to Devs/Monitors and drop the
# headless fallback (its teardown clashes with a real display registering).
mkdir -p "$AROS/Devs/Monitors"
[ -f "$AROS/Storage/Monitors/Cocoa" ] && cp -f "$AROS/Storage/Monitors/Cocoa" "$AROS/Devs/Monitors/Cocoa"
rm -f "$AROS/Devs/Monitors/headless"

# Clean conf (no boot narration) + ensure the full standard module set and the
# resident shell.resource are in the kickstart (same list as run-shell.sh —
# without shell.resource the Initial CLI never starts and the boot hangs).
grep -v '^arguments ' "$BOOTD/AROSBootstrap.conf" > "$BOOTD/AROSBootstrap.conf.tmp" 2>/dev/null \
    && mv "$BOOTD/AROSBootstrap.conf.tmp" "$BOOTD/AROSBootstrap.conf"
for M in \
    Devs/shell.resource \
    Devs/Drivers/hiddclass.hidd Devs/Drivers/gfx.hidd \
    Devs/Drivers/inputclass.hidd Devs/Drivers/keyboard.hidd Devs/Drivers/mouse.hidd \
    Devs/console.device Devs/input.device Devs/keyboard.device Devs/gameport.device \
    Libs/keymap.library Libs/graphics.library Libs/layers.library \
    Libs/intuition.library Libs/gadtools.library L/con-handler ; do
    if [ -e "$AROS/$M" ] && ! grep -q "/$M\$" "$BOOTD/AROSBootstrap.conf"; then
        echo "module $AROS/$M" >> "$BOOTD/AROSBootstrap.conf"
    fi
done

# Minimal startup-sequence: print the version, then drop to the interactive
# prompt inside the window.
printf 'Version\n' > "$AROS/S/Startup-Sequence"

# Share a Mac folder with AROS as TWO volumes, both mapped to the same folder:
#   MacRO:  read-only   (writes are refused with "disk is write-protected")
#   MacRW:  read/write  (changes appear in the Mac folder, and vice-versa)
# Override the folder with the first argument: run-window.sh ~/somewhere
# These are mounted by emul-handler itself from the AROS_HOST_VOLUME env var
# (one "<Vol>:<path>[;WRITE]" per line). ;WRITE is OUR keyword — it is delivered
# this way precisely so it never passes through (and is mangled by) AROS's Mount.
HOST_FOLDER="${1:-$HOME/AROS/Shared}"
mkdir -p "$HOST_FOLDER"
[ -e "$HOST_FOLDER/ReadMe" ] || \
    printf 'Files here show up in AROS as MacRO: (read-only) and MacRW: (read/write).\n' \
        > "$HOST_FOLDER/ReadMe"
AROS_HOST_VOLUME="MacRO:$HOST_FOLDER
MacRW:$HOST_FOLDER;WRITE"
export AROS_HOST_VOLUME
echo ">> Sharing $HOST_FOLDER as MacRO: (read-only) and MacRW: (read/write)."

# Sign with entitlements that allow DYLD_* env + loading the unsigned host shim.
[ -f "$ENT" ] && codesign -s - -f -o runtime --entitlements "$ENT" "$BOOTD/AROSBootstrap" 2>/dev/null \
              || codesign -s - -f "$BOOTD/AROSBootstrap" 2>/dev/null

echo ">> An 'AROS' window will open — click it for keyboard focus, then type."
cd "$BOOTD"
exec env AROS_DARWIN_THREADED=1 DYLD_FALLBACK_LIBRARY_PATH="$HOME/lib" \
    AROS_HOST_VOLUME="$AROS_HOST_VOLUME" \
    ./AROSBootstrap -c "$BOOTD/AROSBootstrap.conf"
