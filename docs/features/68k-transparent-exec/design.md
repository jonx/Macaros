# Transparent 68k execution - design

> Status: design (drafted 2026-08-01) · Builds on the **built** engine in
> [../68k-jit/](../68k-jit/design.md) and the boundary design in
> [../68k-marshalling/](../68k-marshalling/README.md). Process:
> [../CLEANROOM.md](../CLEANROOM.md). The staged plan with verification lives in
> [plan.md](plan.md).

## Goal

A user on AROS (any target, hosted darwin first) launches a classic Amiga 68k
program the same way they launch anything else: type its name in the shell,
double-click its icon, run it from a script. If it is system-friendly it runs
through the JIT as a real AROS process. If it needs the Amiga hardware it is
routed to a full machine emulator, or fails with one clear, remembered question.
Nothing hangs, nothing gurus for a routing problem, and every failure leaves
behind data that tells us what to build next.

## What already exists to build on

| Piece | Where | State |
|---|---|---|
| 68k→AArch64 JIT engine, byte-exact, FPU incl. | [../68k-jit/](../68k-jit/README.md), `hosted/jit68k/` | built (host tool `run68k`) |
| Independent reference interpreter (pure C) | `hosted/jit68k/` (the byte-exact verification CPU) | built |
| Typed marshalling design + `genmodule` source of truth | [../68k-marshalling/](../68k-marshalling/README.md) | designed |
| Hunk loader on **every** arch (magic `0x3F3`) | upstream [rom/dos/internalloadseg.c:85](../../../../aros-upstream/rom/dos/internalloadseg.c) | in tree, today |
| Seglist 68k discriminator: DOS tags hunk seglists (`GSLI_68KHUNK`) and `RunCommand` already queries it before rejecting execution | upstream [rom/dos/runcommand.c:77](../../../../aros-upstream/rom/dos/runcommand.c), [getseglistinfo.c](../../../../aros-upstream/rom/dos/getseglistinfo.c) | in tree, today |
| Workbench 68k-detect + delegate-to-emulator hook | upstream [workbench/libs/workbench/uae_integration.c](../../../../aros-upstream/workbench/libs/workbench/uae_integration.c) (port `"J-UAE Execute"`) | in tree, dormant |
| Crash bundles (fault → self-contained `.tar.gz`) | [run68k.md](../../../hosted/jit68k/run68k.md), `[J5n]` diagnostics | built (host side) |
| Native symbolized guru | [../crash-handling/](../crash-handling/README.md) | built |
| Headless drive/verify harness | [../control-harness/](../control-harness/README.md) (`aros-ctl`) | built |

## Architecture

### 1. `emu68k.library` - the engine moves in-OS

The engine (today a host CLI) is repackaged behind an AROS shared library with
two internal seams:

- **CPU backend seam.** Two backends behind one contract (the existing
  `jit68k.h` / [INTERFACE.md](../68k-jit/INTERFACE.md) seam):
  - *JIT* (Emu68-derived decoders + AArch64 emitter): the fast path, aarch64
    hosts only.
  - *Interpreter* (our independent reference CPU, pure C): the portability
    floor. Runs on every CPU AROS targets, including x86_64. Slow is fine; it
    is the correctness baseline and the "runs everywhere" guarantee.
- **Executable-memory seam.** The only OS-specific piece:
  - hosted darwin: the existing `MAP_JIT` glue (`jit_region.c`), reached
    through `hostlib.resource` like every other host service;
  - hosted linux: plain `mmap`/`mprotect` + cache flush;
  - native (Pi): AROS owns the MMU, straight allocation.
  - The interpreter backend needs none of this.

Two engine properties the host CLI never needed become requirements in-OS:

- **Per-instance context.** Today the block cache, counters and diagnostic
  state are globals ([j5d_engine.c:255](../../../hosted/jit68k/j5d_engine.c),
  one process per host process). In-OS, several translated processes coexist:
  the engine state moves into a per-instance context handle (or, as an
  explicitly interim model, one serialized engine with a documented lock).
  This is an implementation-sized refactor, planned as its own proof
  (`[T0-P3]`), not a footnote.
- **Scheduler safe points.** Chained translated blocks can stay outside the C
  dispatcher indefinitely; on the hosted single scheduler thread a tight 68k
  loop would freeze all of AROS and make "killable" a fiction. Translated
  code must poll a break/quantum flag at back edges (chained loops included),
  returning to the dispatcher often enough for signals, kill, and scheduling
  to work. A forced host fault inside translated code must unwind to
  `RunSeg68k` and leave AROS usable.

**Licensing boundary.** The vendored Emu68 decoders are MPL-2.0 and stay
fork-side per the provenance posture. Everything else in this design (loader
hook, marshal-table generator, library shell, prefs, requesters) is clean,
independent work, kept separable from the engine ("no 68k engine installed"
degrades to today's behavior, `ERROR_OBJECT_WRONG_TYPE` style failure at
launch). **This feature is published to John's repos (`jonx/*`) only; no
upstream AROS submission is part of this plan** (see
[plan.md T6c](plan.md) and the upstream-pr approval gate).

### 2. Divert at the execution boundary, preserve LoadSeg data semantics

The first draft of this design wrapped every hunk seglist with a trampoline at
`LoadSeg`. That is too broad: `LoadSeg` is also AROS's loader for
*non-executable* hunk data, and those consumers inspect the seglist layout
directly (keymaps: [kms/openkeymap.c:117](../../../../aros-upstream/workbench/libs/kms/openkeymap.c),
disk fonts: [diskfont/diskfont_io.c:522](../../../../aros-upstream/workbench/libs/diskfont/diskfont_io.c)).
Prepending a segment would corrupt what they parse.

AROS already provides the right discriminator: DOS tags hunk seglists with
`GSLI_68KHUNK`, and `RunCommand` already queries `GetSegListInfo()` and
rejects non-ELF tracked seglists before jumping
([runcommand.c:77](../../../../aros-upstream/rom/dos/runcommand.c)). So:

- **LoadSeg stays data-faithful.** Hunk files load and relocate exactly as
  today; no wrapping, no layout change; keymaps and fonts unaffected.
- **Execution diverts at the jump, through one shared dispatch service.** The
  few places that turn a seglist into a running program (the `RunCommand` path
  for CLI/`SystemTags`, the `CallEntry`/`DosEntry` path for Workbench-created
  processes) ask the same `emu68k.library` router: where today they would
  reject a `GSLI_68KHUNK` seglist with -1, they instead hand it (plus launch
  context) to the router. Two thin consumers, one service; no other launcher
  changes.
- **A versioned launch-context record** travels into the router: original
  pathname, how launched (CLI args / complete Workbench startup message),
  icon tooltypes if any, explicit mode override (`Run68k MODE=`), console.
  The execution boundary does not naturally know these, so the consumers
  collect and pass them; the record is versioned from day one.

Transparency still comes for free at the user level: shell command lookup,
`Run`, scripts, `SystemTags()`, Wanderer double-click all end at one of those
two jumps. Exit code flows back from 68k `D0`; stdin/stdout are the launching
console. The 68k program is a **real AROS process**: own task, own console,
visible in task lists, killable (see the safe-point requirement in §1).

**The guest address model** (the load-bearing open problem). The engine's
sandbox already separates the logical 32-bit guest address space from
high host backing. AROS-side loading has to adopt the same split, because
"just allocate low" is not available on this host: upstream's hunk loader
hard-requires `MEMF_31BIT` on 64-bit targets
([internalloadseg_aos.c:196](../../../../aros-upstream/rom/dos/internalloadseg_aos.c))
while the darwin bootstrap documents that `MAP_32BIT` fails, the low 4 GiB
being unavailable
([arch/all-unix/bootstrap/memory.c:57](../../../../aros-upstream/arch/all-unix/bootstrap/memory.c)).
`[T0-P1]` decides between: parameterizing the upstream loader to allocate
from (and relocate within) the guest space; returning a proxy seglist whose
segments live in the sandbox; or promoting the engine's existing `[J4]`
sandbox hunk loader to be the execution loader, with DOS keeping only
identity/tracking. Whatever wins must answer seglist identity, lifetime and
`UnLoadSeg` for the new representation, and its proof is a real hunk loaded,
relocated, identified, run and unloaded, not merely an allocation below
4 GiB.

### 3. The routing ladder - `AUTO | JIT | FULL`

The JIT boundary is an API boundary. Programs that hit the machine directly
(custom chips at `$DFF000`, CIAs at `$BFExxx`, supervisor mode, self-installed
exception vectors) have no API to marshal; they need a full machine emulator
(model 1 in the [marshalling doc](../68k-marshalling/README.md)). Per-program
routing decides which engine gets each binary. Windows' per-shortcut
compatibility mode is the UX precedent; AmigaOS 4's `RunInUAE` is the
same-platform precedent; icon **tooltypes** are the Amiga-native carrier.

**Modes** (per program): `AUTO` (default) · `JIT` (force the translator) ·
`FULL` (delegate to the machine emulator).

**Where the setting lives**, highest precedence first:

1. Icon tooltype `EMU68K=AUTO|JIT|FULL` - the "shortcut properties" analog,
   travels with the program, editable in the Icon Information window.
2. Compatibility database `ENVARC:Emu68k/compat` - keyed by binary hash (so
   renames/copies keep their verdict), one record: hash, last-seen name, mode,
   origin of the verdict (user choice / auto detection / shipped default),
   timestamp. Covers iconless files and shell launches. Ships with a small
   seed of known entries; grows by learning (below).
3. System default from the prefs editor (`AUTO` out of the box).

**How `AUTO` decides** (the transparency trick - evidence, not questions):

- *Static scan at load time.* Over the relocated segments: absolute
  references into the chipset/CIA ranges, privileged instructions (`MOVE to
  SR`, `RESET`, `STOP`), writes targeting the exception-vector page. Cheap
  (one linear pass), catches most hardware-bangers before a single instruction
  runs. The outcome carries **confidence, not a boolean**: it is a *hint*
  (clear banger → FULL, clean library-only → JIT, ambiguous → JIT with
  guards), because a linear scan cannot distinguish code from data, sees
  opcode-looking constants, and misses computed addresses. The test corpus
  includes deliberate false-positive material (hardware-looking constants in
  data hunks, ambiguous flow) alongside true positives; the runtime guard is
  the authority.
- *Runtime guard.* The engine's EA path already bounds-checks and classifies
  every guest memory access, so hardware detection is an **address
  classification event inside the engine**, not a host page fault: a guest
  access resolving into the chipset/CIA ranges, or a write into the
  exception-vector page, raises a **routing event**, not a crash. Precise by
  construction: no chipset is mapped, so a hardware-banger cannot half-work
  and corrupt anything.
- *Remembered verdicts.* Whatever the user answers (or detection concludes)
  is written to the compat DB, so the next launch routes directly. The
  database teaches itself; steady state is zero questions.

**The one requester.** On a runtime hardware fault under `AUTO`:

> *"Program X touches Amiga hardware ($DFF180, custom chip DMACON area) and
> cannot run under 68k translation."*
> Buttons: **Launch in emulator** (if one is configured) · **Cancel** ·
> checkbox **Remember for this program** (default on) · link **Save report**.

Relaunch-in-emulator is usually safe (hardware-hitters bang the metal early,
and the static scan catches most before execution), but the requester wording
never promises state was preserved: the program is *relaunched*, not migrated.

**The `FULL` target.** Delegation, in order of arrival:

1. *Host-side emulator* (hosted darwin first): the host bridge launches a
   configured Mac emulator (path + argument template in prefs) pointed at the
   file. Same pattern as every other host service.
2. *In-AROS delegation daemon*: implement a listener on the `"J-UAE Execute"`
   message port protocol, which makes upstream's existing, dormant Workbench
   hook ([uae_integration.c](../../../../aros-upstream/workbench/libs/workbench/uae_integration.c))
   fire without touching workbench.library. **Caution: that hook forwards
   *every* detected 68k executable before the normal Workbench launch path**
   ([openworkbenchobjecta.c:87](../../../../aros-upstream/workbench/libs/workbench/openworkbenchobjecta.c)),
   so the daemon cannot be a dumb forwarder: it must itself run the full
   `AUTO|JIT|FULL` router (JIT-routed programs are launched back through the
   normal path), or its mere existence would force every Workbench 68k
   program to FULL.

Caveats owned by `FULL` mode: it needs a user-supplied Kickstart ROM image
(licensing: strictly opt-in configuration, like any UAE setup), and many
hardware-hitters ship as disk images rather than lone binaries, so `FULL`
accepts "open this file in the emulator" semantics for ADF and friends, not
just hunk executables.

**68k disk-based libraries: deferred, deliberately.** Classic apps
`OpenLibrary()` 68k libraries from disk (`libs:reqtools.library` etc.).
Loading one is not "just more 68k code": it drags in the resident/autoinit
lifecycle, library bases, dependency opening and expunge semantics, plus mixed
chains (68k app → native lib → 68k callback). v1 policy: **native libraries
only.** Same-name native library wins and is marshalled; a 68k disk library
with no native equivalent is a classified capability gap (ledger entry, polite
failure, FULL offered). In-sandbox 68k libraries get their own phase after
callbacks and object proxies work.

### 4. Marshalling: signatures are the index, not the semantics

The `.conf`-generated `lvo_desc[]` tables ([marshalling design
§1](../68k-marshalling/README.md)) tell us registers and scalar/pointer kinds.
They do **not** describe pointer direction, buffer-length relationships, deep
copy/copy-back needs, ownership/lifetime, or what an opaque `APTR` means, and
native structure layouts differ from 68k ones (pointer width, padding, byte
order). `Read(fh, buffer, len)` works on mapped guest bytes; `PutMsg(port,
msg)` does not, because native Exec then *traverses* a structure whose layout
is guest-shaped: that call needs a shadow structure or proxy object, not a
translated raw pointer.

Consequences, binding on the plan:

- The generated layer is **thunks plus per-function semantic annotations**
  (direction, lengths, copy policy, proxy class), with `lvo_desc[]` as the
  index. Annotations are hand-curated, ledger-prioritized, and a function
  without annotations is *unsupported*, never "best-effort".
- An early **schema spike** (`[T0-P4]`) must prove the annotation vocabulary
  on representative cases: a buffer API (`Read`), a traversed structure
  (`PutMsg`), an opaque handle, a callback hook, a taglist call, before T3
  scales anything.
- **No guessing, ever** (this supersedes any earlier wording): an unknown tag
  is not passed as a scalar; an unknown LVO does not fabricate a generic
  error return (functions differ in failure conventions). Both abort the
  translated call/process as a **classified capability gap**: ledger entry,
  polite failure, FULL offered if configured.

## User experience - tools and widgets

Principles: zero configuration for the happy path; at most one question, asked
once; every knob discoverable but none required.

- **Icon Information "Compatibility" page** (Wanderer): a cycle gadget
  `Auto / 68k translation / Full emulator`, writing the `EMU68K` tooltype. The
  Windows compat tab, Amiga-native.
- **`Emu68k` prefs editor** (Zune, standard prefs pattern:
  Save→`ENVARC:`, Use→`ENV:`):
  - default mode, engine selection (JIT/interpreter, mostly for debugging);
  - the FULL target: emulator path/argument template, Kickstart location,
    a "test launch" button;
  - the compatibility database, as a browsable list (name, hash, mode, how
    decided, last run, last outcome) with edit/clear/export/import;
  - the reports drawer (below) and the capability-gap view.
- **Shell commands** (thin wrappers over the same library calls):
  - `C:Run68k file [args]` - explicit launch, `MODE=JIT|FULL|AUTO` override;
  - `C:Emu68kMode file [AUTO|JIT|FULL] [SAVE]` - query/set a program's route;
  - `C:Emu68kWhy file` - explain the routing decision: which rule matched,
    what the static scan found, what the DB says. The debugging front door.
- **Running indicator.** A small "68k" badge on the screen title bar / status
  area while a translated process runs (reuse the
  [status-led-theme](../status-led-theme/README.md) surface), so "is this the
  emulated one?" is answerable at a glance.
- **System monitor integration.** Translated processes get an extra column/row
  in [system-monitor](../system-monitor/README.md): backend (JIT/interp),
  blocks translated, block-chain hit rate, dispatcher round-trips/s, sandbox
  size. The public face of the engine counters we already keep in `[J5n]`.

## Crash handling and the evolution loop

Every failure is one of three classes, each with its own UX and its own
artifact. The artifact side is the point: the solution evolves by measuring
what real programs need, not by guessing.

| Class | Meaning | User sees | We capture |
|---|---|---|---|
| **Routing event** | hardware touch under `AUTO` | the one requester, remembered | address + access type, static-scan summary, decision taken |
| **Capability gap** | call to an unmarshalled LVO / unknown tag / unsupported residue case | polite failure: *"X needs `intuition.library` function N, not yet supported under translation"*, offer FULL if configured | the **gap ledger** entry (below) |
| **Engine fault** | a real bug: bad translation, sandbox violation, marshal error | bounded guru per [crash-handling](../crash-handling/README.md), process dies, system lives | **crash bundle v2** |

**Crash bundle v2.** Extends the existing host-proven bundle (68k regs, block
disassembly, host state) with: program name + hash, engine version + backend,
the routing decision trail (`Emu68kWhy` output), the tail of the LVO call log
(last N library calls with marshalled args), sandbox page map, and the exact
68k PC → block → host PC mapping. Self-contained `.tar.gz`, written to a
`Reports` drawer on a writable volume (host-side `run/` dir on hosted). One
bundle per fault, bounded count (ring of the last K), never blocks the UI.

**Operational guarantees** (rule 4 needs mechanics, not intent):

- The fault path only fills a **preallocated snapshot buffer**; a separate
  worker task turns snapshots into `.tar.gz` bundles. No allocation, no
  filesystem, no compression inside the fault handler.
- If the Reports volume is read-only or full, the snapshot survives in a
  bounded in-memory ring and is also emitted on the serial/debug channel, so
  "every failure leaves an artifact" holds even with no writable disk.
- The compat DB and ledger are written by **atomic replace**
  (write-temp + rename), single-writer through the library (concurrent
  callers serialize on the library's lock), versioned records, and a corrupt
  file is renamed aside and rebuilt empty, never trusted. The DB hash is
  defined once: the file's byte content (not name, not date), streamed, with
  the algorithm and any size cap recorded in the DB header.

**The capability-gap ledger.** Every unmarshalled-LVO hit appends/increments:
library, LVO index + name (from the FD data), calling program hash, count.
Stored beside the compat DB, surfaced in the prefs editor ("what failed and
how often"), exportable as text. This is the data-driven roadmap: the next
library function to marshal is the one at the top of the ledger, not the one
we guess. The same ledger records unknown boopsi/MUI tags for the taglist
tables.

**Privacy/telemetry posture.** Everything stays on the machine. Reports and
ledgers are files the user can read, delete, and *choose* to send us. No
phoning home, nothing automatic.

## Failure-UX rules (binding)

1. Never a silent hang: every unsupported path ends in a requester, a shell
   error with `SetIoErr`, or a bounded guru.
2. Never a guru for a routing problem: hardware touches and capability gaps
   are events, not crashes.
3. One question maximum, remembered by default.
4. A failure with no artifact is a bug in *this* feature, not just in the
   program that failed.

## Open questions and risks

- **The guest address / loader representation** (§2) is the load-bearing
  unknown: `MEMF_31BIT` cannot be satisfied by host mapping on darwin, so
  `[T0-P1]` must pick the loader representation and prove the full
  load-run-unload cycle before anything else is built on it.
- **Engine re-hosting** (per-instance context, safe points, fault
  containment, §1) is implementation-sized work proven in `[T0-P3]`/`[T1]`,
  not a decision. Start per-process sandbox + cache; it is simpler and
  isolating.
- **Marshalling semantics beyond signatures** (§4): the annotation vocabulary
  is unproven until the `[T0-P4]` schema spike passes its five representative
  cases.
- **Callback re-entrancy** (native→68k hooks) doubles the marshal surface;
  scoped in the marshalling doc, lands late (`[T3]`+).
- **Taglist domain tables** are real, finite-but-large work; the gap ledger
  bounds it to what real programs actually use.
- **Kickstart ROM** licensing keeps `FULL` strictly user-configured.
- **Publication scope**: jonx repos only; no upstream AROS actions for this
  goal. The clean-vs-engine split is maintained anyway (cheap, keeps the
  option open should John ever decide otherwise).
