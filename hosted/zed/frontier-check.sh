#!/bin/sh
# frontier-check.sh -- map the AROS compile frontier of a Rust workspace:
# cargo-check each named crate for aarch64-unknown-aros and record pass/fail.
#
# Usage:   frontier-check.sh <workspace-dir> <crate> [crate...]
# Env:     AROS_TARGET_JSON  target spec (default: ../rust/aarch64-unknown-aros.json)
#          FRONTIER_LOG_DIR  where per-crate logs + frontier-results.txt go
#                            (default: ./frontier-logs)
#
# Needs the pinned toolchain + the rust-src symlink to ../rust-aros
# (see hosted/rust/STD-PORT.md, "Dev environment setup").
# Does not stop at the first failure: the point is the per-crate map.
set -u
WS="$1"; shift
JSON="${AROS_TARGET_JSON:-$(cd "$(dirname "$0")/../rust" && pwd)/aarch64-unknown-aros.json}"
LOGDIR="${FRONTIER_LOG_DIR:-$(pwd)/frontier-logs}"
mkdir -p "$LOGDIR"
RESULTS="$LOGDIR/frontier-results.txt"

for crate in "$@"; do
    log="$LOGDIR/$crate.log"
    printf '== %s ==\n' "$crate"
    if (cd "$WS" && cargo +nightly-2026-06-27 check -p "$crate" \
            --target "$JSON" \
            -Zjson-target-spec -Zbuild-std=std,panic_abort) \
            >"$log" 2>&1; then
        echo "PASS $crate" | tee -a "$RESULTS"
    else
        err=$(grep -m1 -E '^error(\[|:)' "$log" || true)
        echo "FAIL $crate :: ${err:-see $log}" | tee -a "$RESULTS"
    fi
done
