# Crash handling — a bounded guru with a symbolized backtrace

> Status: **built** · Target: aarch64-darwin hosted · 2026-06-27
> Lives in the OS tree, not this repo: `../aros-upstream/arch/all-unix/kernel/`
> (`kernel.c`, `kernel_intern.h`). Rebuild + boot recipe: see the memory note
> *Hosted AROS rebuild & boot* (`make kernel-kernel`, headless econsole boot).

## What & why

When the hosted AROS hit a fatal CPU fault (SIGSEGV/SIGBUS/SIGILL/SIGFPE), the
window did **not** stop — it printed the same guru dump (registers + backtrace)
over and over, forever, until you `Ctrl-C`'d the terminal:

```
[KRN] Trap signal 10, SysBase ... 
    X0 = ...  PC =0000000100A3E284 ...
[KRN] Backtrace (innermost first): pc=0000000100a3e284
[KRN]   <- 0000000100a5f154
... (identical block, repeated to infinity)
```

Two problems: (1) the loop buries everything else and makes the window useless
after any crash; (2) the addresses are raw hex — you can't tell *what* faulted.
This feature fixes both, entirely inside the hosted trap handler
`core_TrapHandler()` in `arch/all-unix/kernel/kernel.c`.

## Why it looped (the root cause)

The hosted "trap" is a real POSIX signal; `core_TrapHandler` is the `sigaction`
handler. After printing the dump it dispatches the fault to the exec trap path:

```
core_Trap()  // static inline in rom/kernel/kernel_intr.h
  -> task's tc_TrapCode (the exec guru / Alert)
```

That guru path **redirects the PC** into the alert routine and ultimately ends up
re-executing the *same* faulting instruction. So returning from the signal
handler restarts the faulting instruction, which re-faults identically, which
re-enters the handler — forever. Crucially, every pass `pc != PC(regs)` (the PC
was redirected), while the **faulting PC never changes**.

A first attempt halted only when `pc == PC(regs)` ("PC not redirected"); because
the guru *always* redirects, that branch never ran. The working fix keys off the
invariant that actually holds: **the faulting PC at handler entry is identical
every iteration.**

## How it works

### 1. Loop breaker (stops the spam)

At the very top of `core_TrapHandler`, before any dispatch:

- Capture `pc = PC(regs)` (the faulting instruction) and whether the signal is a
  hard fault (`SIGSEGV/SIGBUS/SIGILL/SIGFPE`, `+SIGSTKFLT` on Linux).
- Keep `static` `loop_sig` / `loop_pc` of the previous fault. If a hard fault
  re-enters at the **same** `(sig, pc)`, it is not making progress → print one
  line and stop the host:

  ```
  [KRN] Trap re-faulting at pc=... (signal 10) -- unrecoverable; halting host.
  ```

  A genuine recovery would not re-fault at the same PC, so this never fires on
  forward progress.
- A separate **re-entrancy guard** (`in_trap`) catches a *nested* fault — one
  that hits inside the dump/symbolizer itself — and halts immediately rather
  than recursing.

"Halt the host" = `_exit(20)` via the host libc. `_exit` was added to the hosted
libc interface (`struct KernelInterface` in `kernel_intern.h` + the
`kernel_functions[]` resolve table in `kernel.c`, at matching slots). The macOS
process exits cleanly, so the window closes after the dump instead of looping.

Net effect: a crash now produces a **bounded** report (the first fault, plus the
duplicate that confirms the loop, then the halt line) and the window closes.

### 2. Symbolized backtrace (says *what* faulted)

Each backtrace line is run through `krnSymbolize()`, which resolves an address to
`module symbol + offset` and appends it. Real capture (a NULL function-pointer
call surfaced during display bring-up — `PC=0`, called from locale.library):

```
[KRN] Trap signal 11 [h2], ...  PC =0000000000000000
[KRN] Backtrace (innermost first): pc=0000000000000000
[KRN]   <- 00000001009625b0  locale.library Locale_25_OpenCatalogA + 0x330
[KRN]   <- 0000000100960264  locale.library Locale_2_Locale_CloseLib + 0x1c
[KRN]   <- 00000001007457ac  exec.library Exec_74_OpenDevice + 0xa0
[KRN]   <- 00000001007906a0  lddemon.resource Lddemon_0_OpenDevice + 0x68
[KRN]   <- 000000010093e504  Cocoa seg 1 (.text) + 0x24e8
```

Where a function symbol is found it prints `module symbol + offset`; otherwise it
falls back to `module seg N (name) + offset` (e.g. the `Cocoa` shim above, and
ARM `$x` mapping symbols in stripped `.text`).

Mechanics:
- The bootstrap hands the kernel a `KRN_DebugInfo` module list
  (`arch/all-hosted/bootstrap/bootstrap.c`), so **debug.library** has every
  kickstart module registered *with its ELF symbol tables*.
- `krnSymbolize` finds the debug base by name in `SysBase->LibList`
  (`FindName(..., "debug.library")`) — a read-only list walk, cached. We do **not**
  call `OpenLibrary` (which may `Wait`/allocate and is unsafe in a trap handler).
  Note: debug is a *library*, not a resource — an earlier `OpenResource(
  "debug.resource")` silently returned NULL, which is why symbols didn't appear.
- It calls `DecodeLocationA()`, which is explicitly safe in supervisor/crash
  context: when `KrnIsSuper()` it skips its semaphore and only walks the
  (read-only) module list (`rom/debug/decodelocation.c`). This is the same
  machinery exec's own `Alert` backtrace uses (`rom/exec/alertextra.c`).
- If the library or address is unknown it prints nothing extra and the raw
  address still stands on its own — so it degrades gracefully.

Why this is safe where the kernel's own `KrnPrintBacktrace` LVO was not: that
risk was the *stack walk* (already solved by the inlined frame-pointer chain).
`DecodeLocation` never touches the stack — it walks the separate, intact module
registry.

### 3. NULL-pointer-call annotation

A call/jump through a NULL (or near-NULL) function pointer faults with `PC` at
(near) address 0 — there is no code there, so a bare `pc=0000000000000000` is
opaque. When `PC(regs) < 0x1000` the handler says so outright and points at the
caller (`LR` holds the return address of the bad call, i.e. the code that made
it), symbolized:

```
[KRN] *** Call through a NULL pointer: PC=0x0 has no code; caller LR=0x100960820  locale.library Locale_2_... + 0x...
```

## Verifying

- The dump's first line carries a **`[h2]`** tag
  (`[KRN] Trap signal 10 [h2], ...`) — a quick visual confirmation that the
  hardened handler is the one running (vs. a stale kernel).
- Rebuild just this module: in the hosted mmake tree,
  `make kernel-kernel` → lands in `.../AROS/boot/darwin/Devs/kernel.resource`,
  the same module `graft/run-window.sh` boots.
- Headless smoke test (no GUI window): boot with `arguments econsole nomonitors`,
  bounded by a SIGKILL watchdog; a clean boot reaches the `1>` emergency shell
  with zero traps.

## Status

- **Loop breaker** — confirmed in the window: a re-faulting SIGBUS now prints the
  `halting host` line and the process exits instead of spinning.
- **Clean GUI boot** — confirmed: full Cocoa display + input + clipboard, no fault.
- **Symbolizer** — confirmed: a live crash printed named frames (see the capture
  above), turning raw `0x1009xxxx` addresses into `locale.library
  Locale_25_OpenCatalogA + 0x330`, etc.

## What it immediately surfaced

The first symbolized crash already paid for itself: a **NULL function-pointer
call** (`PC=0`) reached via the Cocoa input driver opening a device —
`Cocoa → exec OpenDevice → lddemon OpenDevice → ... → locale.library
OpenCatalogA + 0x330`. That OpenCatalogA path calling through a NULL pointer is
the real bug to chase next; the loop breaker keeps it from drowning the log while
we do.

## The out-of-band task dump (`aros-ctl tasks`, 2026-07-02)

Traps only cover faults; **hangs** need a tool that works when the guest
scheduler is wedged. `aros-ctl tasks` sends SIGINFO to the host process; the
kernel handler (`core_DiagHandler`, same `kernel.c`) rides the host signal
path (it converges on the AROS thread because the host threads block it), so
it runs no matter what the guest is doing. It walks current + TaskReady +
TaskWait and prints, per task: name/state/pri/sigWait/sigRecvd and a
symbolized backtrace from the saved context.

**Semaphore forensics:** for each blocked task the dump also scans the saved
callee-saved registers (x19-x28) *and the task's stack* (from the saved SP,
bounded by `tc_SPUpper`, capped at 16 KB) for pointers to valid
`NT_SIGNALSEM` nodes, printing each with its owner task and nest/queue
counts. A task blocked in `InternalObtainSemaphore` keeps the semaphore
pointer live in a register or spilled on its stack, so this names the other
side of a deadlock directly. This is the tool that cracked the window-resize
deadlock (see `graft/reports/resize-freeze-report.md`).

## Host watchdog + event ring (Phase D, 2026-07-02)

Host-side hardening in `hosted/cocoametal/` so a wedged guest can neither
beachball the app nor hide:

- **NSEvent ring:** a repeating 20 ms main-thread timer (created with the
  window, common run-loop modes) drains NSEvents into a host CMEvent ring
  independently of the guest; `cm_pump_events` reads from the ring. macOS
  always sees the event queue serviced, so the app stays responsive
  (move/close/menus) even with the guest dead. Consecutive mouse moves
  coalesce in the ring.
- **Guest-progress watchdog:** the same tick measures time since the guest
  last called `cm_pump_events`. Pump recency is the right liveness signal:
  it stops immediately on an input wedge even while the guest's 50 Hz timer
  and scheduler keep running (a scheduler heartbeat would have missed the
  resize deadlock). When stale past `AROS_CM_WATCHDOG_SECS` (default 5,
  `0` disables), it logs `[cm-watchdog]` and fires the SIGINFO task dump
  once per stall episode, so every future hang self-captures its autopsy in
  the log. Re-arms when pumping resumes.

## Possible follow-ups

- Offline symbolizer in `graft/` (dump + per-boot module map → `addr2line` /
  `llvm-symbolizer` on the ELF modules) for source `file:line`, since in-kernel
  resolution stops at symbol granularity.
- Optionally gate the verbose register dump behind a flag once symbolized
  backtraces are the primary signal.
