# Stabilization record: crash containment + the resize deadlock

The record of the 2026-07 robustness effort on hosted AROS (aarch64-darwin).
Requirement: the hosted AROS instance must never crash out or wedge from a
single guest fault. Two concrete deliverables, both delivered:

1. Fix the freeze that happened when resizing a Wanderer window.
2. Opt-in crash containment so any guest CPU fault is survivable.

Everything guest-side eventually PRs to upstream AROS except the Mac host
layer (Cocoa/Metal/Apple), which stays on the jonx fork. Current design doc:
`docs/features/crash-handling/design.md`. Detailed deadlock forensics:
[reports/resize-freeze-report.md](reports/resize-freeze-report.md).

## Groundwork

- **x18 stale-object bug** (one of the two resize bugs): macOS zeroes x18 on
  any signal, so every module must be compiled `-ffixed-x18`. A make.cfg flag
  change does NOT invalidate .o files, so a flag change needs
  `find gen -name '*.o' -not -path '*tools*' -delete` and a rebuild. Fixed by
  force-delete + full rebuild; the desktop stack now has 0 x18 refs, guarded
  by `graft/deploy-check x18`.
- **FP/NEON never saved across task switches:** fixed in
  `../aros-upstream/arch/all-darwin/kernel/cpu_aarch64.h` (commit 7f55baf7).
  NEON blob in AROSCPUContext + PREPARE_INITIAL_CONTEXT.
- **CrashLab** = `C:CrashLab` guest program, triggers each fault class
  (NULLREAD/NULLWRITE/NULLJUMP/WILDWRITE/ILLEGAL/STACKSMASH/DEADEND/GURU/
  BUSYLOOP/FORBIDLOOP/CRASHTASK), each prints `[CRASHLAB] NAME:`. Source:
  `../aros-upstream/developer/debug/test/crash/crashlab.c` (commit 53240bbe).
- **crash-smoke** = `graft/crash-smoke`, the before/after survival matrix.
  BEFORE matrix: every fault class took the instance down.
- **`aros-ctl tasks`** = SIGINFO -> kernel `core_DiagHandler` -> walks all
  task lists + current, prints name/state/pri/sig + symbolized backtrace per
  task. Works on a wedged guest; the tool for any guest hang. Commits
  1afd2b4a (kernel+bootstrap) / 5ebdb35 (aros-ctl).

## The resize deadlock, found and fixed (2026-07-02)

Proven a real deadlock, not a stale-framebuffer illusion (offscreen
framebuffer identical over 6s, console.device stuck at the same PC 6s apart,
host threads healthy in mach_msg/sigsuspend). The stack-scan forensics
(commit 7ac8dafc, kernel.c) named the owner on the first try:

```
task 'console.device' BLOCKED in InternalObtainSemaphore <- RectFill
    sp+144 -> semaphore ... '(unnamed)' owner=... 'input.device' nest=1 queue=1
task 'input.device' WAIT in WaitPort <- CDInputHandler <- ProcessEvents
```

The cycle:

- console.device's input handler (CDInputHandler, pri 51) forwarded every
  event to the console task via a SINGLE static message, then blocked in
  WaitPort for the reply.
- Intuition's size/drag gadget classes hold LockLayers() across the whole
  interactive drag (rubber band, windowclasses.c).
- The console task redraws with RectFill, which needs the window layer lock.
- Second drag: the console task is still processing the first drag's
  SIZEWINDOW redraw, blocks on the layer lock now held by the drag; the next
  input event makes CDInputHandler wait for a console-task reply that never
  comes; the drag can never release. All input dead.

**Fix (commit ad65a609):** the CDIH -> console task handoff is now
asynchronous: one cdihMessage allocated per event, PutMsg without waiting,
the console task frees it (order preserved by the port FIFO; on OOM the
event is dropped). Principle for upstream: an input handler must never wait
on a task that renders.

**Upstreamed:** https://github.com/aros-development-team/AROS/pull/819
(branch `fix/console-resize-deadlock` on the fork, cherry-picked onto
master; the PR keeps master's handler priority 50, our 51 is port-local
from b009c855).

Verified: `graft/resize-smoke` PASS 10/10 (previously FAIL at cycle 1),
shell typing works, the window redraws correctly after resize. The
resize-smoke FAIL path uses `aros-ctl diag nokill` and leaves a wedged
instance running so `aros-ctl tasks` can autopsy it.

**Follow-up (commit 05c401e1): stale redraw after resize.** Once the
deadlock was gone, resizing revealed the console never repainted at the new
size. Pre-existing upstream bug, NOT caused by the async change (verified by
instrumentation: no SIZEWINDOW ever reached CDIH, and by source): intuition
delivers window events for IDCMP-less windows by GENERATING input events
from its deferred-action path, and generated events are only visible to
input handlers BELOW priority 50; console.device's handler is at 51. The
console task's whole SIZEWINDOW/REFRESHWINDOW/CLOSEWINDOW/GADGETDOWN/UP arm
was dead code. Fix: a second internal handler at priority 0 forwards exactly
those five generated classes (unit matched by ie_EventAddress window for the
first three, active window for gadgets) through the same async port.

## Host hardening (commit f11457f)

Both pieces live in `hosted/cocoametal/` (no bootstrap/kernel ABI change):

- **NSEvent ring:** a 20ms main-thread NSTimer (created with the window,
  common run-loop modes) drains NSEvents into a host CMEvent ring
  independently of the guest; `cm_pump_events` reads from the ring. No
  beachball with a dead guest.
- **Guest-progress watchdog:** same tick, measures time since the guest last
  pumped (pump recency, NOT a core_IRQ heartbeat: the scheduler stays alive
  during input wedges, the pump does not). Stale past
  `AROS_CM_WATCHDOG_SECS` (default 5, 0=off) => `[cm-watchdog]` log +
  SIGINFO task dump auto-fired once per stall episode.

Verified: typing/resize-smoke 10/10 through the ring; CrashLab FORBIDLOOP
produced exactly one watchdog fire + a full auto-dump naming the spinning
task.

## Opt-in trap containment

Guest commits (crash-containment branch): f7fa964f (trap-path fixes),
03e5fdd6 (containment), df3a315d (CrashLab debug-channel markers). Host:
5831e8f (harness + proofs).

- Boot arg `containment` (harness: `AROS_KERNEL_ARGS=containment aros-ctl
  run`): a USER-task CPU trap raises the RECOVERABLE guru (More.../Continue);
  Continue RemTask()s the offender only, the system keeps running.
  Supervisor faults / double-crashes stay dead-end. Default = old behavior.
- Two pre-existing trap bugs found and fixed on the way:
  (a) the hosted trap path misaligned SP when redirecting the crashed task
  (an x86_64-only fixup applied on aarch64), so the crash handler SIGBUSed
  before ANY guru could display; this alone was why every fault killed the
  instance. (b) the trap loop breaker lacked the task in its key (false halt
  when the same program crashes twice under containment).
- AFTER matrix (proof: run/darwin-aarch64/proofs/crashlab-after-matrix-*.txt):
  no CPU-fault class kills the process under either policy now; STACKSMASH
  stays dead-end (the handler needs the overflowed stack); BUSYLOOP/
  FORBIDLOOP are hangs, covered by the watchdog auto-dump.

## Sweeps + full battery

- **x18: the FULL sweep is clean** (`deploy-check x18`: every module <= 4
  refs). 16 stale modules rebuilt; compiler-rt long-double builtins fixed
  surgically (7 objects recompiled from llvmorg-20.1.0 sources with
  -ffixed-x18 and replaced in
  /tmp/aros-crosstools/lib/generic/libclang_rt.builtins-aarch64.a; backup =
  same path + .pre-x18-backup); stdc/cybergraphics/png/tiff relinked; the
  FFView fleet rebuilt from clean ffmpeg sysroots; deploy-check ignores the
  benign stp/ldp x18 save/restore pattern (exec baseline).
- **Battery green:** desktop, resize, clipboard, audio, hostvol, loadmatrix,
  rust, ffmpeg + the crash matrices.
- **Audio stack restored into /tmp/arosbuild**; recipe in
  docs/features/coreaudio-audio/README.md (AHI translations are git
  submodules; sfdc/flexcat/COMPILER_PATH overrides).
- **upstream-patches snapshot refreshed** from crash-containment (84 commits).

## Open items after this effort

- **dos redirected-handle close loses buffered output** (regression, window =
  the 2026-07-01 upstream merge): `C:FF0Smoke >>MacRW:x` captured nothing
  while console output was fine; programs that Flush() are unaffected.
  Repro startup + analysis: commit cf7ed6e. Investigate rom/dos buffered IO /
  Close() flush behavior vs the merge.
- AHI subsystem build is not first-class (the recipe is manual, documented).

## Key file references

Guest (`../aros-upstream`, branch crash-containment):

- `arch/all-unix/kernel/kernel.c` -- core_DiagHandler + forensics
- `arch/all-unix/bootstrap/kickstart.c` -- SIGINFO mask, kick() threaded path
- `arch/all-darwin/kernel/cpu_aarch64.h` -- NEON context fix
- `rom/exec/semaphores.c` -- InternalObtainSemaphore (the blocked frame)
- `rom/exec/traphandler.c` -- trap policy
- `developer/debug/test/crash/crashlab.c` -- CrashLab
- console.device consoletask.c (consoleTaskLock at :189), graphics
  locklayerrom.c

Host (`.`):

- `graft/aros-ctl` -- control harness (has `tasks`)
- `graft/resize-smoke` -- deadlock repro gate
- `graft/crash-smoke` -- crash survival matrix gate
- `graft/deploy-check` -- x18 + deploy guard
- `hosted/cocoametal/` -- the Cocoa/Metal display driver
