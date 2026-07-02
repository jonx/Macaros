# Handoff: AROS-on-Apple-Silicon stabilization

You are taking over an OS-robustness effort on hosted AROS (aarch64-darwin). This
is legitimate OS development: making the OS more resistant to crashes and fixing a
window-resize freeze. It is NOT an attack on any system.

Read this whole file first. Then read the two memory files it points to. Then
resume at **Phase D** below (Phase C is DONE, see section 8).

---

## 0. Mission and the one hard constraint

Overriding requirement, in the user's words: "We can't have the whole instance
coming down because of the smallest crash and take down everything." The hosted
AROS instance must never crash-out or wedge from a single guest fault.

Two concrete deliverables:
1. Fix the freeze that happens when you resize a Wanderer window.
2. Add opt-in crash containment so any guest CPU fault is survivable.

End state: everything eventually PRs to upstream AROS EXCEPT the Mac host
(Cocoa/Metal/Apple), which stays permanently on the jonx fork.

## 1. Working rules (do not violate)

- **Comms:** plain, concise English. NEVER use em-dashes (the user is French).
  Report outcomes and decisions, not disk/monitor/progress play-by-play.
- **Push:** you may push to the jonx fork (`fork` remote = github.com/jonx/AROS,
  and github.com/jonx/AROS-AArch64 for the host repo). NEVER push to aros
  upstream. In `../aros-upstream` the `origin` remote is FETCH ONLY.
- **Docs:** if you learn a durable fact or find a stale doc, fix it the same
  session. Author each fact once, link don't restate. Task lists / session notes
  do NOT go in docs/features. When you add/rename a `docs/features/<folder>/`,
  update the Quick index table in docs/features/README.md the same turn.

## 2. Repos and branches

- `/Users/user/Source/aros-aarch64` = host/graft layer (this repo, github
  jonx/AROS-AArch64). Current branch `graft/aros-ctl-diag-keymap-boot`. Has many
  unrelated uncommitted changes from other threads (jit68k, rust); leave those
  alone, they are not ours.
- `/Users/user/Source/aros-upstream` = the AROS OS source (kernel, modules, libs).
  **This is where guest-side code changes go.** Currently on branch
  `crash-containment` (the correct branch for all this work). Remotes:
  `origin` = aros-development-team (fetch only), `fork` = jonx/AROS.
- `/Users/user/Source/aros-upstream-master` = 2nd git worktree of the same repo,
  pristine upstream snapshot for diffing. Do not build here.
- Build tree: `/tmp/arosbuild` (stable SDK). Crosstools: `/tmp/aros-crosstools`.
  Both survive across sessions. Session scratchpads get GC'd half-deleted, so
  NEVER build in a scratchpad.

## 3. Build rules (the expensive, most-repeated traps)

Read `docs/features/build/README.md` before building. Summary:
- NEVER run a bare `make`. It tries to rebuild the 1-2h LLVM toolchain and breaks
  on darwin-incomplete modules.
- Build explicit module **metatargets**, ONE target per `make` call:
  `kernel-kernel`, `kernel-dos`, `workbench-libs-cgfx` (this is cybergraphics;
  `-cybergraphics` only rebuilds linklib stubs), etc.
- Reuse `/tmp/aros-crosstools`; never rebuild LLVM.
- Env for building/objdump:
  `export PATH="/tmp/graft-tools:/opt/homebrew/bin:$PATH"`.
  objdump = `/opt/homebrew/opt/llvm/bin/llvm-objdump`.
- **x18:** every module must be compiled `-ffixed-x18` (macOS zeroes x18 on any
  signal). A make.cfg flag change does NOT invalidate .o files, so after any flag
  change you must `find gen -name '*.o' -not -path '*tools*' -delete` then rebuild.
  Clean modules disassemble to <=4 x18 refs. `graft/deploy-check x18` sweeps.

## 4. Run / deploy rules

Read `docs/features/deployment/README.md`. There are several runnable copies of
the same artifacts (`~/lib`, boot image, app settings); editing one and running
another is the classic "my fix didn't take" trap.

- Launch the windowed desktop:
  `cd ~/Source/aros-aarch64; AROS_CTL_STARTUP_MODE=desktop ./graft/run-window.sh`
- Drive it headlessly with `aros-ctl` (the control harness). Key subcommands:
  `run`, `kill`, `type "..."`, `enter`, `click`, `shot <png>`, and **`tasks`**
  (out-of-band task dump, see below). Doc:
  `docs/features/control-harness/README.md`.
- Log defaults to `/tmp/aros-window.log`, pid to `/tmp/aros-cm.pid`.

## 5. What is already DONE (do not redo)

- **x18 stale-object bug** (one of the two resize bugs): fixed by force-delete +
  full rebuild; desktop stack now 0 x18 refs. Guarded by `graft/deploy-check`.
- **FP/NEON never saved across task switches:** fixed in
  `../aros-upstream/arch/all-darwin/kernel/cpu_aarch64.h` (commit 7f55baf7). NEON
  blob in AROSCPUContext + PREPARE_INITIAL_CONTEXT.
- **RNG merge** with upstream (Nick's entropy.resource). Done, verified.
- **CrashLab** = `C:CrashLab` guest program, triggers each fault class
  (NULLREAD/NULLWRITE/NULLJUMP/WILDWRITE/ILLEGAL/STACKSMASH/DEADEND/GURU/
  BUSYLOOP/FORBIDLOOP/CRASHTASK), each prints `[CRASHLAB] NAME:`. Source:
  `../aros-upstream/developer/debug/test/crash/crashlab.c` (committed 53240bbe).
- **crash-smoke** = `graft/crash-smoke`, the before/after survival matrix. BEFORE
  matrix already captured: every fault class takes the instance down today.
- **`aros-ctl tasks`** = SIGINFO -> kernel `core_DiagHandler` -> walks all task
  lists + current, prints name/state/pri/sig + symbolized backtrace per task.
  WORKS ON A WEDGED GUEST. Committed 1afd2b4a (kernel+bootstrap) / 5ebdb35
  (aros-ctl). This is THE tool for any guest hang.

## 6. The resize deadlock, localized

Proven a REAL deadlock, not a stale-framebuffer illusion (offscreen framebuffer
identical over 6s, console.device stuck at same PC 6s apart, host threads healthy
in mach_msg/sigsuspend).

From the `aros-ctl tasks` dump during the wedge:
- **console.device task is BLOCKED in `InternalObtainSemaphore`, inside
  graphics.library `RectFill`** (a post-resize refresh redraw). It holds
  `ConsoleDevice->consoleTaskLock` (consoletask.c:189, taken right after its Wait)
  while rendering. RectFill does `LockLayerRom(&layer->Lock)` (locklayerrom.c:60)
  and blocks on the window LAYER lock.
- Every other task is in normal idle Wait. So the layer lock console wants is held
  by a task that then went idle WITHOUT releasing it. It takes the SECOND
  size-gadget drag to wedge (accumulated state), consistent with a lock leaked /
  held-across-Wait on the first drag.
- Scheduler/timer/dispatcher stay ALIVE (clipboard heartbeats forever); no [KRN]
  trap. 100% reproducible via `graft/resize-smoke` (fails at cycle 1).

**Reproduce:** `graft/resize-smoke` (torture-drags the size gadget, probes guest
liveness via pointer repaint). Or boot desktop, open a window, drag its size
gadget twice.

## 7. Uncommitted work in progress (in `../aros-upstream`)

Only one stray file remains modified on branch `crash-containment`:
- `workbench/c/KeymapEditor/keymapeditor.c` = stray/unrelated, from another
  thread. Do NOT commit it with this work; check with the user before touching.

---

## 8. Phase C: DONE (2026-07-02) -- resize deadlock found and fixed

The stack-scan forensics (commit 7ac8dafc, kernel.c) named the owner on the
first try:

```
task 'console.device' BLOCKED in InternalObtainSemaphore <- RectFill
    sp+144 -> semaphore ... '(unnamed)' owner=... 'input.device' nest=1 queue=1
task 'input.device' WAIT in WaitPort <- CDInputHandler <- ProcessEvents
```

The real cycle (NOT the consoleTaskLock inversion guessed earlier):
- console.device's input handler (CDInputHandler, pri 51) forwarded every event
  to the console task via a SINGLE static message then blocked in WaitPort for
  the reply.
- Intuition's size/drag gadget classes hold LockLayers() across the whole
  interactive drag (rubber band, windowclasses.c).
- The console task redraws with RectFill, which needs the window layer lock.
- Second drag: console task is still processing the first drag's SIZEWINDOW
  redraw, blocks on the layer lock now held by the drag; the next input event
  makes CDInputHandler wait for a console-task reply that never comes; the drag
  can never release. All input dead.

**Upstreamed:** PR https://github.com/aros-development-team/AROS/pull/819
(branch `fix/console-resize-deadlock` on the fork, cherry-picked onto master;
the PR keeps master's handler priority 50, our 51 is port-local from b009c855).

**Fix (commit ad65a609):** the CDIH -> console task handoff is now asynchronous:
one cdihMessage allocated per event, PutMsg without waiting, console task frees
it (order preserved by the port FIFO; on OOM the event is dropped). Principle
for upstream: an input handler must never wait on a task that renders.

Verified: `graft/resize-smoke` PASS 10/10 (previously FAIL at cycle 1), shell
typing works, window redraws correctly after resize.

Also: `graft/resize-smoke` FAIL path now uses `aros-ctl diag nokill` and leaves
the wedged instance running so `aros-ctl tasks` can autopsy it.

**Follow-up, also DONE (commit 05c401e1): stale redraw after resize.** Once the
deadlock was gone, resizing revealed the console never repainted at the new
size. Pre-existing upstream bug, NOT caused by the async change (verified by
instrumentation: no SIZEWINDOW ever reached CDIH, and by source): intuition
delivers window events for IDCMP-less windows by GENERATING input events from
its deferred-action path, and generated events are only visible to input
handlers BELOW priority 50; console.device's handler is at 51. The console
task's whole SIZEWINDOW/REFRESHWINDOW/CLOSEWINDOW/GADGETDOWN/UP arm was dead
code. Fix: a second internal handler at priority 0 forwards exactly those five
generated classes (unit matched by ie_EventAddress window for the first three,
active window for gadgets) through the same async port.

## 9. Phase D: DONE (2026-07-02, commit f11457f) -- host hardening

Both pieces live in `hosted/cocoametal/` (no bootstrap/kernel ABI change):
- **NSEvent ring:** a 20ms main-thread NSTimer (created with the window, common
  run-loop modes) drains NSEvents into a host CMEvent ring independently of the
  guest; `cm_pump_events` reads from the ring. No beachball with a dead guest.
- **Guest-progress watchdog:** same tick, measures time since the guest last
  pumped (pump recency, NOT a core_IRQ heartbeat: the scheduler stays alive
  during input wedges, the pump does not). Stale past `AROS_CM_WATCHDOG_SECS`
  (default 5, 0=off) => `[cm-watchdog]` log + SIGINFO task dump auto-fired once
  per stall episode.
Verified: typing/resize-smoke 10/10 through the ring; CrashLab FORBIDLOOP
produced exactly one watchdog fire + full auto-dump naming the spinning task.
Doc: docs/features/crash-handling/design.md.

## 10. RESUME HERE -- Phase E -- opt-in trap containment (guest, crash-containment branch)

In `rom/exec` trap handling (`rom/exec/traphandler.c` and friends): today every
CPU trap gets `AT_DeadEnd` -> ColdReboot. Add a boot-time flag (bootstrap
`arguments containment`, default OFF = today's behavior). When ON:
- a USER-task CPU trap becomes a recoverable guru + `RemTask` of the offender
  (instead of AT_DeadEnd/ColdReboot),
- SUPERVISOR / kernel faults stay dead-end (unsafe to continue).
Verify with the CrashLab AFTER matrix (`graft/crash-smoke`): recoverable fault
classes should leave the instance LIVE where the BEFORE matrix showed them down.

## 11. Phase F/G -- sweeps + full battery

- x18 leftovers sweep: external ports libs (zstd/freetype/png/tiff/jfif/lzma),
  FFView binaries, identify/popupmenu/codesets/expat/gadgets/ilbm (not in the
  earlier rebuild target list). Then the crosstools compiler-rt long-double
  builtins `__divtf3/__multf3/__addtf3` (real fix = rebuild the builtins archive
  with -ffixed-x18). `graft/deploy-check x18` for the full sweep.
- Full smoke battery green: resize-smoke, desktop-smoke, clipboard/audio/rust/
  ffmpeg smokes, `deploy-check`. Refresh `graft/upstream-patches` snapshot.

## 12. Key file references

Guest (`../aros-upstream`, branch crash-containment):
- `arch/all-unix/kernel/kernel.c` -- core_DiagHandler + forensics (Phase C tooling)
- `arch/all-unix/bootstrap/kickstart.c` -- SIGINFO mask, kick() threaded path (D)
- `arch/all-darwin/kernel/cpu_aarch64.h` -- NEON fix (done)
- `rom/exec/semaphores.c` -- InternalObtainSemaphore (the blocked frame)
- `rom/exec/traphandler.c` -- trap policy (Phase E)
- `developer/debug/test/crash/crashlab.c` -- CrashLab
- console.device consoletask.c (consoleTaskLock at :189), graphics locklayerrom.c

Host (`/Users/user/Source/aros-aarch64`):
- `graft/aros-ctl` -- control harness (has `tasks`)
- `graft/resize-smoke` -- deadlock repro (Phase C gate)
- `graft/crash-smoke` -- crash survival matrix (Phase E gate)
- `graft/deploy-check` -- x18 + deploy guard
- `hosted/cocoametal/` -- the Cocoa/Metal display driver (Phase D)

Memory (read these two for full background):
- `~/.claude/projects/-Users-user-Source-aros-aarch64/memory/resize-freeze-x18-stale-objects.md`
- `~/.claude/projects/-Users-user-Source-aros-aarch64/memory/stabilization-goal.md`
