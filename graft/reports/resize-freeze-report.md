# Report: the window-resize freeze on hosted AROS (aarch64-darwin)

Date: 2026-07-02.
Branches: `crash-containment` in `../aros-upstream` (guest fixes),
`graft/aros-ctl-diag-keymap-boot` here (tooling).

## 1. Summary

"Resizing a Wanderer window freezes the whole app" turned out to be three
independent defects stacked on top of each other. Each one masked the next:

| # | Defect | Where | Status |
|---|--------|-------|--------|
| 1 | Stale objects compiled without `-ffixed-x18` | build system | fixed (rebuild) + guarded |
| 2 | Input-chain deadlock on the second size-gadget drag | console.device | fixed, `ad65a609` |
| 3 | Console never repaints after a resize | console.device / intuition event routing | fixed, `05c401e1` |

Bugs 2 and 3 are long-standing upstream AROS bugs, not artifacts of this port.
Bug 3 was invisible until bug 2 was fixed, and bug 2 was hard to see clearly
until bug 1 stopped corrupting random registers.

Verification gate: `graft/resize-smoke` (10 grab-drag-release cycles with a
liveness probe) went from FAIL at cycle 1 to PASS 10/10, and a resized console
now re-lays-out and repaints correctly at the new size.

## 2. Background: how console input works

Three actors matter, all running as AROS tasks inside the single hosted
process:

- **input.device task** runs the input handler chain. Every input event
  (mouse, key, timer) is passed through a priority-sorted list of handlers.
  Two priorities matter here: console.device's handler at **51** and
  intuition's at **50** (higher runs first).
- **intuition** (running inside the input.device task via its handler)
  implements window dragging and sizing. While the user drags a size gadget,
  intuition's gadget class holds `LockLayers()` on the screen, freezing all
  layer rendering until the drag is released. That lock is held *across* many
  input events; the handler chain keeps running while the drag is in
  progress.
- **console.device task** renders console windows. Its input arrives from the
  handler above through a message port. Rendering goes through
  graphics.library (`RectFill` etc.), and every rendering call takes the
  target window's **layer lock**.

The freeze lived entirely in the interaction of these three.

## 3. Bug 1: stale non-`-ffixed-x18` objects (build system)

macOS zeroes register x18 on every signal or kernel entry, and the hosted
port delivers its 50 Hz scheduler tick as a signal. Any AROS code that keeps
a live value in x18 is therefore corrupted at a random instruction whenever a
tick lands. `-ffixed-x18` was already in `make.cfg`, but a make.cfg flag
change does not invalidate compiled `.o` files, so large parts of the desktop
stack were still running pre-flag objects. Symptom: silent guest wedges and
busy-loops under load (redraw storms during resize), with no kernel trap.

Fix: `find gen -name '*.o' -not -path '*tools*' -delete`, then a full
metatarget rebuild. Clean modules disassemble to at most 4 x18 references
(the accepted `Forbid/Permit` pattern in exec). `graft/deploy-check` now
fails any curated module above that limit, so this class of staleness cannot
silently return. Known accepted leftovers (toolchain compiler-rt long-double
builtins, external ports libs) are tracked as the Phase F sweep.

This bug was the *first* flavor of the resize freeze (a busy-loop). Removing
it exposed a deterministic deadlock underneath.

## 4. Bug 2: the input-chain deadlock (console.device, upstream bug)

### Symptom

100% reproducible: drag the console window's size gadget once, fine; start a
second drag and the entire GUI freezes forever. The host app then beachballs
(amplifier: NSEvents are only dequeued by the guest, see Phase D of the
handoff). Proven a real deadlock, not a display illusion: the offscreen
framebuffer stayed byte-identical for 6 s while host threads were healthy.

### Diagnosis method

Two pieces of tooling were built for this:

- **`aros-ctl tasks`**: sends SIGINFO to the hosted process; a kernel handler
  (`core_DiagHandler`, `arch/all-unix/kernel/kernel.c`) walks all task lists
  and prints each task's state and a symbolized backtrace *from its saved
  context*. It works on a fully wedged guest because it rides the host signal
  path, not the guest scheduler.
- **Semaphore forensics** (commit `7ac8dafc`): for each blocked task, the
  dump scans the saved callee-saved registers (x19-x28) *and the task's
  stack* (from the saved SP, bounded by `tc_SPUpper`, capped at 16 KB) for
  pointers to valid `NT_SIGNALSEM` nodes, and prints each with its owner
  task. A task blocked in `InternalObtainSemaphore` keeps the semaphore
  pointer live in a register or spilled on its stack, so this names the other
  side of a deadlock directly. The register scan alone was not enough; the
  compiler had spilled the pointer, and it was found at `sp+144`.

The dump during the wedge:

```
task 'console.device'  BLOCKED in InternalObtainSemaphore <- RectFill
    sp+144 -> semaphore (unnamed) owner='input.device' nest=1 queue=1
task 'input.device'    WAIT in WaitPort <- CDInputHandler <- ProcessEvents
```

### The cycle

`CDInputHandler` (console.device's priority-51 input handler) forwarded every
relevant event to the console task through a **single static message** and
then blocked in `WaitPort()` until the console task replied. That design
plus intuition's drag behavior forms a classic ABBA deadlock:

```
first drag release        second drag start
       |                         |
       v                         v
console task:              input.device (intuition gadget class):
  processing SIZEWINDOW      LockLayers()  <- holds ALL layer locks
  redraw (many RectFills)    for the whole interactive drag
       |                         |
       v                         v
  RectFill blocks on the    next input event reaches CDInputHandler,
  layer lock now held by    which PutMsg's to the console task and
  the drag                  WaitPort()s for its reply
       |                         |
       +------ neither side can ever proceed ------+
```

The console task cannot reply because it is blocked on the layer lock; the
layer lock cannot be released because the drag never finishes; the drag never
finishes because the input.device task is stuck inside `WaitPort` and never
processes another event. All input on the system is dead.

Why the *second* drag: the first drag's release posts a SIZEWINDOW redraw to
the console task. That redraw is a long burst of RectFills, each taking and
releasing the layer lock. The immediate second grab wins the layer lock
between two RectFills, and the trap closes.

### Fix (commit `ad65a609`)

Make the handoff asynchronous. One `cdihMessage` is allocated per forwarded
event, `PutMsg` without waiting, and the console task frees the message after
processing. Event order is preserved by the port FIFO. Under memory pressure
an event is dropped rather than blocking the chain. The static message, its
reply port, and its SIGB_INTUITION signal hack are gone.

The invariant this encodes, and the reason the fix is upstream-correct rather
than a workaround: **an input handler must never wait on a task that
renders.** Rendering can always block on layer locks that input processing
itself (a drag) is holding.

## 5. Bug 3: stale redraw after resize (console.device / intuition routing, upstream bug)

### Symptom

With the deadlock gone, resizing finally worked, and immediately revealed the
next layer: enlarge a console window and the newly exposed area is never
painted; the text keeps the layout of the old size. Shrinking looks fine only
because clipping needs no repaint.

### Diagnosis

Instrumentation on both ends showed the console task received **zero**
SIZEWINDOW or REFRESHWINDOW events, ever, on the old code as well as the new.
The routing explains it:

- Console windows are opened by con-handler with **IDCMP = 0**, so when
  intuition finishes a resize it cannot deliver an IntuiMessage. Its
  fallback (`ih_fire_intuimessage`) *generates an input event*
  (`IECLASS_SIZEWINDOW`, window pointer in `ie_EventAddress`).
- Generated events are appended to the event chain that intuition's own
  priority-50 handler *returns*, so they are visible only to handlers
  **below** priority 50.
- console.device's handler is at priority **51**, above intuition. It can
  never see a generated event.

Consequence: the console task's entire
`SIZEWINDOW / REFRESHWINDOW / CLOSEWINDOW / GADGETDOWN / GADGETUP` arm was
dead code. The console never learned a window's new size. This also means
the console window's close gadget and scroller events were routed into the
void.

### Provenance

This bug predates all work on this port. Nothing in the deadlock fix touched
event routing, priorities, or the class filter, and the instrumented old code
showed the same missing events. It was simply unobservable before: the
instance deadlocked on the second drag before a stale redraw could ever be
seen. Fixing bug 2 unmasked bug 3.

### Fix (commit `05c401e1`)

console.device now installs a **second, internal input handler at priority
0**, below intuition, that forwards exactly the five intuition-generated
classes to the console task through the same asynchronous port:

- `SIZEWINDOW`, `REFRESHWINDOW`, `CLOSEWINDOW`: the unit is matched by the
  window pointer carried in `ie_EventAddress`. This is correct for inactive
  windows too, and events for non-console windows match no unit and are
  ignored.
- `GADGETDOWN`, `GADGETUP`: these carry the gadget, not the window, so they
  fall back to the active-window match.

No double delivery is possible: those five classes only exist as
intuition-generated events, so the priority-51 handler never sees them, and a
window that *does* listen to IDCMP gets an IntuiMessage instead of a generated
event. The public priority-51 handler (`CDInputHandler`, LVO 7) keeps its
exact semantics for rawkey and raw mouse/timer input, so keyboard behavior is
untouched.

## 6. Verification

- `graft/resize-smoke`: PASS 10/10 cycles (before bug 2's fix: FAIL at
  cycle 1; the smoke's FAIL path now uses `aros-ctl diag nokill` and leaves
  the wedged instance running so `aros-ctl tasks` can autopsy it).
- Manual: shrink then enlarge re-renders the full text at the new width
  (a previously clipped line comes back complete), typing into the shell
  works, borders and scroller draw correctly.
- The rebuilt modules disassemble to 0 x18 references (deploy-check guard).

## 7. Upstream relevance

Both guest fixes are self-contained in `rom/devs/console/` and are candidates
for the upstream PR queue (they are on the `crash-containment` branch, which
holds only upstream-clean work):

- `ad65a609` console.device: asynchronous input handler (fixes the deadlock).
- `05c401e1` console.device: receive intuition-generated window events
  (fixes resize redraw, and incidentally revives the console close gadget and
  scroller event path).
- `7ac8dafc` kernel (hosted): semaphore forensics in the SIGINFO task dump
  (diagnostic tooling, hosted-only).

An alternative upstream design worth mentioning in the PR discussion: moving
console.device's single handler below intuition entirely (classic AmigaOS has
it below) would also fix bug 3, but it changes rawkey delivery order and
risks regressions; the two-handler split keeps the public handler's semantics
exactly and is the minimal safe change.
