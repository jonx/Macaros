# Transparent 68k execution - the staged plan

> Status: plan (drafted 2026-08-01, nothing started). Design and rationale:
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
`make hosted-emu68k-t3hello` is the host regression. Still ahead here: the
generated `[T3a]` tables replacing hand marshalling, printf-class varargs,
intuition/graphics, callbacks.

Original plan text follows.

Depends: T1; informed continuously by the `T1d` ledger.

- **`[T3a]` `genmodule` marshal back-end.** The new emitter beside
  `writefd.c` (lives in the OS-source checkout `../aros-upstream`, committed
  to the jonx fork's `aarch64-darwin-graft` branch like all OS-side work;
  kept clean): per-library
  `lvo_desc[]` tables from the `.conf` register maps, per the
  [marshalling design §1](../68k-marshalling/README.md). The tables are the
  *index*; execution goes through **generated thunks + the `[T0-P4]`
  semantic-annotation layer** (direction, lengths, copy policy, shadow/proxy
  classes). A function without annotations is unsupported, never
  best-effort ([design §4](design.md)).
- **`[T3b]` Core libraries wired.** exec, dos, utility first; then intuition,
  graphics, gadtools, asl, icon, workbench, each as thunks + curated
  annotations, ledger-prioritized. Hand-written `T1c` stubs retire as
  generated coverage lands. Structure-traversing calls (`PutMsg`-class) get
  shadow/proxy objects, not raw guest pointers.
- **`[T3c]` Taglist kind tables** for the core attribute domains. Unknown
  tags: ledger entry + **abort the call as a classified capability gap**.
  Never pass-as-scalar (that guess is exactly what the risk register
  forbids; this supersedes the earlier draft of this line).
- **`[T3d]` Callbacks (native→68k re-entry).** Hooks detected by
  sandbox-address test; symmetric marshal through the same descriptors;
  re-entrancy held across the nested crossing.
- **`[T3f]` Real-software corpus.** A curated set of system-friendly classic
  programs (CLI tools, a text viewer, a simple Workbench app) run headlessly;
  each new success is pinned as a regression test. v1 scope: **native
  libraries only**; a 68k disk library with no native equivalent is a
  capability gap.

Exit criteria: the curated corpus runs; ledger top entries trend to zero for
that corpus; callbacks proven by a boopsi/hook-using test app; a
`PutMsg`-class shadow-structure call proven both directions.

## [T3x] 68k disk libraries in-sandbox (medium, after T3d)

Split out of T3 after review: library loading is resident/autoinit lifecycle,
bases, dependency opening and expunge semantics, not "more 68k code". Load a
68k disk library into the sandbox, native-name-wins rule, in-sandbox LVO
calls (no marshalling), mixed-chain guard rails (68k app → native lib → 68k
callback). Depends on T3d (callbacks) and the `[T0-P4]` proxy machinery.
Exit: a classic app that ships its own 68k library runs; expunge verified
leak-free.

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
