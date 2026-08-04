#!/bin/sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
A64=${AROS_BUILD:-$HOME/aros-build}
A64_SYS=$A64/bin/darwin-aarch64
A64_TOOLS=${AROS_CROSSTOOLS:-$HOME/aros-crosstools}
M68K=${AROS_M68K_BUILD:-$HOME/aros-m68k-build}
M68K_SYS=$M68K/bin/amiga-m68k
M68K_CC=${AROS_M68K_CC:-$M68K/bin/darwin-aarch64/tools/crosstools/m68k-aros-gcc}
RASPI=${AROS_RASPI_BUILD:-$HOME/aros-build-850}
RASPI_SYS=$RASPI/bin/raspi-aarch64
RASPI_HOST=$RASPI/bin/darwin-aarch64

[ -x "$A64_TOOLS/bin/clang" ] || {
    echo "FAIL: no AROS clang at $A64_TOOLS/bin/clang" >&2
    exit 1
}
[ -x "$M68K_CC" ] || {
    echo "FAIL: no AROS m68k compiler at $M68K_CC" >&2
    exit 1
}

COMPILER_PATH="$A64_SYS/tools:$A64_TOOLS/bin" \
"$A64_TOOLS/bin/clang" --target=aarch64-unknown-aros \
    -mcmodel=large -ffixed-x18 -O2 -Wall -Wextra -Wconversion \
    -Wsign-conversion -Werror -Wno-pointer-sign \
    -isystem "$A64_SYS/AROS/Developer/include" \
    -isystem "$A64_SYS/gen/include" \
    -isystem "$A64_SYS/gen/include/aros/posixc" \
    -isystem "$A64_SYS/gen/include/aros/stdc" \
    -nostartfiles -nodefaultlibs \
    -L"$A64_SYS/AROS/Developer/lib" -L"$A64_TOOLS/lib/generic" \
    "$A64_SYS/AROS/Developer/lib/startup.o" \
    "$HERE/exfat_hotplug_probe.c" -o "$HERE/EXFATHotplugProbe.aarch64" \
    -Wl,--allow-multiple-definition -Wl,--start-group \
    -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros -lautoinit \
    -llibinit -lutility -lamiga -larossupport -Wl,--end-group \
    -lclang_rt.builtins-aarch64

"$M68K_CC" -O2 -Wall -Wextra -Werror -Wno-pointer-sign \
    -Wno-volatile-register-var \
    -I"$M68K_SYS/gen/include" \
    -I"$M68K_SYS/gen/include/aros/posixc" \
    -I"$M68K_SYS/gen/include/aros/stdc" \
    -I"$M68K_SYS/AROS/Developer/include" \
    "$HERE/exfat_hotplug_probe.c" -o "$HERE/EXFATHotplugProbe.m68k" \
    -L"$M68K_SYS/AROS/Developer/lib" \
    -Wl,--start-group -lposixc -lstdc -lstdcio -ldos -lexec -laros \
    -lautoinit -llibinit -lutility -lamiga -larossupport -Wl,--end-group

if [ -d "$RASPI_SYS/AROS/Developer/include" ]; then
    COMPILER_PATH="$RASPI_HOST/tools:$A64_TOOLS/bin" \
    "$A64_TOOLS/bin/clang" --target=aarch64-unknown-aros \
        -mcmodel=large -ffixed-x18 -O2 -Wall -Wextra -Wconversion \
        -Wsign-conversion -Werror -Wno-pointer-sign \
        -isystem "$RASPI_SYS/AROS/Developer/include" \
        -isystem "$RASPI_SYS/gen/include" \
        -isystem "$RASPI_SYS/gen/include/aros/posixc" \
        -isystem "$RASPI_SYS/gen/include/aros/stdc" \
        -nostartfiles -nodefaultlibs \
        -L"$RASPI_SYS/AROS/Developer/lib" -L"$A64_TOOLS/lib/generic" \
        "$RASPI_SYS/AROS/Developer/lib/startup.o" \
        "$HERE/exfat_hotplug_probe.c" \
        -o "$HERE/EXFATHotplugProbe.raspi-aarch64" \
        -Wl,--allow-multiple-definition -Wl,--start-group \
        -lpthread -lposixc -lstdc -lstdcio -ldos -lexec -laros -lautoinit \
        -llibinit -lutility -lamiga -larossupport -Wl,--end-group \
        -lclang_rt.builtins-aarch64
fi

file "$HERE/EXFATHotplugProbe.aarch64" "$HERE/EXFATHotplugProbe.m68k"
if [ -f "$HERE/EXFATHotplugProbe.raspi-aarch64" ]; then
    file "$HERE/EXFATHotplugProbe.raspi-aarch64"
fi
echo "PASS: read-only physical-hotplug probe built for available AROS targets"
