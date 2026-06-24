#!/bin/sh
#
# bootrun.sh — boot the hosted darwin-aarch64 AROS kickstart and capture output,
# with AROS's own boot narration turned on.
#
# The key trick: exec.library reads a `sysdebug=<flags>` kickstart argument at
# runtime (rom/exec/prepareexecbase.c) and enables ExecLog() narration for the
# named subsystems — NO REBUILD NEEDED. The last narrated line before a crash
# names the resident / task / library that died, so you almost never have to
# symbolicate a raw PC by hand.
#
# Valid sysdebug flags (rom/exec/exec_flags.c):
#   InitCode InitResident FindResident CreateLibrary SetFunction NewSetFunction
#   ChipRam AddTask RemTask GetTaskAttr SetTaskAttr ExceptHandler AddDosNode
#   PCI RamLib MemTrack LogExtended
#
# Usage:
#   bootrun.sh <AROS-boot-dir> [seconds] [sysdebug-flags] [entitlements.plist]
# Example:
#   bootrun.sh .../bin/darwin-aarch64/AROS/boot/darwin 9 \
#       InitCode,InitResident,AddTask,CreateLibrary,FindResident
#
set -u
BOOTD="${1:?usage: bootrun.sh <AROS-boot-dir> [seconds] [sysdebug-flags] [entitlements]}"
SECS="${2:-9}"
DBG="${3:-InitCode,InitResident,AddTask,CreateLibrary,FindResident}"
ENT="${4:-}"
CONF="$BOOTD/AROSBootstrap.conf"
LOG="${TMPDIR:-/tmp}/aros-boot.log"

# Rewrite the single `arguments sysdebug=...` line (drop any previous one).
grep -v '^arguments ' "$CONF" > "$CONF.tmp" 2>/dev/null && mv "$CONF.tmp" "$CONF"
[ -n "$DBG" ] && echo "arguments sysdebug=$DBG" >> "$CONF"

# (Re)sign — the loader maps RW anon memory; the hardened-runtime entitlement is
# kept for forward-compat with executing loaded code. Skips cleanly if no plist.
if [ -n "$ENT" ] && [ -f "$ENT" ]; then
    codesign -s - -f -o runtime --entitlements "$ENT" "$BOOTD/AROSBootstrap" 2>/dev/null
else
    codesign -s - -f "$BOOTD/AROSBootstrap" 2>/dev/null
fi

# Boot in the background and kill after SECS (AROS halts/loops on a fatal alert).
( cd "$BOOTD" && ./AROSBootstrap -c "$CONF" ) > "$LOG" 2>&1 &
BPID=$!
sleep "$SECS"
kill -9 "$BPID" 2>/dev/null

echo "=== boot output ($(wc -l < "$LOG") lines; full log: $LOG) ==="
tail -30 "$LOG"
