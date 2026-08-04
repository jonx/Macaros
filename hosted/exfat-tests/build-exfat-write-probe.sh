#!/bin/sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${AROS_BUILD:-$HOME/aros-build}
[ -d "$BUILD/AROS/Developer/include" ] || BUILD="$BUILD/bin/darwin-aarch64"
TOOLS=${AROS_CROSSTOOLS:-$HOME/aros-crosstools}
CLANG="$TOOLS/bin/clang"
DEV="$BUILD/AROS/Developer"

[ -x "$CLANG" ] || { echo "no AROS clang at $CLANG" >&2; exit 1; }
[ -d "$DEV/include" ] || { echo "no AROS SDK at $DEV" >&2; exit 1; }

export COMPILER_PATH="$BUILD/tools:$TOOLS/bin"
"$CLANG" --target=aarch64-unknown-aros -mcmodel=large -ffixed-x18 -O2 \
    -Wall -Wextra -Wconversion -Wsign-conversion -Werror -Wno-pointer-sign \
    -isystem "$DEV/include" -isystem "$BUILD/gen/include" \
    -isystem "$BUILD/gen/include/aros/posixc" \
    -isystem "$BUILD/gen/include/aros/stdc" \
    -nostartfiles -nodefaultlibs -L"$DEV/lib" -L"$TOOLS/lib/generic" \
    "$DEV/lib/startup.o" "$HERE/exfat_write_probe.c" \
    -o "$HERE/EXFATWriteProbe" \
    -Wl,--allow-multiple-definition -Wl,--start-group \
    -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros -lautoinit \
    -llibinit -lutility -lamiga -larossupport -Wl,--end-group \
    -lclang_rt.builtins-aarch64

echo "OK: $HERE/EXFATWriteProbe"
