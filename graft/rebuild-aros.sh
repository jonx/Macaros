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

# A git branch switch in $AROS_SRC touches configure's mtime without changing
# it; the top Makefile then rejects every make with "The configure script must
# be executed". Refresh the stored configuration instead of re-running
# configure (which risks the LLVM rebuild). If configure CONTENT actually
# changed, a real reconfigure is still on you.
if [ "$AROS_SRC/configure" -nt "$AROS_BUILD/config.status" ]; then
    echo "[rebuild] configure mtime newer than config.status (branch switch?) -- refreshing config.status"
    (cd "$AROS_BUILD" && ./config.status >/dev/null && touch config.status)
fi

# The boot module set (kickstart) -- section 3 of docs/features/build/README.md.
BOOT_TARGETS="kernel-dos kernel-kernel kernel-dosboot kernel-utility kernel-aros
kernel-oop kernel-intuition kernel-graphics kernel-layers kernel-keymap
kernel-debug kernel-bootloader kernel-console kernel-input kernel-keyboard
kernel-gameport workbench-devs-clipboard kernel-hidd kernel-hidd-gfx kernel-hidd-input
kernel-hidd-kbd kernel-hidd-mouse kernel-filesystem kernel-lddemon kernel-fs-con
kernel-fs-ram kernel-expansion kernel-timer kernel-battclock kernel-processor
kernel-hostlib kernel-unixio kernel-fs-emul kernel-entropy compiler-stdcio
workbench-libs-muimaster
workbench-libs-cgfx workbench-libs-datatypes workbench-libs-gadtools
workbench-libs-iffparse workbench-libs-locale workbench-libs-asl
workbench-libs-commodities workbench-libs-coolimages workbench-libs-rexxsyslib
compiler-stdc kernel-hidd-cocoa kernel-bootstrap-hosted"

# The desktop userland -- section 3b. Without these the boot reaches only a CLI
# or dies in the display-driver / dos.library check.
DESKTOP_TARGETS="workbench-c workbench-libs-icon workbench-libs-kms
workbench-libs-lowlevel workbench-libs-diskfont workbench-libs-reaction
workbench-libs-workbench workbench-libs-version workbench-libs-mathffp
workbench-libs-mathieeesingbas workbench-libs-mathieeedoubbas
workbench-libs-mathieeedoubtrans workbench-system-wanderer
workbench-classes-zune workbench-datatypes-picture workbench-datatypes-png
workbench-utilities-clock workbench-utilities-multiview workbench-utilities-more"

# The Workbench apps behind the staged icons (Tools/, System/, Prefs/). The
# icon set is staged wholesale, so every app missing from this list shows up
# as a dead icon on the desktop. NOT here on purpose:
#   - workbench-system (aggregate): broken, workbench-system-vmm-app fails to
#     compile (missing generated locale strings) and VMM is pointless hosted;
#     the aggregate's own FILES (FixFonts + CLI) are built by
#     build_workbench_system_base below instead.
#   - SysExplorer used to trap at launch (the 64-bit taglist bug class,
#     bare-int tag values spilled to stack vararg slots -- see build README
#     3b); fixed 2026-07-16 in the NList sources (upstream 471cb63f).
APP_TARGETS="workbench-tools workbench-tools-ahirecord workbench-tools-hdtoolbox
workbench-tools-installaros workbench-tools-sysexplorer-app
workbench-utilities-help workbench-utilities-installer
workbench-devs-diskimage-gui workbench-prefs-boot
external-openurl-includes external-openurl-fd external-openurl-lib
external-openurl-cmd external-openurl-prefs kernel-usb-trident"

# System/FixFonts + System/CLI (the Shell icon's default tool) have no
# standalone metatarget -- their mmake name IS the broken workbench-system
# aggregate -- so invoke their directory's generated mmakefile directly.
build_workbench_system_base() {
    printf '[rebuild] %-40s ' "workbench-system (base files)"
    if make --no-print-directory -C "$AROS_BUILD/workbench/system" \
        TOP="$AROS_BUILD" SRCDIR="$AROS_SRC" CURDIR=workbench/system \
        TARGET=workbench-system AROS_TARGET_ARCH=darwin AROS_TARGET_CPU=aarch64 \
        AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 \
        --file=mmakefile workbench-system > "$LOGDIR/workbench-system-base.log" 2>&1; then
        echo "OK"; ok=$((ok+1))
    else
        echo "FAIL (see $LOGDIR/workbench-system-base.log)"; fail=$((fail+1)); failed="$failed workbench-system-base"
    fi
}

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

FULL_SET=""
[ -z "${TARGETS:-}" ] && FULL_SET=1
TARGETS="${TARGETS:-$BOOT_TARGETS $DESKTOP_TARGETS $APP_TARGETS $AUDIO_TARGETS $ASSEMBLE_TARGETS}"

cd "$AROS_BUILD"
ok=0; fail=0; failed=""
for t in $TARGETS; do
    printf '[rebuild] %-40s ' "$t"
    if make "$t" > "$LOGDIR/$t.log" 2>&1; then
        # mmake exits 0 on an UNKNOWN target ("Nothing known about target X"),
        # silently no-oping -- this masked stale-source deploys for hours
        # (fixes appeared built but the old library kept shipping). Treat it
        # as the failure it is.
        if grep -q "Nothing known about target" "$LOGDIR/$t.log"; then
            echo "FAIL (unknown target -- see $LOGDIR/$t.log)"; fail=$((fail+1)); failed="$failed $t"
        else
            echo "OK"; ok=$((ok+1))
        fi
    else
        echo "FAIL (see $LOGDIR/$t.log)"; fail=$((fail+1)); failed="$failed $t"
    fi
done

[ -n "$FULL_SET" ] && build_workbench_system_base

# AROS.boot signature: dos's __dos_IsBootable needs :AROS.boot at the boot
# volume root holding the CPU string, else every boot mis-reports "can't open
# dos.library" (see docs/features/build + run-window.sh).
BOOTV="$AROS_BUILD/bin/darwin-aarch64/AROS"
[ -d "$BOOTV" ] && { echo aarch64 > "$BOOTV/AROS.boot"; echo "[rebuild] wrote $BOOTV/AROS.boot (aarch64)"; }

echo ""
echo "[rebuild] done: $ok ok, $fail failed.${failed:+ FAILED:$failed}"
[ "$fail" -eq 0 ]
