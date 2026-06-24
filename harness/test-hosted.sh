#!/usr/bin/env bash
# Phase-2 regression matrix: build + run every hosted spike, assert each marker.
#
# The hosted analog of harness/test.sh. Each Phase-2 milestone is its own native
# macOS binary (one-binary-per-marker), so instead of one boot we run each through
# its `make hosted-*` target (which builds it and gates on its marker via
# run-hosted.sh, exit 0 = PASS). One regression in H4 is caught while working on
# H8. Same uniform PASS/FAIL the agent reads everywhere.
set -uo pipefail
cd "$(dirname "$0")/.."

# label : make target  (order = milestone order)
ROWS=(
  "H1 hosted context switch : hosted-run"
  "H2 hosted preemption     : hosted-preempt"
  "H3 host-call ABI shim     : hosted-abi"
  "H4 hosted scheduler       : hosted-exec"
  "H5 hosted memory (AllocMem): hosted-mem"
  "H6 tiny exec (compose)     : hosted-kern"
  "H7 host display            : hosted-display"
  "H8 exec.library (LVO)      : hosted-library"
)

mkdir -p run
fail=0
echo "==== HOSTED REGRESSION (Phase 2) ===="
for row in "${ROWS[@]}"; do
  label="${row%%:*}"; target="${row##*:}"
  label="${label// /}"; label="${row%%:*}"   # keep label spaces for display
  target="$(echo "$target" | tr -d ' ')"
  if make "$target" >"run/$target.log" 2>&1; then
    printf 'ok    %-30s (make %s)\n' "$label" "$target"
  else
    printf 'FAIL  %-30s (make %s -> run/%s.log)\n' "$label" "$target" "$target"
    fail=1
  fi
done
echo "result=$([ "$fail" = 0 ] && echo PASS || echo FAIL)"
exit "$fail"
