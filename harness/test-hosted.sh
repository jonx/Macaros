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
  "H9 exec Wait/Signal        : hosted-signal"
  "H10 exec message ports     : hosted-msgport"
  "H11 device -> real file    : hosted-device"
  "H12 exec.library boot      : hosted-execboot"
  "J1 MAP_JIT exec memory     : hosted-jit68k"
  "J2 Emu68 emitter hosted    : hosted-jit68k-j2"
  "J3 68k->native LVO bridge  : hosted-jit68k-j3"
  "J4 load->relocate->run     : hosted-jit68k-j4"
  "J5a memory load/store+sbox : hosted-jit68k-j5a"
  "J5b loop+real cond codes   : hosted-jit68k-j5b"
  "J5c re-host REAL decoder+RA : hosted-jit68k-j5c"
  "J5d whole corpus thru JIT  : hosted-jit68k-j5d"
  "J5e block-scoped reg alloc  : hosted-jit68k-j5e"
  "J5f return stack+subroutines: hosted-jit68k-j5f"
  "J5g broaden ISA+addr modes  : hosted-jit68k-j5g"
  "J5h X-bit multi-precision   : hosted-jit68k-j5h"
  "J5i 68k exception/SR model  : hosted-jit68k-j5i"
  "apps68k real 68k programs  : hosted-jit68k-apps"
  "D1 cocoa/metal present     : hosted-cocoametal"
  "ABI cocoametal dylib seam  : cocoametal-abi"
  "HIDDSIM cocoametal D3 harness : cocoametal-hiddsim"
  "D2t cocoametal threading   : cocoametal-d2t"
  "D4D5 cocoametal input pump  : cocoametal-input"
  "SET cocoametal settings    : cocoametal-settings"
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
