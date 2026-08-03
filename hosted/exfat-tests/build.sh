#!/bin/sh
# Build and run the exFAT host tests.
#
# These compile the production headers from ../../../aros-upstream/rom/filesys/exfat
# so the tests exercise the real rules, not a copy of them.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${AROS_UPSTREAM:-$HERE/../../../aros-upstream}/rom/filesys/exfat

if [ ! -f "$SRC/exfat_bounds.h" ]; then
    echo "cannot find $SRC/exfat_bounds.h" >&2
    echo "set AROS_UPSTREAM to the AROS source checkout" >&2
    exit 1
fi

CC=${CC:-cc}
CFLAGS="-O2 -std=c99 -Wall -Wextra -Wconversion -Wsign-conversion -Werror"

# Plain build, then again under the sanitisers. The bounds code is all
# integer arithmetic, so UBSan is the one that matters.
fail=0
for t in t13a_bounds t_boot; do
    name=${t%_bounds}
    $CC $CFLAGS -I"$SRC" -o "$HERE/$name" "$HERE/$t.c"
    $CC $CFLAGS -I"$SRC" -fsanitize=undefined,address \
        -o "$HERE/$name-san" "$HERE/$t.c"

    echo "== $name =="
    "$HERE/$name" || fail=1
    echo "== $name, ASan + UBSan =="
    UBSAN_OPTIONS=halt_on_error=1 "$HERE/$name-san" >/dev/null || fail=1
    echo
done

# ---------------------------------------------------------------- gate 1
# Cross-compile the production headers for the real targets. These are
# skipped rather than failed when a toolchain is absent, so the host suite
# still runs on a bare machine, but a skip is reported loudly.

XT=${AROS_CROSSTOOLS:-$HOME/aros-crosstools}
BUILD=${AROS_BUILD:-$HOME/aros-build}/bin/darwin-aarch64

echo "== gate 1: aarch64 AROS =="
if [ -x "$XT/bin/clang" ] && [ -d "$BUILD/gen/include" ]; then
    for f in cache.c disk.c; do
        printf "  %-10s " "$f"
        "$XT/bin/clang" -target aarch64-unknown-aros -c -O2 \
            -Wall -Wextra -Wno-pointer-sign \
            -I"$SRC" -I"$BUILD/gen/include" \
            -I"$BUILD/gen/include/aros/stdc" \
            -I"$BUILD/gen/include/aros/posixc" \
            -I"$BUILD/AROS/Developer/include" \
            "$SRC/$f" -o "$HERE/$f.aarch64.o" && echo "ok" || fail=1
    done
else
    echo "  SKIPPED: no crosstools at $XT or no build at $BUILD"
fi
echo

echo "== gate 1: m68k, 32-bit big-endian =="
if command -v m68k-elf-gcc >/dev/null 2>&1; then
    for cpu in 68000 68020 68040; do
        printf "  -m%-8s " "$cpu"
        m68k-elf-gcc -m$cpu -c -O2 -std=c99 \
            -Wall -Wextra -Wconversion -Wsign-conversion -Werror \
            -I"$SRC" "$HERE/m68k_check.c" -o "$HERE/m68k_$cpu.o" \
            && echo "ok" || fail=1
    done

    # A2 is a claim about generated code, so check the generated code: the
    # byte-wise accessors must emit byte loads at an odd offset, never a
    # word read that a 68000 would fault on.
    printf "  %-11s " "byte loads"
    if m68k-elf-objdump -d "$HERE/m68k_68000.o" 2>/dev/null \
        | grep -qE '\bmove[wl]\b[^,]*@\((1|3|5|7|9)\)'; then
        echo "FAIL: odd-address word access emitted"
        fail=1
    else
        echo "ok, no odd-address word access"
    fi
else
    echo "  SKIPPED: no m68k-elf-gcc"
fi
echo

exit $fail
