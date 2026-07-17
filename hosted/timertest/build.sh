#!/bin/bash
# build.sh -- cross-build C:TimerTest (the concurrent ExNext/ExAll emul-handler
# stress test) against the AROS SDK, using the same grounded flags as
# hosted/ffmpeg/aros-cc.sh: -mcmodel=large (no GOT in LoadSeg), -ffixed-x18
# (Darwin platform register), spec startfiles/libs suppressed and this tree's
# startup.o + lib group supplied explicitly.
#
#   AROS_BUILD       SDK tree root or its bin/darwin-aarch64 dir (default ~/aros-build)
#   AROS_CROSSTOOLS  patched clang/lld (default ~/aros-crosstools)
#
# Output: hosted/exwalk/TimerTest (copy into the boot image's C: to deploy).
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"

T="${AROS_BUILD:-$HOME/aros-build}"
[ -d "$T/AROS/Developer/include" ] || T="$T/bin/darwin-aarch64"
[ -d "$T/AROS/Developer/include" ] || {
    echo "build.sh: no AROS SDK at \$AROS_BUILD (need AROS/Developer/include)" >&2
    exit 1
}

CT="${AROS_CROSSTOOLS:-$HOME/aros-crosstools}"
CLANG="$CT/bin/clang"
[ -x "$CLANG" ] || { echo "build.sh: AROS clang not found at $CLANG" >&2; exit 1; }

DEV="$T/AROS/Developer/include"
LIBDIR="$T/AROS/Developer/lib"
OUT="${1:-$DIR/TimerTest}"
export COMPILER_PATH="$T/tools:$CT/bin"

"$CLANG" --target=aarch64-unknown-aros -mcmodel=large -ffixed-x18 -O2 -Wall -Wno-pointer-sign \
    -D_GNU_SOURCE \
    -isystem "$DEV" -isystem "$T/gen/include" \
    -isystem "$T/gen/include/aros/posixc" -isystem "$T/gen/include/aros/stdc" \
    -nostartfiles -nodefaultlibs \
    -L"$LIBDIR" -L"$CT/lib/generic" \
    "$LIBDIR/startup.o" "$DIR/timertest.c" -o "$OUT" \
    -Wl,--allow-multiple-definition \
    -Wl,--start-group -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros -lautoinit -llibinit -lutility \
    -lamiga -larossupport -Wl,--end-group \
    -lclang_rt.builtins-aarch64

echo "OK: $OUT"
