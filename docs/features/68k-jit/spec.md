# Implementation spec — 68k→AArch64 JIT (adapt Emu68 core, hosted under macOS)

> Status: drafting (Role A) · Target: aarch64-darwin hosted · Drafted 2026-06-24
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Licence banner — this is an ADAPTATION spec, not clean-room

This feature is unlike the other five specs: its translator core is **adopted** from
**Emu68** (`github.com/michalsc/Emu68`), which is **MPL-2.0** — *file-level* copyleft,
**adoptable** per the licence map in [../CLEANROOM.md](../CLEANROOM.md). So Role B **MAY
read the adopted Emu68 files directly** (they ship in-tree). The rules are containment,
not a wall:

- **Emu68 files live verbatim in a quarantined dir** with their MPL Exhibit-A headers
  intact (proposed `arch/all-darwin/jit/emu68/` at graft; `hosted/jit/emu68/` during
  spikes). Edits to those files stay MPL and their source is disclosed under MPL.
- **Never paste Emu68 source into an AROS file.** Copying source is the *only* thing that
  makes an AROS file MPL (MPL §1.10(b)); calling/linking/compiling-together does not
  (Mozilla FAQ Q11/Q13). AROS code reaches the translator **only** through a C-ABI header
  *we* author from scratch (`jit68k.h`, below) — no Emu68 declarations copied in.
- **MPL↔LGPL is compatible** via MPL §3.3 *provided* the adopted files carry only
  Exhibit A and **not** the Exhibit B "Incompatible With Secondary Licenses" notice.
  `[ACTION]` grep the adopted Emu68 files for `Incompatible With Secondary Licenses`
  before adopting. **DONE for the `[J2]` files** (`hosted/jit68k/emu68/A64.h`,
  `RegisterAllocator.h`): grep is empty — Exhibit-A only, compatible. Re-run before
  adopting the `[J3]` decoder/RA files (recorded in `hosted/jit68k/emu68/NOTICE`).
- **Still forbidden (GPL):** the UAE family (WinUAE/FS-UAE/Amiberry/E-UAE/Janus-UAE) and
  vAmiga — do NOT read them for the translator. **emumiga** (LGPL-2.1) is **design
  reference only** for the in-OS LoadSeg/dispatch shape — abandoned, not adopted as code.

Provenance tags as elsewhere: `[PUB]` public API / standard (Apple docs, MPL text, the
m68k ISA, the AmigaOS hunk format), `[AROS]` in-tree AROS header/module (path given,
APL/LGPL — ours), `[OURS]` this project's spikes (the H-series, `graft/*`, `hosted/*`),
`[EMU68]` an adopted MPL file (cite the Emu68 path; ships in the quarantine dir).
`[REF-CONFIRM]` items were checked against a reference but are restated from an
independent justification — Role B implements from that.

## Scope

**Decision — `[J0]` resolved: ADAPT Emu68's translator core; do NOT build fresh.**
Grounded in the three `[J0]` questions (Architecture, below, carries the detail):

1. **Separation is good enough to lift the core, bad enough that we must re-host the
   runtime.** Emu68's AArch64 **emitter** `include/A64.h` `[EMU68]` is a self-contained
   header of `static inline uint32_t` instruction-encoders (e.g. `mov_immed_u16`,
   `add_immed`, `b`, `bx_lr`) emitted as `*ptr++ = add_immed(...);` — it makes **no**
   MMU/MMIO/cache/RasPi calls and is cleanly portable. The per-opcode **decoders**
   (`src/M68k_LINE0.c`, `LINE4.c`, … `LINEF.c`, `M68k_MOVE.c`, `M68k_EA.c`, `M68k_CC.c`,
   `M68k_SR.c`, `M68k_MULDIV.c`) plus `RegisterAllocator.h` are portable logic but wired
   to Emu68 conventions (its JIT-cache bookkeeping, its software m68k cache `src/cache.c`,
   its fixed reg-mapping and exception model). The **machine-owning runtime** —
   `src/aarch64/{start.c,vectors.c,mmu.c}`, `src/raspi/*`, `src/pistorm/*`,
   `src/boards/*`, and the bare-metal cache asm in `src/support.c` — is **not** lifted;
   we re-host it. Writing a fresh m68k→AArch64 emitter from nothing would discard a
   battle-tested, ISA-complete encoder for no licence benefit (MPL is already adoptable).
2. **The block-exit model maps directly onto our hooks.** Every Emu68 translated block
   exits via `RET` (`bx_lr()`) back to a central C dispatcher `MainLoop()`
   (`src/ExecutionLoop.c`) — there is **no** block-to-block chaining to patch around. At
   exit the m68k PC sits in AArch64 **x18** and the architectural state is flushed to a
   `struct M68KState` (reached via `TPIDRRO_EL0`); `MainLoop` checks `M68KState.INT32`
   after each block. That single funnel (`[EMU68]` `ExecutionLoop.c`) is exactly where
   our LVO-call bridge and our hosted scheduler intercept — see Architecture §3.
3. **MPL containment is cheap and standard** (banner above) — a quarantine dir + our own
   ABI header keeps the copyleft to the Emu68 files; nothing in AROS becomes MPL.

**In.** A hosted host-side service that: takes a relocated 68k seglist entry from AROS,
translates 68k basic blocks to AArch64 into a `MAP_JIT` code cache, runs them, **bridges
a 68k library call (negative-offset `0x4EF9` vector) into a native AArch64 `AROS_LH`
call** on the real AROS library, and returns the program's 68k `d0` exit code — all
verifiable unattended by markers + register/arg/exit-code asserts.

**Out (non-goals, this spec).** 68881/68882 FP / NEON FP translation (integer-only core;
plumbing exists — `graft/cpu_aarch64.h` NEON state — but 68k↔IEEE semantics are a
separate sub-feature); self-modifying / dirty-code-page coherency beyond detection
(Emu68 has CRC32 unit-verification we keep, but full SMC tracking is later);
68k MMU/030-040 PMMU emulation; the `HUNK_OVERLAY` overlay loader; multi-core /
true-parallel translated execution (the H6 scheduler is single-threaded — see W^X /
threading); a full chipset (no chip RAM / custom registers — a 68k *program*, not a
whole Amiga; whole-machine emulation is what J-UAE does and is not this).

## Architecture

Three pieces, joined by a flat C-ABI we author (`jit68k.h`). Pieces (1) and (3) are
**ours** `[OURS]`; piece (2) is the **adopted Emu68 core** `[EMU68]` behind that ABI.

```
AROS side (aarch64, AROS crosstools)                Host side (Apple toolchain, our code + Emu68)
┌───────────────────────────────────┐               ┌──────────────────────────────────────────┐
│ dos: InternalLoadSeg_AOS  [AROS]   │               │  (1) W^X exec-memory layer  [OURS]/[PUB]   │
│   → relocated 68k seglist          │  hostlib +    │      mmap MAP_JIT code cache               │
│ RunCommand / CallEntry  [AROS]     │  H3 host-call │      pthread_jit_write_protect_np toggle   │
│   · tag entry as 68k ──────────────┼──────────────►│      sys_icache_invalidate                │
│   · divert to jit68k_run()         │   jit68k.h    │  (2) translator = ADAPTED Emu68  [EMU68]   │
│                                    │   C ABI       │      A64.h emitter + LINE* decoders        │
│ native AROS libraries (AArch64)    │◄──────────────┤      MainLoop dispatcher (re-hosted)       │
│   exec/dos/graphics … AROS_LH      │  LVO bridge   │  (3) dispatch + LVO-call bridge  [OURS]    │
└───────────────────────────────────┘  marshals     │      0x4EF9-vector → AROS_LH marshaller    │
        struct M68KState (D0-D7/A0-A7/PC/SR) ◄──read/write──┘  (the reverse of the H3 shim)
```

### (1) W^X-aware executable-memory layer — `[J1]` · `[OURS]` + `[PUB]`

The boot push hit the wall: *"W^X refuses a one-shot RWX anon mmap even with JIT
entitlements"* and *"executing code loaded into the pool (LoadSeg) needs the W^X-aware
path later"* (`NOTES.md`) `[OURS]`. The JIT code cache is a **separate** region from the
RW AROS RAM pool, mapped and patched per Apple's MAP_JIT contract `[PUB]`:

- **R-JIT-MAP.** `mmap(NULL, len, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0)`.
  The region is never `PROT_WRITE` and `PROT_EXEC` simultaneously via `mprotect`; writes
  happen only through the toggle below. `[PUB]` Apple "Porting Just-In-Time Compilers to
  Apple Silicon".
- **R-JIT-WRITE.** To emit/patch code: `pthread_jit_write_protect_np(0)` → write the
  AArch64 words → `pthread_jit_write_protect_np(1)`. The toggle is **per-thread state**
  `[PUB]` — see W^X / threading for the H4/H6 caveat.
- **R-JIT-ICACHE.** After patching a range and before executing it,
  `sys_icache_invalidate(addr, len)` `[PUB]`. Apple-silicon I-cache/D-cache are **not**
  coherent for self-written code; skipping this yields stale-I-cache wrong results, not a
  crash — so `[J1]` asserts the *value*, not "no crash". (This is the same
  clean-then-invalidate discipline Emu68 does bare-metal via `arm_flush_cache` +
  `arm_icache_invalidate` in `src/support.c` `[EMU68]`; we do **not** lift that asm — we
  use the macOS libc primitive.)
- **R-JIT-ENTITLE — RESOLVED by `[J1]`** (empirically, macOS 26.5, Apple silicon;
  `hosted/jit68k/`). The matrix, with all three signature states tested:
  - **Default linker ad-hoc signature (NO hardened runtime)** — `flags=0x20002
    (adhoc,linker-signed)`, what `clang -arch arm64 -o foo` already produces:
    `mmap(MAP_JIT)` + the toggle + execution **work with NO codesign step at all**.
    So the plain `make hosted-jit68k` build is green unsigned-by-hand. This is also
    why the existing ad-hoc-signed `run.sh`/`bootrun.sh` path did *not* block the JIT
    by itself — the block was the **hardened runtime**, below.
  - **Hardened runtime, NO entitlement** (`codesign -s - -f -o runtime`, `flags=0x10002
    (adhoc,runtime)`): `mmap(MAP_JIT)` fails **EINVAL (errno 22)**. This is exactly the
    boot-push wall.
  - **Hardened runtime + `com.apple.security.cs.allow-jit`** (the entitlements plist
    `hosted/jit68k/jit68k.entitlements.plist` + `codesign -s - -f -o runtime
    --entitlements …`): works again.
  **Minimal incantation:** `com.apple.security.cs.allow-jit` is the *single* key needed,
  and only once the hardened runtime is on. The exact command the AROS bootstrap needs
  (it already passes `-o runtime`, see `graft/bootrun.sh`): `codesign -s - -f -o runtime
  --entitlements jit68k.entitlements.plist <binary>`. (`com.apple.security.cs.allow-unsigned-executable-memory`
  is **not** required — `allow-jit` + MAP_JIT is the W^X-clean path.) This retires the
  deferred-LoadSeg W^X wall.

This layer is `[OURS]`, native macOS, not AROS crosstools — the peer of `hosted/display.c`,
implemented in `hosted/jit68k/jit_region.{h,c}` as a reusable API
(`jit_region_alloc`/`jit_write_begin`/`jit_write_end`/`jit_finalize`/`jit_region_free`)
that `[J2]` (the adapted Emu68 emitter) and the native `LoadSeg` path both build on.

### (2) The translator — ADAPTED Emu68 core behind `jit68k.h` · `[EMU68]`

**What we lift, verbatim, into the quarantine dir** (keep MPL headers):

- **The AArch64 emitter** `include/A64.h` `[EMU68]` — `static inline uint32_t` encoders,
  no runtime coupling. The single highest-value asset; re-deriving it is the work `[J0]`
  declines.
- **The per-opcode decoders** `src/M68k_LINE{0,4,5,6,8,9,B,C,D,E,F}.c`, `M68k_MOVE.c`,
  `M68k_MULDIV.c`, EA decode `M68k_EA.c`, condition codes `M68k_CC.c`/`M68k_SR.c`,
  exception emit `M68k_Exception.c`, and the register-allocator interface
  `include/RegisterAllocator.h` + `src/aarch64/RegisterAllocator64.c` `[EMU68]`.
- **The translation front-end** `src/M68k_Translator.c` (`M68K_GetTranslationUnit`,
  `M68K_Translate`, `M68K_TranslateNoCache`, `M68K_VerifyUnit`, the `ICache`/`LRU`
  hash-cache) and the dispatcher `src/ExecutionLoop.c` (`MainLoop`) `[EMU68]`.

**What we DO NOT lift — we provide our own behind the ABI** (these are the re-hosting
points where Emu68 assumed it owned the machine):

| Emu68 bare-metal thing | Our replacement |
|---|---|
| `src/aarch64/start.c` `_boot`/MMU enable/EL detect | nothing — we are a hosted dylib; no boot, no EL |
| `src/aarch64/vectors.c` (owns EL1 VBAR) | no host vectors; 68k exceptions handled in C (Risks) |
| `src/aarch64/mmu.c` page tables | the 68k sandbox is plain AROS `AllocMem` RW memory `[AROS]` |
| `src/support.c` `arm_flush_cache`/`arm_icache_invalidate` (bare-metal cache asm) | `sys_icache_invalidate` + the MAP_JIT toggle (R-JIT-*) `[PUB]` |
| `src/raspi/*`, `src/pistorm/*`, `src/boards/*` | nothing — no firmware, no FPGA bus, no SD/UART |
| `src/tlsf.c` code-buffer allocator | code lands in the MAP_JIT region (R-JIT-MAP) |

**Adaptation note (the one real surgery).** Emu68's emitted blocks and `M68K_Translator.c`
call its hardware cache flush at unit-emit time and use Emu68's exception/register
conventions. The adaptation: (a) route unit-emit's "make executable" through R-JIT-WRITE
+ R-JIT-ICACHE instead of `arm_*_cache`; (b) intercept the post-block funnel in `MainLoop`
(below) for our LVO bridge and scheduler. Both are at the **unit boundary**, not inside
the per-opcode emitters — so the high-value decoder/emitter logic is reused unmodified.
The lifted files that we *do* edit stay MPL and are disclosed (banner).

**Block / dispatch model (verified from Emu68 master) `[EMU68]`:**
- Unit = an *extended* basic block: straight-line across the m68k stream until a hard
  terminator (RTS/JMP/exception emit the `0xffffffff` sentinel → `break_loop`) or the
  instruction cap `var_EMU68_M68K_INSN_DEPTH` (`JIT_CONTROL`-tunable, ≤255). Cached in
  `struct M68KTranslationUnit { mt_M68kAddress; mt_ARMEntryPoint; mt_CRC32; mt_ARMCode[]; … }`
  in a PC-keyed hash table `ICache[]` + an `LRU` list.
- **Exit is a central-dispatcher RET, no chaining.** A block ends with `bx_lr()` after the
  epilogue flushes live state (`EMIT_FlushPC`, `RA_StoreDirtyM68kRegs`, `RA_StoreCC`).
  `MainLoop` calls the block via a C function pointer (`mt_ARMEntryPoint`), then re-reads
  the m68k PC and loops. The only intra-block "chain" is a self-loop guarded by a
  `cbz ctx->INT` interrupt poll. **This single funnel is our intercept point** (§3).
- **Register state at the funnel** (Emu68's fixed map, from
  `internals/RegisterMapping.html`, cross-checked against the `mrs`/`msr` encodings in
  source `[EMU68]`): m68k D0–D7 → W19–W26, A0–A4 → W13–W17, A5–A7 → W27–W29, **PC → x18
  (`REG_PC`)**, SR/CCR → `TPIDR_EL0`, the `struct M68KState*` context → `TPIDRRO_EL0`.
  At each block return the full architectural state is in memory (`struct M68KState`),
  PC in x18 — so our bridge reads/writes registers there, no JITed-code patching needed.

### (3) Dispatch + the LVO-call bridge — `[OURS]`

The bridge is the spec's load-bearing integration boundary; it has its own section next.

## The integration boundary / C ABI (`jit68k.h`)

Hand-authored, ASCII, **no Emu68 declarations copied in** (banner). It is the only
contact surface between AROS-crosstools code and the MPL translator. AROS calls *into*
it; it calls *back* into AROS through a registered marshal callback.

```c
/* jit68k.h — our boundary header (NOT MPL; no Emu68 source copied here). */
typedef struct JIT68K JIT68K;                /* opaque translator instance */

/* The 68k sandbox: AROS-owned RW memory (AllocMem), 32-bit big-endian address space.
   base = host pointer to where 68k address 0 maps; size in bytes (<= 4 GiB sandbox). */
JIT68K *jit68k_create(void *sandbox_base, uint32_t sandbox_size);
void    jit68k_destroy(JIT68K *);

/* Called when translated 68k code hits a negative-offset library vector (0x4EF9).
   libbase  = the 68k-space address of the library base the call targets;
   lvo      = the positive LVO index n  (vector sits at libbase - n*6);
   regs     = pointer to the live m68k register file (D0-D7,A0-A7) in M68KState,
              host-readable; the callee marshals from / returns into it.
   Return:  0 = handled (translator resumes at the 68k return address);
            nonzero = unhandled (raise a 68k exception). */
typedef int (*jit68k_lvo_fn)(void *user, uint32_t libbase, int lvo, struct m68k_regs *regs);
void    jit68k_set_lvo_handler(JIT68K *, jit68k_lvo_fn, void *user);

/* Run from a 68k entry PC until the program returns (top-level RTS) or faults.
   On return *exit_d0 = the final m68k D0 (the program's exit code). */
int     jit68k_run(JIT68K *, uint32_t entry_pc, uint32_t *exit_d0);

/* The hosted scheduler asks the translator to yield at the next block boundary. */
void    jit68k_request_yield(JIT68K *);
```

**How a 68k library call reaches a native AROS library — the core marshalling** `[AROS]`:

1. **Recognise the call, don't emulate the vector.** A 68k caller reaches library function
   *n* by computing `libbase - n*6` and jumping to a 6-byte `JumpVec { uint16 jmp=0x4EF9;
   void *vec; }` (`arch/m68k-all/include/aros/cpu.h`: `__AROS_ASMJMP 0x4EF9`,
   `__AROS_GETJUMPVEC(lib,n) = &((JumpVec*)lib)[-(n)]`, `__AROS_USE_FULLJMP` defined for
   m68k) `[AROS]`. The translator does **not** emit a native branch into a fake 68k
   vector. Instead, the adapted dispatcher detects that the next-block PC (x18 at the
   funnel) lands in the **negative-vector address range** of a registered library base,
   recovers `n = (libbase - pc)/6`, and invokes `jit68k_lvo_fn` rather than translating.
   **UNVERIFIED:** the exact recognition trigger — PC-range check at the `MainLoop` funnel
   vs. a reserved-address translation stub vs. a planted `0xFxxx` trap whose
   `EMIT_Exception` we redirect; Role B picks the cheapest in `[J3]`. (Emu68's own trap
   path routes through the 68k VBR and stays in m68k space `[EMU68]`, so the funnel/PC-range
   approach — which exits to host C — is the natural fit.)
2. **Read the 68k argument registers.** At the funnel the m68k register file is in
   `struct M68KState` (D0–D7 in W19–W26, A0–A7 in W13–W17/W27–W29 — Emu68 map `[EMU68]`).
   `jit68k_lvo_fn` receives them as `struct m68k_regs *` (D and A arrays, host-readable,
   already byte-order-normalised to host 64-bit registers).
3. **Marshal into a native `AROS_LH` call via the per-function register map.** Each AROS
   library function declares which 68k register each argument arrives in, with
   `AROS_LHA(type,name,reg)` (`compiler/arossupport/include/libcall.h`, `__AROS_LHA`) or
   `AROS_UFHA(type,name,reg)` for hook/user funcs (`compiler/arossupport/include/asmcall.h`,
   e.g. `AROS_UFHA(struct Hook *, h, A0)`) `[AROS]`. That `(A0,D0,…)` map is the exact,
   authoritative table the bridge needs: for LVO *n* of a given library, read each
   declared arg from its 68k register in `struct m68k_regs`, place it in the matching
   AArch64 AAPCS64 argument register, and call the **real** native AArch64 library
   function. This is the **reverse** of the H3 host-call shim (`hosted/abishim.S`, which
   marshals AROS_LH→host C ABI) — here it runs 68k-regs → AArch64 AROS_LH, and is
   **data-driven by the FD/register table**, not by a variadic stack walk.
4. **Return value back to 68k.** The native function's return goes into the 68k `d0`
   (W19 in `M68KState`) `[AROS]`/`[EMU68]`; the bridge sets it, returns 0, and the
   dispatcher resumes at the 68k return address (the longword the `bsr`/`jsr` pushed on
   the 68k stack). 32-bit results occupy `d0`; pointer/`BPTR` results stay in the 32-bit
   big-endian sandbox.
5. **Pointer translation.** Any pointer argument is a 32-bit big-endian *sandbox* address;
   the bridge converts it to a host pointer (`sandbox_base + addr`) before the native
   call, and converts a returned host pointer that lies in the sandbox back to a 32-bit
   sandbox address. **UNVERIFIED:** which native AROS calls return pointers *outside* the
   sandbox (e.g. a freshly `AllocMem`'d host buffer) and therefore need the 68k program's
   memory to come from a sandbox-backed allocator — flagged in Risks (32-bit sandbox).

**How the bridge is wired** `[AROS]`: AROS registers each library it exposes to 68k code
(its 68k-space base + a pointer to its FD/LVO→register table) with the translator via the
host-call boundary, then sets `jit68k_set_lvo_handler` to an AROS callback that does
steps 2–4. The native libraries are unchanged — they receive an ordinary `AROS_LH` call.

## AROS-side dispatch — `[AROS]`

LoadSeg is **reused unchanged** — we do not reimplement the loader or relocator:

- **Load + relocate (already correct, already present).** `InternalLoadSeg_AOS`
  (`rom/dos/internalloadseg_aos.c`), selected by file magic `0x000003f3` in
  `rom/dos/internalloadseg.c`, allocates one buffer per hunk, chains them as a seglist
  (payload at `BADDR(seg)+sizeof(BPTR)`, `GETHUNKPTR`), and applies `HUNK_RELOC32`
  (absolute, 32-bit **big-endian**: `val = AROS_BE2LONG(*addr) + (IPTR)GETHUNKPTR(target)`)
  `[AROS]`. This already produces a relocated 68k seglist *today* on this Mac — it just
  has nothing to execute it. The JIT supplies exactly that.
- **The single hook: the seglist entry.** `RunCommand` (`rom/dos/runcommand.c`) computes
  the entry `args.Args[2] = (IPTR)BADDR(segList) + sizeof(BPTR)` and `NewStackSwap`s into
  `CallEntry` (verified: runcommand.c:134,137) `[AROS]`. On m68k the entry thunk is
  `arch/m68k-all/dos/callentry.S` `AOS_CallEntry`, which takes the entry in **A4**
  (`AROS_UFHA(LONG_FUNC, entry, A4)`, verified callentry.S:18) and branches native
  `[AROS]`. **The change is here:** where the AArch64 host would branch native, it instead
  recognises a 68k seglist and calls `jit68k_run(jit, entry_pc, &exit_d0)` — handing the
  entry PC to the translator instead of executing it as AArch64.
- **Tagging a seglist as 68k.** `InternalLoadSeg_AOS` was reached *because* the file magic
  was the hunk magic `0x000003f3` — so the loader path inherently knows the result is 68k.
  The seglist must carry that fact to the `RunCommand`/`CallEntry` decision point.
  **UNVERIFIED (Role B to resolve):** the tagging mechanism — a flag stored alongside the
  seglist (e.g. in the DOS-side segment bookkeeping), a distinguished seglist sentinel, or
  a per-task/per-process "is-68k" attribute set on the load path. The design doc defers
  this to `[J4]`'s design; it is the one genuinely new AROS-side data-flow change and must
  not be guessed. The 68k code/data/BSS hunks live in the 32-bit big-endian sandbox the
  relocator already produced.

## W^X / threading model — the load-bearing constraint

Restated from two independent facts, not from any reference:

- **`[PUB]` Apple MAP_JIT contract.** A MAP_JIT region is the only way to get executable
  JIT memory under hardened-runtime W^X; you toggle writability with
  `pthread_jit_write_protect_np(0/1)`, and **that toggle is per-thread** — it affects only
  the calling thread's view of all MAP_JIT pages. You must `sys_icache_invalidate` written
  ranges (I/D-cache not coherent for self-modified code). The process needs
  `com.apple.security.cs.allow-jit`.
- **`[OURS]` H4/H6 single-thread scheduler.** The hosted exec scheduler runs all AROS
  tasks on a **single underlying host thread** (H6, `NOTES.md`; `hosted/kern.c`), modelling
  the macOS main thread as the boot/anchor task (H4). There is no true host-thread
  parallelism among AROS tasks today.

**Requirement R-JIT-THREAD.** Because the write-protect toggle is per-thread, **all JIT
emission and all execution of translated blocks must happen on the same host thread** —
the single scheduler thread (H6). Under the current single-thread model this is satisfied
automatically: the AROS task running the 68k program, the emit (R-JIT-WRITE window), and
the block execution all sit on the one scheduler thread. The toggle and
`sys_icache_invalidate` run on that thread before the block is entered. **This is also a
constraint on the future:** if the H4/H6 scheduler ever dispatches AROS tasks onto
multiple host threads, the JIT breaks unless each emitting/executing thread re-asserts the
toggle on itself — **UNVERIFIED** interaction, flagged in design doc Risks, owned by the
scheduler milestone, not this spec. Yielding a long-running 68k program to the scheduler
uses `jit68k_request_yield` → honoured at the next `MainLoop` block boundary (Emu68's
`INT32`-check funnel `[EMU68]`), bounding yield latency to ≤255 m68k instructions.

## Verification (unattended — `[OURS]` H-series discipline)

No TCC, no Screen-Recording, no manual click. Each spike is a standalone binary printing a
unique `[J1]`…`[J4]` marker with PASS/FAIL, greppable like `[H1]`…`[H12]`. Every PASS
asserts a **value** (returned constant, matching register set, observed args, exit code) —
no-crash is necessary but never sufficient, so a silent mistranslation cannot pass.

- **`[J1]` MAP_JIT stub returns a constant — IMPLEMENTED / GREEN.** `mmap` a MAP_JIT
  region; inside a `pthread_jit_write_protect_np(0/1)` window write a hand-assembled
  AArch64 stub (`movz w0,#0x6804; ret`); `sys_icache_invalidate`; call it via a function
  pointer. **PASS (observed):** the call returns `0x6804`; a control stub returns `0x1ED5`
  from freshly-written code in the same region (proving live codegen, not a cached
  result); and the negative control — a write WITHOUT the toggle, run in a forked child —
  **faults with SIGBUS (signal 10)**, proving W^X genuinely bites and that our toggle is
  what makes the write legal. Files: `hosted/jit68k/{jit_region.h,jit_region.c,j1_test.c}`;
  target `make hosted-jit68k` (plain) / `make hosted-jit68k-hardened` (hardened runtime
  + entitlement). The R-JIT-ENTITLE incantation is recorded above (resolved). This
  retires the deferred-LoadSeg W^X wall. **FAIL** would be SIGBUS/SIGSEGV/EXC_BAD_ACCESS,
  or a wrong value (stale I-cache).
- **`[J2]` one 68k block vs a reference register-trace — IMPLEMENTED / GREEN.** The fixed
  block `moveq #10,d0 ; moveq #7,d1 ; add.l d1,d0 ; rts` (expect `d0=17`) is translated to
  AArch64 by the **adopted Emu68 emitter** (the verbatim, MPL-quarantined
  `hosted/jit68k/emu68/A64.h`), written into the `[J1]` MAP_JIT region via the
  `jit_region` API, and **executed under W^X**. **PASS (observed):** the JITed register
  file (`d0–d7/a0–a7/CCR/PC`) is **bit-identical** to the reference on all 18 fields and
  `d0 == 17` (`0x11`). Files: `hosted/jit68k/{j2_jit68k.h,j2_build.c,j2_interp.c,j2_test.c}`
  + the quarantine `hosted/jit68k/emu68/{A64.h,RegisterAllocator.h,NOTICE}`; target
  `make hosted-jit68k-j2` (plain ad-hoc — `flags=0x20002`, no codesign step, MAP_JIT
  works). **FAIL** would be any register diverging, `d0 != 17`, or a fault/hang (a 10 s
  SIGALRM watchdog converts a hang into `[J2] FAIL`).

  *Reference oracle — DEVIATION from the spec's recommendation, deliberate.* The spec
  above recommended verifying against **Emu68's own `M68K_TranslateNoCache`**. Role B
  chose instead a **tiny from-scratch independent 68k interpreter** (`j2_interp.c`, OURS —
  uses NO Emu68 code) as the oracle. Rationale: (a) verifying the Emu68 emitter's output
  against *Emu68's own decode* is close to a tautology — it cannot catch a wrong-semantics
  hand-decode, only a wrong *emit*; an independent interpreter catches both. (b) Lifting
  `M68K_TranslateNoCache` would require pulling in the whole decoder + register-allocator +
  `M68KState`/`ICache` machinery — the coupled runtime `[J0]` says is `[J3]` scope — just to
  build the `[J2]` oracle, defeating the point of a minimal emitter-only spike. The
  independent interpreter is the stronger, cheaper oracle and is what makes the PASS a
  genuine cross-check. (Vendored Musashi was considered and rejected as unnecessary weight.)

  *Decoder: HAND-ROLLED, not lifted (honest scope statement).* This spike does **not** lift
  Emu68's `M68K_LINE*.c` decoders or `RegisterAllocator64.c`. The two opcodes are
  **hand-decoded** in `j2_build.c` (OURS) and turned into AArch64 by *calling* Emu68's
  encoders. So `[J2]` proves the **emitter** path only; full decoder + register-allocator
  adoption is `[J3]`. The hand-decode also takes two documented shortcuts that a real
  decoder must generalise: `moveq` is emitted via `mov_immed_u16` (zero-extend), correct
  only because the operands `10`/`7` are `< 0x80` (real `moveq` sign-extends the 8-bit
  immediate); and the CCR is statically folded to `XNZVC=0` (valid only because the
  asserted operands produce no flags) — the independent interpreter computes CCR the long
  way (full `moveq`/`add.l` flag rules) and the test asserts they agree, so the static fold
  only passes if it matches the from-scratch flag computation.

  *`[J0]`-separability — VALIDATED (the concrete finding).* Emu68's AArch64 emitter
  separated **cleanly** from its bare-metal runtime, exactly as `[J0]` bet. Total cost to
  host it: **two vendored files** (`A64.h` + the declarations-only `RegisterAllocator.h`,
  needed only so `A64.h`'s verbatim `#include <RegisterAllocator.h>` resolves) and **one
  no-op stub** (`void kprintf(const char*,...)`, the single external symbol `A64.h`
  references — via the `ASSERT_REG` debug macro; never actually called for valid registers).
  No MMU/cache/MMIO/RasPi/PiStorm symbol is touched; `A64.h`'s pure-encoder section
  (lines 1–650) needs only `<stdint.h>`. The register-map constants (`REG_D0..REG_D7` =
  W19..W26) and encoders (`mov_immed_u16`, `add_reg`, `ldr_offset`, `str_offset`,
  `bic_immed`, `add_immed`, `ret`) were reused verbatim; emitted words were cross-checked
  against `llvm-mc` and run correctly. Endianness: `A64.h`'s `I32()` normaliser is identity
  on little-endian arm64, so words land as native AArch64. **Conclusion: the highest-value
  asset (`A64.h`) is genuinely portable; the `[J0]` "adopt the emitter, re-host the runtime"
  bet holds.** The licence boundary that worked: `A64.h`/`RegisterAllocator.h` verbatim in
  `hosted/jit68k/emu68/` with their MPL Exhibit-A headers + a `NOTICE` (upstream repo,
  commit `305f686f84712f88c4d80d35769af5c60a4e988b` / v1.0.7, file list, Exhibit-B
  check = clean); our glue/interpreter in separate AROS-licensed files that *call* the
  encoders without copying any Emu68 source (MPL FAQ Q11–Q13).
- **`[J3]` LVO-call thunk arg/return capture — IMPLEMENTED / GREEN.** The integration
  boundary (68k library call → native AArch64 `AROS_LH`) is proven in three grounded,
  value-asserting parts. Files: `hosted/jit68k/{j3_jit68k.h,j3_vector.c,j3_marshal.c,
  j3_test.c}`; target `make hosted-jit68k-j3` (plain ad-hoc, MAP_JIT works). **PASS
  (observed):**
  1. **Vector recognition (the dispatch math).** `j3_vector.c` restates `__AROS_GETJUMPVEC`
     with the **target** stride: vector *n* sits at `libbase − n*6` (`J3_M68K_LIB_VECTSIZE=6`,
     because `struct JumpVec` is packed 6 bytes on m68k — `UWORD jmp + 32-bit vec`,
     `AROS_SIZEOFPTR==4`). Grounded against the REAL contract `arch/m68k-all/include/aros/cpu.h`
     lines 50-54 (`struct JumpVec`), 61 (`__AROS_ASMJMP 0x4EF9`), 81 (`LIB_VECTSIZE =
     sizeof(struct JumpVec)`), 82 (`__AROS_GETJUMPVEC(lib,n) = &((JumpVec*)lib)[-(n)]`).
     **A deliberate host/target divergence the spike gets right:** host `sizeof(struct
     JumpVec)` is **16** (8-byte host pointer), not 6 — using 6 is the whole point of
     grounding against the m68k ABI. The PASS round-trips `n ∈ {1,2,5,42,119}` through
     `addr→recognise` and rejects a PC above the base and a 6-misaligned PC (both → −1).
  2. **The marshaller (the REVERSE of the H3 shim) — EMITTED, not hand-trampolined.**
     *Realization choice: emit the fixed marshal sequence via the adopted Emu68 emitter
     (`emu68/A64.h`) into a `jit_region`* — the spec's PREFERRED path, because it is the
     real translated-code path and directly extends `[J2]` (same emitter, same MAP_JIT
     region, same `jit_region` API), rather than a hand-written reverse-H3 `.S` trampoline
     (which would prove the marshalling but not that it lives in the JIT pipeline). The
     emitted thunk (`j3_marshal.c`, AAPCS64 `void thunk(M68KState*)`): frame prologue
     (`stp64`) → `mov64 x19,x0` (keep the state ptr across the call) → for each native
     arg *i*, `ldr w_i,[x19,#off(src[i])]` (32-bit load of the source 68k register, zero-
     extends into the 64-bit arg register) → build the stub address with
     `mov64/movk64_immed_u16` → `blr` → (if the LVO returns a value) `str w0,[x19,#d0]` →
     epilogue/`ret`. Three native stubs with **different** register maps were each declared
     with the REAL macros and the descriptor's source-register list taken DIRECTLY from
     them: `Square` `AROS_LHA(ULONG,n,D0)` (return → d0); `WriteN`
     `AROS_LHA(ULONG,count,D0)+AROS_LHA(APTR,buf,A0)` (the mixed D/A case, most prone to
     mismarshal); `Blit` `AROS_LHA(APTR,src,A1)+AROS_LHA(ULONG,len,D1)+AROS_LHA(UWORD,
     mode,D2)` (non-first, out-of-order registers, **void** return). Grounded against:
     `AROS_LHA(t,n,r)=t,n,r` (`compiler/arossupport/include/libcall.h:1586`),
     `AROS_UFHA(t,n,r)=t,n,r` (`compiler/arossupport/include/asmcall.h:822`); the third
     element `reg` is the m68k source register (`D0..A7 → %d0..%a7`,
     `arch/m68k-all/include/aros/cpu.h:125-139`), which the call machinery loads each
     argument into before the library jump (`arch/m68k-all/include/gencall.c`:
     `__AROS_LSA="%"#reg` line 309, `asm_regs_init` line 201 `jsr o*LIB_VECTSIZE(%a6)`); the
     32-bit result is read back from `%d0` (`gencall.c:153-157,66`, `_ret0 asm("%d0")`) — so
     the bridge stores the native return into `M68KState.d[0]`.
  3. **Verify (value-asserting).** Each stub records the exact args it received; the test
     asserts every arg equals the seeded 68k register value marshalled into the right AAPCS64
     slot, that d0 received each return, and that the void function leaves d0 unchanged. A
     NONZERO seeded state proves it read the REAL registers (not a zeroed-vs-zeroed match). A
     **negative control** (swap `WriteN`'s `{D0,A0}` source order in the descriptor) was run
     and correctly produces three MISMATCH lines + `[J3] FAIL` exit 1 — so the PASS is a
     genuine cross-check, not a tautology. Watchdog: 10 s SIGALRM → `[J3] FAIL`.

  *Honest scope / deferrals carried to `[J4]` (and noted in the source).* (i) The `[J2]`
  decoder deferral STANDS: this spike **hand-constructs** the call scenario (the descriptor +
  a seeded `M68KState`) rather than **decoding a real `jsr`-through-vector from a 68k stream**
  — adopting Emu68's `M68k_LINE*.c`/`M68k_EA.c`/`M68k_CC.c`/`M68k_SR.c` + `RegisterAllocator64.c`
  (which also generalises the `[J2]` `moveq` sign-extension and per-opcode CCR shortcuts) is
  still `[J4]`. (ii) **Pointer/sandbox marshalling is NOT done:** the spike keeps all pointer
  args (e.g. `A0`/`A1`) as in-range 32-bit sandbox addresses and passes them through verbatim;
  it does **not** convert `sandbox_base + addr` for the native call, nor handle a native call
  that returns or takes memory *outside* the sandbox (the return-pointer case). How 68k `An`
  pointers map to native host addresses, and a sandbox-backed allocator for 68k-visible memory,
  are an open `[J4]`/production concern (Risks: Endianness / 32-bit sandbox). (iii) `moveq`
  sign-extension / real per-opcode CCR are decoder work, not `[J3]`.
- **`[J4]` real hunk binary, load→relocate→run chain — IMPLEMENTED / GREEN.** This is a
  SCOPED increment: it proves the **load → relocate → place-in-sandbox → translate → run →
  return** pipeline end-to-end for a REAL (hand-assembled) big-endian 68k hunk binary whose
  entry code uses only the register-only opcodes the `[J2]` path handles (`moveq`/`add`/
  `rts`). It does **not** lift the full Emu68 decoder/RA — that mountain is `[J5]`, below.
  Files: `hosted/jit68k/{j4_hunk.h,j4_loader.c,j4_build.c,j4_test.c}`; target
  `make hosted-jit68k-j4` (plain ad-hoc, MAP_JIT works). **PASS (observed):**
  1. **Minimal hunk loader + relocator (OURS, `j4_loader.c`).** Parses a big-endian
     AmigaOS hunk file — `HUNK_HEADER` (name list, `numhunks`/`first`/`last`, per-hunk
     size words with the FAST/CHIP flag bits masked), then `HUNK_CODE`/`HUNK_DATA`/
     `HUNK_BSS` payloads and `HUNK_RELOC32` tables, to `HUNK_END`. Hunk-type constants
     grounded against `compiler/include/dos/doshunks.h` (`HUNK_HEADER 1011`, `HUNK_CODE
     1001`, `HUNK_DATA 1002`, `HUNK_BSS 1003`, `HUNK_RELOC32 1004`, `HUNK_END 1010`).
     Each hunk is bump-allocated inside a **32-bit sandbox** (a `malloc`'d host region
     with a nonzero 68k-space origin `0x00210000`; 68k addr `a` → `host_mem[a−origin]`).
  2. **`HUNK_RELOC32` applied EXACTLY as `rom/dos/internalloadseg_aos.c:292-332`.** Per
     `(target-hunk, offset)` pair: read the BE32 value at `GETHUNKPTR(lasthunk)+offset`
     (the patch site lives in the most-recently-loaded CODE/DATA hunk, `lasthunk`, line
     288), compute `val = AROS_BE2LONG(*addr) + (IPTR)GETHUNKPTR(target)` (line 323 — add
     the **target** hunk's runtime base, which in the sandbox is its 32-bit runtime
     address), and write it back big-endian `*addr = AROS_LONG2BE(val)` (line 326). The
     only difference from the real loader is *where* hunks live (sandbox bump-alloc vs
     `AllocMem`+`MEMF_31BIT`); the relocation math is byte-identical.
  3. **The test binary actually exercises relocation.** `j4_test.c` hand-assembles (big-
     endian) a `HUNK_CODE` (`moveq #42,d0 ; rts` = `0x702A4E75`) + a `HUNK_DATA` holding
     a 32-bit pointer slot with a **nonzero addend** `0x8` + a `HUNK_RELOC32` patching
     `DATA[0]` with the **CODE** hunk's base — so after relocation `DATA[0]` must read
     `CODE_base + 8` (proving `val = original + base`, not a no-op write of the base).
  4. **Run via the `[J2]` translate→emit path.** `j4_build.c` hand-decodes the register-
     only entry hunk **read from the relocated sandbox** (not a hardcoded block) and emits
     the AArch64 with the adopted Emu68 encoders (`emu68/A64.h`) into a `jit_region`,
     running it under W^X; the 68k `d0` it leaves is the exit code.
  5. **Verify (value-asserting), PASS iff BOTH hold:** (a) the relocated `DATA[0]`, read
     back **byte-exact big-endian** from the sandbox, equals `0x00210008` (= `CODE_base
     0x00210000` + addend `0x8`); (b) the executed entry returns `d0 == 42`. A **negative
     control** loads the same binary with relocation SKIPPED → `DATA[0]` holds only the
     raw addend `0x8` (≠ the relocated pointer), proving the relocation assert genuinely
     bites. Watchdog: 10 s SIGALRM → `[J4] FAIL`.

  *AAPCS64 fix (a real bug `[J4]` surfaced).* The Emu68 m68k→AArch64 map puts D0..D7 in
  **x19..x26**, which are **callee-saved** in AAPCS64 — so a translated block entered as a
  C function pointer **must** save/restore them (the host-context save Emu68's `MainLoop`
  does before entering a unit). `[J2]`/`[J3]`'s emitted blocks omitted this and passed only
  because their test callers happened not to keep live values in x19..x26 across the call;
  at `-O2` `j4_run_entry` does, so the block corrupted the caller and faulted. `j4_build.c`
  wraps the body in a proper `stp/ldp` prologue/epilogue preserving x19..x28 + x29/x30 — a
  latent-bug fix the real decoder path must keep.

  *Honest deferrals carried into `[J5]` (stated in source + below).* (i) **Register-only
  entry code only:** the run path hand-decodes `moveq`/`add.l Dm,Dn`/`rts`; the `moveq`
  immediate is emitted via `mov_immed_u16` (zero-extend) and `j4_build.c` **rejects** an
  immediate `≥ 0x80` so the `[J2]` zero-vs-sign-extend shortcut can never silently lie.
  (ii) **No real `jsr`-through-vector decode** — the entry doesn't call a library LVO; the
  `[J3]` LVO-call bridge is proven separately and not wired into a decoded `jsr` here.
  (iii) **No pointer/sandbox boundary for memory ops or library calls from the running
  program** — the relocated `DATA` pointer is asserted at rest in the sandbox; the running
  block does not dereference it. (i)–(iii) were exactly `[J5]`. **`[J5a]` (below) now does
  the memory-op half of (iii)** — the sandbox-pointer boundary for load/store through `An`
  (the running block *does* dereference, byte-exact-verified); (ii) and the library-calls /
  return-pointer half of (iii) remain `[J5b]`, and (i)'s `moveq` sign-extend generalisation
  is `[J5b]` decoder work.

- **`[J5a]` memory load/store + the sandbox-pointer boundary — IMPLEMENTED / GREEN.** A
  SCOPED increment of the `[J5]` mountain: it proves a small block that TOUCHES MEMORY
  (`move.l (a0),d0 ; addq #1,d0 ; move.l d0,(a0) ; rts` — load a longword from the sandbox
  via A0, increment, store back), translated by the adopted Emu68 *emitter*, with the 68k
  `An` → host-pointer **sandbox boundary** around every load/store, verified byte-exact
  (registers AND sandbox memory) against an independent reference. Files:
  `hosted/jit68k/{j5a_jit68k.h,j5a_build.c,j5a_interp.c,j5a_test.c}` (reuses the `[J4]`
  loader `j4_loader.c`/`j4_hunk.h` for load+relocate); target `make hosted-jit68k-j5a`
  (plain ad-hoc, MAP_JIT works). **PASS (observed):** JITed `d0`/sandbox memory both
  `0x12345679` (= initial `0x12345678` + 1), byte-identical big-endian to the interpreter,
  `fault=0`.

  *RA/EA adoption finding — the primary deliverable. HAND-ROLLED, because Emu68's
  `M68k_EA.c` + `RegisterAllocator64.c` do NOT lift incrementally for a hosted sandbox.*
  Three concrete, citable blockers (Emu68 commit `305f686`, v1.0.7), recorded in
  `j5a_jit68k.h`:
  1. **`An` is dereferenced as a HOST pointer, no sandbox base.** In
     `M68k_EA.c:EMIT_LoadFromEffectiveAddress`, mode 2 `(An)` size 4 emits
     `reg_An = RA_MapM68kRegister(&ptr, src_reg+8); *ptr++ = ldr_offset(reg_An, *arm_reg, 0);`
     — `ldr w_dst,[x_An]` straight off the 68k address register (`M68k_EA.c:635-639`; the
     store mirror in `EMIT_StoreToEffectiveAddress`). Emu68 can do this because bare-metal
     it maps the 68k address space **1:1** onto host RAM via its MMU (`src/aarch64/mmu.c` —
     not lifted). Our sandbox needs `host = sandbox_base + An`, and there is **no hook
     inside the per-opcode emit** to insert that base add — adopting EA would require
     EDITING the MPL file at the load/store site (surgery *inside* the opcode emit, not at
     the unit boundary the adaptation note keeps clean).
  2. **Every extension word is read through Emu68's SOFTWARE instruction cache.**
     `M68k_EA.c` reads displacements/immediates via `cache_read_16(ICACHE, …)` at ~26 sites
     (`M68k_EA.c:760,765,772,…`); `ICACHE`/`cache_read_16` are Emu68's software m68k cache
     (`include/cache.h`, `src/cache.c`). Adopting `M68k_EA.c` pulls in `cache.c`+`cache.h`+
     the `ICACHE`/`DCACHE` model wholesale.
  3. **The register allocator assumes SR/context live in EL0 system registers.**
     `RegisterAllocator64.c` reads the condition codes with `mrs(reg,3,3,13,0,2)`
     (`TPIDR_EL0`) and the `M68KState` pointer with `mrs(…,3,3,13,0,3)` (`TPIDRRO_EL0`) —
     `RegisterAllocator64.c:288-303` (`RA_GetCC`), `:175-185` (`RA_GetCTX`). That is a
     bare-metal convention (Emu68 owns those thread-ID system registers); our hosted blocks
     pass the state pointer in `x0` and keep CCR in the `struct M68KState` in memory. It also
     drags in `support.h` + `M68k.h` (the `struct M68KState`).

     **Conclusion (architectural):** the EA decode + register allocator are **not adoptable
     piecemeal** — they are coupled to three parts of the bare-metal runtime `[J0]` re-hosts:
     the **1:1 MMU address model**, the **software instruction cache**, and the
     **EL0-system-register SR/context model**. So `[J5b]`'s register allocator must be
     **OURS**, built *around* the adopted emitter — exactly as the `[J2]` finding predicted
     for the emitter-vs-runtime split. The **emitter** (`A64.h`) keeps lifting cleanly: the
     `[J5a]` memory path reuses its `ldr_offset`/`str_offset`/`add64_reg_ext`/`rev`/
     `add_immed`/`cmp_reg`/`b_cc`/`b` encoders verbatim; the sandbox-base add, the
     big-endian byteswap, and the bounds-check are the small OURS logic the adoption could
     not provide. **No new Emu68 file was vendored** for `[J5a]` (the quarantine still holds
     only `A64.h`+`RegisterAllocator.h`); the Exhibit-B grep is unchanged/clean.

  *The sandbox memory-access path (the carried-forward `[J3]`/`[J4]` pointer boundary,
  now realised).* The block is entered as `void(*)(struct M68KState*, uint64_t base_adj)`
  — `x0` = state, `x1` = the adjusted sandbox base `host_mem − origin`. Each memory op
  through `An`: (a) **bounds-check** with a single unsigned compare `(An − origin) >u
  (size − 4)` (catches both `An < origin` and `An+4 > origin+size`); on out-of-range the
  block sets `state->fault = J5A_FAULT_OOB` and **skips the access** (no host OOB, clean
  fault, no SIGSEGV); (b) **map to host** with `add x_addr, x1, w_An, UXTW` (zero-extend
  the 32-bit `An` into the 64-bit add); (c) **byteswap big-endian** with `REV` around the
  host `ldr`/`str` (68k is big-endian; the sandbox stays big-endian as the relocator wrote
  it). The store-side write-back and the load-side read are both byte-exact-verified.

  *Verification (value-asserting) + negative controls.* The independent reference
  `j5a_interp.c` (OURS, no Emu68) runs the same block over a model of the **same** sandbox
  (big-endian loads/stores, same bounds rule) on its own copy of the memory; the test
  asserts the JITed register file AND the JITed sandbox memory are byte-exact equal to the
  interpreter's, plus `d0 == value+1` and `stored == value+1`. Three negative controls each
  **bite**: skip-store (memory stays at the initial value), wrong-endianness (omit the
  `REV` → `d0` diverges), and out-of-range `A0` (faults cleanly, `fault=0xBADADD12`, no
  crash). Watchdog 10 s → `[J5a] FAIL`.

- **`[J5b]` control flow — a real loop with a conditional backward branch + genuine
  condition codes — IMPLEMENTED / GREEN.** A SCOPED increment of the `[J5]` mountain: it
  proves a self-contained 68k LOOP — `moveq #0,d0 ; moveq #5,d1 ; L: add.l d1,d0 ;
  subq.l #1,d1 ; bne.s L ; rts` (sums 5+4+3+2+1 = 15 into `d0` over 5 iterations, `d1=0`)
  — translated by the adopted Emu68 *emitter* into a **single `jit_region` with an
  internal backward branch**, with **real condition codes** the branch consumes, verified
  byte-exact (registers AND the full CCR) against an independent reference, plus the loop
  ran the expected iteration count and terminated. Files: `hosted/jit68k/{j5b_jit68k.h,
  j5b_build.c,j5b_interp.c,j5b_test.c}` (reuses the `[J4]` loader `j4_loader.c`/`j4_hunk.h`
  for load+relocate); target `make hosted-jit68k-j5b` (plain ad-hoc, MAP_JIT works).
  **PASS (observed):** JITed `d0=15` over **5 iterations**, `d1=0`, **CCR=0x04 (Z set)**,
  byte-identical to the interpreter on every register and the CCR; the full-CCR
  cross-check (a 0−1 borrowing `subq` → `CCR=0x19 = N|C|X`) matches; the broken-branch
  negative control fails to terminate (caught). `[J1]`–`[J5a]` re-confirmed green.

  *Real condition codes (how the branch's flags are computed) — the primary `[J5b]`
  deliverable, generalising the `[J2]`/`[J4]` CCR=0 fold.* `subq.l #1,d1` is emitted as
  `subs w_d1, w_d1, #1` — the **flag-setting** subtract (`emu68/A64.h:320`, reused
  verbatim). The conditional `bne.s L` is an AArch64 **`b.ne`** (`A64_CC_NE`, Z==0) that
  consumes the **NZCV the `subs` just produced** — i.e. the AArch64 flags ARE the live 68k
  branch condition; no recompute is needed for the branch itself (Z is exact). The full
  68k CCR (`N/Z/V/C/X`) is **also** recomputed into `state->ccr`, but with **non-flag-
  setting ops only** (`cset` from the condition + shifted `orr` + `str`,
  `j5b_build.c:emit_ccr_from_subs`) emitted **between** the `subs` and the `b.ne`, so the
  branch still sees the `subs` flags (adjacency preserved). The one subtlety the emit gets
  right: **68k subtract C = borrow = AArch64 carry-CLEAR** (`A64_CC_CC`), the *opposite* of
  AArch64's C bit (ARM SUBS sets C on no-borrow); `X := C`. The independent interpreter
  (`j5b_interp.c`, OURS, no Emu68) computes the same subtract rule the long way, and the
  test asserts the JITed CCR equals it — so the borrow inversion is genuinely checked
  (the borrow cross-check sets N, C, X, which the Z-set loop exit alone would not exercise).

  *Single-region internal backward branch (the control-flow realization).* The whole loop
  is emitted **once** into one `jit_region` (no cross-region chaining). The emit records,
  for each 68k opcode byte offset, the **output-word index** where its AArch64 begins; the
  68k `bne.s disp8` target byte offset is mapped to that recorded word index and a `b.ne`
  is emitted with the signed **word** offset `target_word − bne_word` — **negative** for
  the loop's backward branch. `A64.h`'s `b_cc`/`b` take signed word offsets and mask the
  two's-complement low bits; `ASSERT_OFFSET` is a no-op at `-O2`, so backward branches
  encode cleanly. The no-RA memory-backed scheme stays: D0–D7 are loaded from
  `struct j5b_m68k_state` (x0) in the prologue and stored back in the epilogue, with the
  AAPCS64 callee-saved x19..x28 preserve (the `[J4]` fix). Block-boundary handling /
  cross-region chaining is deferred — the single region holds the self-contained loop.

  *Verification (value-asserting) + the negative control / watchdog.* The reference runs
  the same loop, counting **loop-body passes** (= `bne` executions; a 5-trip loop reaches
  the `bne` 5 times, taken 4 then falling through — counting "taken" would undercount by
  one). PASS iff: JITed registers == the interpreter's; JITed CCR == the interpreter's full
  N/Z/V/C/X; `d0==15 && d1==0 && Z` set; the loop ran **exactly 5 iterations and
  terminated**; and the full-CCR borrow cross-check matches. The **negative control** emits
  the backward branch as **always-taken** (broken Z test) so the loop never terminates;
  it runs in a forked **child** with its own 2 s alarm and the test asserts the child
  **hung** (was killed by the alarm) — proving the termination path genuinely depends on
  the real Z and that a broken branch is caught. The main process has a 10 s SIGALRM
  **watchdog** that converts any hang/fault into `[J5b] FAIL`.

  *Honest scope / what is OURS vs adopted.* Only existing `A64.h` encoders are used
  (`add_reg`, `subs_immed`, `cset`, `orr_reg`, `b_cc`, `b`, `mov_immed_u16`, `ldr/str`,
  `stp/ldp`) — **NO new Emu68 file was vendored** for `[J5b]` (the quarantine still holds
  only `A64.h`+`RegisterAllocator.h`), so the Exhibit-B grep is unchanged/clean. Per the
  `[J5a]` finding, Emu68's `RegisterAllocator64.c`/`M68k_EA.c` are not adoptable
  incrementally (1:1-MMU / software-ICACHE / EL0-system-register coupling), so the loop
  decode, the branch-target word mapping, and the CCR derivation are OURS, built around
  the emitter. The opcode subset is `moveq`/`add.l`/`subq.l`/`bne.s`/`rts`; `moveq` still
  uses the `<0x80` zero/sign-extend shortcut (enforced — rejects `≥0x80`).

- **`[J5c]` RE-HOST Emu68's REAL decoder + register allocator — IMPLEMENTED / GREEN.**
  The make-or-break experiment: instead of hand-rolling (as `[J2]`..`[J5b]` did), `[J5c]`
  DRIVES Emu68's **REAL per-opcode decoders** (the verbatim, MPL-quarantined
  `emu68/M68k_LINE{8,9,B,C,D}.c` + `M68k_MOVE.c` + `M68k_MULDIV.c` + `M68k_EA.c`) behind
  hosted replacements for exactly the three `[J5a]` couplings, and verifies a **richer
  block** the hand-rolled path can't reach, byte-exact against our independent
  interpreter. Files: `hosted/jit68k/{j5c_jit68k.h,j5c_shims.c,j5c_ra.c,j5c_build.c,
  j5c_interp.c,j5c_test.c}` + `hosted/jit68k/emu68_darwinize.pl`; vendored decoders in
  `hosted/jit68k/emu68/` (NOTICE updated, Exhibit-B re-run = clean); target
  `make hosted-jit68k-j5c` (plain ad-hoc, MAP_JIT works). **The block (8 opcodes across 6
  real LINE decoders):** `moveq #-5,d2 ; add.l d2,d0 ; sub.l d3,d0 ; and.l d4,d0 ;
  or.l d5,d0 ; eor.l d6,d1 ; muls.w d7,d1 ; cmp.l d1,d0 ; and.l d2,d2 ; rts`. **PASS
  (observed):** the REAL-decoder-emitted block run under W^X gives `d0..d7` =
  `12000feb 00000027 fffffffb 00000010 0000ffff 12000000 0000000a 00000003`, **byte-exact
  equal to the independent interpreter** on all 16 D/A registers; `d2 == 0xFFFFFFFB`
  (Emu68's REAL `moveq` sign-extend — generalising the `[J2]`/`[J4]`/`[J5b]` `imm<0x80`
  shortcut); the final CCR `0x08` (N set, Z clear) matches the reference byte-exact; and a
  **negative control** that corrupts the decoded opcode stream makes the REAL decoder emit
  a different (valid) instruction so `d0` diverges (`12000fe9` vs `12000feb`) — proving the
  asserts bite. Watchdog 10 s → `[J5c] FAIL`. `[J1]`–`[J5b]` + `apps68k` re-confirmed green.

  *THE RE-HOSTING VERDICT (the key deliverable). RE-HOSTING WORKS — for the register /
  ALU / control opcode class — and broad coverage of that class is now "vendor more
  `M68k_LINE*.c` + extend the interpreter oracle".* The `[J5a]` finding ("the EA decode +
  RA do not lift incrementally") stands, but `[J5c]` shows it is a **scoped** block, not a
  wall: the three couplings each have a small hosted hook, and once hooked, Emu68's REAL
  decoders + REAL condition-code derivation run correctly and verify byte-exact.
  - **HOOK 2 — instruction fetch (`cache_read_16`).** Provided as a direct big-endian read
    from the host instruction stream (`j5c_shims.c`); no Emu68 software ICache (`cache.c`)
    is linked. **A deeper sub-finding than `[J5a]` named:** `cache_read_16(enum, uint32_t
    address)` takes a **32-bit** address (bare-metal the 68k space is 32-bit and 1:1 with
    low physical RAM), which **truncates** a 64-bit host pointer; the shim splices back the
    constant high 32 bits of the stream's host base. Cheap, but it confirms the fetch path
    is coupled to a ≤32-bit address space.
  - **HOOK 3 — RA SR/CTX.** Provided as OUR register allocator (`j5c_ra.c`) implementing
    the `RA_*` interface, with a **memory-backed CCR** (`ldr/str` from the `struct
    j5c_m68k_state` via x0) instead of Emu68's `mrs/msr TPIDR_EL0`/`TPIDRRO_EL0`. The RA
    interface turned out **thin enough to re-host** (Dn/An are always-mapped; only the
    EL0-sysreg SR/CTX bodies needed replacing) — so the `[J5a]` "RA is OURS" conclusion is
    realised as a clean ~200-line file that *drives the real decoders*, not a from-scratch
    decoder.
  - **HOOK 1 — sandbox memory / EA — STILL THE ONE REAL BLOCKER, now precisely bounded.**
    The register/ALU/control opcodes touch **no 68k memory**, so the REAL EA decoder
    (`EMIT_LoadFromEffectiveAddress` mode 0/1) lifts cleanly and `[J5c]` drives it as-is.
    The **memory-EA modes** (`(An)`/`(An)+`/`-(An)`/`(d16,An)`/absolute/PC-rel) remain
    blocked by TWO assumptions baked into the per-opcode emit, with **no hook inside it**:
    (a) `An` is dereferenced as a raw host pointer (no `sandbox_base` add —
    `M68k_EA.c:635-639` `ldr w,[x_An]`), and (b) **Emu68 runs the whole AArch64 CPU
    big-endian** (`src/aarch64/start.c` sets `SCTLR.EE|E0E` under `EMU68_HOST_BIG_ENDIAN`),
    so the emitted `ldr/str` are big-endian by hardware — which `macOS`/Apple-silicon EL0
    cannot be. Fixing either needs **editing `M68k_EA.c` at the load/store site** (the
    `[J5a]`/`[J5b]` hand-rolled sandbox-base + `REV` byteswap), i.e. surgery *inside* the
    opcode emit, not at the unit boundary. Cost estimate to instead build OUR full decoder
    from scratch: large (re-deriving the ISA-complete `M68k_LINE*` decode + per-opcode CCR
    that Emu68 already encodes) — so the right path is **edit `M68k_EA.c`'s ~6 load/store
    sites** (a disclosed MPL edit) and keep adopting everything else.
  - **A FOURTH coupling `[J5c]` surfaced (portability, not runtime):** the decoder SOURCE is
    not byte-portable to the macOS toolchain — Emu68 uses GNU `__attribute__((alias(...)))`
    function aliases, which clang/Mach-O rejects outright. Bridged WITHOUT editing the
    verbatim files by `emu68_darwinize.pl` (OURS), which rewrites the alias chains into
    plain-C tail-call forwarders in build-dir copies; the quarantine stays byte-verbatim
    (`diff` vs upstream is empty for every file). The `M68k_SR.c` all-16-line
    `GetSRMask`/`INSNLength` fan-out is severed by stubbing `M68K_GetSRMask`→`SR_CCR` (the
    conservative "compute all flags" answer).

  *What broad coverage now requires (the one-line path).* For non-memory opcodes: **vendor
  the remaining `M68k_LINE{0,4,5,6,E}.c` + `M68k_CC.c` (for `Bcc`/`Scc`/`DBcc` via
  `EMIT_TestCondition`) + `M68k_Exception.c`, Exhibit-B-grep each, and extend
  `j5c_interp.c`** — the hooks are already in place. For memory opcodes: additionally apply
  the disclosed `M68k_EA.c` load/store edit (sandbox-base add + `REV` byteswap, the `[J5a]`
  logic) so adopted `(An)`-class EA routes through the sandbox. Cross-region chaining +
  `M68KTranslationUnit` `ICache`/`LRU` + the `MainLoop` funnel (PC in x18) + the real
  `jsr`-through-vector decode wired to the `[J3]` recognition + library-calls-from-the-
  program + a sandbox-backed allocator for out-of-sandbox return-pointers remain the
  larger, separately-scoped adoption beyond `[J5c]`.

- **`[J5d]` THE WHOLE apps68k CORPUS THROUGH THE JIT — IMPLEMENTED / GREEN.** `[J5d]`
  walks the "one-line path" above to its conclusion: it broadens the `[J5c]` re-hosting
  into a little **engine** that runs *all four* real vasm-assembled hunk binaries
  (`mul`/`fact`/`arraysum`/`libcall`) through the REAL Emu68 decoders. Files:
  `hosted/jit68k/{j5d_jit68k.h,j5d_engine.c,j5d_ea_helpers.c,j5d_interp.c,j5d_test.c}` +
  the rewritten `apps68k/runner.c`; vendored `emu68/{M68k_LINE5.c,M68k_CC.c}` (NOTICE
  updated, Exhibit-B re-grep clean); targets `make hosted-jit68k-j5d` (marker `[J5d]
  PASS`) and `make hosted-jit68k-apps` (marker `[apps68k] PASS`, the REAL `.exe`
  binaries). **PASS (observed, all four THROUGH THE JIT, byte-exact vs an independent
  from-scratch interpreter `j5d_interp.c`):** `mul`→`d0=42` (3 blocks), `fact`→`d0=120`
  (reg-to-reg `move.l` + `cmp.l` + nested loops, 5 blocks), `arraysum`→`d0=150`
  (relocated `lea` DATA + `add.l (a0)+,d0` through the REAL EA decoder — 2 sandbox memory
  accesses, byteswapped), `libcall`→`d0=0` with `AllocMem(256,MEMF_CLEAR)`/`PutChar('A')`/
  `FreeMem(ptr,256)` observed via the REAL `[J3]` marshaller with the right args/returns
  and `bytes_outstanding=0`. A corrupt-decode negative control makes the REAL decoder
  emit a different valid instruction so `d0` diverges (asserts bite); watchdog → FAIL.

  *The architecture — Emu68's own block/dispatch split, re-hosted.* `[J5c]` drove the
  real decoders for ONE straight-line block; `[J5d]` turns that into a **per-basic-block
  translator + OUR re-hosted dispatcher**. The translator drives the REAL decoders
  (`EMIT_move`/`line5`/`line8`/`line9`/`lineB`/`lineC`/`lineD`/`moveq`) for *every*
  register/ALU/flag/move/memory opcode — the heavy semantic work — up to a control-flow
  terminator, emits the block into a MAP_JIT region, and caches it by entry PC (OUR
  `[J5d]` ICache). The dispatcher (`j5d_engine.c`) is Emu68's `MainLoop` re-hosted: it
  runs a block, then decodes the terminator in C to compute the next PC, looping until
  the top-level `RTS`. This is exactly the spec's section (3) split — the LVO bridge +
  the inter-block PC funnel are OURS, behind the translator; Emu68's blocks already exit
  via RET to a central C MainLoop, so no block-to-block chaining had to be patched.
  - **Memory EA (HOOK 1, the `[J5a]` blocker — now bridged).** The disclosed `M68k_EA.c`
    edit is realised as a *build-dir* transform: `emu68_darwinize.pl --ea-sandbox`
    rewrites each `(An)`-class load/store site in the build copy to call OUR `j5d_ea_mem`
    emitter (`j5d_ea_helpers.c`) — `host = sandbox_base_adjust + An` (UXTW, from a fixed
    callee-saved reg the prologue seeds) + `REV` byteswap + the post/pre index. The
    **quarantine `M68k_EA.c` stays BYTE-VERBATIM** (`diff` vs upstream empty); only the
    build copy is patched, and the patch is a pure call-substitution (KIND derived
    mechanically from the original encoder name). `arraysum`'s `add.l (a0)+,d0` runs
    through the REAL EA decoder this way.
  - **`jsr`-through-vector → `[J3]` from the DECODED stream.** The dispatcher decodes
    `jsr d16(A6)` (0x4EAE), maps the target to `(libbase, LVO n)` via the REAL
    negative-offset rule `n=(libbase−target)/6` (`j3_vector_recognise`), and invokes the
    registered bridge — which marshals the 68k regs into the native stub through the REAL
    `[J3]` marshaller (`j3_build_marshal_thunk`). The `jsr` is decoded from the
    instruction stream, not hand-constructed — the "library-calls-from-the-running-
    program" item `[J3]` deferred.
  - **What the dispatcher decodes itself (not re-hosted from LINE4/6).** `Bcc`/`BRA`
    (8-bit), `JSR d16(A6)`, `LEA abs.l,An`, `RTS`. Re-hosting Emu68's LINE4/LINE6 faithful
    (its REG_PC select + `M68K_PushReturnAddress` + `EMIT_LocalExit` branch-target model)
    is entangled with `M68k_Translator.c`'s funnel — that funnel IS what our dispatcher
    re-hosts, so for the corpus we decode these few control-flow ops in C (reading the
    branch decision from the REAL CCR the real decoders produced). LINE4/LINE6 themselves
    were therefore NOT vendored.

  *What a BIGGER app would still need (honest debt beyond `[J5d]`).* A real return stack
  for nested `bsr`/`jsr`/`rts` + computed `jmp(An)` + `bcc.W/.L` (the dispatcher's PC
  model is currently flat) — **DONE by `[J5f]` below** (the PC-driven dispatcher + the real
  68k return stack + the full Bcc widths + computed jumps + a reported block-cache win);
  OUR own SR/exception model (a host SIGSEGV inside translated
  code → a 68k bus/illegal/divide-by-zero vector, the SECOND mapping layer in Risks) —
  **DONE by `[J5i]` below** (the dispatcher-level 68k exception model: a sandbox vector table,
  the SR+PC frame, `rte`, the S bit, and the `graft/cpu_aarch64.h` host-signal seam stated);
  dirty-page SMC invalidation (`M68K_VerifyUnit`/CRC32 we did not re-host); the FPU
  (`LINEF`) + privileged ISA; and a sandbox-backed allocator so a native call returning
  memory *outside* the 32-bit sandbox is reachable by the 68k program. The `[J5d]` engine
  is integer-user-mode, single-region-flat-PC, single-thread (R-JIT-THREAD) — enough for
  the corpus and for the LoadSeg→RunCommand hook, not yet for an arbitrary Amiga binary.

- **`[J5e]` BLOCK-SCOPED REGISTER ALLOCATOR — the "optimize" pass — IMPLEMENTED / GREEN.**
  The `[J5d]` engine runs the corpus, but it bracketed *every* basic block with a FIXED
  frame: a prologue loading **all 16** Dn/An from `struct j5d_m68k_state` and an epilogue
  storing **all 16** back, unconditionally (32 state-struct `ldr`/`str` per block) no matter
  what the block touched. `[J5e]` replaces that with a real **block-scoped allocator** that
  keeps the 68k register file live in host registers across the block and minimises the
  state-memory traffic at the frame. Files: `hosted/jit68k/j5e_test.c` (the measure/verify
  driver) + the RA in `hosted/jit68k/j5c_ra.c` (liveness) + the minimal frame in
  `hosted/jit68k/j5d_engine.c`; target `make hosted-jit68k-j5e` (marker `[J5e] PASS`).
  **PASS (observed, the WHOLE corpus byte-exact AND a measured reduction):** AArch64 words
  emitted **918 → 451 (−51%)**, state-struct `ldr`/`str` **512 → 45 (−91%)** across the four
  programs; per program: mul 173→85 words / 96→8 state-accesses, fact 309→167 / 160→18,
  arraysum 225→107 / 128→10, libcall 211→92 / 128→9. Every program's JIT register file +
  sandbox memory + the libcall stub-call log stay byte-exact equal to the independent
  interpreter (`j5d_interp.c`, OURS, no Emu68), and the corrupt-decode negative control
  still bites — **correctness gates the marker, not the smaller instruction count**.

  *The key finding — "RA is a perf pass, not correctness", confirmed concretely.* The REAL
  Emu68 decoders ALREADY keep the architectural file in the fixed Emu68 host-register map
  (D0–D7→w19–w26, A0–A4→w13–w17, A5–A7→w27–w29) across the whole block — `RA_MapM68kRegister`
  returns the fixed reg and reg-to-reg ops emit directly against it, with **no per-op
  `ldr`/`str` to the state struct inside the decoders** (verified: `M68k_EA.c` mode-0/1
  register-direct, the `LINE*` ALU paths). So the per-block win is entirely at the FRAME:
  load only what the block *reads* before writing, store back only what it *writes*.
  - **Liveness/dirty tracking (OURS, in `j5c_ra.c`).** As the decoders run, the RA records
    two 16-bit masks (bit i = Di for i<8, A(i−8) for i≥8): **live-in** (a register read via
    `RA_MapM68kRegister`/`RA_CopyFromM68kRegister` *before* it was written this block) and
    **dirty** (a register written via `RA_MapM68kRegisterForWrite` or `RA_SetDirtyM68kRegister`).
    A "written-so-far" gate implements read-before-write so a read *after* an in-block write
    does **not** become live-in (the value is already in the host reg). The masks are exposed
    by `j5c_ra_get_masks`.
  - **The read-before-write rule is the load-bearing correctness point.** A FULL `.L` write
    (`moveq #imm,Dn`, `move.l #imm,Dn`, sign-extended `.W`/`.B`) goes through
    `RA_MapM68kRegisterForWrite` with **no prior read** → NOT live-in → the prologue **skips
    its load** (that load was pure waste). A PARTIAL `.W`/`.B` write (no sign-extend) must
    preserve the upper bits, so the decoder maps it **for read first** (`bfi`/`bfxil`) → it
    IS live-in (loaded) AND dirty (stored). A read-modify-write (`addq`, `add.l Dm,Dn`)
    reads then dirties → live-in and dirty. The decoders are already written this way, so
    the masks are exact without touching the MPL files.
  - **The minimal frame (`j5d_engine.c`).** `translate_block` now emits the decoder body
    into a temporary buffer FIRST (so the masks are complete — they are only final after the
    last opcode), then composes: callee-saved preserve → seed x12 (sandbox base-adjust) →
    **prologue loads only live-in regs** → body → **epilogue stores only dirty regs** →
    callee-saved restore → `ret`. The body is position-independent (a `[J5d]` straight-line
    block has no intra-block branches — branches are inter-block via the dispatcher), so the
    body-buffer + `memcpy` after the prologue is sound. The AAPCS64 x19..x28 + x29/x30
    preserve (the `[J4]` fix) is kept in full — cheap, off the per-op path, and keeps the
    frame well-formed for any Dn/An set; the `[J5e]` win is the eliminated *state-struct*
    traffic, which is exactly the per-op memory-traffic the optimize clause targets.

  *SPILL POLICY at boundaries (the correctness contract).*
  - **Block exit (RTS / fall-through / Bcc/BRA):** the epilogue stores all **dirty** regs to
    `struct j5d_m68k_state` before returning to the dispatcher. A clean (read-only) register
    was never modified in its host copy, so memory already holds its value — no store needed,
    and the next block reloads it as a live-in if it reads it.
  - **The `jsr`-through-vector library-call bridge (`[J3]`):** the block ENDS at the `jsr`
    (it is a terminator, not inside the block), so the block's epilogue has already stored
    every dirty reg to memory **before** the dispatcher invokes the bridge. The bridge
    marshals the 68k argument registers **from the memory state**, which is therefore fully
    consistent for *any* register at the call boundary. There is no partial-store hazard: a
    register that carries an argument was either written in the block (dirty → stored) or
    only loaded/untouched (memory already correct). The native return is written into
    `st->d[0]` in memory, which the next block reloads as a live-in.
  - **`(An)` memory access (which may alias code/data):** the access is emitted by OUR
    sandbox EA helper (`j5d_ea_mem`), which dereferences `host_mem` directly — it does **not**
    touch the state struct, so it cannot race the deferred register file. The An register
    itself, when modified by `(An)+`/`-(An)`, is marked **dirty** by the REAL EA decoder's
    `RA_SetDirtyM68kRegister`, so it is correctly stored at the block exit.

  *What is still deferred (honest, named in `j5e_test.c`).* (i) **Cross-block register
  caching:** today each block reloads its live-ins from memory even if the previous block
  left them in the same host regs — because blocks exit through the C dispatcher (Emu68's
  MainLoop model) and the host regs are not guaranteed preserved across that funnel. Caching
  live registers across the dispatcher hop (or chaining hot blocks directly) is the next
  optimization and pairs with the `[J5d]`-deferred cross-region chaining / block cache.
  (ii) **Linear-scan spilling under register pressure:** not needed yet — there are enough
  host callee-saved registers for the entire 68k file (8 Dn + 8 An), so the "all-live,
  no-spill" allocation never runs out of registers. A future ABI that frees fewer host regs,
  or FP/wide state, would need spill-under-pressure. The current RA is correct and minimal
  for the integer file; it is a **perf pass layered on the already-correct `[J5d]` engine**.

- **`[J5f]` PC-DRIVEN DISPATCHER + a REAL 68k RETURN STACK + a BLOCK CACHE — IMPLEMENTED /
  GREEN.** Generalises the `[J5d]`/`[J5e]` flat-PC engine toward real subroutine-structured
  programs: the dispatcher becomes a PC-driven loop with a genuine 68k return stack (nested
  `bsr`/`jsr`/`rts` + computed `jmp(An)`/`jsr(An)` + the full `Bcc`/`BRA`/`BSR` `.B`/`.W`/`.L`
  displacement widths), over the PC-keyed block cache. Files: `hosted/jit68k/j5f_test.c`
  (the measure/verify driver) + the generalised dispatcher in `hosted/jit68k/j5d_engine.c`
  + the SP/stack/control-flow model in the independent reference `hosted/jit68k/j5d_interp.c`
  + the new program `hosted/jit68k/apps68k/sumsq.s` → `bin/sumsq.exe`; target
  `make hosted-jit68k-j5f` (marker `[J5f] PASS`); also wired into the `apps68k` runner so
  the REAL `.exe` runs through the loader. **PASS (observed):** sumsq — sum of squares
  `1²+2²+3²+4²+5² = 55` via a `square` subroutine (which nests a `mul` helper, 2-deep) called
  from a loop **plus** once through a **computed `jsr (a0)`** — runs through the REAL Emu68
  decoders to `d0 = 55`, **byte-exact equal to the independent interpreter** on the full
  register file (incl. `a7` back at the initial SP `0x250000`) AND the **sandbox memory
  including the return-stack region**; the return-stack telemetry is `pushed == popped == 12`,
  `max nest depth == 2`, `computed jumps == 1`; the **block cache** translated **8** distinct
  blocks (cache misses) for **31** executions = **23 cache hits / re-translations avoided**.
  Negative controls bite: a corrupt subroutine argument (`move.l d6,d0`→`d5,d0`) makes the
  loop pass `0` so `d0 = 0 ≠ 55` (the byte-exact assert is not a tautology), and a wild
  computed `jmp (a1)` with `a1 = 0` (out of sandbox) is **caught cleanly** by the dispatcher's
  sandbox bound (`rc != 0`, no host crash). Watchdog 15 s → FAIL. `[J1]`–`[J5e]` + `apps68k`
  re-confirmed green.

  *The generalisation (the `[J5d]`-deferred control-flow debt, now paid).* `[J5d]`'s
  dispatcher was **flat-PC**: `rts` = top-level exit, `jsr d16(A6)` = library vector, `Bcc`
  8-bit only, no SP. `[J5f]` keeps that as a strict subset (the flat corpus is unchanged) and
  adds, all **decoded from the instruction stream** (not hand-constructed), at the dispatcher
  level (OURS — these are exactly the control-flow ops whose Emu68 LINE4/6 decoders are
  entangled with `M68k_Translator.c`'s funnel, which our dispatcher *is*):
  - **The real return stack.** `a7` (`st->a[7]`) is a sandbox address; the dispatcher seeds it
    to the top of the sandbox (the initial SP) and records that baseline. `bsr`/`jsr abs.l`/
    `jsr (An)` **push** the 4-byte 68k return address **big-endian** to `(a7-4)` and set
    `a7 -= 4` (68k predecrement-push); `rts` reads the longword big-endian at `(a7)`, sets
    `a7 += 4`, and resumes there. A `rts` that pops back to the initial SP is the **top-level
    return = program exit** — so the flat corpus, which never pushes, hits its first `rts` at
    the initial SP and exits exactly as under `[J5d]`. The pushed bytes live in the sandbox the
    test inspects, so the **return-stack contents are byte-exact-verifiable**. Push/pop run in
    the dispatcher C, not inside translated blocks, because they occur at the terminator
    boundary the dispatcher already owns (Emu68's MainLoop re-reads the PC there); inlining the
    push/pop is the cross-region-chaining engine's job, deferred.
  - **Computed `jmp(An)`/`jsr(An)`.** The target is read from the register `st->a[n]` after the
    block ran; `jsr (An)` also pushes the return address. sumsq's `jsr (a0)` exercises this.
  - **Full `Bcc` widths.** `.B` (disp in the opcode), `.W` (disp `0x00` → next word), `.L`
    (disp `0xFF` → next longword), for the whole condition table (`bcc_taken`, M68000 PRM,
    reading the REAL 68k CCR the decoders produced) plus `BRA`/`BSR`. Also `lea (d16,pc),An`
    (vasm resolves the in-hunk `square` label PC-relative) is decoded.
  - **The block cache** was already present (PC-keyed `g_cache`); `[J5f]` enlarges it
    (64→256 entries) and reports the win: a loop body / repeatedly-called subroutine translates
    **once** and is re-run (8 translated vs 31 executed = 23 re-translations avoided on sumsq).

  *Verification (value-asserting) + the independent reference.* The reference interpreter
  (`j5d_interp.c`, OURS, no Emu68) is **extended to model the same SP/stack/control-flow** —
  it seeds `a7` to the same initial SP, pushes/pops big-endian on the same sandbox, and decodes
  the same `bsr`/`jsr`/`rts`/`jmp(An)`/full-`Bcc`/`lea(pc)` set — so the test compares the JITed
  register file AND the JITed sandbox memory (**including the stack**) byte-exact against it, plus
  the return-stack telemetry and the block-cache counts. This made the `[J5d]`/`[J5e]` regression
  tests assert `a7` too (both now seed it identically); they stay byte-exact green.

  *Honest deferrals (still beyond `[J5f]`, stated in `j5f_test.c` + below).* OUR SR/exception
  model (host `SIGSEGV` inside translated code → a 68k bus/illegal/divide-by-zero **vector**,
  the second mapping layer in Risks); **self-modifying-code / dirty-page block-cache
  invalidation** (a write into a cached code page must drop the stale translation — we do not
  `M68K_VerifyUnit`/CRC32 yet); the **FPU (`LINEF`) + privileged ISA**; the rest of the ISA /
  addressing modes (the new program exercises register-direct + `(An)`-class EA + the control
  flow above, not the full mode set); and the **boot-gated real AROS library environment** (the
  `LoadSeg`→`RunCommand`/`CallEntry` hook + real libraries — `[J5f]` still uses the stub `exec`).
  What sumsq does/doesn't exercise: it DOES exercise nested `bsr`/`rts` (2 deep) over the real
  return stack, a computed `jsr(An)`, a `cmp.l`/`bne.s` loop with genuine condition codes, and
  the block cache; it does NOT make a library call, dereference memory, or self-modify. No new
  Emu68 file was vendored (the return stack / branch decode / computed jumps are dispatcher-level
  C, in OUR files) — the quarantine still holds the `[J5d]` set, Exhibit-B grep unchanged/clean.

- **`[J5g]` BROADEN the ISA + addressing-mode coverage — IMPLEMENTED / GREEN.** A substantial,
  honest batch toward running any self-contained 68k program: vendor + drive three MORE real
  Emu68 decoders and the full M68000 addressing modes, with the independent interpreter kept
  byte-exact for everything executed. Files: `hosted/jit68k/{j5g_test.c,j5g_shims.c}` + the new
  decoders in `j5d_engine.c`/`j5d_interp.c`/`j5d_ea_helpers.c`/`emu68_darwinize.pl` + the program
  `apps68k/bubsort.s`→`bin/bubsort.exe`; target `make hosted-jit68k-j5g` (marker `[J5g] PASS`),
  also wired into the `apps68k` runner (now SIX programs). **PASS (observed):** bubsort — a bubble
  sort of `{17,3,42,8,99,23}` in place via `(0,a0,Dn.L)` indexed load/store → `{3,8,17,23,42,99}`,
  then a checksum/mixer over the sorted array using the full shift/rotate + immediate + misc set →
  **`d0 = 0x00F5B9F5`** — runs through the REAL Emu68 decoders, **byte-exact equal to the
  independent interpreter** on the FULL register file AND the sandbox memory (including the in-place
  sorted array) AND the **full CCR**; the negative control (flip the sort branch `ble.s`→`bgt.s`)
  makes the array sort wrong so the checksum diverges (the byte-exact assert is not a tautology).
  `[J1]`–`[J5f]` + `apps68k` re-confirmed green. Watchdog 15 s → FAIL.

  *EXACTLY what coverage `[J5g]` adds (the honest scope statement).*
  - **`M68k_LINE0.c` (VENDORED VERBATIM, driven):** `addi.l/subi.l/andi.l/ori.l/eori.l/cmpi.l
    #imm,Dn` + `btst #bit,Dn` (static bit test → Z). The file also defines `bset/bclr/bchg/movep/
    cas` etc. — present (it links verbatim) but NOT exercised by the verified program.
  - **`M68k_LINE4.c` (VENDORED VERBATIM, driven):** `clr.l / tst.l / swap / ext.l Dn` and `lea
    (d16,An)/(d8,An,Xn),An`. `neg.l/not.l` are decoded-correctly but DELIBERATELY NOT in the
    verified program (see the X-bit deferral below). `movem/pea/movec/jmp/jsr/rts/link/unlk/trap`
    are present (link verbatim) but the dispatcher owns the control-flow ones; the rest are not
    exercised.
  - **`M68k_LINEE.c` (VENDORED VERBATIM, driven):** `lsl.l/lsr.l/asl.l/asr.l/rol.l/ror.l
    #count,Dn` (all six, immediate count, `.L`). `roxl/roxr` + the bitfield ops (`bfXXX`) are
    present (link verbatim) but NOT exercised (ROXL/ROXR are X-chain ops, deferred).
  - **ADDRESSING MODES now routed through the sandbox EA:** `(An)`, `(An)+`, `-(An)` (carried from
    `[J5d]`) **plus the new** `(d16,An)`, `(d8,An,Xn.L)` indexed (the bubble-sort array access, LOAD
    *and* STORE), `abs.w`/`abs.l`, `(d16,PC)`/`(d8,PC,Xn)`, and `#imm`. byte/word/long sizes are all
    handled by the EA helper (the program is `.L`; `.W`/`.B` paths exist and are size-parameterised).
  - **SIZES:** `.L` throughout the verified program; the EA + the move decoders handle `.B`/`.W`
    too (the engine routes groups 1/3 = `move.b`/`move.w` to `EMIT_move`).

  *THE EA broadening (the primary new mechanism).* `[J5d]`'s `j5d_ea_mem` bridged only the DIRECT
  `(An)`/`(An)+`/`-(An)` sites (modes 2/3/4) in `M68k_EA.c`. The displacement/indexed/PC-relative
  modes — `(d16,An)` (mode 5), `(d8,An,Xn)` (mode 6), `(d16,PC)`/`(d8,PC,Xn)` (mode 7.2/7.3), and
  `abs.w` (mode 7.0) — route through FOUR inline funnel helpers (`load_reg_from_addr[_offset]` /
  `store_reg_to_addr[_offset]`). `emu68_darwinize.pl --ea-sandbox` replaces the BODY of each of those
  four (a name-based, whole-body call-substitution) with a tail-call to OUR
  `j5d_ea_addr_offset`/`j5d_ea_addr_index` (`j5d_ea_helpers.c`): compute the 68k address
  (`base + offset` or `base + index<<shift`), add the sandbox base-adjust (`host = base_adjust + addr`,
  UXTW), big-endian `REV`, the size-correct load/store; `size==0` is the "load effective address"
  case (LEA/PEA — compute the 68k address into the destination, NO memory touch, NO base-adjust, NO
  REV). The **`abs.l` (mode 7.1)** path is an Emu68 inconsistency: it does NOT use the funnel — it
  materialises the 32-bit address into a scratch `tmp_reg` and emits a DIRECT
  `ldr_offset(tmp_reg, *arm_reg, 0)`, so the `--ea-sandbox` DIRECT-site rewrite is extended to match
  `tmp_reg` (not just `reg_An`) and routes it through `j5d_ea_mem` too. The DECODE (which mode, the
  extension-word reads via `cache_read_16`, the index sign/scale) stays 100% the REAL Emu68 decoder;
  the QUARANTINE `M68k_EA.c` stays BYTE-VERBATIM (diff vs upstream empty); only the build copy is
  patched. **All five new memory modes are verified byte-exact** in `j5g_test.c`'s `ea-modes`
  sub-test (abs.l + `(d16,An)` loads + a `(d8,An,Xn)` store, register file + the written cell
  byte-exact vs the oracle) on top of bubsort's `(d8,An,Xn)` + `(An)`.

  *A FOURTH bare-metal coupling found + fixed (beyond the three `[J5a]`/`[J5c]` named).* Emu68's
  decoders hardcode **w0 as a 1-shot scratch for flag extraction** — `cset(0,cond); bfi(cc,0,…)`
  (`EMIT_BTST`, `EMIT_ASx`, the `EMIT_GetNZ*` helpers) — because bare-metal the CTX/CCR live in EL0
  system registers (`TPIDRRO_EL0`/`TPIDR_EL0`), so w0 is genuinely free. The `[J5c]`/`[J5d]` re-host
  had overloaded **w0 as the memory-backed state pointer**; the register/ALU corpus never hit a
  `cset(0,…)` site (it took the `EMIT_GetNZCV` fast path), so this was LATENT — `[J5g]`'s `btst`
  surfaced it as a host SIGSEGV (the epilogue's `str w_cc,[x0,#CCR]` faulted with x0=0). **Fix:** keep
  the state pointer in **x1** for the whole block (the decoders never hardcode w1/w2/w3 — verified by
  grep), freeing w0 exactly as Emu68 expects. Realised in `j5c_ra.c` (CCR/CTX hooks use x1), the
  engine prologue/epilogue (`mov x12,x1; mov x1,x0`; Dn/An ldr/str base x1) and `j5c_build.c`.

  *A CCR-layout finding (the second latent issue `[J5g]` surfaced).* Emu68's internal CCR byte SWAPS
  C and V relative to the standard 68k SR order: **bit0=V, bit1=C, bit2=Z, bit3=N, bit4=X** (cf.
  `SRB_Valt=0`/`SRB_Calt=1` in `M68k.h`, and `bfi(cc,..,1,1)` for C / `bfi(cc,..,0,1)` for V in
  `A64.h`'s `EMIT_GetNZCV`; sub/cmp use the `…alt` swapped representation). The earlier corpus only
  ever consumed Z/N (layout-stable), so the swap was latent; `[J5g]`'s `ble.s`/`blt.s`/`bcc`/`bcs`
  branches consume C and V, so the dispatcher's `bcc_taken` AND the independent oracle were updated to
  use Emu68's actual layout (the `J5D_CCR_C`/`J5D_CCR_V` constants are swapped). The register RESULTS
  (the load-bearing proof) are still byte-exact; only the CCR bit POSITIONS for C/V follow Emu68's
  storage — a future SR-read normalisation pass could re-emit a standard-68k SR byte at the boundary.

  *Verification (value-asserting) + the negative control.* The independent reference `j5d_interp.c`
  (OURS, no Emu68) is extended to decode + execute EVERY new opcode/mode/size: the move.l EA modes
  0–6 (load + store, big-endian, sandbox-bounds-checked), `move.w Dm,Dn`, `lea (d16,An)/(d8,An,Xn)`,
  the six LINE0 immediates + `btst`, the six LINE4 misc ops, and the six LINEE shifts/rotates with
  their exact N/Z/V/C/X rules. The test asserts `d0 == 0x00F5B9F5` (JIT == oracle), the FULL register
  file byte-exact, the sandbox memory (incl. the sorted array) byte-exact, the full CCR byte-exact,
  the sorted array reads back `{3,8,17,23,42,99}`, and ≥1 `(An)`-class sandbox access went through the
  JIT. The negative control flips the sort comparison's branch so the array sorts the wrong way and
  the checksum diverges — proving the byte-exact assert genuinely bites.

  *Honest deferrals / what `[J5g]` does NOT cover (stated in source + here).* (i) **`neg.l`/`not.l`
  in a long block:** decoded correctly, but their **X bit** was UN-ORACLED at `[J5g]` time — so they
  were DELIBERATELY excluded from the verified program (the rule "don't run what the oracle can't
  check byte-exact"). This is exactly the deferred **X-bit multi-precision chain**. **[RESOLVED by
  `[J5h]`:** empirically ground against the PRM, Emu68's register-direct X handling is byte-exact
  CORRECT real 68k — the deferral was conservative, not a real divergence; the oracle was extended to
  match and `addx`/`subx`/`negx`/`neg`/`not` are now driven + byte-exact. See the `[J5h]` section.] (ii) **`roxl`/`roxr`**
  (the X-rotate-through-carry ops) and the **`bfXXX` bitfield** + **`movem`/`movep`/BCD** opcodes are
  vendored (the files link verbatim) but not driven/oracled. (iii) **The SR/exception model** (host
  SIGSEGV inside translated code → a 68k bus/illegal/divide-by-zero vector) — **DONE by `[J5i]`**
  (vectors 2/3/4/5/32+n, the SR+PC frame, `rte`, the S bit; the host-signal seam to
  `graft/cpu_aarch64.h` stated). **SMC / dirty-page
  block-cache invalidation** (no `M68K_VerifyUnit`/CRC32 yet), the **FPU (`LINEF`) + privileged +
  line-A/F** ISA, and the **boot-gated real AROS library environment** (the `LoadSeg`→`RunCommand`/
  `CallEntry` hook + real libraries vs the stub `exec`) all remain. (iv) `j5g_shims.c` (OURS) supplies
  link stubs for the un-driven LINE4 sub-ops it references (the `debug`/`disasm` trace gate +
  `M68K_PopReturnAddress` for `EMIT_MOVEC`/`EMIT_RTS`, never on a hot path — the dispatcher owns RTS).

  *Quarantine / licence.* `M68k_LINE0.c`, `M68k_LINE4.c`, `M68k_LINEE.c` vendored BYTE-VERBATIM
  (`diff` vs upstream `305f686` empty for each), Exhibit-A header present, **Exhibit-B grep clean**
  (no "Incompatible With Secondary Licenses"), `emu68/NOTICE` updated. The flag helpers
  (`EMIT_GetNZCV` etc.) they call are `static inline` in the already-vendored `A64.h`. The EA funnel
  rewrite + the x1-state fix + the CCR-layout fix are all in OUR (AROS-licensed) files; no Emu68
  source is copied. **NEXT coverage gap (CLOSED by `[J5h]`):** the deferred X-bit multi-precision
  chain (`neg`/`not`/`addx`/`subx`/`negx` with byte-exact X) — needed before a `move.l`-with-X-carry
  decompressor-style program can be verified.

- **`[J5h]` the X-bit MULTI-PRECISION chain — IMPLEMENTED / GREEN.** Closes the one bounded coverage
  gap `[J5g]` deferred: byte-exact, X-bit-exact `addx.l`/`subx.l`/`negx.l`/`neg.l`/`not.l` (and the
  `.w`/`.b` sizes via the existing size-param path) through the REAL Emu68 decoders, with the
  independent oracle agreeing on REAL 68k semantics. Files: `hosted/jit68k/j5h_test.c` + the oracle
  `j5d_interp.c` (extended with `addx`/`subx`/`negx` + the multi-precision Z rule + `neg` generalised
  to all sizes) + the new program `apps68k/mp64.s`→`bin/mp64.exe`; target `make hosted-jit68k-j5h`
  (marker `[J5h] PASS`), added to the `test-hosted.sh` regression matrix (alongside the previously-
  missing `[J5f]`/`[J5g]` rows). **PASS (observed):** mp64 — a 64-bit ADD (`add.l` low + `addx.l`
  high, X carries lo→hi) of `0x00000001_FFFFFFFF + 0x00000002_00000001 = 0x00000004_00000000`, then a
  64-bit NEGATE (`neg.l` low + `negx.l` high, X borrows lo→hi) `−S = 0xFFFFFFFC_00000000`, folded to
  **`d0 = 0x000004FC`** — runs through the REAL Emu68 decoders, **byte-exact equal to the independent
  oracle** on the FULL register file AND the **CCR byte INCLUDING the X bit** AND the sandbox memory; a
  `subx.l` 64-bit-subtract sub-test (`0 − 1 = 0xFFFFFFFF_FFFFFFFF`) is also byte-exact; the negative
  control flips `addx.l d4,d2`→plain `add.l d4,d2` (drops the X carry) so the high longword is off by
  one (`d0 = 0x000003FD`), proving the byte-exact assert is not a tautology. `[J1]`–`[J5g]` + `apps68k`
  re-confirmed green. Watchdog 15 s → FAIL.

  *The RESOLVED X-bit / Z semantics — was Emu68 right, or the oracle? (the load-bearing finding).*
  The `[J5g]` report read "Emu68's in-context X handling diverges" and deferred these ops. `[J5h]`
  resolves it EMPIRICALLY: a throwaway probe ran each register-direct X-chain op (`.l`/`.w`/`.b`,
  with X seeded both ways and the prior Z seeded both ways) through the REAL Emu68 decoders and dumped
  the resulting register + CCR byte; each was then checked AGAINST THE M68000 PRM (4th ed., the
  ADDX/SUBX/NEGX/NEG/NOT instruction pages) by hand. **Verdict: Emu68 is byte-exact CORRECT real 68k
  for the register-direct path — the `[J5g]` deferral was conservative (the ops were UN-ORACLED, i.e.
  "don't run what the oracle can't yet check", NOT proven wrong).** The PRM rules now implemented in
  BOTH the engine path (already, via the real decoder) AND the oracle:
  - **`neg`**: `X = C = (result != 0)` (the borrow out of `0 − Dn`); `V` set iff `Dn` was the sized
    sign-minimum (`0x80…`); `N`,`Z` from the sized result. (PRM "NEG".)
  - **`not`**: `N`,`Z` from result; `V = 0`; `C = 0`; **`X` UNAFFECTED** — confirmed against Emu68
    (`EMIT_NOT` declares `SR_dirty = SR_NZVC`, never `SR_X`). (PRM "NOT".)
  - **`negx`/`addx`/`subx`**: `Dx = 0 − Dx − X` / `Dx + Dy + X` / `Dx − Dy − X`; `X = C =` carry or
    borrow out of the sized MSB; `V` = the sized two's-complement overflow; and **THE MULTI-PRECISION
    Z RULE**: "Z — Cleared if the result is nonzero; UNCHANGED otherwise" — i.e. these ops NEVER SET
    Z, they only ever CLEAR it, so Z ANDs across the words of a multi-precision value (a chain leaves
    Z set only if EVERY word was zero). Emu68 implements exactly this (it emits a clear-only
    `b.eq +2 ; bic Z`, never an unconditional set); the oracle's `sized_x_ccr` mirrors it (it carries
    the prior Z forward when the result is zero and clears it otherwise). The probe's
    `addx 0+0 X=0` case with the prior Z seeded 0 vs 1 produced CCR `0x00` vs `0x04` from BOTH the
    engine and the oracle — the discriminating proof that Z is ANDed, not plainly set.
    (Note: the CCR byte uses Emu68's internal `[J5g]` layout — bit0=V, bit1=C, bit2=Z, bit3=N,
    bit4=X — so the oracle and dispatcher already agree on bit positions.)

  *Exactly what `[J5h]` adds + the honest scope statement.* Driven through the REAL decoders +
  oracled byte-exact: `addx.l`/`addx.w`/`addx.b Dy,Dx` (LINED), `subx.l`/`subx.w`/`subx.b Dy,Dx`
  (LINE9), `negx.l`/`negx.w`/`negx.b Dx` + `neg.l`/`neg.w`/`neg.b Dx` + `not.l Dx` (LINE4) — all the
  REGISTER-DIRECT (`Dn`) forms. **No new Emu68 file is vendored** — these decoders all live in the
  already-driven, already-vendored verbatim `M68k_LINE4/LINE9/LINED.c`; Exhibit-B is UNCHANGED and
  re-grep clean, and no `emu68/` file is touched by `[J5h]`. The change is entirely in OUR
  (AROS-licensed) files: the oracle's three new decoders + the `sized_x_ccr` helper, the test driver,
  the `.s`/`.exe` program, the Makefile target, and the regression-matrix rows.

  *Honest deferrals / what `[J5h]` does NOT cover.* (i) The `(An)`/`(An)+`/`-(An)`/displacement
  **MEMORY-EA forms** of `addx`/`subx`/`negx` — Emu68's in-place memory `EMIT_NEGX` carries an
  upstream `// BROKEN!!!!` marker on its byte/word memory path, so only the register-direct chain
  (which is byte-exact) is scoped in. (ii) **`roxl`/`roxr`** (the X-rotate-through-carry ops) — still
  vendored-but-undriven. (iii) Everything `[J5g]` already listed (movem/movep/bitfield/BCD, the
  SR/exception model, SMC, the FPU/privileged/line-A/F ISA, the boot-gated real AROS library env).

- **`[J5i]` the 68k EXCEPTION / SR model — IMPLEMENTED / GREEN.** Closes the highest-value
  remaining gap the prior spikes deferred (named in `[J5d]`/`[J5f]`/`[J5g]`/`[J5h]` + Risks
  "68k exceptions → host signal"): a bounded, **dispatcher-level (C)** 68k exception model —
  a vector table in the sandbox, the standard SR+PC exception frame on the supervisor stack,
  dispatch through the normal PC loop to the vector's handler, and `rte`. A real vasm-assembled
  hunk program (`apps68k/j5i.s` → `bin/j5i.exe`, `-kick1hunks` for the `jmp finish` `RELOC32`)
  installs handlers and raises three exceptions from three REAL causes; three hand-built
  micro-tests cover the frame/`rte`-resume in isolation and the bus-error path. Files:
  `hosted/jit68k/{j5i_test.c, apps68k/j5i.s}` + the model in `hosted/jit68k/j5d_engine.c`
  (engine) / `j5d_interp.c` (oracle) / `j5d_jit68k.h` (the SR/vector/frame types); target
  `make hosted-jit68k-j5i` (marker `[J5i] PASS`, watchdog 15 s), added to the
  `test-hosted.sh` matrix. **PASS (observed):**
  1. **`j5i.exe`** — installs handlers for vectors 33 (TRAP #1), 5 (div0), 4 (illegal) at the
     sandbox VBR stand-in (`0x00240000`), then `trap #1` → vector 33, `divu.w #0,d0` →
     vector 5, `ILLEGAL` (`0x4AFC`) → vector 4. The trap + div0 handlers tally a bit and
     `rte`-resume (saved PC = the instruction after); the illegal handler tallies, pops its
     own frame, and redirects. Exit `d0 = 0x0000075A` (`(tally=7)<<8 | 0x5A`). All three
     exceptions dispatched to the **correct vector** with the **correct frame** (SR + return
     PC, the frame address recorded), byte-exact (registers + sandbox memory + the per-exception
     log) vs the independent oracle.
  2. **`trapframe`** — `trap #2` → vector 34; the handler writes a witness (`d1=0x11`) and
     `rte`s; the saved frame PC is asserted to equal the post-trap instruction's address and
     `rte` is asserted to resume there (`d0=0x5A`). Proves the frame contents + `rte` resume
     in isolation.
  3. **`buserror`** — `jmp 0x00100000` (below the sandbox origin) → a 68k **BUS error**
     (vector 2); the installed handler runs (`d0=0x59`) and the frame carries the bad target
     PC `0x00100000`. This is the **`graft/cpu_aarch64.h` SIGSEGV seam modeled in-band** (below).
  4. **negative control** — NOP the `move.l a0,VBR+33*4` that installs the trap handler →
     the slot stays 0, the trap dispatches to PC 0 (out of sandbox), the frame push then
     faults cleanly; the program does **not** reach `0x075A`. The host does not crash either
     way; the assert bites (`DIVERGED`).

  *Exactly what `[J5i]` models (the honest scope statement).* (a) A **256-longword vector
  table** in the sandbox at a fixed base `J5I_VBR=0x00240000` (our stand-in for the VBR
  register); the tested vectors are populated by the program at runtime
  (`lea handler(pc),An ; move.l An, vbr+vec*4`). (b) The standard **short exception frame**:
  on entry the dispatcher pushes the 16-bit SR then the 32-bit return PC, big-endian,
  predecrement (`a7 -= 6`) — the 68000 format-less 6-byte frame (SR @ a7, PC @ a7+2). (c)
  **`rte`** (`0x4E73`): pop SR (16-bit) + PC (32-bit), `a7 += 6`, restore the condition codes +
  the system byte, resume at the popped PC. (d) The **S (supervisor) bit**: set on every
  exception entry (`sr_high |= S`), saved in the pushed frame, restored by `rte`. (e) The
  **SR packing**: the pushed SR is a genuine architectural 68k SR — the CCR low byte is
  re-ordered from Emu68's internal C/V-swapped storage (`j5d_pack_sr`, shared by engine +
  oracle) so a real handler / `rte` reads standard bits. The vectors covered, each from a
  real cause decoded straight from the stream: `TRAP #n` → 32+n; `divu.w`/`divs.w` with a
  zero divisor → 5; `ILLEGAL` → 4; an out-of-sandbox / odd PC → bus/address 2/3 (reusing the
  `[J5a]` clean-fault detection, now turned into a real 68k dispatch instead of a flag).

  *The architectural finding (why the exception model is OURS, in C).* Emu68's div/illegal
  decoders call `EMIT_Exception(ptr, VECTOR_*, …)` which emits a **branch into Emu68's
  bare-metal VBR-based exception path** (`src/M68k_Exception.c`, NOT vendored — it is part of
  the machine-owning runtime `[J0]` re-hosts). In the hosted runtime that symbol is the
  documented **no-op stub** (`j5c_shims.c`), so an exception-causing instruction driven
  through the decoders would silently do nothing in translated code. The faithful, bounded
  realisation is therefore exactly the spec's Architecture §(2) "**68k exceptions handled in
  C**": the **dispatcher** decodes the exception-causing instructions as terminators (the same
  funnel that already owns the inter-block PC, the return stack, and the `[J3]` LVO bridge),
  builds the frame, and vectors through the sandbox table. `divu`/`divs` are computed in C at
  the terminator (so a zero divisor can vector, and the non-fault result + CCR stay byte-exact
  with the oracle). No `emu68/` file is touched; the body opcodes (`ori`/`lsl`/`move`/…) still
  run through the REAL Emu68 decoders. The Exhibit-B grep is unchanged/clean; no new vendored
  file.

  *The `graft/cpu_aarch64.h` integration seam (stated clearly, not faked).* In an
  AROS-integrated JIT a genuinely wild 68k access (a translated `ldr`/`str` off a corrupt
  `An`, or a jump to a bad PC) faults the **host** with SIGSEGV/SIGBUS.
  `graft/cpu_aarch64.h`'s `SAVEREGS`/`RESTOREREGS` + `struct ExceptionContext` bridge that
  host signal into AROS's trap machinery at the **AArch64** level. The 68k-level layer this
  spike implements is the **piece that pairs with it**: the host signal handler recovers the
  faulting m68k PC (Emu68 keeps it in `x18` at a block boundary; our hosted blocks keep
  `st->pc`) + the fault kind, then calls the SAME `j5d_raise_exception()` path this test
  exercises to build the 68k frame + vector. The seam is:
  `{ host SIGSEGV in translated code } --(graft/cpu_aarch64.h SAVEREGS)--> { recover m68k PC +
  fault address } --> j5d_raise_exception(BUS/ADDRESS) --> { 68k vector dispatch }`. In THIS
  spike the sandbox bounds-check raises the SAME `j5d_raise_exception` path **without** a real
  host SIGSEGV (the clean-fault `[J5a]` detection turned into a real 68k dispatch), so the 68k
  model is exercised end-to-end while the host-signal wiring stays an AROS-side integration
  task — documented, not faked.

  *Honest deferrals (stated in source + here).* The **real VBR register** (we use a fixed
  sandbox base); the **USP/SSP split** (one `a7` — supervisor and user share it; fine for
  self-contained programs that do not switch modes mid-exception); all **68010+ frame formats**
  (the 2-byte format/vector word, the long bus/address-error frame — we model the 68000 6-byte
  frame); **group-0 vs group-1/2 exception priorities** (we dispatch the one faulting
  instruction; no nesting-priority arbitration); and the **actual host-SIGSEGV → this-path
  wiring** (lands at AROS integration via `graft/cpu_aarch64.h`, the seam above). The
  `divu`/`divs` C-decode covers only the register-direct + `#imm.w` source forms the test uses
  (an unsupported EA is a clean error, not a silent miss).

## Risks

Honest debt — restated from the design doc, sharpened by the `[J0]` findings:

- **Self-modifying code / dirty pages.** Amiga decompressors, copy-protection, overlays
  (`HUNK_OVERLAY 1013`) write into their own code hunks, invalidating cached translations.
  Emu68 already CRC32-verifies each unit at dispatch (`M68K_VerifyUnit`, `mt_CRC32`
  `[EMU68]`) and drops a unit on mismatch — we keep that, which gives *correctness* for
  SMC; *performance* dirty-tracking (write-protect 68k code pages, flush only affected
  blocks) is later. Out of `[J1]`–`[J4]` scope.
- **FP / NEON / 68881-68882.** Translating 68k FPU opcodes (`LINEF` `[EMU68]`) to AArch64
  NEON/VFP is a separable large sub-feature; integer-only is the spike scope. NEON state
  plumbing exists (`graft/cpu_aarch64.h`, `_STRUCT_ARM_NEON_STATE64`, `AARCH64_FPU_SIZE`)
  but 68k↔IEEE-754 semantics (extended 80-bit, rounding, traps) are work.
- **68k exceptions → host signal — a SECOND mapping layer.** A host SIGSEGV inside
  translated code must become a **68k** exception (bus/address error, illegal,
  divide-by-zero, TRAP), not AROS's native AArch64 Guru. The project's host-signal→AROS-trap
  bridge (`graft/cpu_aarch64.h` SAVEREGS/RESTOREREGS) targets the **AArch64**
  `struct ExceptionContext` (`graft/cpucontext-aarch64.h`) — *not* a 68k frame `[OURS]`. So
  a second layer is needed: map host signal → `struct M68KState` + Emu68's
  `EMIT_Exception`/VBR vectoring (`VECTOR_ILLEGAL_INSTRUCTION 0x010`,
  `VECTOR_PRIVILEGE_VIOLATION 0x020`, `VECTOR_LINE_A/F`, `VECTOR_INT_TRAP(n)` `[EMU68]`).
  Note Emu68's exceptions stay inside m68k VBR space, which is what we want for genuine 68k
  traps — but it means our *host* SIGSEGV handler must translate the faulting AArch64
  context back to the m68k PC (x18) before invoking the 68k vector. **UNVERIFIED** scope.
- **Endianness / 32-bit sandbox.** 68k is 32-bit **big-endian**; the relocator already
  emits 32-bit big-endian pointers (`AROS_BE2LONG` `[AROS]`), and every load/store the
  translator emits must byte-swap and stay inside the ≤4 GiB sandbox. Emu68 is a 32-bit
  big-endian m68k emulator by construction, so its decoders already byte-swap — but the
  **bridge** marshalling (sandbox-addr ↔ host-pointer, step 5 above) is where this is easy
  to get subtly wrong; the `[J2]` reference diff and `[J3]` arg capture are the guards.
- **MPL boundary.** Containment depends on never pasting Emu68 source into AROS files and
  on the Exhibit-B check (banner `[ACTION]`). A future contributor copying an Emu68 macro
  into an AROS file silently relicenses that file — process risk, mitigated by the
  quarantine dir + the own-authored `jit68k.h`.
- **Entitlement / codesign — RESOLVED (`[J1]`).** On this Mac, the default linker ad-hoc
  signature already permits MAP_JIT (no codesign step); only the **hardened runtime**
  (`-o runtime`) blocks it, and `com.apple.security.cs.allow-jit` unblocks it. So the live
  risk is narrowed to: once the AROS bootstrap adopts the hardened runtime, its signing
  step MUST add `com.apple.security.cs.allow-jit` (the plist + command are in
  `hosted/jit68k/`, mirrored into `graft/bootrun.sh`'s `--entitlements` slot). See
  R-JIT-ENTITLE for the full tested matrix.

## Open questions / UNVERIFIED (Role B to resolve)

- The **seglist 68k-tagging** mechanism at the `RunCommand`/`CallEntry` decision point
  (AROS-side data flow — see AROS-side dispatch).
- The **LVO recognition trigger** at the dispatcher funnel (PC-range check vs. reserved
  stub vs. redirected `EMIT_Exception`). **PARTIALLY RESOLVED by `[J3]`:** `[J3]` proves the
  **recognition math** — a jump-target PC is mapped to `(library base, LVO n)` by the real
  negative-offset rule `n = (libbase − pc)/6` (`j3_vector.c`, `j3_vector_recognise`), with
  above-base and misaligned PCs rejected. What remains for `[J4]` is wiring that math to the
  actual `MainLoop` funnel on a **decoded** `jsr` (PC-range check over registered library
  bases vs. a reserved-address stub) rather than a hand-constructed call.
- ~~The `[J2]` **reference oracle**: Emu68's own `M68K_TranslateNoCache` (recommended, since
  we adopt) vs. an independent interpreter / vendored Musashi.~~ **RESOLVED (`[J2]`):** a
  **from-scratch independent interpreter** (`hosted/jit68k/j2_interp.c`, OURS, no Emu68),
  not Emu68's own decode — see the `[J2]` bullet for the rationale (an Emu68-vs-Emu68 check
  is near-tautological and would need the deferred coupled decoder just to build the oracle).
- **Pointer/sandbox** marshalling for native calls that return memory outside the 68k
  sandbox (needs a sandbox-backed allocator for 68k-visible memory).
- ~~The exact **entitlement string set** and whether ad-hoc `run.sh` signing suffices —
  `[J1]` (R-JIT-ENTITLE).~~ **RESOLVED:** `com.apple.security.cs.allow-jit` only, and only
  under the hardened runtime; default linker ad-hoc signing already suffices unsigned.
  See R-JIT-ENTITLE / Risks.
- ~~The **Exhibit-B** per-file check on the adopted Emu68 files (MPL↔LGPL compatibility).~~
  **RESOLVED for the `[J2]` files:** `grep -n "Incompatible With Secondary Licenses"
  hosted/jit68k/emu68/{A64.h,RegisterAllocator.h}` is **empty** — both carry only the MPL
  Exhibit-A header, so MPL §3.3 LGPL-compatibility holds. Re-run the grep before vendoring
  any *further* Emu68 files (the decoder/RA set `[J3]` will add); recorded in the quarantine
  `NOTICE`.
- The future **multi-thread scheduler** interaction with the per-thread JIT write-protect
  toggle (R-JIT-THREAD) — owned by the scheduler milestone, not this spec.

## Provenance summary

`[J0]` decision: **adapt Emu68's core** (lift `A64.h` emitter + `M68k_LINE*`/EA/CC/SR
decoders + `M68k_Translator.c`/`ExecutionLoop.c`; re-host the bare-metal runtime). ·
`[PUB]` Apple MAP_JIT / `pthread_jit_write_protect_np` / `sys_icache_invalidate` /
`com.apple.security.cs.allow-jit`; the m68k ISA; the AmigaOS hunk format; the MPL-2.0 text
+ Mozilla FAQ. · `[AROS]` `rom/dos/internalloadseg_aos.c`, `rom/dos/internalloadseg.c`,
`rom/dos/runcommand.c`, `arch/m68k-all/dos/callentry.S`, `arch/m68k-all/include/aros/cpu.h`
(`__AROS_ASMJMP 0x4EF9`, `__AROS_GETJUMPVEC`, `__AROS_USE_FULLJMP`),
`compiler/arossupport/include/libcall.h` (`__AROS_LHA`),
`compiler/arossupport/include/asmcall.h` (`__AROS_UFHA`), `compiler/include/dos/doshunks.h`. ·
`[OURS]` the H3 host-call shim (`hosted/abishim.S`, the bridge's mirror-image), the H4/H6
single-thread scheduler (`hosted/kern.c`), the H7 unattended-readback discipline
(`hosted/display.c`), `graft/cpu_aarch64.h` / `graft/cpucontext-aarch64.h` (the
host-signal→AArch64-trap bridge that the 68k-exception layer sits *beside*); the `[J2]`
spike — glue + independent interpreter — in `hosted/jit68k/{j2_jit68k.h,j2_build.c,
j2_interp.c,j2_test.c}`; the `[J3]` LVO-call bridge — vector recognition + reverse-H3
marshaller emitted via the adopted Emu68 emitter — in `hosted/jit68k/{j3_jit68k.h,
j3_vector.c,j3_marshal.c,j3_test.c}`, grounded against `arch/m68k-all/include/aros/cpu.h`
(`__AROS_GETJUMPVEC`/`LIB_VECTSIZE`/`struct JumpVec`),
`compiler/arossupport/include/{libcall.h:1586 (AROS_LHA),asmcall.h:822 (AROS_UFHA)}`, and
`arch/m68k-all/include/gencall.c` (the `reg→%reg` source-register mapping + `_ret0 asm("%d0")`
return); the `[J4]` load→relocate→run chain — minimal hunk loader + `HUNK_RELOC32`
relocator + 32-bit sandbox + the `[J2]` translate→run path — in `hosted/jit68k/{j4_hunk.h,
j4_loader.c,j4_build.c,j4_test.c}`, grounded against `compiler/include/dos/doshunks.h` (the
hunk-type constants) and `rom/dos/internalloadseg_aos.c` (the `HUNK_HEADER` parse + the
`HUNK_RELOC32` BE32-read / add-target-base / BE32-write at lines 292-332, `GETHUNKPTR`
line 25, `lasthunk` line 288); the `[J5a]` memory load/store + sandbox-pointer boundary —
hand-rolled EA + sandbox-base/bounds-check/byteswap around the adopted Emu68 *emitter*, with
an independent reference asserting registers AND sandbox memory byte-exact — in
`hosted/jit68k/{j5a_jit68k.h,j5a_build.c,j5a_interp.c,j5a_test.c}` (reuses `j4_loader.c`),
grounded against the m68k ISA (the `move.l (An)`/`(An)←Dn`/`addq` encodings) and the
**RA/EA non-adoption finding** cited to Emu68 `M68k_EA.c:635-639,760+` and
`RegisterAllocator64.c:175-185,288-303`; the `[J5b]` control-flow spike — a self-contained
loop with a conditional backward branch + **real condition codes** translated into a single
`jit_region` with an internal `b.ne` (reading the live NZCV straight from the emitted `subs`,
the full 68k CCR recomputed with non-flag-setting ops so `subs`→`b.ne` adjacency holds; 68k
subtract C = AArch64 carry-CLEAR borrow), verified registers + full CCR + iteration count +
termination vs an independent reference, with a broken-branch (always-taken) negative control
caught by a forked-child watchdog — in `hosted/jit68k/{j5b_jit68k.h,j5b_build.c,j5b_interp.c,
j5b_test.c}` (reuses `j4_loader.c`), grounded against the m68k ISA (the
`moveq`/`add.l`/`subq.l`/`bne.s` encodings + the M68000 subtract flag rule) and the existing
`A64.h` encoders only (no new Emu68 file). · `[EMU68]` **adopted so far (in-tree, MPL —
quarantine `hosted/jit68k/emu68/`):** `A64.h` (the AArch64 emitter) + `RegisterAllocator.h`
(declarations only, to resolve `A64.h`'s `#include`), per the quarantine `NOTICE` (commit
`305f686f84712f88c4d80d35769af5c60a4e988b`). **`[J5a]` confirmed these do NOT lift
incrementally** (EA assumes `An`-is-host-pointer 1:1 MMU; reads ext words via the `ICACHE`
software cache; the RA keeps SR/CTX in EL0 system registers) — so the register allocator is
OURS around the emitter, and **no new Emu68 file was vendored for `[J5a]` or `[J5b]`** (the
`[J5b]` loop/branch/real-CCR path uses only existing `A64.h` encoders, so the Exhibit-B grep
is unchanged/clean for both). **Future `[J5c]` adoption (not yet vendored, if any):**
`src/M68k_LINE*.c`, `src/M68k_{MOVE,MULDIV,CC,SR,Exception}.c`, `src/M68k_Translator.c`,
`src/ExecutionLoop.c` — block model (`M68KTranslationUnit`, `ICache`/`LRU`, `MainLoop`
RET-to-dispatcher funnel, PC in x18, `INT32` interrupt hook, `EMIT_Exception`/VBR). ·
`[REF-CONFIRM]` none — the translator is adopted MPL, read directly, not clean-roomed.
