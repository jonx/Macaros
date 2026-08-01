# Transparent 68k execution

**Status: design + plan** (drafted 2026-08-01, no implementation yet). The umbrella
feature that turns the built [68k JIT engine](../68k-jit/README.md) and the
[typed marshalling boundary](../68k-marshalling/README.md) into something a user
never has to think about: double-click or shell-type a classic Amiga 68k program
on AROS and it just runs.

Three ideas, one ladder:

- **Transparent launch.** Preserve LoadSeg data semantics, divert at the
  execution boundary: upstream already loads, relocates and tags classic hunk
  seglists on every architecture (`GSLI_68KHUNK`), so the two places that jump
  into a seglist (`RunCommand`, the Workbench `CallEntry` path) route tagged
  seglists through one shared dispatch service, and the shell, `Run`, scripts,
  and Wanderer all work unchanged.
- **Per-program routing** (the "run this under Win95" idea, Amiga-native). Every
  68k binary is routed `AUTO | JIT | FULL`: the JIT for system-friendly programs,
  delegation to a full machine emulator for hardware-bangers, and an evidence-based
  `AUTO` default (static scan + runtime guard + remembered verdicts) so the user
  answers at most one question, once.
- **Crashes as fuel.** Every failure is classified (routing signal, capability
  gap, engine bug), handled with a bounded, polite UX, and captured as a
  structured report; the capability-gap ledger is what drives which library gets
  marshalled next.

## Docs

- [design.md](design.md) - architecture: `emu68k.library`, the execution-boundary
  dispatch and guest address model, the routing ladder, the customization tools
  and widgets, crash handling and the evolution loop
- [plan.md](plan.md) - the detailed staged plan (`[T0]` four proofs, then
  `[T1]`..`[T7]`), with per-phase verification and exit criteria
- Companions: [68k-jit](../68k-jit/README.md) (the engine, built) ·
  [68k-marshalling](../68k-marshalling/README.md) (the boundary design) ·
  [crash-handling](../crash-handling/README.md) (the native guru this extends)
