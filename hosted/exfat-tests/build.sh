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
CFLAGS="-O2 -std=c99 -Wall -Wextra -Wconversion -Wsign-conversion -Werror -I${AROS_UPSTREAM:-$HERE/../../../aros-upstream}/compiler/include"

# Plain build, then again under the sanitisers. The bounds code is all
# integer arithmetic, so UBSan is the one that matters.
fail=0
for t in t13a_bounds t_boot t_meta t_time; do
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

echo "== gate 1: m68k headers and code generation =="
echo "   (headers only: cache.c/disk.c/exfat_fs.h need an AROS m68k toolchain)"
if command -v m68k-elf-gcc >/dev/null 2>&1; then
    for cpu in 68000 68020 68040; do
        printf "  -m%-8s " "$cpu"
        m68k-elf-gcc -m$cpu -c -O2 -std=c99 \
            -Wall -Wextra -Wconversion -Wsign-conversion -Werror \
            -I"$SRC" "$HERE/m68k_headers_check.c" -o "$HERE/m68k_$cpu.o" \
            && echo "ok" || fail=1
    done

    # A2 is a claim about generated code, so inspect generated code.
    #
    # Every memory read through the caller's byte pointer must be a byte
    # load. A word or long access through an address register would be an
    # address error on a 68000 for an odd offset, and depends on the buffer's
    # base alignment even for an even one. %sp is exempt: that is argument
    # loading, not buffer access.
    #
    # The probes are noinline so their disassembly can be isolated by name.
    VIOLATION='[a-z]+[wl][[:space:]].*%a[0-6]@'
    probe_disasm() {
        m68k-elf-objdump -d "$1" \
            | awk '/<exfat_probe_(rd|wr)(16|32|64)>:/{p=1} /^$/{p=0} p'
    }

    # Self-test: the gate must catch a cast. If this stops failing, the
    # assertion below has stopped meaning anything.
    printf "  %-11s " "gate self-test"
    cat > "$HERE/.badprobe.c" <<'BAD'
typedef unsigned short UWORD;
typedef unsigned char  UBYTE;
__attribute__((noinline)) UWORD exfat_probe_rd16(const UBYTE *p, unsigned o)
{ return *(const UWORD *)(p + o); }
BAD
    m68k-elf-gcc -m68000 -c -O2 -o "$HERE/.badprobe.o" "$HERE/.badprobe.c"
    if probe_disasm "$HERE/.badprobe.o" | grep -qE "$VIOLATION"; then
        echo "ok, a cast is caught"
    else
        echo "FAIL: the gate no longer detects a cast"
        fail=1
    fi
    rm -f "$HERE/.badprobe.c" "$HERE/.badprobe.o"

    printf "  %-11s " "byte I/O"
    if probe_disasm "$HERE/m68k_68000.o" | grep -qE "$VIOLATION"; then
        echo "FAIL: word/long access through the buffer pointer"
        probe_disasm "$HERE/m68k_68000.o" | grep -E "$VIOLATION"
        fail=1
    else
        echo "ok, byte loads/stores only"
    fi
else
    echo "  SKIPPED: no m68k-elf-gcc"
fi
echo

echo "== gate 1: complete m68k AROS source compile =="
M68K_AROS_ROOT=${AROS_M68K_BUILD:-$HOME/aros-m68k-build}
M68K_AROS_CC=${AROS_M68K_CC:-$M68K_AROS_ROOT/bin/darwin-aarch64/tools/crosstools/m68k-aros-gcc}
M68K_AROS_OBJDUMP=${AROS_M68K_OBJDUMP:-$M68K_AROS_ROOT/bin/darwin-aarch64/tools/crosstools/m68k-aros-objdump}
M68K_AROS_SYS=$M68K_AROS_ROOT/bin/amiga-m68k
if [ -x "$M68K_AROS_CC" ] && [ -x "$M68K_AROS_OBJDUMP" ] \
    && [ -d "$M68K_AROS_SYS/gen/include" ]; then
    for src in "$SRC"/*.c; do
        base=${src##*/}
        obj="$HERE/${base%.c}.m68k-aros.o"
        printf "  %-14s " "$base"
        if "$M68K_AROS_CC" -c -O2 -Wall -Wextra -Werror \
            -Wno-pointer-sign -Wno-volatile-register-var \
            -I"$SRC" -I"$M68K_AROS_SYS/gen/include" \
            -I"$M68K_AROS_SYS/gen/include/aros/stdc" \
            -I"$M68K_AROS_SYS/gen/include/aros/posixc" \
            -I"$M68K_AROS_SYS/AROS/Developer/include" \
            "$src" -o "$obj" \
            && "$M68K_AROS_OBJDUMP" -f "$obj" \
                | grep -q 'architecture: m68k'; then
            echo "ok, m68k AROS"
        else
            echo "FAIL"
            fail=1
        fi
    done
else
    echo "  SKIPPED: set AROS_M68K_BUILD or AROS_M68K_CC for an AROS toolchain"
fi
echo

exit $fail
