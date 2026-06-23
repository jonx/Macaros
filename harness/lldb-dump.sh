#!/usr/bin/env bash
# The "see the CPU state" channel: scripted lldb, never an interactive session.
#
# lldb is the native macOS debugger (no codesigning dance, unlike gdb on Apple
# Silicon) and speaks QEMU's gdbstub protocol. Boot QEMU frozen with a stub via
#   GDB=1 ./harness/run.sh ...        (adds -s -S: gdbstub on :1234, paused)
# then run this for a one-shot, parseable dump. Most useful at M3 (vectors) and
# M4 (MMU), where the failure is "we faulted" and you need PC + system regs.
set -uo pipefail
PORT="${PORT:-1234}"
SYMS="${SYMS:-build/aros-aarch64.elf}"   # ELF with symbols for source-level context

command -v lldb >/dev/null || { echo "lldb-dump: lldb not found"; exit 1; }

lldb -b \
  -o "target create $SYMS" \
  -o "gdb-remote localhost:$PORT" \
  -o "register read --all" \
  -o "disassemble --pc --count 16" \
  -o "thread backtrace" \
  -o "detach" -o "quit"
