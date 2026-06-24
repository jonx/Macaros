#!/usr/bin/env bash
# Regression check: boot ONCE, assert every expected marker is present.
#
# `make run` only gates on the latest milestone's marker; this asserts the whole
# cumulative sequence, so a regression in an earlier milestone (M3) is caught
# while working on a later one (M7). Markers are passed as args (see Makefile).
set -uo pipefail

IMG="${IMG:-build/aros-aarch64.elf}"
RUNDIR="${RUNDIR:-./run}"

# Boot once; the per-marker grep below is the real check, so we don't gate here.
IMG="$IMG" RUNDIR="$RUNDIR" ./harness/run.sh '__regression_boot__' >/dev/null 2>&1
SER="$RUNDIR/serial.log"

fail=0
echo "==== REGRESSION ===="
for m in "$@"; do
    if grep -qF -- "$m" "$SER"; then
        echo "ok    $m"
    else
        echo "MISS  $m"
        fail=1
    fi
done
echo "result=$([ "$fail" = 0 ] && echo PASS || echo FAIL)"
exit "$fail"
