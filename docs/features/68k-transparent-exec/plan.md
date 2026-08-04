# Transparent 68k execution - the staged plan

> Status: active implementation (T0-T2 complete; T3a/T3b0/T3e complete;
> first production T3b/T3c type-policy slice complete, 2026-08-02). Design and rationale:
> [design.md](design.md). Marker prefix for this feature: **`[T*]`** (greppable,
> same convention as the engine's `[J*]`). Every phase ends in the unattended
> loop: build → boot → drive with [aros-ctl](../control-harness/README.md) →
> one PASS/FAIL.

Ordering logic: prove the in-OS execution spine first (`T1`), make failure
handling excellent early (`T2`, because every later phase produces failures we
want captured), then scale coverage (`T3`), then polish UX (`T4`), then the
emulator hand-off (`T5`), then portability (`T6`). `T7` is the standing
performance track. Phases after `T1` overlap freely; the listed dependencies
are the only hard ones.

## [T0] Four proofs before the spine (medium; was "groundwork", upgraded after review)

T0 is no longer a decision list: each item is a working proof, because each
one, wrong, forces a rework of the loader, engine API or marshaller in T1/T3.
Decisions and outcomes recorded in [NOTES.md](../../../NOTES.md).

- **`[T0-P1]` Guest address model + loader representation.** ✅ **PROVEN
  2026-08-01** (`make hosted-emu68k-t0p1`, `hosted/emu68k/t0p1_seglist.c`).
  Representation chosen: the **proxy seglist** — guest payloads relocated
  with guest addresses by the `[J4]` loader; LoadSeg returns a native
  `[BPTR next][descriptor]` chain (fast-BPTR identity for `GetSegListInfo`,
  blind-walk `UnLoadSeg` compatible); the guest arena teardown hangs off the
  seg-registry removal, so identity and lifetime share one mechanism. Full
  rationale + alternatives: the NOTES.md entry of the same date. Exit bar
  met: two real hunk binaries (integer + hardware FP) loaded, relocated,
  identified (positive + negative), run byte-exact from the proxy chain
  alone, unloaded leak-free. Byproduct: first-hand confirmation that the
  engine is single-run-per-process (stale chained blocks in freed JIT memory
  on a second run), sharpening `[T0-P3]`.
- **`[T0-P2]` Execution interception + launch context.** ✅ **PROVEN
  2026-08-01** (OS-side commit `1eb9082854` on `aarch64-darwin-graft`,
  pushed to the jonx fork; verified on booted AROS). `rom/dos/dos_emu68k.h`
  defines the versioned `Emu68kLaunchCtx` (origin CLI/PROC, seglist, name,
  args, process, mode); `rom/dos/emu68k_route.c` is the stub router (reports,
  declines). Both boundaries wired: `RunCommand` (CLI context incl.
  `cli_CommandName` + argument string) and `DosEntry` (process context; only
  when the entry PC was derived from the seglist, so `SystemTagList`'s
  explicit-entry shell case stays native). Enabler discovered on the way:
  hunk `LoadSeg` FAILED entirely on darwin (`MEMF_31BIT` unsatisfiable), so
  64-bit targets now fall back to plain memory for the hunk allocation;
  nothing interprets the truncated relocations (execution diverts, the
  keymap loader already declines >4 GiB seglists, and `ReadDiskFont` now
  declines them the same way). New `C:RunSeg` exercises the created-process
  path. Verified live: both boundaries log correct context for a real hunk
  binary, launches fail politely, desktop boot (fonts included) stays clean.
- **`[T0-P3]` Re-entrant engine, safe points, fault containment.** ✅ **PROVEN
  2026-08-01** (`make hosted-emu68k-t0p3`, `hosted/emu68k/t0p3_engine.c`).
  Landed in the engine itself: per-instance state (`j5d_engine_new/activate/
  free`; the historical globals are `#define` views of the active instance, so
  translated blocks bake the OWNING instance's addresses), an emitted safe
  point at every block **chain entry** (covers fully-chained loops; C-side
  poll covers the rest), `j5d_request_stop()` (async-signal-safe) +
  poll/YIELD/KILL with resumable yields (`J5D_RC_YIELD`, resume from
  `st->pc`; the run's SP baseline + call depth survive in the instance), and
  host-fault containment (`sigsetjmp` in `j5d_run`, `siglongjmp` from the
  `[J5n]` handler after bundling — program dies, embedder lives). Exit bar
  met: two instances interleaved on one thread byte-exact; two sequential
  same-process runs (the old single-run crasher); a chained infinite loop
  killed from a signal handler; a real translated-code SIGSEGV unwound to a
  clean error with a bundle and a reusable process. Corpus + diagnostics
  regression green with safe points emitted in every block.
- **`[T0-P4]` Marshalling schema spike.** ✅ **PROVEN 2026-08-01**
  (`make hosted-emu68k-t0p4`, `hosted/emu68k/t0p4_marshal.c`). The annotation
  vocabulary (`ARG_U32 / PTR_IN / PTR_OUT+len / CSTR / HANDLE / SHADOW /
  HOOK / TAGLIST` + per-field shadow maps + per-domain tag kinds) survives
  all five cases, driven by one generic descriptor-driven marshaller: buffer
  writes land in guest memory with clean bounds faults; a natively-traversed
  struct works via a host-layout shadow with big-endian OUT-field copy-back;
  handles validate against a table; a native→68k hook re-enters the engine
  NESTED and returns its D0 end-to-end through the real jsr-d16(A6) LVO
  bridge; unknown tags and unknown LVOs abort as ledger-recorded capability
  gaps (the design §4 no-guessing rule, executed). Vocabulary revision the
  spike forced: **marshalling is two-pass** — scalars (lengths) first, then
  pointer kinds — because a buffer's length can be declared after the buffer,
  and single-pass order bounds-checks against length 0 (caught as a real
  64-byte heap overwrite). Nested-run prerequisite landed in the engine: the
  fault-recovery registration is a save/restore pair.
- **`[T0a]` Engine-as-library split** (unchanged, enables P3/P4): ✅ **DONE
  2026-08-01** — `make libjit68k` builds `build/libjit68k.a` (the exact engine
  object set; CLI + stub-OS stay with the front-end), `run68k` and the
  `[T0-P1]` proof both link it. Corpus green (`j5m`/`j5t`/`apps`/`j5l`/`args`
  byte-exact). Gotcha, documented at the Makefile target: consumers must link
  `-Wl,-force_load` (weak-default/strong-override pairs across archive
  members; lazy archive loading silently drops the strong overrides).
- **`[T0d]` Naming + layout** (unchanged): `emu68k.library`,
  `hosted/emu68k/`, engine stays in `hosted/jit68k/`; compat DB + prefs
  formats versioned from day one (`EMU1` header).

Exit criteria: all four proofs PASS, decisions in NOTES.md, corpus green.

## [T1] A 68k process inside AROS - "it runs from the shell" ✅ DONE 2026-08-01

**Real classic-Amiga 68k binaries run from the booted AROS shell**, byte-exact
against the host engine. Graft commits through `libemu68k.dylib` + the engine
chain budget; OS-side `7165e883ce` + follow-up on `aarch64-darwin-graft`.

What shipped: `build/libemu68k.dylib` (the engine behind a quantum-run API:
create from hunk bytes, bounded quanta, streaming output sink, async kill,
contained faults; smoke `make hosted-emu68k-t1dyl`) · `emu68k.library`
(`arch/all-darwin/libs/emu68k`, binds the dylib through `hostlib.resource`,
runs the program as the calling process, console output, CTRL-C between
quanta, run semaphore) · the real DOS router (`rom/dos/emu68k_route.c`)
completing the launch context from a source-image stash the hunk loader keeps
with the registry node (freed at unload) · the `[T1d]` capability-gap ledger
(per-LVO hit counts + program name; unmarshalled call = classified abort, never
a guess) · `C:RunSeg` for the created-process path.

Verified live in one boot (`run/darwin-aarch64/t1x-195732`): hardware-FP `j5t`
(717 bytes) and `fact` byte-exact from the shell; **two translated processes
interleaved** (background Dhrystone + foreground Mandelbrot, both byte-exact);
a capability gap reported and survived; a real translated-code SIGSEGV
contained with a crash bundle, the next 68k program running fine after it; a
fully-chained infinite loop killed with `Break`; the system alive throughout.

Engine change this forced: the chain-entry safe point now decrements a **chain
budget** instead of only testing a flag, because a self-chained loop never
returns to C on its own, so nothing in-OS could poll CTRL-C.

Original plan text follows.

The transparency spine. Depends: T0.

- **`[T1a]` `emu68k.library` skeleton.** Open/close/expunge, engine init via
  the exec-mem seam (hostlib → `jit_region` on darwin), per-process engine
  instance (`[T0-P3]` shape), one public entry:
  `RunSeg68k(seglist, launch_context)` running the caller's process context.
- **`[T1b]` Execution-boundary divert, for real.** Land the `[T0-P2]` shape
  in the graft's dos.library build (non-m68k guard): the `RunCommand` and
  `CallEntry` consumers hand `GSLI_68KHUNK` seglists plus launch context to
  the router. LoadSeg data semantics untouched. Behind a boot switch
  (`EMU68K=off` host arg) until T2's guards land.
- **`[T1c]` Minimal marshal set, hand-written.** Promote the stub-OS: startup
  (argv/argc from the shell command line, `pr_CLI` conventions), dos I/O
  (`Output/Input/Write/Read/Seek/Open/Close/IoErr`), exec basics
  (`AllocMem/FreeMem` from the guest pool, `OpenLibrary/CloseLibrary` name
  resolution), exit via `D0` → shell return code. Enough for every corpus
  binary and Dhrystone.
- **`[T1d]` LVO gap logging from day one.** Any LVO outside the hand set hits
  the logger (library + LVO + program hash + count) and **aborts the
  translated call/process as a classified capability gap** (no fabricated
  error returns; failure conventions differ per function). The ledger exists
  before the first real program does.
- **`[T1e]` Harness.** `aros-ctl`-driven test: copy corpus binaries + a
  rebuilt `dhry.exe` to a volume, run each from the AROS shell, compare
  output byte-exact against the host `run68k` reference, check exit codes.

Exit criteria (the review's five, plus the originals):

- all corpus binaries + Dhrystone run from the booted AROS shell, byte-exact
  output, correct exit codes;
- two translated processes interleave safely (concurrent output correct);
- an infinite-loop 68k guest is killed from the shell while native tasks and
  the desktop stay live (safe points proven in-OS, chained loops included);
- a forced translated-code host fault unwinds to `RunSeg68k`, the process
  dies, AROS remains usable;
- `C:` native commands unaffected (boot smoke green); keymap/diskfont
  loading unaffected; gap ledger records a deliberate unknown-LVO probe.

## [T2] Routing ladder + failure UX — **`[T2a]`+`[T2b]` DONE 2026-08-01**

The detection half of the ladder is live and verified on booted AROS: a program
that drives the Amiga hardware is routed away with a sentence naming the exact
register, by the static scan when it can be seen and by the runtime guard when
it cannot. `[T2c]`/`[T2d]`/`[T2e]` (the requester, the compat database and
tooltype, crash bundle v2) are still open.

- **`[T2a]` static scanner + `scan68k` CLI** ✅ (`make hosted-emu68k-t2scan`).
  Confidence-graded, and the calibration work was the substance: a naive 2-byte
  sweep reads operands as opcodes and flagged *every* real program, so the scan
  walks true instruction boundaries with a length table and stops rather than
  guessing a resync; with exact lengths the absolute operand is located instead
  of guessed; and relocation sites are excluded, because `move.l #0,$64`
  initialising a global is not an exception-vector write. Zero false positives
  across the whole corpus. `RTE` is deliberately NOT a banger signal: the engine
  implements 68k exception dispatch, so a program with a handler must not be
  routed away.
- **`[T2b]` runtime guard** ✅ (`make hosted-emu68k-t2guard`). Implemented as
  absence rather than checking: the low 16 MiB of guest space is reserved
  PROT_NONE and only the arena window inside it is writable, so the hardware
  genuinely is not there and a touch faults; the classifier turns the faulting
  host address back into the guest address and reports the register. Zero hot-path
  cost. Two traps had to be fixed first: a malloc'd arena does not fault past its
  end (so the arena became a real mapping), and `mprotect` rounds a length UP,
  which put the CIA registers back inside the writable window.

Original plan text follows.

Make failure excellent before scaling coverage. Depends: T1.

- **`[T2a]` Static scanner, with confidence semantics.** One pass over
  relocated segments: chipset/CIA absolute refs, privileged opcodes,
  vector-page stores. Emits a confidence-graded hint, not a boolean
  ([design §3](design.md)). **Deliverable includes a standalone diagnosis
  CLI** (`scan68k <binary>`, host tool first): the same scanner core exposed
  as "how would this run here", reporting the predicted route + the evidence,
  plus (once `[T3a]` tables exist) the static LVO list checked against
  marshal coverage — predicted capability gaps BEFORE running. Run-and-see
  stays the authority (the runtime guard is exact); the tool is the
  inspectable prediction, and `C:Emu68kWhy` (`[T4c]`) reuses its core. Test corpus, positives *and* negatives: new
  `apps68k` members `chipbang.s` (writes `$DFF180`), `ciapeek.s`,
  `vecwrite.s`, `superviolate.s`, plus `datadecoy.s` (hardware-looking
  constants in data hunks), `opdecoy.s` (opcode-looking constants),
  `computedhw.s` (address computed at runtime; must slip the scan and be
  caught by `[T2b]`). Negatives must route JIT.
- **`[T2b]` Runtime guards.** Guest-address classification in the engine's
  bounds-checked EA path: chipset/CIA-range accesses and vector-page writes
  raise routing events (not crash bundles, not host page faults).
- **`[T2c]` The requester + verdict memory.** The single question from
  design.md, with remember-checkbox writing the compat DB. Requester works
  from any process (Intuition EasyRequest path); shell-only sessions get the
  text equivalent.
- **`[T2d]` Compat DB + tooltype + precedence.** `ENVARC:Emu68k/compat`
  (content-hash-keyed, versioned records), `EMU68K` tooltype read from the
  launch context, precedence icon > DB > default. With the operational
  guarantees from [design](design.md): atomic replace, single-writer via the
  library lock, corrupt-file quarantine + rebuild, hash definition in the
  header. `FULL` with no emulator configured says so and offers Cancel only.
- **`[T2e]` Crash bundle v2.** Extend the host bundle with program hash,
  routing trail, LVO-log tail, sandbox map. Fault path fills a preallocated
  snapshot only; a worker task writes the `.tar.gz` to the Reports drawer;
  bounded ring of K bundles; in-memory + serial fallback when the volume is
  read-only/full.

Exit criteria: each `T2a` test binary is caught (statically or at runtime) and
routed per the ladder; verdicts persist across reboots; a forced engine fault
produces a v2 bundle and a live system; `aros-ctl` script covers all paths.

## [T3] Generated marshalling at scale — STARTED 2026-08-01 (the bootstrap slice works)

The AmigaOS library bootstrap runs end to end on booted AROS: a 68k program
reads SysBase from absolute address 4, opens `dos.library` through exec's
`OpenLibrary`, and its `Output()`/`Write()` calls are performed by the REAL
native dos.library. Pieces: multi-libbase recognition in the engine, a guest
exec (read-only low page + OpenLibrary serving per-library bases, AllocMem from
the guest heap), the `oscall` seam where the embedder performs native calls,
and `emu68k_oscall.c` in-OS with the first hand-marshalled set
(dos I/O: Input/Output/Open/Close/Read/Write/Seek/Delay). File handles cross
the boundary as opaque TOKENS (a native BPTR is 64-bit and a 68k register is
32; truncation was the first live failure, exactly as `[T0-P4]` predicted).
`make hosted-emu68k-t3hello` is the host regression. The generated `[T3a]`
table now sits BEHIND this hand-written set: a call the hand-written switch
does not claim falls through to the derived crossings, so a crossing that
needs judgement is never silently replaced by one inferred from a signature.
Still ahead here: printf-class varargs, the tier-2 structure types, callbacks.

**The standing rule** (see NOTES.md, 2026-08-02): bridge to AROS's own
implementation by default; implement in the guest ONLY when the result cannot
cross the boundary (a guest base, guest-arena memory, a structure the program
walks). MatchFirst therefore calls the native MatchFirst into a native
AnchorPath and copies back the fields the program reads - it does NOT
reimplement AmigaDOS pattern matching.

## STATUS 2026-08-03: a program that builds its own Intuition structures runs; the next stop is a second 68k context

PhotoDemo (a 1995 commercial paint demo) now gets through its whole GUI setup:
it opens its screen and window, builds a `Gadget` family in memory it allocated
itself, attaches it with `AddGList`, refreshes it, draws, and reaches its
helper library.

Three things it needed, all of which were general rather than specific to it:

- **Guest-owned object adoption.** Classic Intuition code allocates its own
  `Gadget` list and hands it over; nothing issued a handle, so the crossing had
  nothing to resolve. A structure the program owns is now mirrored natively
  under its guest address, re-validated and converted on every crossing, adopted
  whole as a family, with the library's relinking written back as guest
  addresses. See `emu68k_object_adopt_guest`; the gate is `T3OWNGAD` plus the
  uncarryable-field and cycle controls.
- **Generated guest-structure offsets.** `tc_SPLower` was two bytes out, so any
  program reading its own stack bounds read a wrong pointer. Those offsets are
  now derived from the headers and locked by the layout tool's self-check.
- **Importer reach.** A structure the program owns crosses by value; a pointer
  stored into a helper's own scratch is not a lifetime obligation; nullability
  is read from the library testing the pointer; a fully resolved tag domain
  crosses and carries itself into the policy; and an entry the importer wrote
  stays derivable, so later rules reach vectors already promoted.

### DONE since: message ports

`PutMsg`, `GetMsg`, `ReplyMsg`, `Signal`, `AllocSignal`/`FreeSignal` and the
satisfiable half of `Wait` are in, gated by `T3PORT`. Every structure a port
involves is the program's own memory, so these are guest-memory list operations
plus the signal that makes a `WaitPort` return; handing them to the native exec
would give it addresses it cannot dereference and put the program's messages on
a list it cannot see. Ports are the shape every interactive Amiga program is
written in, and there were none.

A `Wait` nothing can satisfy is a NAMED gap, not a hang: with one context there
is nobody to send the signal, and an unbreakable wait inside translated code
would have to be killed from outside to find out why.

### Before any device I/O: make OpenDevice deterministic

The same build opened `keyboard.device` in one run and failed in the next, with
no input difference. That has to be settled before command marshalling, because
an intermittent open makes every result after it untrustworthy - and
uninitialised request DATA cannot explain the open itself changing outcome, so
stale slot or request state is the first suspect, ahead of leaked signals.

The contract, its invariants and the determinism matrix that separates the
causes are in `graft/emu68k-runtime-contracts.json` under `device.open`, which
is where an agent picks them up. Two negative results already narrow it: the
guest allocator is a clean bump allocator and does NOT hand out overlapping
blocks, and an above-arena address must stay a routing verdict - reclassifying
it as a fault turned two correctly-routed memory scanners into crashes, which
the legacy corpus caught immediately.

### The next piece: a 68k process

**Correction to the design below.** Threads are the wrong shape. Both contexts
are 68k, everything they share is guest memory, and they communicate only
through signal bits - so they never need to run at the same instant, only to
make progress. Run them COOPERATIVELY on one thread, which the engine already
proves it can do (T0P3: "two instances interleaved on one thread"), and the
locking, the shared-state races and the `ExecBase->ThisTask` problem all
disappear - only one context is current, so that one word is always right.

The switch point is `Wait`: it runs the OTHER context nested (the same nested
entry `emu68k_run_call_hook` already uses) until the signal it wants arrives or
that context blocks back. Bounded by the number of contexts, no scheduler state
machine. The one case to refuse by name is a context that would have to re-enter
one already on the stack: that is a genuine deadlock and must be reported, not
recursed into.

What is still needed:

- a context struct - its own engine instance, 68k state, guest stack and guest
  `Task` - created by `CreateNewProc` from `NP_Entry`/`NP_StackSize`/`NP_Name`;
- `FindTask(NULL)` answering the CURRENT context rather than the one fixed
  guest Process;
- `Wait` switching as above.


`dos.CreateNewProc` is the current stop, and it is a subsystem rather than a
capability gap. PhotoDemo's helper library asks for `NP_Entry` (68k code),
`NP_StackSize` 16K and `NP_Name`. The spawned code is a textbook worker:

```
FindTask(NULL) -> a3
loop:  WaitPort(&a3->pr_MsgPort)
       GetMsg  -> a2 ; exit when a2->offset24 == -1
       ...do the work...
       ReplyMsg(a2)
```

So it spends nearly all its time blocked, which is what makes this tractable:

- create a real AROS process whose entry is a native thunk, and enter the run at
  the guest `NP_Entry` on a guest stack of the requested size (the nested-entry
  path `emu68k_run_call_hook` already proves re-entry works);
- serialize 68k execution in one sandbox with a lock, since two contexts share
  guest memory and one JIT arena. They need to make progress, not to run at the
  same instant;
- release that lock around the vectors that BLOCK (`Wait`, `WaitPort`,
  `GetMsg`-with-wait), marked as such in the policy. Without this the first
  `WaitPort` deadlocks the pair, which is the whole risk of the design;
- give each context its own guest `Task`/`Process`, because `FindTask(NULL)` and
  `pr_MsgPort` are per-process and the run currently has exactly one at a fixed
  guest address.

Open question worth deciding first: whether a vector the bridge cannot serve
should always abort the run, or whether some (`CreateNewProc` among them) should
return the failure the API already defines, which is what a real Amiga does when
it is out of memory and what programs are written to handle.

## STATUS 2026-08-04: the waterline pivot — guest-side SYSTEM libraries

**Decision (John): stop bridging tail libraries; run real m68k libraries
guest-side above the waterline.** Rationale and evidence in
[emuv0-reference.md](emuv0-reference.md); standing rules in the
`waterline-pivot` memory. The former import queue (icon, diskfont, asl,
workbench, datatypes) is cancelled as bridging targets.

Mechanism state after the first spike (AROS m68k `gadtools.library` from the
20260804 nightly, driven by the `gengadget` fixture, all committed):

- `EMU68K_GUESTSIDE_LIBS` (comma-separated leaf names) routes a name past the
  bridged-servable list into the T3e guest loader; a missing 68k file fails
  the open rather than silently falling back, so bridged vs guest-side runs
  stay comparable. `EMU68K_LIBS_PATH` supplies host-side library dirs.
- Reserved (zero) LVO slots in a resident's vector table load fine and fail
  at CALL time: the slot is filled with ILLEGAL, so the fault pc names base
  and LVO. Real system libraries have such slots; xpk never did.
- The engine's `move to/from CCR` dispatcher existed but was unreachable
  (over-tight guard also required `40xx`); m68k gadtools hit it immediately.
  Unknown terminators now report opcode and pc.
- `intuition.new_object`'s tag domain grew the imageclass (IA_) and
  gadgetclass (GA_) families: scalars as u32, `GA_Image`/`GA_LabelImage` as
  Object, `GA_Previous` as Gadget, `GA_DrawInfo` as DrawInfo, `GA_TextAttr`
  as struct, `GA_Text` as cstr; pointer-ambiguous tags stay absent and fail
  closed.

How far it gets: gadtools loads, constructs its base, inits, opens its five
(all-bridged) dependencies, survives DrawInfo/pen queries, pooled exec
allocs, text measurement, builds its frame image via bridged `NewObjectA`,
crosses its label as **GA_IntuiText** (deep-marshalled: policy
`follow_fields` → generated `EmuFollow`/`EmuStructDesc` tables → whole
reference graph rebuilt run-lifetime via `emu68k_persist_alloc`, because the
class RETAINS the label), and passes its own guest context gadget as
**GA_Previous** (object tags with `"adopt": true` carry their mirror and
adopt guest-owned structures).

**Current frontier: `MakeClass` with guest dispatchers.** gadtools creates
its 11 BOOPSI kind classes at init (the 11 `MakeClass` calls, `d0` =
instance sizes) with 68k dispatcher code, then `NewObjectA` on that guest
classPtr returns NULL. The T3BOOPSI hook machinery already proves
native→guest class dispatch; `MakeClass` needs to build a native `IClass`
whose dispatcher trampolines into the guest hook. After that:
`gengadgetbad` control guest-side, then iffparse → locale in dependency
order. Bridged baseline (`t3gen` and friends) green throughout.

Verification rule reinforced twice today: BSD `grep` silently lacks `\|`
alternation (false negatives), the shell wrapper skips non-UTF-8 files —
check file content with python, and check bytes, not greps, before
diagnosing "the file changed".

## STATUS 2026-08-02: T3e complete; a real disk-library chain runs unchanged

9 run; ADocReader fails with its own message (no MUI installed); AMIGAPeek and
DSPPeek are correctly ROUTED as needing a real machine, which is the right
answer for a memory scanner rather than a bug. LhA compresses, lists and
extracts byte-exact under `make hosted-emu68k-t3lha`.

**What remains in T3.** Nothing below should be assumed to exist:

| piece | state |
|---|---|
| tier 2b generation from the field tables | first production slice built: generated `DateStamp` IN/OUT shadows and guest-argument pointer result mapping; broaden to more value structures and proxies |
| structures with structures in them | done: the layout tool flattens nested records to their leaf scalars (taking one member per union, since the members are the same bytes), converts arrays of scalars element by element, and refuses to emit a row whose believed width would reach into the next field. `struct Preferences` crosses whole and is compared against a native oracle |
| a size argument as a bound | done: a `size_arg` in the policy clamps the crossing to the bytes the caller asked for, on the range check as well as the copy. An element that would cross the bound is not written at all, and an array fills as far as it reaches |
| values in a register PAIR | done: a `double` argument or result is the raw 64 bits across the two registers the `.conf` already names (`D0/D1`), so the IEEE double libraries generate from their prototypes with no policy at all. `mathieeedoubbas`, `mathieeedoubtrans`, `mathieeesingtrans` and `mathtrans` are served, 62 crossings |
| a guest library shadowed by a native one | done: `LIBS:` on a booted AROS holds native libraries, so the first name match was an ELF object where a hunk file had to be. A candidate that is not a hunk file no longer ends the search, and `PROGDIR:libs/` is searched, which is where a package keeps its own |
| clone (per-open) library bases | done: opens are tracked one entry per open, not one per library, so a library that hands a fresh base to each opener works and each base closes on its own. `clonelib.s` is the artifact (a library that clones itself from its own `lib_NegSize`/`lib_PosSize`), covered by `make hosted-emu68k-t3guestlive` |
| a tag whose value is a structure | done: a domain tag may be `struct`, and the crossing rebuilds what the guest pointer points at into caller-provided scratch for the length of the call. `POINTERA_BitMap` is the first, which also needed a structure field that is an array of guest pointers to raw bytes (`BitMap.Planes`): a decision written down per field, since the same shape could equally be an address that must never be rebased |
| a native pointer the program has to hold | `AllocRaster` allocates in the guest instead, which is both an address a 68k program can keep and where the `BitMap` crossing expects planes. Any vector handing back memory the program writes into wants the same treatment |
| BOOPSI attributes of a GUEST-DEFINED class | NOT done, and not what Photogenics needed after all: it uses the SYSTEM classes (`pointerclass`, `sysiclass`, `buttongclass`, `propgclass`), whose attributes are describable. A guest-defined class remains open, and for one the taglist should cross structurally rather than being translated. A program that makes its own class with `MakeClass`/`AddClass` then creates objects of it passes attributes only that class understands, so there is nothing to describe and nothing to translate: for a guest class the taglist should cross structurally, tag and 32-bit value, the way it already does for a guest `Hook`. Selecting that path means knowing the named class is the guest's own, which `AddClass` is the place to record. Photogenics stops here |
| the policy schema (per-type class) | schema v1 built and checked; two TagItem domains plus DateStamp direction/result policy compiled, broader domain/object/callback coverage remains |
| broad integer conformance | shifts, bit operations, MULS/DIVU and unary operations covered; continue from the conformance ledger |

### [T3e] Third-party 68k libraries, run IN the guest — ✅ DONE 2026-08-02

This is the piece that makes the system general rather than a list of handled
cases. AROS's own libraries are bounded and the generators cover them; the
third-party surface is not bounded and never will be, so it must not be
bridged. A `.library` on disk is 68k code, and the JIT already runs 68k code.

The real proof is the freely redistributable `xpk_User 5.2a` package, pinned by
archive SHA-256
`ec48c2820e45e64e1c1970592f06dc32c64546a6c7d545b39c79b469012879f3`.
Its unmodified `xQuery` program opens the unmodified 68k
`xpkmaster.library`, which in turn opens the unmodified 68k
`compressors/xpkNONE.library`. The complete chain reports the packer name,
description, mode, chunk limits and speed/memory table under both the hosted
proof and a rebooted AROS system. No third-party binary is checked into this
repository; `graft/68k-xpk-query` verifies and extracts the pinned archive.

This is a resident/autoinit lifecycle, not merely another hunk load. The
implementation is split into proofs so a synthetic direct initializer cannot
accidentally stand in for the format real libraries use:

1. **Two truthful test libraries.** A non-AUTOINIT resident whose `rt_Init` is
   executable 68k code, and an `RTF_AUTOINIT` resident whose `rt_Init` points to
   the real four-long construction table (`dataSize`, vectors, structure-init,
   final init function). The latter must exercise vector construction rather
   than laying its library base out by hand.
2. **Find and validate the requested resident.** Scan every loaded CODE/DATA
   hunk for `RTC_MATCHWORD` (`$4AFC`) plus the self-referencing `rt_MatchTag`;
   validate `rt_EndSkip`, strings, flags and every pointer against the complete
   loaded segment set; select `NT_LIBRARY` by the name OpenLibrary requested,
   not merely the first resident in the file. The self-reference finder and its
   real-program negative control are built (`scan68k --resident`).
3. **Construct the library in guest memory.** For AUTOINIT, implement the m68k
   semantics of `MakeLibrary`/`MakeFunctions`/`InitStruct`: allocate the
   negative vector area and positive base together inside the sandbox, build
   six-byte 68k vectors from the absolute or relative function table, apply the
   structure-init stream, copy the Resident identity fields, then call the
   final init function with `D0=base`, `A0=seglist`, `A6=SysBase`. For a
   non-AUTOINIT resident call `rt_Init` with `D0=0`, `A0=seglist`, `A4=0`,
   `A6=SysBase`; it owns construction itself.
4. **Distinguish library-base kinds.** A registered native façade means
   `BRIDGED_NATIVE`: reaching a vector invokes the typed AROS bridge. A disk
   library means `GUEST_68K`: reaching a vector pushes the ordinary return
   address and executes the guest vector stub/code. A single untyped base list
   is wrong because it sends both kinds to the bridge.
5. **Implement OpenLibrary lifecycle.** Resolve through DOS semantics
   (`LIBS:`, `PROGDIR:`/the launch directory as applicable), enforce the
   requested version, maintain `LOADING/READY/FAILED` states so dependency
   cycles are detected, register only after successful initialization, and
   invoke the library's Open vector for each successful open.
6. **Close, expunge and rollback.** Route `CloseLibrary` to guest code, maintain
   ownership/open state, honour delayed expunge, remove the base before freeing
   code, unload the seglist returned by Expunge, and unwind every allocation and
   dependency after a failed init. A recursion-depth bound alone is not enough;
   cycles need explicit state.
7. **Real-software proof.** Run a real third-party library and a program using
   it end to end, then pin the dependency chain as a regression. `xpkmaster`
   and `muimaster` are the first corpus candidates, not substitutes for a
   deliberately small lifecycle test.

Three regressions divide the evidence cleanly. `make hosted-emu68k-t3guestlib`
proves resident selection, direct and AUTOINIT construction, relative and
absolute MakeFunctions tables, InitStruct and `GUEST_68K` vector dispatch in
isolation. `make hosted-emu68k-t3guestlive` drives a real program through the
live exec seam: native AROS DOS resolves `LIBS:`/`PROGDIR:`, synthesized guest
continuations finalize init/Open/Close only after their 68k routines return,
requested versions are enforced, Expunge unregisters before reload, and an
A→B→A initializer dependency fails without publishing either base. A later
valid open proves the failure path restored the loader state.
`EMU68K_XPK_ARCHIVE=/path/xpk_User.lha make hosted-emu68k-t3ereal` supplies the
real program/library dependency proof and asserts the full metadata output,
not just successful process exit.

Expunge now reaches a permanent safe-point trampoline only after the guest
Close/Expunge routine has returned, unregisters the base, invalidates the
run's entire translated-block cache (including incoming cross-block chains),
clears the old guest image and trampolines, and then redirects through a
permanent RTS. If the allocation is at the bump allocator's tail, its guest
address range is immediately reusable; interleaved allocations become cleared
holes rather than being unsafely reused. A reload regression proves the next
open constructs a fresh library and no stale translation survives.

The final multi-library regression also exposed and removed a fixed-address
collision that the earlier one-library tests could hide: the second and third
native library facades overlapped the planted Process/CLI/file-handle records,
and the sixteenth facade overlapped the callback stub library. The low guest
map is now disjoint by construction: runtime records occupy `$210000` through
`$2127ff`, the stub facade is based at `$21e000`, Exec at `$220000`, all 16
native facades span `$221000` through `$230000`, and allocator space starts at
`$231000`. The generated bridge, XPK dependency chain, LhA roundtrip, guest
library lifecycle/reload and broad JIT suite all pass with this layout.

The real package found four compatibility defects that the synthetic fixtures
did not: `exec/StackSwap`, the Shell `cli_CommandName` BSTR, preservation of
the 68k ABI's D2-D7/A2-A6 registers across asynchronous guest library
continuations, and basename selection for residents reached through a path
containing `/`. It also required guest-memory implementations of the
pointer-sensitive core utility tag/string walkers. These are generalized
runtime fixes, not XPK-specific vector stubs.

`locale.library/OpenCatalogA` remains intentionally fail-closed until the type
compiler can generate a TagItem shadow plus opaque Catalog handle. The bridge
first offers the call to the native crossing; if that crossing is unavailable,
it returns NULL, the API's normal "no catalog" result, so clients use their
built-in strings without receiving an invalid native pointer.

The engine and library registry are per run. v1 deliberately does not promise
that a disk library's globals are shared between separate translated processes;
libraries that create tasks, devices or interrupt servers remain classified
gaps until those guest services exist.

Exit criteria: **met.** Both resident forms construct and return a guest base; public
vectors run as guest code without touching the native bridge; dependency
failure and A→B→A cycles roll back cleanly; Open/Close/Expunge leave no live
base pointing into released memory; and one real third-party library works
through a rebooted AROS corpus run.

**The evidence gap that matters most.** The corpus is 12 binaries, and there
are no others on this machine. Everything above is a claim about twelve
programs, not about legacy 68k software as a class. Scaling the corpus and
fixing gaps in ledger-FREQUENCY order (rather than one program at a time, which
is what today was) is the work that would turn it into one.

**A debugging rule this port paid three wrong guesses for in a single day.**
Never diagnose a 68k fault by inference; print the value. The jump-table
theory, the size-overrun theory and "my engine change broke GetAsmIncludeIndex"
were all wrong, and each was settled in one run by an actual number.
`EMU68K_TRACE_FAULT` names the decoded guest address of an unclassified fault,
`J5G_TRACE` gives per-block PCs, `EMU68K_TRACE_CALLS` gives library calls.

Original plan text follows.

Depends: T1; informed continuously by the `T1d` ledger.

- **`[T3a]` Generated crossings from the `.conf` files. FIRST SLICE DONE
  2026-08-02.** `graft/gen-emu68k-bridge` parses every `##begin functionlist`
  block (full C prototype + m68k register map per vector) and emits
  `emu68k_gen_<lib>.c` into the OS tree. 64 crossings across dos, exec,
  utility, intuition, graphics, icon and commodities, none hand-written.
  `make hosted-emu68k-t3gen` is the regression: it fails if a generated file
  drifts from the `.conf` it came from, then proves the emitted crossings work
  in-OS with a 68k program.

  What is generatable is bounded by the **tier** split, which is the durable
  idea here:

  | tier | what crosses | cost |
  |---|---|---|
  | 1 | scalars, and pointers to bytes (a C string, a raw buffer) | free, generated |
  | 2 | a structure, a BPTR, an untyped APTR | one description per **TYPE**, shared by every function using it |
  | 3 | the callee retains the pointer, or calls back into the guest | hand-written, always |

  `--report-all` classifies **every** `.conf` in the tree, which is what sizes
  the remaining work honestly: **175 libraries, 2069 public vectors, of which
  309 (15%) are tier 1**. The other 1760 are blocked by **283 distinct types**,
  but those concentrate hard, and that is the whole finding:

  | rank | type | vectors blocked | cumulative |
  |---|---|---|---|
  | 1 | `struct TagItem *` | 250 | 250 |
  | 2 | `void *` | 102 | 352 |
  | 3 | `struct RastPort *` | 98 | 450 |
  | 4 | `struct Window *` | 93 | 543 |
  | 5 | BPTR (the handle table, which already exists) | 78 | 621 |
  | … | … | … | |
  | 20 | `struct BitMap *` | 25 | **1093 of 1760** |

  So the remaining work is **a list of types, not a list of functions**: twenty
  descriptions cover 62% of everything left. `TagItem` is both the largest
  lever and the hardest, because a tag's VALUE type depends on its tag ID, so
  it needs the per-tag kind table of `[T3c]` rather than a layout alone.
  `struct Hook *` (28) is the callback class, `[T3d]`.

  Code is emitted for the 20 application-facing libraries (126 crossings after
  the first policy-compiled type slice); the
  rest are classified but not compiled, since nothing links the bridge against
  HIDDs and internal modules. Adding a library is one name on `GEN_LIBS`: the
  base name and type come from its generated proto header, whose presence is
  also the test for whether it can be compiled against at all.

  Three lessons the first slice paid for, all of them the same shape (a
  signature that lies):
  - **The self-check earns its keep.** LVO numbering is `firstlvo` by module
    type, except that a `.conf` declaring `noresident` spells out the four
    standard vectors itself and starts at 1. Validating against offsets known
    from the 68k ABI (`Open` = -30, `MatchFirst` = -822) caught the resulting
    4-vector shift in three libraries, and caught a hand-written
    off-by-one-`.skip` in this plan's own earlier notes.
  - **Simple signature is not safe to forward.** `exec.ShutdownA`, `SetSR`,
    `ReadGayle`, `Exception`, `RawPutChar` are all tier 1 by shape and all
    drive the real machine, so exec is **allowlisted** while everything else is
    denylisted. `exec.Wait` would block the AROS task hosting the JIT.
  - **A C prototype can under-describe the vector.** `utility.UDivMod32` is
    declared `ULONG` but returns the quotient in D0 *and the remainder in D1*;
    generating it would have left D1 stale and the arithmetic silently wrong.
    `dos.MakeLink`'s `dest` is a string or a lock depending on another
    argument. Both are refused by name, with the reason recorded.

  The first tier-2 type descriptions and semantic annotations are now compiled
  by `[T3b]`/`[T3c]`; broadening those policies, printf-class varargs and
  production callbacks remain. A function without annotations stays
  unsupported, never best-effort ([design §4](design.md)).

  One vector deserves an implementation rather than a refusal:
  **`exec.CacheClearU`** means "I have just modified code, forget what you
  cached". Under translation the honest equivalent is flushing the JIT's
  translated blocks, so it belongs in the engine, not in a generated crossing.
  Until it exists, a self-modifying program that calls it stops with a
  classified gap, which is the right failure: no-oping it would let the JIT run
  stale translations of code the program has already rewritten, silently.

  **A vector is not always reached as `jsr -N(a6)`, and missing one is
  SILENT.** A program copies the base to another register, hoists the vector
  address out of a loop (`lea -216(a2),a3` … `jsr (a3)`), or tail-calls the
  vector (`jmp -138(a0)`). The offset is gone by the time the instruction
  executes, so the only way left to recognise it is from the address it lands
  on, which is what `libbase_of_target` does. That was wired into
  `jsr (d16,An)` only; `jsr (An)`, `jmp (An)` and `jmp (d16,An)` fell through
  to the plain computed-jump handlers, jumped into the library base region, and
  the decoder translated structure bytes until the runaway guard fired. The
  reported address (`@pc=00220f28` for lha) is `DOSBase - 216`, which looks like
  nothing until you know the base. All four forms now reach the bridge; the
  `jmp` ones are tail calls, so they resume at the return address on the stack.

  **PC-RELATIVE DATA ACCESS was broken outright, and x18 is why.** Emu68
  computes a PC-relative EA from `REG_PC`, which is **x18** (`emu68/A64.h`) -
  the one AArch64 register Darwin reserves. It is never seeded, and the 68k file
  already occupies x12..x29 (x12 base_adjust, x13-x17 A0-A4, x19-x26 D0-D7,
  x27-x29 A5-A7) with nothing free to move it to. So emitted code for a
  PC-relative access read from a garbage base.

  This was *partly* handled, one form at a time (`lea (d16,pc),An`,
  `jsr/jmp (d16,PC)`, FP immediates), chosen from what the test corpus used. It
  missed the ordinary case: a **data** access through a PC-relative EA, which is
  how position-independent code (every Amiga executable) reads its constants and
  jump tables. Two instructions expose it:

      move.w tbl(pc),d1        faulted
      move.w tbl(pc,d0.w),d2   faulted

  `has_pcrel_src()` now ends the block on the whole class and
  `j5d_exec_pcrel()` executes the instruction in C, where the 68k PC is exact:
  MOVE/MOVEA, LEA, PEA, TST, and CMP/ADD/SUB/AND/OR into Dn. Anything else fails
  **by name**. Only opcode families whose low six bits are genuinely a source EA
  are eligible; in lines 5/6/7/A/E that pattern is a Bcc displacement, a MOVEQ
  immediate or a register shift's type-plus-register.

  The follow-up, if PC-relative code ever shows up hot in a profile: extend the
  darwinize rewrite pass to rewrite the `REG_PC`-based EA sites the way it
  already rewrites the `(An)` ones, so they compute `sandbox_base + 68k PC`.
  That restores full JIT speed and needs a spare register, which today there
  is not.

  **A guest's dos calls raise the OS's own requesters.** A path the guest cannot
  resolve produced the usual "please insert volume" requester, and the run
  blocked inside a native call forever - past where the wall-clock guard can
  reach, so it presented as a silent hang with an empty log. `emu68k_runseg.c`
  sets `pr_WindowPtr = -1` for the duration (restored on every exit path). This
  is very likely the same class as the `ADhelp`/`PPMore` hangs.

  Two lessons worth keeping:
  - **"block decode guard tripped" never means a missing instruction.** It is a
    runaway: >4000 instructions with no terminator, i.e. the decoder is walking
    data. Look for where control flow left the program, not for an opcode.
  - **Case order in the dispatcher is load-bearing.** Each libbase-checking case
    MUST precede its plain counterpart, or it never fires and the symptom is
    identical to not having written it.

  **The PC-relative bug was producing FALSE "needs a full emulator" verdicts,
  so any recorded before 2026-08-02 is suspect.** `AddText` was routed as
  needing the Amiga hardware because it "touched the exception vector page
  $000". It does not: a garbage PC-relative read sent it to a low address and
  the [T2b] runtime guard faithfully reported what it saw. With the read fixed
  it runs clean, and the static scanner agrees ("no hardware use found, route:
  JIT"). When the runtime guard and `scan68k` disagree, believe the scanner and
  look for a translation bug.

  **On measuring this with the corpus:** a sweep is NOT a reliable regression
  signal on its own. Two programs (`ADhelp`, `PPMore`) can hang past the
  wall-clock guard, because a guest blocked inside a *native* call is past the
  point the guard can reach; the same build and inputs produced a clean
  `***Break` on one run and a hang on the next. Attribute a changed line with
  an A/B (build with the change disabled and re-run the single program), not
  with a diff of two sweeps.
- **`[T3b0]` The struct-layout generator. MACHINERY PROVEN 2026-08-02.** Field
  names, offsets and sizes for BOTH layouts come out of one tool, with one
  output format, for every type at once:

      clang -target m68k-unknown-elf   -fpack-struct=2 \
            -Xclang -fdump-record-layouts -fsyntax-only probe.c -I$AROS_INC
      clang -target aarch64-unknown-aros \
            -Xclang -fdump-record-layouts -fsyntax-only probe.c -I$AROS_INC

  where `probe.c` just declares one variable per type. Nothing is run and
  nothing is hand-counted.

  **`-fpack-struct=2` is load-bearing.** The m68k-ELF psABI aligns 32-bit
  fields to 4, the Amiga ABI packs to 2. Without it `struct Node.ln_Name` comes
  out at 12 instead of 10 and every conversion using it would be silently
  wrong. So the generator needs the same kind of self-check the LVO numbering
  got: assert known AmigaOS truths (`Node` = 14 bytes with `ln_Name` at 10,
  `FileInfoBlock` = 260, `DateStamp` = 12, `TagItem` = 8) and refuse to emit if
  they do not hold.

  **BUILT 2026-08-02** as `graft/gen-struct-layouts` (`make struct-layouts`),
  emitting `emu68k_layouts.h`. Three things the build of it turned up, all of
  which would have been silent:
  - clang does **not** compute a layout for a tentative definition, so the probe
    has to force it with `sizeof`. Without that the record is simply absent.
  - AROS names several of these `<Name>32` / `<Name>64` and lets the target
    pick, so `struct FileInfoBlock` resolves to a *different record* on each
    side. That is the conversion, not an obstacle, but the generator has to
    resolve by suffix or it finds nothing.
  - the self-check caught the `-fpack-struct=2` omission immediately, exactly
    as the LVO self-check caught the `firstlvo` shift.

  First use: `dos.Examine`/`ExNext`. FileInfoBlock is 260 bytes on the guest
  side and 264 here, with **14 of its 15 fields at a different offset**, so the
  native call fills a native structure and the fields are copied back through
  the generated offsets. With that plus `FilePart`/`PathPart` (a pointer INTO
  the caller's string, so what crosses is the guest's own pointer advanced by
  the same offset) and host-served `CopyMem`, **LhA compresses, lists and
  extracts byte-exactly**.

  What this does NOT generate, and cannot:
  - **Direction** (does the callee read the struct, fill it, or both) is a
    property of each (function, argument), not of the type. `const` in the
    prototype implies in-only; the rest is a judgement call, and the safe
    default is to refuse rather than guess.
  - **Retention** (does the callee keep the pointer past the call).

  And a large part of the ranked list needs no layout at all: `Window`,
  `Screen`, `RastPort`, `Region`, `Layer`, `MsgPort`, `CxObj`, `Object`,
  `Class`, `IFFHandle`, `ColorMap`, `ViewPort` are OS-owned things the guest
  receives and hands straight back, so they want the **token** mechanism that
  already exists for BPTR. The types that genuinely need converting are the
  data structures the guest reads and writes: `FileInfoBlock`, `AnchorPath`,
  `TagItem`, `DateStamp`, `InfoData`, `DiskObject`. That is the real list, and
  it is short.

  The token mechanism holds only while the guest does not read fields out of
  the handle. Real programs do (`window->RPort`), and that is where the
  fault-driven shadow of the design notes becomes necessary - per type, and
  later.

- **`[T3b]` The bridge compiler + core libraries. FIRST PRODUCTION SLICE DONE
  2026-08-02.** Discover the bounded AROS
  surface up front rather than adding functions when a program happens to hit
  them. `.conf` remains the authority for LVO order, prototypes and m68k source
  registers; Clang parses the AROS headers under BOTH the m68k Amiga ABI and the
  native target to emit canonical type graphs (sizes, signedness, fields,
  arrays, nested records and pointer targets). A small policy schema supplies
  what C types cannot say: direction/length, retention, ownership, object-proxy
  class, tag domain, callback ABI, guest redirect and explicit refusal.

  The output is precompiled, type-checked C thunks plus tables, not a runtime C
  parser and initially not an untyped AAPCS call gate. Runtime work is only
  `(library,LVO)` lookup, marshalling and cached object identity. Automatic
  classes are scalars/strings, byte buffers with a proven length, BPTR handles,
  value-structure field maps and declared callbacks. `Window`/`RastPort`-class
  OS objects use per-run proxies; types the guest reads get a guest façade, not
  merely an opaque token. `TagItem`, `APTR`, retained pointers, multi-register
  returns and process-affecting calls require policy and otherwise fail closed.

  Generate the complete report/table every build; an unclassified public
  vector is visible in the report and unavailable at runtime, never guessed.
  Wire exec, dos and utility first, then intuition, graphics, gadtools, asl,
  icon and workbench. Hand-written `T1c` cases retire only after generated
  equivalents pass the same end-to-end regression.

  `graft/emu68k-bridge-policy.json` is now schema v1 and is validated against
  the parsed `.conf` functions on every emit/check: a stale function or argument
  name, missing domain, invalid direction, unsupported residual argument/result,
  repeated tag, or unknown value kind fails generation. It compiles two real
  value-structure functions without per-vector C: `dos.DateStamp` (`DateStamp`
  OUT plus its returned pointer mapped back to the guest argument) and
  `dos.CompareDates` (two `DateStamp` IN shadows). Guest ranges are checked
  before conversion; generated field tables provide every offset and width.

  The rebooted regression drives both functions through native dos.library and
  verifies pointer identity, OUT copy-back and the AmigaOS CompareDates ordering.
  This is the reusable direction/result machinery; adding another plain value
  structure is policy data, provided its generated field table has no unresolved
  pointer field.
- **`[T3c]` Taglist kind tables. FIRST TWO DOMAINS DONE 2026-08-02** for the
  core attribute domains. Unknown
  tags: ledger entry + **abort the call as a classified capability gap**.
  Never pass-as-scalar (that guess is exactly what the risk register
  forbids; this supersedes the earlier draft of this line).

  The generic guest→native walker now reads packed big-endian 8-byte TagItems,
  emits native-layout TagItems, bounds-checks every guest read/string, flattens
  `TAG_MORE`, implements `TAG_IGNORE`/`TAG_SKIP`, caps traversal, and rejects
  cycles, overlong lists, unknown tags and explicitly refused pointer tags.
  Policy domains cover `graphics.BestModeIDA` (scalar BIDTAGs; ViewPort refused
  until its proxy exists) and `cybergraphics.BestCModeIDTagList` (scalar tags
  plus CSTR BoardName). The generated dispatcher preserves the exact tag/domain
  reason through both bridge layers.

  `make hosted-emu68k-t3gen` proves all of this inside rebooted AROS: a real
  BestModeIDA succeeds through a `TAG_IGNORE`/`TAG_SKIP`/`TAG_MORE` chain, a
  string-valued cybergraphics tag reaches native code, and an unknown-tag 68k
  negative control terminates with `unknown tag 8fffffff in
  graphics.best_mode` rather than reaching graphics.library.
- **`[T3d]` Callbacks (native→68k re-entry). DONE 2026-08-02.** Generated
  `utility.CallHookPkt` and `intuition.NewObjectA` crossings temporarily install
  native trampolines and re-enter the same JIT engine on a dedicated callback
  stack. The rebooted positive tests verify the Hook and BOOPSI register ABIs,
  including `OM_NEW`; zero/out-of-range guest entries fail before native code.
- **`[T3f]` Real-software corpus. FIRST CATEGORIZED SWEEP DONE 2026-08-02.** A curated set of system-friendly classic
  programs (CLI tools, a text viewer, a simple Workbench app) run headlessly;
  each new success is pinned as a regression test. Native-name-wins; a 68k
  disk library with no native equivalent follows `[T3e]`, and still fails as a
  classified capability gap until that lifecycle accepts it.

  `make hosted-emu68k-t3legacy` stages isolated per-program fixtures using
  `legacy-corpus.manifest`, runs the CLI/GUI/Workbench/hardware categories,
  prints category coverage, and ranks aggregate bridge gaps by the number of
  programs blocked. The same command separately requires the real LhA
  create/list/extract byte-exact round trip. The first 12-program sweep has
  eight run/quiet exits, three correctly classified hardware fallbacks, and
  one library gap: `exec.library/FindSemaphore` (one program). No installed
  sample currently exercises `datatypes.library`; the report says zero and the
  optional `EMU68K_DATATYPE_PROGRAM` slot accepts a real `ViewDT` binary when
  supplied rather than substituting a synthetic success.

Exit criteria: the curated corpus runs; ledger top entries trend to zero for
that corpus; callbacks proven by a boopsi/hook-using test app; a
`PutMsg`-class shadow-structure call proven both directions.

## [T4] Customization tools and widgets (medium)

Depends: T2 (DB/formats stable). Parallel with T3.

- **`[T4a]` `Emu68k` prefs editor** (Zune, ENVARC/ENV pattern): default mode,
  engine pick, FULL-target config + test button, compat-DB browser
  (edit/clear/export/import), Reports drawer view, gap-ledger view.
- **`[T4b]` Icon Information Compatibility page** in Wanderer: cycle gadget
  writing the `EMU68K` tooltype.
- **`[T4c]` Shell commands.** `C:Run68k`, `C:Emu68kMode`, `C:Emu68kWhy`
  (the routing explainer), quick-built via the `-quick` metatargets.
- **`[T4d]` Running indicator** ("68k" badge via the status surface) and
  **system-monitor columns** (backend, blocks, chain %, dispatcher rate) fed
  by the `[J5n]` counters.

Exit criteria: every routing decision is inspectable (`Emu68kWhy` matches
behavior) and settable (prefs, tooltype, command) without editing files by
hand; `aros-ctl` drives prefs save/use and verifies effect.

## [T5] `FULL` delegation - the emulator hand-off (medium)

Depends: T2 (routing + requester exist).

- **`[T5a]` Host-side delegation** (hosted darwin): host bridge launches the
  configured Mac emulator with an argument template; file path mapped through
  the host-volume view; fire-and-forget with launch-failure surfaced back.
- **`[T5b]` `"J-UAE Execute"` port daemon.** Our listener implements the
  message protocol upstream's Workbench hook already speaks
  ([uae_integration.c](../../../../aros-upstream/workbench/libs/workbench/uae_integration.c)),
  so Wanderer's dormant path lights up with zero workbench.library changes.
  The hook forwards **every** detected 68k executable before the normal
  launch path ([openworkbenchobjecta.c:87](../../../../aros-upstream/workbench/libs/workbench/openworkbenchobjecta.c)),
  so the daemon runs the **full `AUTO|JIT|FULL` router**: JIT-routed programs
  are relaunched through the normal path, only FULL-routed ones go to `T5a`.
  A dumb forwarder would force every Workbench 68k program to FULL. (If the
  `T1b` divert already handles Workbench launches well, this daemon may be
  unnecessary; decide on evidence.)
- **`[T5c]` Beyond lone binaries.** ADF/disk-image association: `FULL` mode
  accepts "open this in the emulator" for images; Kickstart path is opt-in
  prefs config with a clear licensing note.

Exit criteria: a hardware-banger double-clicked in Wanderer ends up running in
the configured Mac emulator with at most one (remembered) question; with no
emulator configured, the polite decline path verifies.

## [T6] Portability (medium)

Depends: T1 stable; T3 helps but is not required.

- **`[T6a]` Interpreter backend in-OS.** The reference interpreter behind the
  same library contract; selectable in prefs; corpus green (slow is fine).
- **`[T6b]` Exec-mem seam ports.** linux-hosted (`mmap`) and native-Pi (MMU)
  implementations; JIT on aarch64 targets, interpreter elsewhere (x86_64).
- **`[T6c]` Keep the clean/engine split, publish to jonx only.** This whole
  feature ships on John's repos (`github.com/jonx/*`), including the parts
  that would in principle be upstreamable (execution-boundary hook,
  `genmodule` marshal back-end, prefs/commands). **No upstream PRs, branches,
  comments or any other `aros-development-team/AROS` action for this goal**;
  per the approval gate in `.claude/skills/upstream-pr/SKILL.md`, any upstream
  step would need John's explicit say-so for that specific action, and none is
  planned. We still keep the clean-vs-MPL-engine separation: it costs little
  and keeps the option open if John ever asks.

Exit criteria: corpus passes under the interpreter backend on at least one
non-darwin target; everything pushed to the jonx fork/repos.

## [T7] Performance track (standing, opportunistic)

Not gating anything. Driven by the [benchmark rig](../benchmarks/README.md)
(the 68k Dhrystone, plus a chaining-friendly kernel to show the ceiling):

- `jsr`/`rts` chaining (the big Dhrystone win; today every call returns to the
  dispatcher);
- redundant byte-swap elimination; superblock formation; cross-block register
  allocation - in bang-for-buck order per the
  [benchmarks analysis](../benchmarks/README.md#interpreting-them).

Each optimization lands only with the byte-exact corpus and Dhrystone
integrity values green.

## Risk register

| Risk | Phase | Mitigation |
|---|---|---|
| No workable guest-address/loader representation | T0-P1 | three candidates prototyped before committing; low-mmap is NOT assumed available (darwin: it is not) |
| Engine re-hosting (contexts, safe points) costs more than planned | T0-P3 | it is scoped as implementation, not a decision; interim serialized-engine model allowed, documented |
| Safe-point polling costs measurable JIT speed | T1 | benchmark before/after in the T7 rig; tune poll placement, never remove it |
| Divert destabilizes native launch paths | T1 | boot switch `EMU68K=off`; boot smoke + resize smoke + keymap/diskfont checks stay in the gate |
| Annotation vocabulary doesn't survive contact with real APIs | T0-P4 | five-case spike gates T3; vocabulary revisions are design updates, not silent code drift |
| Marshal surface balloons | T3 | ledger-driven ordering; hard rule that unsupported = classified abort + ledger, never a guess |
| Requester from non-Workbench contexts deadlocks | T2 | text fallback path; requesters always on their own signal, never inside the fault handler |
| Emulator delegation feature-creeps into "configure UAE for people" | T5 | scope: launch + hand the file over, nothing more; config is the emulator's job |
| Product/brand naming leaking into clean-side code | T6 | provenance sweep per CLEANROOM, even though nothing is being sent upstream |
