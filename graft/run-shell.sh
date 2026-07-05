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
# The AROS build dir lives outside the repo (often under /private/tmp, ephemeral);
# this script discovers it, or you can set AROS_CTL_BOOTD.
set -e

# --- Relocatable paths -----------------------------------------------------
# Resolve this script's own dir (following symlinks) -> repo root, so the in-repo
# entitlements are found wherever the checkout lives. Each is overridable.
SELF="$0"
while [ -h "$SELF" ]; do
    dir="$(cd "$(dirname "$SELF")" && pwd)"
    SELF="$(readlink "$SELF")"; case "$SELF" in /*) ;; *) SELF="$dir/$SELF" ;; esac
done
HERE="$(cd "$(dirname "$SELF")" && pwd)"   # .../graft
ROOT="$(cd "$HERE/.." && pwd)"             # repo root

# Entitlements travel beside this script (shared AROS-host set). Override with
# AROS_CTL_ENT. (Terminal-only boot — no cocoametal dylib here.)
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
        /private/tmp/*/*/*/scratchpad/arosbuild/bin/darwin-aarch64/AROS/boot/darwin \
        /tmp/*/bin/darwin-aarch64/AROS/boot/darwin ; do
        [ -x "$d/AROSBootstrap" ] || continue
        t="$(stat -f %m "$d/AROSBootstrap" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}
BOOTD="$(find_bootd)"

if [ -z "$BOOTD" ] || [ ! -x "$BOOTD/AROSBootstrap" ]; then
    echo "run-shell.sh: no AROSBootstrap found." >&2
    echo "  point AROS_CTL_BOOTD at <build>/bin/darwin-aarch64/AROS/boot/darwin," >&2
    echo "  or build one with graft/build-darwin-aarch64.sh (BUILD defaults to /tmp/arosbuild)." >&2
    exit 1
fi
echo ">> boot dir: $BOOTD" >&2

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
