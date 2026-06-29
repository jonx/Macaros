#!/bin/bash
# aros-cc.sh — the C compiler+linker entry point for cross-building C libraries
# (ffmpeg's libav*) for darwin-aarch64 AROS. Point ffmpeg's `./configure --cc=` at
# this; it also serves as the linker for configure's link probes and the final lib.
#
# Two pieces, which on this machine live in DIFFERENT places:
#   * the SDK tree  ($T = .../bin/darwin-aarch64): AROS/Developer/{include,lib} +
#     tools/collect-aros. Must be COMPLETE (see find_tree). The canonical one is
#     /tmp/arosbuild; stale claude-session scratchpad copies under /private/tmp get
#     garbage-collected and go half-empty, so we require posixc/stdio.h + libmui.a.
#   * the crosstools ($CT): the AROS-patched clang + ld.lld + clang_rt. Read from the
#     tree's config/make.cfg CROSSTOOLSDIR, else $AROS_CROSSTOOLS, else /tmp/aros-crosstools.
#
# The patched clang gives the AROS predefines (__AROS__) with --target=aarch64-unknown-aros,
# but its built-in spec bakes a (possibly stale) build dir for the includes/startfiles/
# libs. So we DO NOT rely on the auto-spec: we pass the include dirs explicitly with
# -isystem, and at link we suppress the spec startfiles/libs (-nostartfiles -nodefaultlibs)
# and supply the correct AROS startup + lib group from THIS tree. Grounded flags:
#   -mcmodel=large                  GOT-free MOVW_UABS_* relocs; AROS LoadSeg has no GOT.
#   -Wl,--allow-multiple-definition the AROS_LIBREQ duplicate marker (UPSTREAM-NOTES 18).
#   COMPILER_PATH=tools:crosstools  so collect-aros (the driver's linker) finds ld.lld.
set -eu

# --- discover a COMPLETE SDK tree (not merely newest clang) ------------------
find_tree() {
    if [ -n "${AROS_BUILD:-}" ]; then printf '%s\n' "$AROS_BUILD"; return; fi
    local best="" bt=0 d t
    for d in \
        "${BUILD:-/tmp/arosbuild}/bin/darwin-aarch64" \
        /tmp/*/bin/darwin-aarch64 \
        /private/tmp/claude-*/*/*/scratchpad/arosbuild/bin/darwin-aarch64 ; do
        [ -e "$d/AROS/Developer/include/aros/posixc/stdio.h" ] \
            && [ -e "$d/AROS/Developer/lib/libmui.a" ] \
            && [ -x "$d/tools/collect-aros" ] || continue
        t="$(stat -f %m "$d/AROS/Developer/lib/libmui.a" 2>/dev/null || echo 0)"
        if [ "$t" -ge "$bt" ]; then bt="$t"; best="$d"; fi
    done
    printf '%s\n' "$best"
}
T="$(find_tree)"
[ -n "$T" ] || { echo "aros-cc: no COMPLETE AROS SDK tree (need posixc/stdio.h + libmui.a + collect-aros; set AROS_BUILD)" >&2; exit 1; }

# --- locate the crosstools (patched clang + ld.lld + clang_rt) ---------------
BUILDROOT="$(cd "$T/../.." && pwd)"
CT="${AROS_CROSSTOOLS:-}"
[ -z "$CT" ] && CT="$(sed -n 's/^CROSSTOOLSDIR[[:space:]]*:=[[:space:]]*//p' "$BUILDROOT/config/make.cfg" 2>/dev/null | head -1)"
[ -n "$CT" ] && [ -x "$CT/bin/clang" ] || CT=/tmp/aros-crosstools
CLANG="$CT/bin/clang"
[ -x "$CLANG" ] || { echo "aros-cc: AROS clang not found ($CLANG); set AROS_CROSSTOOLS" >&2; exit 1; }

DEV="$T/AROS/Developer/include"
LIBDIR="$T/AROS/Developer/lib"
export COMPILER_PATH="$T/tools:$CT/bin"

COMMON=(--target=aarch64-unknown-aros -mcmodel=large)
INC=(-isystem "$DEV/aros/posixc" -isystem "$DEV/aros/stdc" -isystem "$DEV" -isystem "$T/gen/include")

# compile / preprocess / depgen only -> just the front end, no link machinery
case " $* " in
    *" -c "*|*" -S "*|*" -E "*|*" -M "*|*" -MM "*)
        exec "$CLANG" "${COMMON[@]}" "${INC[@]}" "$@" ;;
esac

# link: suppress the spec's stale startfiles/libs, supply this tree's (the Rust recipe).
AUTOLIB=(-lmui -lamiga -larossupport -lamiga -lcodesets -lkeymap -lexpansion
         -lcommodities -ldiskfont -lasl -lmuimaster -ldatatypes -lcybergraphics
         -lworkbench -licon -lintuition -lgadtools -llayers -laros -lpartition
         -liffparse -lgraphics -llocale -ldos -lutility -loop -llibinit -lautoinit)
STD=(-lposixc -lstdcio -lstdc -lexec)
exec "$CLANG" "${COMMON[@]}" "${INC[@]}" -nostartfiles -nodefaultlibs \
    -L"$LIBDIR" -L"$CT/lib/generic" \
    "$LIBDIR/startup.o" "$@" \
    -Wl,--allow-multiple-definition \
    -Wl,--start-group "${AUTOLIB[@]}" "${STD[@]}" -Wl,--end-group \
    -lclang_rt.builtins-aarch64
