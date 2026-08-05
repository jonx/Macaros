#!/bin/sh
# build-dhry-native.sh — the NATIVE arm64-darwin baseline for the 68k Dhrystone:
# the same freestanding Dhrystone 2.1 sources (fetched + patched by build-dhry.sh)
# compiled by clang for the host CPU instead of by vbcc for the 68k, so the JIT
# number and the native number measure the same program on the same machine.
# See docs/features/benchmarks/README.md.
#
#   ./build-dhry.sh          # first: fetch + patch the sources into src/
#   ./build-dhry-native.sh   # -> bin/dhry-native-<runs>, then the two-point rate
#
# Knobs: DHRY_LO / DHRY_HI (the two run counts), NATIVE_CFLAGS, REPS.
# NATIVE_CFLAGS defaults to "-O2 -fno-builtin": the 68k build has no libc SIMD
# string routines, so keeping clang off the builtins measures the same work.
# "-O3" (builtins on) is the upper bound a native Dhrystone would quote.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
LO="${DHRY_LO:-100000000}"
HI="${DHRY_HI:-400000000}"
CFLAGS="${NATIVE_CFLAGS:--O2 -fno-builtin}"
REPS="${REPS:-2}"

[ -f "$SRC/dhry_1.c" ] || { echo "!! $SRC missing — run ./build-dhry.sh first"; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/src" "$HERE/bin"
cp "$SRC/dhry_1.c" "$SRC/dhry_2.c" "$WORK/src/"

# The sources compile from a copy with an edited dhry.h (`#include "dhry.h"`
# resolves next to the .c file, so a shadow header on the include path would be
# ignored): the run count, plus real prototypes for the three glue functions.
# arm64-darwin passes variadic arguments on the stack, so a call through the
# benchmark's K&R "extern int dhry_printf();" would pass them in registers and
# print garbage.
sed -e "s/#define DHRY_RUNS 20000/#define DHRY_RUNS RUNS_HERE/" \
    -e 's/extern int  dhry_printf();/int dhry_printf(const char *fmt, ...);/' \
    -e 's/extern char \*strcpy();/char *strcpy(char *, const char *);/' \
    -e 's/extern int  strcmp();/int strcmp(const char *, const char *);/' \
    "$SRC/dhry.h" > "$WORK/dhry.h.in"

echo ">> compiling (clang $CFLAGS, -std=gnu89 for the K&R sources)"
for RUNS in "$LO" "$HI"; do
    sed "s/RUNS_HERE/$RUNS/" "$WORK/dhry.h.in" > "$WORK/src/dhry.h"
    clang -std=gnu89 $CFLAGS -Wno-everything -I"$WORK/src" \
        "$WORK/src/dhry_1.c" "$WORK/src/dhry_2.c" "$HERE/dhry_glue_native.c" \
        -o "$HERE/bin/dhry-native-$RUNS"
    echo ">> built $HERE/bin/dhry-native-$RUNS"
done

# Integrity + byte-exactness against the 68k reference output (the pointer
# values are the only implementation-dependent lines).
sed "s/RUNS_HERE/20000/" "$WORK/dhry.h.in" > "$WORK/src/dhry.h"
clang -std=gnu89 $CFLAGS -Wno-everything -I"$WORK/src" \
    "$WORK/src/dhry_1.c" "$WORK/src/dhry_2.c" "$HERE/dhry_glue_native.c" -o "$WORK/dhry20000"
"$WORK/dhry20000" > "$WORK/out.txt" || { echo "!! integrity check failed"; exit 1; }
grep -av 'Ptr_Comp:' "$WORK/out.txt" > "$WORK/a.txt"
grep -av 'Ptr_Comp:' "$HERE/dhry-reference.txt" > "$WORK/b.txt"
if cmp -s "$WORK/a.txt" "$WORK/b.txt"; then
    echo ">> output matches dhry-reference.txt (the 68k run), modulo pointer values"
else
    echo "!! output differs from the 68k reference"; exit 1
fi

echo ">> timing (best of $REPS at each run count, two-point)"
python3 - "$HERE/bin/dhry-native-$LO" "$LO" "$HERE/bin/dhry-native-$HI" "$HI" "$REPS" <<'EOF'
import subprocess, sys, time
lo_bin, lo, hi_bin, hi, reps = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
def best(cmd):
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter()
        subprocess.run([cmd], stdout=subprocess.DEVNULL, check=True)
        ts.append(time.perf_counter() - t0)
    return min(ts)
t1, t2 = best(lo_bin), best(hi_bin)
rate = (hi - lo) / (t2 - t1)
print(f"   N={lo} {t1:.3f}s | N={hi} {t2:.3f}s")
print(f"   -> {rate:,.0f} Dhrystones/sec ({rate/1757:,.0f} VAX MIPS)")
EOF
