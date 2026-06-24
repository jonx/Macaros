#!/usr/bin/env bash
# Hosted-AROS loop: run the native macOS binary, observe stdout, emit a verdict.
#
# The hosted observation loop is even simpler than the QEMU one — no emulator, the
# binary IS a Mac process. Same uniform PASS/FAIL block so the agent reads one
# format whether the target is bare-metal QEMU or a hosted macOS process.
#
# Usage:  ./harness/run-hosted.sh '<success-marker>'   (env: BIN=, TIMEOUT=)
set -uo pipefail

BIN="${BIN:-build/host-aros}"
MARKER="${1:-}"
TIMEOUT="${TIMEOUT:-10}"
RUNDIR="${RUNDIR:-./run}"
mkdir -p "$RUNDIR"
OUT="$RUNDIR/host.log"
: > "$OUT"

[ -x "$BIN" ] || { echo "result=FAIL"; echo "reason=no executable $BIN (run 'make hosted')"; exit 1; }

"$BIN" > "$OUT" 2>&1 &
PID=$!
# Portable watchdog (stock macOS has no GNU timeout).
( sleep "$TIMEOUT"; kill -9 "$PID" 2>/dev/null ) &
WPID=$!
disown "$WPID" 2>/dev/null || true

wait "$PID" 2>/dev/null
RC=$?
kill "$WPID" 2>/dev/null
case "$RC" in 137|143) RC=124;; esac     # killed by watchdog -> timed out

PASS=0
if [ -n "$MARKER" ] && grep -qF -- "$MARKER" "$OUT"; then PASS=1; fi

echo "==== VERDICT (hosted) ===="
echo "result=$([ "$PASS" = 1 ] && echo PASS || echo FAIL)"
echo "marker=${MARKER:-<none>}"
echo "exit=$RC$([ "$RC" = 124 ] && echo '   # timed out / hung')"
echo "log=$OUT"
echo "---- output ----"
cat "$OUT"
[ "$PASS" = 1 ] && exit 0 || exit 1
