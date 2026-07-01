#!/bin/bash
# build.sh -- compile-check the HostBind sample against the AROS SDK. It proves the
# <aros/hostbind.h> pattern compiles clean with the crosstools; it is a template, not
# a deployable module (the shape-(b) dylib is illustrative).
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
T="${AROS_BUILD:-/tmp/arosbuild/bin/darwin-aarch64}"
CC="${AROS_CC:-clang}"

[ -f "$T/gen/include/aros/hostbind.h" ] || {
    echo "note: <aros/hostbind.h> not staged in $T/gen/include/aros -- build compiler-posixc (or 'make includes') first" >&2
}

"$CC" --target=aarch64-unknown-none-elf -mcmodel=large -ffixed-x18 -D__arm64__ -O2 \
    -Wall -Wno-pointer-sign \
    -I"$T/gen/include" -I"$T/AROS/Developer/include" \
    -c "$DIR/hostbind_sample.c" -o /tmp/hostbind_sample.o

echo "OK: hostbind_sample.c compiles clean against the SDK"
