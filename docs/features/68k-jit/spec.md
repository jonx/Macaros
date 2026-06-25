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

- **`[J5c]` the rest of the decoder/RA mountain — DEFERRED.** What `[J5b]` does NOT do, and
  `[J5c]`/beyond still needs: **cross-region block chaining + an instruction cache**
  (`[J5b]` is one self-contained region; no block-to-block linking, no `M68KTranslationUnit`
  `ICache`/`LRU`); **full `Bcc`/`DBcc` condition coverage** (`[J5b]` does `bne.s` only) and
  **forward branches across blocks**; **full opcode + addressing-mode coverage** (the rest
  of `move`, the `(d16,An)`/`(d8,An,Xn)`/absolute/PC-relative EA modes, `MULDIV`, the line
  decoders) with **real per-opcode CCR** generalised across opcodes and **`moveq`
  sign-extend** generalised; **OUR register allocator built around the emitter** (still
  memory-backed here — the `[J5a]` finding: Emu68's `RegisterAllocator64.c` is not
  adoptable: 1:1-MMU / software-ICACHE / EL0-system-register coupling); the **real
  `jsr`-through-vector decode from a stream** wired to the `[J3]` recognition math at the
  dispatcher funnel; **library calls FROM the running program** (the `[J3]` bridge invoked
  on a decoded `jsr`, with sandbox-pointer marshalling of `An` args); and a **sandbox-backed
  allocator** for native-call return-pointers that lie *outside* the sandbox. Whatever lift
  remains (the `M68k_LINE*`/`M68k_CC`/`M68k_SR`/`M68k_Exception` decoders,
  `M68k_Translator.c`/`ExecutionLoop.c` block cache + `MainLoop` funnel, PC in x18) must
  re-run the Exhibit-B grep (`Incompatible With Secondary Licenses`) before vendoring each
  new Emu68 file and record it in `hosted/jit68k/emu68/NOTICE`. This is large-scale Emu68
  adoption + re-hosting, not a session-sized spike.

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
