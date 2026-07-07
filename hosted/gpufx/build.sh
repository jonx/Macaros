#!/bin/bash
# Build C:GpuFxTest -- the gpufx.library driver + GPU-vs-CPU verifier. Plain C
# (mirrors hosted/exwalk/build.sh); the GfxFx_* calls are inline register-call
# stubs from proto/gpufx.h, so no extra link lib is needed beyond the library
# being present at runtime.
#
#   hosted/gpufx/build.sh            # build + deploy C:GpuFxTest
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
OUT="$DIR/GpuFxTest"
export COMPILER_PATH="$T/tools:$CT/bin"

"$CLANG" --target=aarch64-unknown-aros -mcmodel=large -ffixed-x18 -O2 -Wall -Wno-pointer-sign \
    -isystem "$DEV" -isystem "$T/gen/include" \
    -isystem "$T/gen/include/aros/stdc" -isystem "$T/gen/include/aros/posixc" \
    -nostartfiles -nodefaultlibs \
    -L"$LIBDIR" -L"$CT/lib/generic" \
    "$LIBDIR/startup.o" "$DIR/gpufxtest.c" -o "$OUT" \
    -Wl,--allow-multiple-definition \
    -Wl,--start-group -ldos -lexec -laros -lautoinit -llibinit -lutility \
    -lamiga -larossupport -lstdcio -lstdc -Wl,--end-group \
    -lclang_rt.builtins-aarch64
"$CT/bin/llvm-strip" --strip-debug "$OUT" 2>/dev/null || true
echo "OK: $OUT ($(stat -f%z "$OUT") bytes)"

# Deploy into the boot image's C: dir.
CDIR="$T/AROS/C"
cp -f "$OUT" "$CDIR/GpuFxTest"; chmod +x "$CDIR/GpuFxTest"
echo "deployed -> $CDIR/GpuFxTest"
