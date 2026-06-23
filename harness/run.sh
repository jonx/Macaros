#!/usr/bin/env bash
# AROS AArch64 bring-up harness — the loop the agent runs EVERY iteration.
#
#   build (via make) -> launch headless QEMU -> observe -> verify -> verdict
#
# "Seeing" is generalized past screenshots: the agent observes through whatever
# channel is faithful for the current milestone (serial text, fault trace,
# framebuffer image, CPU-state dump) and always gets the SAME verdict block back.
#
# Usage:   ./harness/run.sh '<success-marker>'
#   env:   IMG=... TIMEOUT=30 SHOT=1 (framebuffer screendump) GDB=1 (freeze+stub)
set -uo pipefail

# ---- config (override via env) ----
IMG="${IMG:-build/aros-aarch64.elf}"
MACHINE="${MACHINE:-virt}"
CPU="${CPU:-cortex-a72}"
MEM="${MEM:-512}"
TIMEOUT="${TIMEOUT:-30}"
RUNDIR="${RUNDIR:-./run}"
MARKER="${1:-}"          # success string to grep for in serial output
WANT_SHOT="${SHOT:-0}"   # SHOT=1 -> also grab a framebuffer screendump (M9+)
WANT_GDB="${GDB:-0}"     # GDB=1  -> freeze at boot + gdbstub on :1234 (lldb-dump.sh)

mkdir -p "$RUNDIR"
SERIAL="$RUNDIR/serial.log"
QEMULOG="$RUNDIR/qemu.log"
QMP="$RUNDIR/qmp.sock"
SERSOCK="$RUNDIR/serial.sock"
SHOTPNG="$RUNDIR/screen.png"
: > "$SERIAL"; : > "$QEMULOG"

[ -f "$IMG" ] || { echo "result=FAIL"; echo "reason=no image at $IMG (run 'make image')"; exit 1; }

# ---- launch: headless + self-terminating ----
# The serial chardev is a SOCKET with a logfile, which gives us both at once:
#   * logfile  -> the agent reads progress markers (observe)
#   * socket   -> the agent can inject keystrokes for M8 (drive)
# semihosting lets a finished program power off cleanly, so a PASS exits in <1s;
# a hung boot is reaped by the watchdog below instead of blocking the loop.
GDBOPT=""
[ "$WANT_GDB" = "1" ] && GDBOPT="-s -S"   # gdbstub on :1234, frozen at reset

qemu-system-aarch64 \
  -M "$MACHINE" -cpu "$CPU" -m "$MEM" -display none -no-reboot \
  -semihosting-config enable=on,target=native \
  -kernel "$IMG" \
  -chardev "socket,id=ser0,path=$SERSOCK,server=on,wait=off,logfile=$SERIAL" \
  -serial chardev:ser0 \
  -qmp "unix:$QMP,server=on,wait=off" \
  -d int,guest_errors,unimp -D "$QEMULOG" $GDBOPT &
QPID=$!

# Portable timeout: GNU/coreutils 'timeout' isn't on stock macOS, so use a bash
# watchdog that TERM- then KILL-s a stuck QEMU. Skipped under GDB=1 (manual debug).
WPID=""
if [ "$WANT_GDB" != "1" ]; then
  ( sleep "$TIMEOUT"; kill -TERM "$QPID" 2>/dev/null; sleep 2; kill -KILL "$QPID" 2>/dev/null ) &
  WPID=$!
  disown "$WPID" 2>/dev/null || true   # keep shell job-control noise out of the verdict
fi

# Optional framebuffer view once it's plausibly up (the "image" way of seeing).
if [ "$WANT_SHOT" = "1" ]; then
  sleep 2
  python3 "$(dirname "$0")/qmp.py" --sock "$QMP" screendump "$SHOTPNG" >/dev/null 2>&1 || true
fi

wait "$QPID" 2>/dev/null
RC=$?
[ -n "$WPID" ] && kill "$WPID" 2>/dev/null      # cancel watchdog if QEMU self-exited
case "$RC" in 124|137|143) RC=124;; esac        # normalize timed-out/killed -> 124

# ---- verdict: uniform + machine-parseable, regardless of which channel mattered ----
PASS=0
if [ -n "$MARKER" ] && grep -qF -- "$MARKER" "$SERIAL"; then PASS=1; fi
# Count real CPU faults, but not the benign "Semihosting call" pseudo-exception
# QEMU logs for our clean-exit trap.
FAULTS=$(grep -iE "exception|fault|abort|unimp|invalid" "$QEMULOG" 2>/dev/null \
          | grep -ivE "semihosting call" | wc -l | tr -d ' ')

echo "==== VERDICT ===="
echo "result=$([ "$PASS" = 1 ] && echo PASS || echo FAIL)"
echo "marker=${MARKER:-<none>}"
echo "qemu_exit=$RC   # 124 = timed out / likely hung boot"
echo "fault_lines=$FAULTS"
echo "serial=$SERIAL"
echo "qemu_trace=$QEMULOG"
[ "$WANT_SHOT" = "1" ] && echo "screenshot=$SHOTPNG"
echo "---- serial tail (last 15) ----"
tail -n 15 "$SERIAL" 2>/dev/null
[ "$PASS" = 1 ] && exit 0 || exit 1
