#!/bin/sh
#
# run-shell.sh — boot hosted darwin-aarch64 AROS straight to the interactive
# AmigaDOS Shell, in your real terminal. Type commands at the "1> " prompt.
#
#   Dir            list the current directory (works)
#   <Ctrl-D>       end-of-file -> Shell prints "Process N ending" and AROS quits
#   <Ctrl-C>       also interrupts
#
# Most C: commands are not built yet, so they report "<cmd>: object not found".
# The build dir lives under /private/tmp and is ephemeral (cleared on reboot);
# if it's gone, rebuild and re-point BOOTD below.
set -e

BOOTD="/private/tmp/claude-501/-Users-user-Source-aros-aarch64/a7d73cfa-b608-4959-b0ec-f7720da95bb3/scratchpad/arosbuild/bin/darwin-aarch64/AROS/boot/darwin"
ENT="/private/tmp/claude-501/-Users-user-Source-aros-aarch64/a7d73cfa-b608-4959-b0ec-f7720da95bb3/scratchpad/aros-entitlements.plist"

if [ ! -x "$BOOTD/AROSBootstrap" ]; then
    echo "AROSBootstrap not found at:  $BOOTD" >&2
    echo "(the /private/tmp build dir was probably cleared — rebuild it)" >&2
    exit 1
fi

# Clean conf (no boot narration so the prompt is front and centre).
grep -v '^arguments ' "$BOOTD/AROSBootstrap.conf" > "$BOOTD/AROSBootstrap.conf.tmp" 2>/dev/null \
    && mv "$BOOTD/AROSBootstrap.conf.tmp" "$BOOTD/AROSBootstrap.conf"

# Ensure the full standard module set is in the kickstart (the build generates a
# stripped conf). Idempotent: only appends modules not already listed. This is what
# makes the whole library stack (graphics/intuition/layers/keymap/gadtools + the
# gfx & input HIDDs + console/input devices + con-handler) load, and crucially the
# resident shell.resource — without it SystemTags(NULL) has no "shell"/"CLI"
# segment, so the Initial CLI never starts and the boot hangs at "initialising CLI".
AROS="$(cd "$BOOTD/../.." && pwd)"   # .../darwin-aarch64/AROS
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
[ -f "$ENT" ] && codesign -s - -f -o runtime --entitlements "$ENT" "$BOOTD/AROSBootstrap" 2>/dev/null \
              || codesign -s - -f "$BOOTD/AROSBootstrap" 2>/dev/null

cd "$BOOTD"
exec ./AROSBootstrap -c "$BOOTD/AROSBootstrap.conf"
