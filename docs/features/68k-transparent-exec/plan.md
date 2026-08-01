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

- **`[T0-P1]` Guest address model + loader representation.** Not an
  allocation spike: an address-model proof. Upstream's hunk loader
  hard-requires `MEMF_31BIT` on 64-bit targets
  ([internalloadseg_aos.c:196](../../../../aros-upstream/rom/dos/internalloadseg_aos.c))
  and the darwin bootstrap documents that the low 4 GiB is unavailable
  ([bootstrap/memory.c:57](../../../../aros-upstream/arch/all-unix/bootstrap/memory.c)),
  so relocations must produce **guest addresses**, not truncated host
  pointers. Prototype the candidates (parameterized upstream loader / proxy
  seglist / the engine's `[J4]` sandbox loader promoted to execution loader,
  per [design §2](design.md)) and pick one, answering seglist identity,
  lifetime and `UnLoadSeg`. **Exit: a real hunk binary loaded, relocated,
  identified via `GetSegListInfo`, run, and unloaded** in the chosen
  representation (host-harness acceptable, representation final).
- **`[T0-P2]` Execution interception + launch context.** Prove the
  execution-boundary divert: `RunCommand`'s existing `GSLI_68KHUNK` check
  ([runcommand.c:77](../../../../aros-upstream/rom/dos/runcommand.c)) routes
  to a stub router instead of returning -1; same for the Workbench
  `CallEntry` path; LoadSeg data consumers (keymap, diskfont) demonstrably
  untouched. Define the **versioned launch-context record** (pathname,
  CLI/WB startup args, tooltypes, mode override, console) and carry it
  end-to-end. Exit: stub router receives correct context from both consumers;
  keymap/diskfont regression green.
- **`[T0-P3]` Re-entrant engine, safe points, fault containment.** The engine
  refactor spike: per-instance context (today the cache and counters are
  globals, [j5d_engine.c:255](../../../hosted/jit68k/j5d_engine.c)), back-edge
  safe-point polling that covers **chained** loops, and fault unwind to the
  run entry. Exit (host harness): two engine instances interleaved on one
  thread; an infinite-loop guest interrupted via the safe-point flag; a
  forced translated-code fault unwound cleanly; corpus still byte-exact with
  safe points enabled.
- **`[T0-P4]` Marshalling schema spike.** Prove the annotation vocabulary of
  [design §4](design.md) on five representative cases: buffer + length
  (`Read`), natively-traversed structure (`PutMsg`-class, via shadow/proxy),
  opaque handle, callback hook, taglist call. Exit: each case runs under the
  host harness with the generated-thunk + annotation shape, or the vocabulary
  is revised and the design updated.
- **`[T0a]` Engine-as-library split** (unchanged, enables P3/P4): extract the
  engine core from the `run68k` CLI behind the existing `jit68k.h` seam; the
  host tool remains the fast dev harness. Corpus stays green.
- **`[T0d]` Naming + layout** (unchanged): `emu68k.library`,
  `hosted/emu68k/`, engine stays in `hosted/jit68k/`; compat DB + prefs
  formats versioned from day one (`EMU1` header).

Exit criteria: all four proofs PASS, decisions in NOTES.md, corpus green.

## [T1] A 68k process inside AROS - "it runs from the shell" (large)

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

## [T2] Routing ladder + failure UX (medium)

Make failure excellent before scaling coverage. Depends: T1.

- **`[T2a]` Static scanner, with confidence semantics.** One pass over
  relocated segments: chipset/CIA absolute refs, privileged opcodes,
  vector-page stores. Emits a confidence-graded hint, not a boolean
  ([design §3](design.md)). Test corpus, positives *and* negatives: new
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

## [T3] Generated marshalling at scale (large, the long pole)

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
