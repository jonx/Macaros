# AROS-AArch64 — Roadmap

The big-picture arc, so the plan doesn't live only in a chat log. Phase 1 has its
own detailed milestone checklist in the
[QEMU bring-up notes](docs/hosted/qemu-virt/PHASE1.md);
this file is the map
above it and the rationale for the ordering. Granular cross-cutting tasks (the
"we'll get to it" backlog) live in [TODO.md](TODO.md).

## The one rule that shapes everything

An AI agent must be able to run the **whole loop unattended** — build → boot →
observe → judge — with no manual step. Every phase is chosen and ordered to keep
that true. It's also why the hard CPU work happens on QEMU, not on real Apple
hardware: QEMU exposes the machine programmatically (serial, QMP, gdbstub), and a
locked-down MacBook does not.

## Two independent hard problems — never worked at once

1. **The AArch64 CPU backend** — vectors, MMU, context switch, exception model.
   Greenfield, reusable on *every* ARM64 target, and the genuine contribution.
2. **Apple Silicon specifics** — undocumented hardware, custom interrupt
   controller, signed boot. Deferred as long as possible.

Phase 1 does #1 entirely on QEMU. Phase 2 reaches the Mac by going *hosted* (AROS
as a process on macOS, macOS owns the drivers) — which sidesteps #2 almost
entirely. Native-on-Apple-Silicon is explicitly a non-goal.

---

## Phase 0 — Toolchain & harness ✅ (done)

The autonomous loop itself. LLVM cross-toolchain (clang + ld.lld + lldb) + QEMU,
a `make run` that builds an AArch64 ELF, boots it headless on QEMU `virt`, and
emits one uniform PASS/FAIL verdict. Observation channels validated on the Mac:
serial markers, QEMU fault trace, lldb CPU-state. See [NOTES.md](NOTES.md).

## Phase 1 — AArch64 backend on QEMU `virt`  ✅ (done)

Brought the lowest layer up on 64-bit ARM, on a fully observable target.
Milestones **M1…M10**: serial → C runtime → exception vectors → MMU → timer IRQ →
physical memory → context switch → shell → framebuffer → **preemptive
multitasking**. All green in the loop (`make test`); detail in
[QEMU bring-up notes](docs/hosted/qemu-virt/PHASE1.md),
retrospective in [NOTES.md](NOTES.md). Standalone
deliverable: AROS's first *native* AArch64 bring-up.

## Phase 2 — Hosted on macOS  (in progress)

AROS as a native arm64 macOS process, macOS owning every driver. Rather than
attempt the full port at once, **de-risk the scary parts cheapest-first** — each
a standalone, grounded, loop-verified spike (`make hosted-*`, all green via `make
hosted-test`). Done so far:

- **H1/H2 foundation + preemption** — our bare-metal context switch runs at EL0 in
  a macOS process; SIGALRM-as-timer + `mcontext` swap gives hosted preemption.
- **H3 host-call ABI shim** — the make-or-break boundary (it killed Darwin-PPC).
  Bridges AROS→Apple's arm64 ABI; grounded the divergences (variadic-on-stack &c)
  against the live compiler. ✅ primary risk retired.
- **H4 scheduler / H5 memory / H6 composition** — the real AROS `exec` shapes,
  hosted and grounded against the tree: `core_Schedule`/`cpu_Switch` + priority
  `TaskReady`; the `MemHeader`/`MemChunk` allocator over `mmap`; the two composed
  into a tiny exec with `Forbid`-safe allocation.
- **H7 display** — AROS draws a framebuffer from its own heap; macOS presents it
  (ImageIO PNG), observed unattended like M9's screendump. (A live on-screen
  window is deferred — verifying it unattended needs Screen-Recording permission.)
- **H8 library/LVO** — a tiny `exec.library` via the real jump-vector mechanism +
  `SetFunction`; grounded that 64-bit AROS uses data-pointer vectors, so there's
  **no Apple-Silicon W^X / MAP_JIT wall**.

**Remaining — the graft (the honest mountain):** stop spiking and integrate the
real AROS tree. Grounded entry points are mapped in [GRAFT.md](GRAFT.md): AROS has
a *darwin hosted* backend (`arch/all-darwin` + `arch/all-unix`) but it lacks
AArch64 and is bit-rotted to ~2010 Xcode. The punch list — add the `aarch64` case
to `configure`'s darwin flavour, write `arch/all-darwin/kernel/cpu_aarch64.h` (the
H2/H4 `mcontext` glue), fix the wrong `arch/aarch64-all` `ExceptionContext`, point
the crosstools at native `clang -arch arm64`, then drive `configure`/`mmake` — is
large-scale integration against a backend nobody's built in a decade, not a
session-sized spike.

Note: AArch64 binaries share their software island with the ARM Pi world, not the
big x86 AROS catalog — so app availability is a Phase-2/3 concern (recompiling
contrib apps), not something that comes for free.

## Phase 3 — Polish & upstream  (planned)

Shake out ABI edge cases, get a real application ecosystem building, and submit to
the `aros-development-team` tree per their CONTRIBUTING guidelines so it doesn't
bit-rot as a private fork. The standalone deliverable even if Phase 2 stalls:
**AROS's first *native* AArch64 backend**. (Grounded check of the upstream tree:
`arch/aarch64-all` is header-only scaffolding for an unfinished *hosted-Linux*
flavour — real atomics, an incomplete `ExceptionContext`, no kernel — so there's
no native boot/vectors/MMU/context-switch to duplicate. We reuse and fix those
headers at the graft. See NOTES.md.)

---

### Observability is the through-line

Each phase keeps a faithful "way of seeing" so the loop never needs a human:
QEMU's serial/QMP/gdbstub in Phase 1; macOS's own debugging + a window in Phase 2.
If a step can't be observed unattended, it gets redesigned until it can.
