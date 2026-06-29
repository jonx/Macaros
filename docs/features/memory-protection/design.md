# Memory protection, resource tracking & virtual memory — robustness without breaking the ABI

> Status: design (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-29
> Builds on the **built** [crash-handling](../crash-handling/design.md) bounded-guru
> work (`../aros-upstream/arch/all-unix/kernel/kernel.c` `core_TrapHandler`). Source of
> the question: the classic AROS "MP/SVM/RT/SMP" passage — *"alert if something dubious
> is happening … tell you in great detail what was happening when the machine crashed …
> allow you to save your work and then crash … check what has been saved."*

## What & why

The Amiga robustness wishlist — **MP** (memory protection), **SVM** (swappable virtual
memory), **RT** (resource tracking), **SMP** (symmetric multiprocessing) — has been
"planned, low priority" for 25 years. This doc asks the concrete version of the question:
*given this source tree and the fact that we are hosted on a large Apple-Silicon machine,
how would we actually add them here, and which are worth doing?*

The headline finding: **the blocker was never the MMU. It is the ABI.** And being hosted
on a powerful Mac opens a route the bare-metal Amiga never had — **process-per-app
isolation** — that sidesteps the ABI problem entirely for the common case. So this is not
one feature; it is a small family of independent, additive capabilities, each with a
different cost/compat trade-off.

## The real obstacle (mechanism, not folklore)

"Hundreds of experts failed for three years" is true but misleading. Unix/NT-style MP
puts each task in its **own address space**; AmigaOS/AROS is built on
**shared-memory-by-reference**, and the source says so outright:

- **Messages are raw pointers into shared memory.** `PutMsg()` does **not** copy — the
  comment in [putmsg.c](../../../../aros-upstream/rom/exec/putmsg.c) states messages
  "must lie in shared memory." Every `IORequest`, every Intuition IDCMP message, every
  `ReplyMessage` hands a pointer across a "task boundary."
- **Library calls are a bare `JSR` through a vector table** — no syscall transition, no
  marshalling. A library dereferences caller pointers freely; callers walk `ExecBase`
  and the global lists directly.

Put tasks in separate address spaces and every one of those pointers becomes meaningless.
That — not the absence of page tables — is what defeated the retrofit, and it is why
AROS's own plan was always *"protect new programs that know about it"* (opt-in MP via a
cooperating ABI), never transparent MP for existing software.

## What this port already gives us (the hooks)

The hosted Darwin/aarch64 port already has the primitives. We are not starting from zero:

| Capability | Status today | File |
|---|---|---|
| Page protection | **Real** — `KrnSetProtection` → host `mprotect()` (+ i-cache invalidate) | [setprotection.c:15-44](../../../../aros-upstream/arch/all-unix/kernel/setprotection.c) |
| Page allocation | `KrnAllocPages` → `mmap` (always R/W, flip later) | [allocpages.c](../../../../aros-upstream/arch/all-unix/kernel/allocpages.c) |
| Fault catch | SIGSEGV/SIGBUS → handler w/ fault-addr decode, symbolized backtrace, guru-loop breaker | [kernel.c:111-266](../../../../aros-upstream/arch/all-unix/kernel/kernel.c) · see [crash-handling](../crash-handling/design.md) |
| Corruption detect | MungWall walls (`0xDB`) + per-alloc **owner task + caller** record | [mungwall.c](../../../../aros-upstream/rom/exec/mungwall.c), [mungwall.h](../../../../aros-upstream/rom/exec/mungwall.h) |
| Per-task mem tracking | `tc_MemEntry` list freed on death | [remtask.c:163](../../../../aros-upstream/rom/exec/remtask.c) |
| RT slot | `iet_RT` field exists — **zero references**, dormant | [etask.h:47](../../../../aros-upstream/rom/exec/etask.h) |
| Managed-heap hook | `MEMF_MANAGED` (bit 15) + `MemHeaderExt` alloc/free callbacks | [allocate.c:86](../../../../aros-upstream/rom/exec/allocate.c), `memheaderext.h` |
| Unused VM flags | `MEMF_VIRTUAL` (bit 3), `MEMF_EXECUTABLE` (bit 4) defined, idle | [include/exec/memory.h](../../../../aros-upstream/compiler/include/exec/memory.h) |
| W^X exec memory | proven `MAP_JIT` + `pthread_jit_write_protect_np` + `sys_icache_invalidate` | [hosted/jit68k/jit_region.c](../../../hosted/jit68k/jit_region.c) |
| Host-side AROS RAM | the bring-up harness `mmap`s its own RAM + per-task stacks | [hosted/mem.c:208](../../../hosted/mem.c), [hosted/exec.c:127](../../../hosted/exec.c) |

Two facts about the model shape everything below:

1. **One host thread, one address space.** All AROS tasks are multiplexed inside a single
   host pthread by signals — a context switch is literally overwriting `ucontext->uc_mcontext`
   in a SIGALRM/SIGUSR1 handler ([kernel_cpu.c:76-162](../../../../aros-upstream/arch/all-unix/kernel/kernel_cpu.c)),
   and a Darwin guard *drops* any signal that lands on another host thread
   ([kernel.c:272-285](../../../../aros-upstream/arch/all-unix/kernel/kernel.c)). So
   intra-AROS protection is region-granular and global, never per-task — until/unless the
   scheduler is re-architected.
2. **`Forbid()`/`Disable()` are the entire Amiga lock model** and they *mean* "single CPU"
   ([forbid.c:81-93](../../../../aros-upstream/rom/exec/forbid.c),
   [disable.c:78-93](../../../../aros-upstream/rom/exec/disable.c)). That is the wall SMP
   hits.

## The fork in the road — three routes

| Route | Isolation strength | Compat | Effort | Best for |
|---|---|---|---|---|
| **A — intra-AROS diagnostic protection** `[MP*]` | soft (catch & report, region-global) | full | low | the integrated desktop: catch wild writes / stack overflow / NULL / UAF early and attribute them |
| **B — shared public arena (single-system-image)** `[B*]` | hard for private state, *shared* for opt-in public objects | new cooperating code / value-IPC | high | letting C's isolated instances still cooperate via Amiga IPC — the realized "protect programs that know about it" |
| **C — multi-instance (process-per-app)** `[MI*]` | **hard, kernel-enforced, free** | breaks tight cooperation | medium | running *independent* / risky / heavy apps robustly on a big host |

Routes **A** and **C** are the cheap, buildable wins and they compose (below). Route **B** is
the most ambitious — the bridge that lets C's isolated instances still cooperate through Amiga
IPC, and the place where the SMP question actually lives. It is detailed *after* C because it
builds on C's substrate. Route B is **not** just a "someday ABI redesign": scoped to a bounded,
value-oriented arena it is buildable incrementally, opt-in, without touching legacy software.

---

## Route A — intra-AROS diagnostic protection `[MP*]`

Keep the single address space; use the host MMU (via the existing `KrnSetProtection`) to
poison/guard regions *within* it, and lean on the existing `core_TrapHandler` to turn a
touch into a precise, attributed report. This is exactly the "alert on something dubious +
tell me what happened + let me save my work" system the AROS passage actually asks for —
and most of the plumbing is already built.

- **`[MP1]` Guard pages around stacks.** Bracket every task stack with a `PROT_NONE` page.
  Today a stack overflow silently corrupts a neighbouring allocation; with a guard it
  becomes an immediate SIGSEGV that the trap handler already decodes to *which task / which
  guard / full symbolized backtrace*. The bring-up harness allocates stacks by `mmap`
  ([hosted/exec.c:127](../../../hosted/exec.c), [hosted/execboot.c](../../../hosted/execboot.c)) — a
  guard page is a few lines there; in the real OS, `PrepareContext`-adjacent stack alloc.
- **`[MP2]` Trap the low page.** Map page 0 `PROT_NONE` so a NULL deref / call-through-NULL
  faults immediately. (crash-handling already *annotates* NULL calls after the fact; this
  makes the data write case fault too, not just the call.)
- **`[MP3]` Read-only system structures — highest value.** Make `ExecBase`, library jump
  tables, and loaded code R/X after init (LoadSeg already flips code R/X via
  `KrnSetProtection` — commit `71f75760`, see [native-modules](../native-modules/design.md)).
  The classic "fog and 100-ft pole" crash *is* silent ExecBase corruption; this converts it
  into an instant, attributable fault at the moment of the wild write. Cost: anything that
  legitimately writes ExecBase must go through a narrow `Forbid`+unprotect/reprotect window.
- **`[MP4]` Free-poison + quarantine** for use-after-free: on `FreeMem`, `mprotect(PROT_NONE)`
  the page(s) for a quarantine window before returning them to the free list (gate behind a
  debug pool / `MEMF_HWALIGNED` so it is page-granular). MungWall already records the freeing
  task + caller, so a UAF names its culprit.
- **`[MP5]` Panic-save hook** — the "save your work, then crash" clause. We are
  single-threaded, so inside `core_TrapHandler` every task is *already frozen*. Before the
  guru: snapshot a structured dump (task list, faulting regs+backtrace, a MungWall scan, the
  memory map), then run a registered **panic-save callback chain** on a known-good emergency
  stack under a watchdog, letting apps flush work; write a checksum/journal beside each saved
  file so the "did my save survive intact?" check is answerable on reboot.

`[MP1]`–`[MP3]` deliver most of the value and are low-risk and compat-preserving.

---

## Route C — multi-instance / process-per-app `[MI*]`  (the M5 question)

> *"Given how much resource my M5 has, would it not be easier to just launch each program
> in its own AROS instance, sharing the filesystem and things like that?"*

**Yes — and for the robustness goal it is arguably the *better* answer, precisely because
we are hosted on a big machine.** This deserves to be a first-class route, not a footnote.

### Why it is strong

- **Hard isolation, for free, kernel-enforced.** Each AROS instance is its own Darwin
  process → its own host address space. A crash in app A's instance **cannot** corrupt app
  B's instance — macOS guarantees it. This is the exact isolation that is *impossible*
  inside one AROS (the shared-pointer ABI), obtained without touching the ABI at all,
  because within each instance pointers stay valid and between instances you never pass an
  AROS pointer.
- **An "AROS instance" is just a Unix process.** We are already hosted; spawning one is
  cheap. On an M5 (tens of GB RAM, P+E cores to spare) dozens of instances are trivial, and
  AROS boots hosted in well under a second to a few seconds. The bring-up harness *already*
  runs multiple headless boots — the substrate exists.
- **Resource tracking becomes free.** When an app's instance dies, the host process dies and
  macOS reclaims **everything** — memory, file handles, the lot. The host *is* the resource
  tracker for cross-app leaks. This is a large part of why "RT was never finished" simply
  evaporating.
- **It fits the Mac model.** The natural rendering choice — *each AROS instance = one Mac
  window = one app* — is exactly how a Mac user expects apps to behave, and it is the logical
  endpoint of the project thesis: *macOS owns the drivers*; here macOS also owns **isolation
  and reclamation**, and AROS instances reach *each other* through host channels, not raw
  pointers.

### The catch (why it is not a total replacement for MP)

The *point* of AmigaOS MP was to protect **cooperating** programs that talk through Amiga
IPC — shared public screens, message ports, AppWindow drag-and-drop, ARexx, datatypes,
commodities. The moment app A wants to `PutMsg` app B, or open a window on B's public
screen, or hand over a `BitMap *`, that pointer lives in A's address space and is garbage in
B's. So multi-instance buys isolation **at the cost of tight inter-app cooperation**. It is
not "MP for AmigaOS"; it is "**run several AmigaOS-es side by side that share storage.**"

That trade is *excellent* for independent or risky workloads (a compiler, a port under test,
a downloaded binary, the 68k JIT running random classic software) and *wrong* for the
integrated Workbench desktop where apps are meant to cooperate.

### What is shared, and how (the design surface)

| Shared resource | Mechanism | Status |
|---|---|---|
| Filesystem | both instances mount the same host volume | **built** — [host-volume](../host-volume/design.md) |
| Clipboard (copy/paste across apps) | host `NSPasteboard` is the shared channel | **built** — [clipboard-bridge](../clipboard-bridge/design.md) |
| Display | **MVP: each instance = its own top-level Mac window** (no shared screen → no AROS-pointer sharing) | per-instance [cocoa-metal-display](../cocoa-metal-display/design.md) |
| Networking | host sockets; instances talk via loopback like any two hosts | **built** — [bsdsocket-net](../bsdsocket-net/design.md) |
| Launch / lifecycle | a small **host-side launcher** spawns + supervises instances | new (`[MI1]`) |

Two of the four channels (filesystem, clipboard) are **already built and proven**, which is
why this route is "medium effort," not "from scratch."

### What it deliberately does *not* try to do (initially)

- **No shared public screen.** A shared screen would require a host-side compositor/window-
  server proxy that each instance talks to over an IPC protocol — i.e. re-inventing the very
  pointer-free message bus we are avoiding. Defer it. One window per instance is the clean
  MVP and is very Mac-native.
- **No message-port / ARexx / AppWindow bridging across instances** in the C MVP. Those are the
  "tight cooperation" cases — they are exactly what **Route B** (the shared public arena, below)
  adds, incrementally and opt-in.

### Where it leaves intra-AROS MP

Multi-instance shrinks the **blast radius** to one instance but does **not** protect an app
from *itself* (a wild write inside instance 1 still corrupts instance 1). So Route A is still
worth having *inside* each instance — now much cheaper to justify, because a crash there is
contained. **A and C compose**: run the integrated desktop as one instance (cooperation
intact, Route-A diagnostics on), and launch each risky/heavy program in its own throwaway
instance that shares the filesystem + clipboard. That hybrid is the strongest robustness
story available on this target, and it is uniquely enabled by being hosted on a powerful Mac.

---

## Route B — shared public arena (single-system-image cooperation) `[B*]`

The bridge between A and C: let Route C's isolated instances still cooperate through Amiga IPC,
by giving them a **bounded shared region** — mapped identically in every instance — where
explicitly-public objects live. This is AROS's own *"protect programs that know about it"* plan
made concrete, and it is where the SMP question actually has to be answered.

### The mechanism (the easy part — and the part that "just works")

- A shared host object (`shm_open` + `mmap MAP_SHARED`, or Mach `vm_allocate` + `mach_vm_remap`)
  mapped with **`MAP_FIXED` at an agreed base** in every instance — the launcher reserves a fixed
  slice of the 48-bit AArch64 VA space (ASLR off for that slice). A pointer into the arena then
  denotes the same bytes in every instance — the *"same indexes look like the same computer."*
- On one machine the arena is the **same physical RAM**, hardware cache-coherent. So there is **no
  software DSM / coherence protocol** to write (that is the hard part of *distributed* shared
  memory *across machines*; we don't have it). Writes are immediately visible; the only software
  cost is **mutual exclusion**, not coherence.
- Proven-here: the bring-up harness already maps a fixed AROS RAM region
  ([hosted/mem.c:208](../../../hosted/mem.c)), so the fixed-base technique is established.

### The two real costs (the hard part)

1. **Transitive closure — "no private pointers in shared objects."** Anything in the arena must
   reference only arena-resident (or translated) memory. A shared `Message`'s `mn_ReplyPort`
   ([putmsg.c](../../../../aros-upstream/rom/exec/putmsg.c)), an `AppMessage`'s `WBArg.wa_Lock`
   (a BPTR into a handler's world), an `IORequest`'s buffers — each drags its reachable graph in.
   So the arena is for **flat / value payloads**; deep pointer graphs are kept private (don't
   cross) or marshalled at the edge. (Disk is the tractable corner: route file I/O to one shared
   handler / the host FS, and a `Lock` becomes a handle into that service, not a raw shared BPTR.)
2. **The arena is an SMP domain — this is the wall.** `Forbid()`/`Disable()`
   ([forbid.c:81-93](../../../../aros-upstream/rom/exec/forbid.c)) stop only *this instance's*
   scheduler; they do **not** exclude another process. Every mutable arena structure therefore
   needs a **real cross-process lock** (process-shared `pthread_mutex` / futex / Mach semaphore in
   the arena). That is exactly the SMP problem — and the `mp_SpinLock` / `EXECSMP` scaffolding
   upstream (declined for the *general* case in this doc) is what an arena needs, scoped to the
   arena. Discipline: **arena objects are SMP-locked; private objects keep Forbid/semaphores.**

### The boundary rule (what keeps isolation)

ExecBase, library bases, heaps, and tasks stay **private per instance**. Only a designated public
set lives in the arena — public message ports, an ARexx host-port registry, a clipboard arena,
(later) public screens. Anything that would put a private pointer in the arena is marshalled by
value instead. This preserves Route C's hard isolation *everywhere except* the small, locked,
audited arena — and keeps the failure mode off the table: *share everything so all IPC is
transparent → you have rebuilt one SMP AROS with N processes, and a wild write to shared ExecBase
corrupts everyone — i.e. you lost the protection that motivated separate processes.* **Isolation
and transparent shared-pointer IPC are opposed; the arena picks a deliberate point on that curve.**

### Why ARexx is the `[B1]` POC — and what we have today

ARexx is the **loosely-coupled, value-oriented** Amiga IPC: a host app creates a public message
port found *by name*, and a `RexxMsg` carries **argument strings** (`rm_Args[0..15]`, made with
`CreateArgstring`) — data crosses **by string value**, not by deep pointer graph
(`compiler/include/rexx/storage.h`). That is exactly the payload shape the arena handles best, so
it exercises the whole arena machinery (named public port *in* the arena, cross-process lock on
the port's message list, string payloads copied in) **without** dragging Intuition's screen graph.

**Status on this port (checked 2026-06-29): ARexx is _not_ running — but the piece `[B1]` needs
exists.**

- `rexxsyslib.library` — the message-construction layer (`CreateRexxMsg`, `CreateArgstring`,
  `FillRexxMsg`, …) — **is built and deployed** in the boot set
  ([graft/run-window.sh](../../../graft/run-window.sh), `graft/aros-ctl`).
- **`RexxMast` (the interpreter) is not in the main OS repo** — `RX.c` only *tries* to
  `SystemTags("RexxMast", …)` and errors "no Rexx interpreter seems to be installed". The `rx` /
  `rxlib` commands have source (`workbench/rexxc/`) but are **not deployed**. The *official*
  interpreter does exist — it is **Regina REXX**, ported as `regina.library` + a thin `RexxMast`
  front-end, in the AROS **contrib** repo (`contrib/regina/`), not the main repo — but it is not
  built or deployed on this port, so **no `.rexx` script execution is possible today**. Full survey
  + where the official port lives: [../arexx-host-port/README.md](../arexx-host-port/README.md).
- **Crucially, `[B1]` does not need the interpreter.** The ARexx *host-port message protocol*
  (named public port + `RexxMsg` + string args) is independent of `RexxMast` — any app implements
  a host port directly. `[B1]` proves *that protocol* crosses a process boundary via the arena,
  using only the already-deployed `rexxsyslib.library`. Bringing up an actual interpreter (port
  Regina, or revive a native RexxMast) is a *separate* task, **out of scope** for the IPC proof.
- Caveat to verify first: the aarch64 build of `rexxsyslib.library` is deployed but its functions
  are **unproven on this target** (the arch helpers had m68k asm; the generic C paths need a
  round-trip check) — hence `[B0]` below.

### Explicitly deferred

Shared public **screens / windows / BOOPSI** — full SMP-on-Intuition (a huge mutable shared graph).
Keep one-window-per-instance (Route C) + a host compositor there. The arena is for control/data
IPC, not the rendering path.

### `[B*]` spike ladder

- **`[B0]` rexxsyslib sanity (single instance).** `CreateRexxMsg` → `CreateArgstring` → set
  `rm_Args[0]` → `PutMsg` to a local port → `GetMsg` → read the argstring back →
  `DeleteArgstring`/`DeleteRexxMsg`. **PASS:** the string round-trips byte-exact in one instance
  (proves the deployed aarch64 library actually works before we cross a boundary).
- **`[B1]` ARexx host-port across two instances.** Instance **B** registers a public port named
  e.g. `"AROS.POC"` *in the arena*; instance **A** builds a `RexxMsg` with a command string and
  `PutMsg`s it to that port; B receives it, runs a trivial handler, replies with a result string;
  A reads the reply. **PASS:** command + result strings cross the process boundary byte-exact via
  the arena; **FAIL:** port not found, garbage pointer, or torn data.
- **`[B2]` arena-lock correctness.** Hammer the shared port from both instances concurrently; no
  lost/torn messages (proves the cross-process lock — the SMP-safe path — not luck).
- **`[B3]` boundary audit.** A deliberate attempt to place a *private* pointer in an arena object
  is caught (debug scrub / assertion), proving "no private pointers in shared objects" is
  enforceable, not just a convention.

---

## Resource tracking `[RT*]` — best ROI, lowest risk

The passage's "very basic RT has been added" undersells *and* oversells reality: per-task
**memory** is already auto-freed (`tc_MemEntry`, [remtask.c:163](../../../../aros-upstream/rom/exec/remtask.c)),
but *everything else* — open libraries, `Lock`s, semaphores, signals, IORequests, ports —
leaks on abnormal death. Plan:

- Light up the dormant `iet_RT` slot ([etask.h:47](../../../../aros-upstream/rom/exec/etask.h),
  currently zero references): a `ResourceNode { type, ptr, cleanupFn }` list.
- Wrap the acquiring calls (`OpenLibrary`, `Lock`, `AllocSignal`, `CreateMsgPort`,
  `ObtainSemaphore`…) to append a node when the caller has opted in; walk and invoke
  cleanups in reverse inside [Exec_CleanupETask:207-307](../../../../aros-upstream/rom/exec/exec_util.c).
- **Make it opt-in per task** with explicit `RT_Disown()/RT_Adopt()`. This is the real reason
  RT was never finished — it is a *policy* problem, not a mechanism one: resources are
  routinely handed between tasks via messages, so you cannot blindly free on exit. Opt-in +
  an ownership-transfer pair is the pragmatic answer, and it is purely additive.

Note the synergy with Route C: under multi-instance, the **host** does cross-app RT for free;
`[RT*]` then only has to clean up *within* an instance — a smaller, safer problem.

## Swappable VM `[SV*]` — mostly already free

Literal demand-paged swap is low value here: **macOS already pages the whole AROS process**,
so the host gives AROS swap for free. What is worth adding is *over-commit past the fixed RAM
region*: a `MEMF_MANAGED` `MemHeaderExt` backend ([allocate.c:86](../../../../aros-upstream/rom/exec/allocate.c))
whose `mhe_Alloc` grows host pages on demand (`mmap`) and `madvise(MADV_FREE)`s cold ones,
with `MEMF_VIRTUAL` (bit 3, defined and idle) routing allocations to it. A true AROS-managed
pager (mark a region `PROT_NONE`, fault it in from a backing file inside `core_TrapHandler`,
resume — the handler already gets the fault address and knows how to modify-and-resume) is a
real possibility but only earns its keep on a future **native** port with no host VM. Skip it
on hosted.

## SMP — explicitly declined on this target

Not feasible without re-architecting the single-host-thread, signal-multiplexed scheduler
into pthread-per-CPU with real spinlocks — and more fundamentally, `Forbid()`/`Disable()`
*mean* "single CPU"; you cannot honour them on real parallel cores without breaking apps. The
struct scaffolding (`mp_SpinLock` in `MsgPort`, the `EXECSMP` guards) is upstream's bet, but
SMP belongs to a native port with a from-scratch scheduler and an opt-in SMP-safe ABI — not
here. (And note: Route C already turns "use more cores" into "run more instances," which is
the parallelism a desktop user actually wants.)

## Recommended sequencing

1. **`[MP1]`–`[MP3]` diagnostic protection** (guard pages + low-page + read-only ExecBase) and
   **`[RT1]` opt-in RT skeleton** — additive, compat-safe, build directly on hooks that
   already exist. Together they *are* the "alert + diagnose + save your work" system.
2. **`[MI1]`–`[MI2]` multi-instance MVP** — one window per instance, shared host volume +
   clipboard, host-side launcher. The strategically strongest robustness win for independent
   apps, and two of its four channels are already built.
3. **`[MP4]`–`[MP5]`** quarantine + panic-save.
4. **Route B `[B0]`–`[B1]`** — once C exists, the shared-arena ARexx host-port bridge: the first
   *cooperation* primitive across isolated instances, and the on-ramp to opt-in cooperative MP.
   `[B2]`–`[B3]` (arena locking + boundary audit) follow as the arena grows.
5. **`[SV*]`** only if RAM over-commit is actually needed.
6. **SMP**: remains declined on hosted (Route B's arena is the only SMP domain worth the locking).

## The POC (next step, in `hosted/`)

Two candidate first POCs, both self-contained in the bring-up harness, both a single
build→run→one-PASS/FAIL spike (house style). See [spec.md](spec.md):

- **POC-A `[MP1]`/`[MP2]`/`[MP3]`** — prove intra-AROS protection: a guard page turns a stack
  overflow / NULL write / ExecBase-style write into a clean, *attributed* SIGSEGV instead of
  silent corruption. Smallest possible diff; proves the headline "memory protection" claim.
- **POC-C `[MI1]`** — prove multi-instance isolation: spawn two AROS instances sharing one
  host volume, kill one mid-write, show the other survives and the file is intact (checksum),
  then `[MI2]` copy/paste across instances via the built clipboard bridge.

Recommendation: do **POC-A first** (one file, proves the mechanism the whole MP claim rests
on), then **POC-C** (proves the architecture you are most drawn to). They are independent and
can be built in either order.

A third, later POC closes the loop on cooperation — **POC-B `[B0]`/`[B1]`**: prove the deployed
`rexxsyslib.library` round-trips a `RexxMsg` argstring in one instance, then that a named ARexx
host port + string command/result cross *between* two instances through the shared arena. It
depends on POC-C's launcher and is the first concrete step of Route B; specced under
[Route B](#route-b--shared-public-arena-single-system-image-cooperation-b) above.

## Relationship to existing docs

- [crash-handling](../crash-handling/design.md) — **built.** It is the *reactive* half (when a
  fault happens, report it bounded + symbolized). Route A is the *proactive* half (make the
  dubious access fault *earlier* and attributably). `[MP5]` panic-save extends its
  `core_TrapHandler`.
- [native-modules](../native-modules/design.md) — owns the R/W→R/X flip + `MEMF_EXECUTABLE`;
  `[MP3]` read-only-code reuses it.
- [host-volume](../host-volume/design.md), [clipboard-bridge](../clipboard-bridge/design.md),
  [bsdsocket-net](../bsdsocket-net/design.md) — the already-built shared channels Route C
  stands on.
- [darwin-aarch64-port-inventory](../darwin-aarch64-port-inventory.md) — the gap map.

## Provenance

Independent work. The analysis is grounded only in this project's `aros-upstream` checkout
(every contract cited points at a real file:line), this repo's `hosted/` spikes, public OS
design knowledge, and published Apple/POSIX VM APIs. No third-party MP/RT/VM implementation
source was read or consulted; any resemblance is coincidental. Governed by
[CLEANROOM.md](../CLEANROOM.md).
