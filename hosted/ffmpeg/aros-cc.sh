#!/bin/bash
# aros-cc.sh — the C compiler+linker entry point for cross-building C libraries
# (ffmpeg's libav*) for darwin-aarch64 AROS. Point ffmpeg's `./configure --cc=` at
# this; it also serves as the linker for configure's link probes and the final lib.
#
# It is the AROS-patched clang DRIVER (tools/crosstools/bin/clang, target
# aarch64-unknown-aros) plus the three AROS-specific flags the live FF0 probe proved
# necessary (see NOTES.md "[FF0] Gate 1" and docs/features/ffmpeg-native/README.md):
#
#   -mcmodel=large                  GOT-free MOVW_UABS_* relocs; AROS LoadSeg has no
#                                   GOT, so small/PIC code won't load. (Same as Rust.)
#   -Wl,--allow-multiple-definition the AROS_LIBREQ duplicate marker, UPSTREAM-NOTES
#                                   item 18 — at LINK only (warns/unused on -c).
#   COMPILER_PATH=tools:crosstools  so the driver finds collect-aros and collect-aros
#                                   finds ld.
#
# The driver supplies the correct AROS posixc/stdc includes itself, so libc headers
# (<stdio.h>/<stdlib.h>/<math.h>) resolve cleanly — no manual -isystem, and none of
# the host-SDK `cc_include` pollution that the Rust glue had to dodge.
#
# Tree discovery mirrors graft/deploy-check; set $AROS_BUILD=.../bin/darwin-aarch64
# to pin it (build.sh does this so configure doesn't re-glob on every probe).
set -eu

find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    best="" ; bt=0
    for d in \
        "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64" \
        /private/tmp/claude-*/*/*/scratchpad/arosbuild/bin/darwin-aarch64 \
        /tmp/*/bin/darwin-aarch64 ; do
        [ -x "$d/tools/crosstools/bin/clang" ] && [ -x "$d/tools/collect-aros" ] || continue
        t="$(stat -f %m "$d/tools/collect-aros" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}

T="$(find_tree)"
[ -n "$T" ] && [ -x "$T/tools/crosstools/bin/clang" ] || {
    echo "aros-cc: no AROS build tree found (set AROS_BUILD=.../bin/darwin-aarch64)" >&2
    exit 1; }

export COMPILER_PATH="$T/tools:$T/tools/crosstools/bin"
ACLANG="$T/tools/crosstools/bin/clang"

# Add the link-only multidef fix unless this invocation is compile/preprocess/
# depgen-only (where a -Wl flag is unused). One token, no spaces -> safe unquoted.
MULTI=""
case " $* " in
    *" -c "*|*" -S "*|*" -E "*|*" -M "*|*" -MM "*) ;;
    *) MULTI="-Wl,--allow-multiple-definition" ;;
esac

exec "$ACLANG" --target=aarch64-unknown-aros -mcmodel=large $MULTI "$@"
