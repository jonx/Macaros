#!/bin/bash
# rebuild-aros.sh -- rebuild the hosted-AROS boot + desktop module set into the
# STABLE build tree, reusing the preserved crosstools. This is the recovery /
# from-scratch-artifacts tool: after the macOS /tmp cleaner eats a build tree,
# or to populate a fresh one, run this. It never rebuilds the LLVM crosstools
# (the expensive part is preserved at $AROS_CROSSTOOLS).
#
# Stable locations (canonical, out of /tmp so the periodic cleaner can't eat them):
#   AROS_BUILD        ~/aros-build          the AROS build tree / SDK / boot image
#   AROS_CROSSTOOLS   ~/aros-crosstools     the patched clang/lld + compiler-rt
#   AROS_SRC          ~/Source/aros-upstream (branch: crash-containment)
#
#   graft/rebuild-aros.sh                 # build the whole boot+desktop set
#   TARGETS="kernel-dos kernel-intuition" graft/rebuild-aros.sh   # subset
#
# Each metatarget is built in its own `make` call (one per call avoids the mmake
# stale-DB "Nothing known about project X" failure). Failures are logged and the
# run continues; a summary prints at the end. NEVER a bare `make` (that fetches
# and builds LLVM from source).
set -uo pipefail

AROS_SRC="${AROS_SRC:-$HOME/Source/aros-upstream}"
AROS_BUILD="${AROS_BUILD:-$HOME/aros-build}"
AROS_CROSSTOOLS="${AROS_CROSSTOOLS:-$HOME/aros-crosstools}"
LOGDIR="${LOGDIR:-$AROS_BUILD/rebuild-logs}"
mkdir -p "$LOGDIR"

# objcopy: macOS has none; the crosstools ship llvm-objcopy. Ensure a plain
# `objcopy` name exists on PATH (symlinked next to llvm-objcopy, a stable dir).
[ -e "$AROS_CROSSTOOLS/bin/objcopy" ] || \
    ln -sf "$AROS_CROSSTOOLS/bin/llvm-objcopy" "$AROS_CROSSTOOLS/bin/objcopy"
export PATH="$AROS_CROSSTOOLS/bin:/opt/homebrew/bin:$PATH"

[ -x "$AROS_CROSSTOOLS/bin/clang" ] || { echo "FATAL: no crosstools at $AROS_CROSSTOOLS" >&2; exit 2; }
[ -f "$AROS_BUILD/Makefile" ] || { echo "FATAL: $AROS_BUILD is not a configured build tree (no Makefile). Configure it first:" >&2
    echo "  cd $AROS_BUILD && $AROS_SRC/configure --target=darwin-aarch64 --with-toolchain=llvm --with-aros-toolchain=yes --with-aros-toolchain-install=$AROS_CROSSTOOLS --without-x" >&2
    exit 3; }

# The boot module set (kickstart) -- section 3 of docs/features/build/README.md.
BOOT_TARGETS="kernel-dos kernel-kernel kernel-dosboot kernel-utility kernel-aros
kernel-oop kernel-intuition kernel-graphics kernel-layers kernel-keymap
kernel-debug kernel-bootloader kernel-console kernel-input kernel-keyboard
kernel-gameport kernel-clipboard kernel-hidd kernel-hidd-gfx kernel-hidd-input
kernel-hidd-kbd kernel-hidd-mouse kernel-filesystem kernel-lddemon kernel-fs-con
kernel-fs-ram kernel-expansion kernel-timer kernel-battclock kernel-processor
kernel-hostlib kernel-unixio kernel-fs-emul kernel-entropy compiler-stdcio
workbench-libs-muimaster
workbench-libs-cgfx workbench-libs-datatypes workbench-libs-gadtools
workbench-libs-iffparse workbench-libs-locale workbench-libs-asl
workbench-libs-commodities workbench-libs-coolimages workbench-libs-rexxsyslib
workbench-libs-stdc kernel-hidd-cocoa kernel-bootstrap-hosted"

# The desktop userland -- section 3b. Without these the boot reaches only a CLI
# or dies in the display-driver / dos.library check.
DESKTOP_TARGETS="workbench-c workbench-libs-icon workbench-libs-kms
workbench-libs-lowlevel workbench-libs-diskfont workbench-libs-reaction
workbench-libs-workbench workbench-libs-version workbench-libs-mathffp
workbench-libs-mathieeesingbas workbench-libs-mathieeedoubbas
workbench-libs-mathieeedoubtrans workbench-system-wanderer
workbench-classes-zune workbench-datatypes-picture workbench-datatypes-png
workbench-utilities-clock workbench-utilities-multiview workbench-utilities-more"

# AHI audio (CoreAudio host path). AHI is an autotools subsystem, so the order
# matters: the coreaudio-bridge linklib must exist in Developer/lib BEFORE the
# AHI subsystem configures, or configure's `-lcoreaudio-bridge` probe fails and
# the CoreAudio driver is silently dropped (the `-quick` targets skip the
# prereq that would otherwise build it). Build the bridge first, then the
# subsystem+drivers (which install ahi.device, coreaudio.audio, the COREAUDIO
# mode file and AddAudioModes), then the AHISmoke test client.
# See docs/features/coreaudio-audio/README.md.
AUDIO_TARGETS="AHI-coreaudio-bridge-darwin workbench-devs-AHI-quick
workbench-c-ahismoke"

# Final assembly: link the hosted kickstart core (the `boot/darwin/kernel` file
# AROSBootstrap loads first — darwin hosted uses the unix bootstrap flavor), then
# `boot` stages the boot image + AROS.boot. Without kernel-link-unix the boot
# dies immediately with "Failed to open file kernel!".
ASSEMBLE_TARGETS="kernel-link-unix boot"

TARGETS="${TARGETS:-$BOOT_TARGETS $DESKTOP_TARGETS $AUDIO_TARGETS $ASSEMBLE_TARGETS}"

cd "$AROS_BUILD"
ok=0; fail=0; failed=""
for t in $TARGETS; do
    printf '[rebuild] %-40s ' "$t"
    if make "$t" > "$LOGDIR/$t.log" 2>&1; then
        echo "OK"; ok=$((ok+1))
    else
        echo "FAIL (see $LOGDIR/$t.log)"; fail=$((fail+1)); failed="$failed $t"
    fi
done

# AROS.boot signature: dos's __dos_IsBootable needs :AROS.boot at the boot
# volume root holding the CPU string, else every boot mis-reports "can't open
# dos.library" (see docs/features/build + run-window.sh).
BOOTV="$AROS_BUILD/bin/darwin-aarch64/AROS"
[ -d "$BOOTV" ] && { echo aarch64 > "$BOOTV/AROS.boot"; echo "[rebuild] wrote $BOOTV/AROS.boot (aarch64)"; }

echo ""
echo "[rebuild] done: $ok ok, $fail failed.${failed:+ FAILED:$failed}"
[ "$fail" -eq 0 ]
