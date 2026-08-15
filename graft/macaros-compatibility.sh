#!/bin/sh
# Macaros end-user compatibility check. This file is copied into release DMGs as
# "Check Macaros Compatibility.command", so keep it dependency-free and usable
# by double-clicking in Finder.
set -u

MIN_MACOS="12.0"
MIN_MEMORY_GIB=4
RECOMMENDED_MEMORY_GIB=8
INSTALL_HEADROOM_KIB=2097152

failures=0
warnings=0

pass() { printf '  PASS  %s\n' "$1"; }
warn() { printf '  WARN  %s\n' "$1"; warnings=$((warnings + 1)); }
fail() { printf '  FAIL  %s\n' "$1"; failures=$((failures + 1)); }

version_at_least() {
    awk -v have="$1" -v need="$2" 'BEGIN {
        split(have, h, "."); split(need, n, ".")
        for (i = 1; i <= 3; i++) {
            hv = h[i] + 0; nv = n[i] + 0
            if (hv > nv) exit 0
            if (hv < nv) exit 1
        }
        exit 0
    }'
}

human_kib() {
    awk -v kib="$1" 'BEGIN {
        if (kib >= 1048576) printf "%.1f GiB", kib / 1048576
        else if (kib >= 1024) printf "%.0f MiB", kib / 1024
        else printf "%d KiB", kib
    }'
}

case "${1:-}" in
    -h|--help)
        printf 'Usage: %s\n\nChecks whether this Mac can run the Macaros Apple Silicon release.\n' "$(basename "$0")"
        exit 0
        ;;
    "") ;;
    *) printf 'Unknown option: %s (try --help)\n' "$1" >&2; exit 2 ;;
esac

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$SCRIPT_DIR/Macaros.app"

printf 'Macaros compatibility check\n'
printf '===========================\n'

if [ "$(uname -s 2>/dev/null || printf unknown)" = "Darwin" ]; then
    pass "macOS detected"
else
    fail "Macaros requires macOS"
fi

hardware_arm64=$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null || printf 0)
model=$(/usr/sbin/sysctl -n hw.model 2>/dev/null || printf unknown)
if [ "$hardware_arm64" = "1" ]; then
    pass "Apple Silicon detected ($model)"
else
    fail "Apple Silicon is required; Intel Macs are not supported"
fi

macos_version=$(/usr/bin/sw_vers -productVersion 2>/dev/null || printf unknown)
if [ "$macos_version" != "unknown" ] && version_at_least "$macos_version" "$MIN_MACOS"; then
    pass "macOS $macos_version (minimum $MIN_MACOS)"
else
    fail "macOS $MIN_MACOS or newer is required (found $macos_version)"
fi

memory_bytes=$(/usr/sbin/sysctl -n hw.memsize 2>/dev/null || printf 0)
memory_gib=$((memory_bytes / 1073741824))
if [ "$memory_gib" -ge "$RECOMMENDED_MEMORY_GIB" ]; then
    pass "${memory_gib} GiB memory (8 GiB or more recommended)"
elif [ "$memory_gib" -ge "$MIN_MEMORY_GIB" ]; then
    warn "${memory_gib} GiB memory; Macaros should run, but 8 GiB or more is recommended"
else
    fail "at least ${MIN_MEMORY_GIB} GiB memory is required (found ${memory_gib} GiB)"
fi

if /usr/sbin/ioreg -r -c AGXAccelerator 2>/dev/null | /usr/bin/grep -q 'MetalPlugin'; then
    pass "Apple GPU with Metal support detected"
else
    warn "could not verify an available Metal GPU"
fi

if /bin/launchctl print "gui/$(id -u)" >/dev/null 2>&1; then
    pass "graphical login session available"
else
    warn "no graphical login session detected; Macaros needs one to show its desktop"
fi

if [ -d "$APP" ]; then
    pass "Macaros.app is present beside this checker"

    inner_binary="$APP/Contents/Resources/AROS/boot/darwin/Macaros"
    if [ -f "$inner_binary" ] && /usr/bin/file "$inner_binary" 2>/dev/null | /usr/bin/grep -q 'arm64'; then
        pass "bundled Macaros runtime is arm64"
    else
        fail "the bundled arm64 Macaros runtime is missing or invalid"
    fi

    app_kib=$(/usr/bin/du -sk "$APP" 2>/dev/null | awk '{print $1}')
    app_kib=${app_kib:-0}
    required_kib=$((app_kib + INSTALL_HEADROOM_KIB))
    # The checker normally runs from a read-only DMG, whose deliberately small
    # free-space value says nothing about the destination. Check the standard
    # installation volume instead.
    install_volume="/Applications"
    [ -d "$install_volume" ] || install_volume="$HOME"
    free_kib=$(/bin/df -Pk "$install_volume" 2>/dev/null | awk 'END {print $4}')
    free_kib=${free_kib:-0}
    if [ "$free_kib" -ge "$required_kib" ]; then
        pass "$(human_kib "$free_kib") free on the installation volume; $(human_kib "$required_kib") needed to install safely"
    else
        fail "$(human_kib "$required_kib") free disk space is needed to install safely; $(human_kib "$free_kib") is available"
    fi

    if /usr/bin/codesign --verify --deep --strict "$APP" >/dev/null 2>&1; then
        pass "application signature is internally valid"
    else
        warn "application signature is absent or invalid; use the notarized release image"
    fi
else
    warn "Macaros.app is not beside this checker, so package integrity and install size were not checked"
fi

state_parent="$HOME/Library/Application Support"
if [ -d "$state_parent" ] && [ -w "$state_parent" ]; then
    pass "user application-data folder is writable"
elif [ -d "$HOME/Library" ] && [ -w "$HOME/Library" ]; then
    pass "user Library is writable (Macaros will create its application-data folder)"
else
    fail "Macaros cannot write its per-user state under ~/Library/Application Support"
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
    if [ "$warnings" -eq 0 ]; then
        printf 'RESULT: COMPATIBLE — this Mac meets the Macaros requirements.\n'
    else
        printf 'RESULT: COMPATIBLE WITH %d WARNING(S) — review the messages above.\n' "$warnings"
    fi
    exit 0
fi

printf 'RESULT: NOT COMPATIBLE — %d required check(s) failed.\n' "$failures"
exit 1
