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
$CC $CFLAGS -I"$SRC" -o "$HERE/t13a" "$HERE/t13a_bounds.c"
$CC $CFLAGS -I"$SRC" -fsanitize=undefined,address \
    -o "$HERE/t13a-san" "$HERE/t13a_bounds.c"

echo "== t13a =="
"$HERE/t13a"
echo
echo "== t13a, ASan + UBSan =="
UBSAN_OPTIONS=halt_on_error=1 "$HERE/t13a-san"
