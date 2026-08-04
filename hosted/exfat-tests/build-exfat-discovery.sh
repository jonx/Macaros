#!/bin/sh
# Isolated build gate for both exFAT USB automounters.
#
# Exact output-file targets are intentional: invoking the public MetaMake
# targets can expand into a complete AROS rebuild and overwrite unrelated
# generated work.  This gate compiles and links only the two USB modules.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${AROS_UPSTREAM:-$HERE/../../../aros-upstream}
A64=${AROS_BUILD:-$HOME/aros-build}
M68K=${AROS_M68K_BUILD:-$HOME/aros-m68k-build}

# A directly buildable module is insufficient: normal hosted/native images
# must actually request it wherever they already request the FAT handler.
for graph in \
    "$SRC/arch/all-hosted/mmakefile" \
    "$SRC/arch/all-native/mmakefile.src" \
    "$SRC/arch/arm-raspi/boot/mmakefile.src" \
    "$SRC/arch/ppc-sam440/boot/mmakefile.src" \
    "$SRC/arch/riscv-native/boot/mmakefile.src"
do
    grep -q 'kernel-fs-exfat' "$graph" || {
        echo "FAIL: normal build graph omits exFAT: $graph" >&2
        exit 1
    }
done
if [ -f "$SRC/arch/aarch64-raspi/boot/mmakefile.src" ]; then
    grep -q 'kernel-fs-exfat' \
        "$SRC/arch/aarch64-raspi/boot/mmakefile.src" || {
        echo "FAIL: native AArch64 Pi package omits exFAT" >&2
        exit 1
    }
fi
grep -q 'FS_XTRA_HANDLERS.*exfat' "$SRC/arch/all-native/mmakefile.src" || {
    echo "FAIL: native filesystem package omits exfat-handler" >&2
    exit 1
}
grep -q '^Resident=exfat-handler$' "$SRC/rom/filesys/exfat/exfat.conf" || {
    echo "FAIL: shared automounter name disagrees with exfat.conf" >&2
    exit 1
}

build_modules()
{
    root=$1
    arch=$2
    cpu=$3
    sys=$4
    rom_dir=$root/rom/usb/classes/massstorage
    hidd_dir=$root/workbench/devs/USB/classes/MassStorage

    [ -f "$root/compiler/include/mmakefile" ] || {
        echo "FAIL: no configured AROS build at $root" >&2
        return 1
    }

    # Refresh the public content-probe/name header without building anything
    # outside compiler/include.
    make -C "$root/compiler/include" -f mmakefile \
        TOP="$root" SRCDIR="$SRC" \
        AROS_TARGET_ARCH="$arch" AROS_TARGET_CPU="$cpu" \
        AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 \
        CURDIR=compiler/include TARGET=compiler-includes compiler-includes

    make -C "$rom_dir" -f mmakefile \
        TOP="$root" SRCDIR="$SRC" \
        AROS_TARGET_ARCH="$arch" AROS_TARGET_CPU="$cpu" \
        AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 \
        CURDIR=rom/usb/classes/massstorage \
        TARGET=kernel-usb-classes-massstorage \
        "$sys/AROS/Classes/USB/massstorage.class"

    # A newly configured tree needs the generated HIDD include directory
    # before genmodule can write into it.
    make -C "$hidd_dir" -f mmakefile \
        TOP="$root" SRCDIR="$SRC" \
        AROS_TARGET_ARCH="$arch" AROS_TARGET_CPU="$cpu" \
        AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 \
        CURDIR=workbench/devs/USB/classes/MassStorage \
        TARGET=hidd-usb-classes-mstorage-includes-dirs \
        hidd-usb-classes-mstorage-includes-dirs
    make -C "$hidd_dir" -f mmakefile \
        TOP="$root" SRCDIR="$SRC" \
        AROS_TARGET_ARCH="$arch" AROS_TARGET_CPU="$cpu" \
        AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 \
        CURDIR=workbench/devs/USB/classes/MassStorage \
        TARGET=hidd-usb-classes-mstorage-includes \
        hidd-usb-classes-mstorage-includes
    make -C "$hidd_dir" -f mmakefile \
        TOP="$root" SRCDIR="$SRC" \
        AROS_TARGET_ARCH="$arch" AROS_TARGET_CPU="$cpu" \
        AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 \
        CURDIR=workbench/devs/USB/classes/MassStorage \
        TARGET=hidd-usb-classes-mstorage \
        "$sys/AROS/Classes/USB/mstorage.hidd"

    for module in \
        "$sys/AROS/Classes/USB/massstorage.class" \
        "$sys/AROS/Classes/USB/mstorage.hidd"
    do
        if ! strings "$module" | grep -Fqx 'exfat-handler'; then
            echo "FAIL: $module does not select installed exfat-handler" >&2
            return 1
        fi
        if strings "$module" | grep -Fqx 'exfat.handler'; then
            echo "FAIL: $module retains nonexistent exfat.handler name" >&2
            return 1
        fi
        file "$module"
    done
}

build_modules "$A64" darwin aarch64 "$A64/bin/darwin-aarch64"
build_modules "$M68K" amiga m68k "$M68K/bin/amiga-m68k"

echo "PASS: normal builds include exFAT and both AArch64/m68k USB stacks select exfat-handler"
