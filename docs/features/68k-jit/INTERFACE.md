# 68k JIT — the frozen JIT↔AROS seam (single source of truth)

> Status: **v1 — proposed, freezing in progress** · Drafted 2026-06-25
> Companion to [spec.md](spec.md) (the `[J*]` spikes) and [design.md](design.md)
> (the `[J0]` decision + the AROS-side dispatch section). Process:
> [../CLEANROOM.md](../CLEANROOM.md).

The 68k JIT is **built and proven host-side**: `[J1]`–`[J5i]` re-host Emu68's real
per-opcode decoders + register allocator, run nine real vasm-assembled Amiga hunk
programs byte-exact against an independent interpreter, with a block-scoped register
allocator, a real 68k return stack, broad ISA + addressing-mode coverage, and a 68k
exception/SR model — all in `hosted/jit68k/`. The **AROS-side integration** (the
LoadSeg hook, the real library bases, the boot) is the project owner's track.

This document freezes the seam so the two sides meet without negotiation — the way
[../cocoa-metal-display/INTERFACE.md](../cocoa-metal-display/INTERFACE.md) froze the
display seam so wiring became plumbing. It freezes **five seam points** plus the
done-vs-gated split (§6) and the checklist (§7). Every seam splits **JIT side
(done, host-proven)** vs **AROS side (to wire)**, each grounded in a real path. The
contact surface is the flat C-ABI header `jit68k.h` (spec.md "The integration
boundary / C ABI") — AROS calls *into* it; it calls *back* into AROS through a
registered marshal callback. Nothing else crosses, and the MPL Emu68 core stays
behind that header (no Emu68 declaration is ever copied into an AROS file).

The single biggest UNVERIFIED the owner must decide is **§1's seglist-68k-tagging
mechanism** — the one genuinely-new AROS-side data-flow change. It must not be
guessed; options are proposed below.

---

## 1. The LoadSeg / RunCommand hook — where a 68k seglist diverts into the JIT

The loader and relocator are **reused unchanged** — the JIT does not reimplement
them. The seam is a single divert at the entry point, plus one new tagging decision.

### JIT side (done)

The `[J4]`/`[J5a]`–`[J5i]` engine takes a relocated 68k entry PC and runs it:
`jit68k_run(jit, entry_pc, &exit_d0)` (spec.md `jit68k.h`) translates basic blocks
into a `MAP_JIT` code cache, drives the real Emu68 decoders, dispatches via the
re-hosted `MainLoop` (PC-driven, with a real return stack — `[J5f]`), and returns
the program's final 68k `d0`. `[J4]` proved load→relocate→place-in-sandbox→
translate→run→return end-to-end on a real big-endian hunk binary, applying
`HUNK_RELOC32` **byte-identically** to `rom/dos/internalloadseg_aos.c:292-332`
(`val = AROS_BE2LONG(*addr) + (IPTR)GETHUNKPTR(target)`). So the translator side of
the divert is complete and value-asserted.

### AROS side (to wire)

`InternalLoadSeg_AOS` (`rom/dos/internalloadseg_aos.c`), selected by the hunk magic
`0x000003f3` in `rom/dos/internalloadseg.c`, **already produces a relocated 68k
seglist on this Mac today** — it just has nothing to execute it. The entry/divert
point is grounded (verified against the real files):

- `RunCommand` (`rom/dos/runcommand.c`) computes the entry
  `args.Args[2] = (IPTR)BADDR(segList) + sizeof(BPTR)` (runcommand.c:134) and
  `NewStackSwap`s into `CallEntry` (runcommand.c:137).
- On m68k the entry thunk is `arch/m68k-all/dos/callentry.S` `AOS_CallEntry`, which
  receives the entry in **A4** (`AROS_UFHA(LONG_FUNC, entry, A4)`, callentry.S:18)
  and reaches it natively — `jsr (%a4)` for a CLI app (callentry.S:100) or an
  `rts`-to-entry for Workbench (callentry.S:104-115).

**The change is here:** where the AArch64 host would branch native, it instead
recognises a 68k seglist and calls `jit68k_run(jit, entry_pc, &exit_d0)` — handing
the entry PC to the translator instead of executing it as AArch64. The 68k code,
data and BSS hunks live in the 32-bit big-endian sandbox (§5) the relocator already
produced; the native AArch64 host is not a 68k, so a native branch is exactly the
thing that cannot happen.

### The one genuinely-new AROS-side decision — UNVERIFIED (owner decides)

`InternalLoadSeg_AOS` was reached **because** the file magic was `0x000003f3`, so
the loader path inherently knows the result is 68k. That fact must be carried to the
`RunCommand`/`CallEntry` decision point — and there is no existing channel for it.
This is the **seglist-68k-tagging mechanism**, the one new data-flow change, and the
project's biggest open question. It must not be guessed. Proposed options (the owner
picks the cheapest that survives the seglist's whole lifetime — note a seglist can
outlive the load call, be passed around, and be run more than once):

1. **A flag in the DOS-side segment bookkeeping** — tag the seglist (or its owning
   `Segment`/`pr_SegList` record) "is-68k" on the `InternalLoadSeg_AOS` path; the
   divert reads it. Most localised; needs a spare bit in a structure that travels
   with the seglist.
2. **A distinguished seglist sentinel** — a recognisable marker word ahead of the
   first hunk (the loader writes it; the divert sniffs it). Self-describing and
   survives copying, but spends a longword and must not collide with valid code.
3. **A per-task/per-process "is-68k" attribute** — set on the load→run path (e.g.
   on the `Process`); simplest to read at `CallEntry`, but fragile if one process
   runs both native and 68k seglists in its lifetime.

Recommendation to evaluate first: option 1 (the bit travels with the thing being
diverted, so the divert decision is local and lifetime-correct). Whichever is
chosen, the divert at `CallEntry`/the AArch64 entry thunk is the single consumer.

---

## 2. The `jit_region` host-call routing — MAP_JIT through `hostlib.resource`

### JIT side (done)

The spikes call the three Apple-silicon JIT primitives **directly** today, wrapped
in `hosted/jit68k/jit_region.{h,c}` as a reusable API the emitter and the LoadSeg
path both build on:

- `jit_region_alloc` → `mmap(NULL, len, PROT_READ|PROT_WRITE|PROT_EXEC,
  MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0)` (R-JIT-MAP).
- `jit_write_begin`/`jit_write_end` → `pthread_jit_write_protect_np(0)` … `(1)`,
  the per-thread write window (R-JIT-WRITE).
- `jit_finalize` → `sys_icache_invalidate(ptr, len)` before execution (R-JIT-ICACHE;
  arm64 I/D caches are **not** coherent for self-written code — skipping it yields a
  stale-I-cache *wrong value*, not a crash, so `[J1]` asserts the value).

`[J1]` proved this green and **resolved the entitlement** (R-JIT-ENTITLE, empirical,
macOS 26.5, Apple silicon): the default linker ad-hoc signature already permits
MAP_JIT unsigned-by-hand; only the **hardened runtime** (`-o runtime`) blocks it
(`mmap(MAP_JIT)` → `EINVAL`), and the **single** key
`com.apple.security.cs.allow-jit` unblocks it. The exact command the AROS bootstrap
needs (it already passes `-o runtime`, see `graft/bootrun.sh`):

```
codesign -s - -f -o runtime --entitlements jit68k.entitlements.plist <binary>
```

(`com.apple.security.cs.allow-unsigned-executable-memory` is **not** required —
`allow-jit` + MAP_JIT is the W^X-clean path.) This retires the deferred-LoadSeg W^X
wall NOTES.md flagged ("executing code loaded into the pool (LoadSeg) needs the W^X-
aware path later").

### AROS side (to wire)

At integration, the three primitives are not called directly — they route through
**`hostlib.resource` + `abishim.S`**, exactly like every other macOS call in the
hosted port (NOTES.md H3; the boot already brings up `hostlib.resource`). The JIT
code cache is a **separate** region from the RW AROS RAM pool (which is mapped RW,
not RWX — NOTES.md "Map the AROS RAM pool RW"); the sandbox of §5 is ordinary
`AllocMem` RW memory, only the code cache is MAP_JIT. The signing step in
`graft/bootrun.sh` must add the `allow-jit` entitlement once the bootstrap adopts
the hardened runtime (it carries the `--entitlements` slot already).

### The per-thread caveat — R-JIT-THREAD (frozen constraint)

`pthread_jit_write_protect_np` toggles **per-thread** writability of all MAP_JIT
pages. So **all JIT emission and all execution of translated blocks must happen on
the same host thread** — the single H6 scheduler thread (NOTES.md H4/H6;
`hosted/kern.c`). Under today's single-thread model this is satisfied automatically.
**Frozen forward constraint:** if the scheduler ever dispatches AROS tasks onto
multiple host threads, each emitting/executing thread must re-assert the toggle on
itself — UNVERIFIED, owned by the scheduler milestone, not the JIT. Long-running 68k
programs yield at the next block boundary via `jit68k_request_yield` (bounded by the
`MainLoop` `INT32`-check funnel).

---

## 3. The LVO-call bridge — a 68k `jsr` → a native `AROS_LH` on the real base

This is the load-bearing integration boundary. It is the **reverse** of the H3 host-
call shim (`hosted/abishim.S` marshals AROS_LH→host C; here it runs 68k-regs →
AArch64 AROS_LH) and is **data-driven by the FD/register table**, not a variadic
stack walk.

### JIT side (done)

`[J3]` proved the bridge in three value-asserting parts (`hosted/jit68k/{j3_vector.c,
j3_marshal.c}`); `[J5d]`/`[J5f]` wired it to a **decoded** `jsr d16(A6)` from the
real instruction stream (libcall→`AllocMem`/`PutChar`/`FreeMem` observed with the
right args/returns and `bytes_outstanding==0`):

1. **Recognition (the dispatch math), grounded against the REAL contract**
   `arch/m68k-all/include/aros/cpu.h`: a 68k caller reaches function *n* via a 6-byte
   `struct JumpVec { unsigned short jmp; void *vec; }` (cpu.h:50-54) at `libbase −
   n*6`, where `jmp == __AROS_ASMJMP 0x4EF9` (cpu.h:61), `LIB_VECTSIZE ==
   sizeof(struct JumpVec)` (cpu.h:81), and `__AROS_GETJUMPVEC(lib,n) =
   &((JumpVec*)lib)[-(n)]` (cpu.h:82). The dispatcher does **not** emit a native
   branch into a fake 68k vector — it recovers `n = (libbase − pc)/6`
   (`j3_vector_recognise`) and invokes the handler. The spike gets the host/target
   divergence right: it uses **stride 6** (the m68k packed `JumpVec`), not host
   `sizeof` (16).
2. **The marshaller (emitted via the adopted Emu68 emitter into a `jit_region`).**
   For LVO *n*, read each declared arg from its 68k register in `struct M68KState`
   and place it in the matching AAPCS64 register, then `blr` the native function;
   a 32-bit result is stored back into 68k `d0`. The source-register map is taken
   **directly** from the REAL macros: `AROS_LHA(type,name,reg)`
   (`compiler/arossupport/include/libcall.h:1586`) /
   `AROS_UFHA(type,name,reg)` (`compiler/arossupport/include/asmcall.h:822`); the
   `reg` element is the m68k source register (`arch/m68k-all/include/aros/cpu.h:125-
   139`), loaded before the library jump and read back from `%d0`
   (`arch/m68k-all/include/gencall.c`). Three stubs with different/out-of-order
   D-and-A maps were verified; a swapped-order negative control correctly FAILS.

The C-ABI hook is frozen in `jit68k.h` (spec.md):

```c
typedef int (*jit68k_lvo_fn)(void *user, uint32_t libbase, int lvo,
                             struct m68k_regs *regs);
void jit68k_set_lvo_handler(JIT68K *, jit68k_lvo_fn, void *user);
/* return 0 = handled (resume at the 68k return address);
        nonzero = unhandled (raise a 68k exception). */
```

### AROS side (to wire)

Today a **stub `exec`** stands in for the library base. The seam is: **AROS passes
the JIT the real library bases.** AROS registers each library it exposes to 68k code
— its 68k-space base + a pointer to its FD/LVO→register table — with the translator,
then sets `jit68k_set_lvo_handler` to an AROS callback doing the marshal (read the
declared args from `struct m68k_regs`, call the real native AArch64 `exec`/`dos`/
`graphics` function, write the return into 68k `d0`). **The native libraries are
unchanged** — they receive an ordinary `AROS_LH` call. A 68k program that draws
ends up going 68k→(this bridge)→native graphics.library→host display driver — three
defined boundaries, no shortcuts.

---

## 4. The exception bridge — host `SIGSEGV` in translated code → a 68k vector

### JIT side (done)

`[J5i]` implemented a bounded, dispatcher-level (C) **68k exception/SR model**
(`hosted/jit68k/{j5d_engine.c, j5d_jit68k.h}`, oracle `j5d_interp.c`): a 256-longword
vector table in the sandbox (VBR stand-in `0x00240000`), the 68000 format-less 6-byte
exception frame (SR @ a7, return-PC @ a7+2, predecrement push), `rte` (`0x4E73`), the
S (supervisor) bit set on entry / restored by `rte`, and a genuine architectural SR
(`j5d_pack_sr` re-orders the CCR from Emu68's internal C/V-swapped storage). A real
hunk program (`apps68k/j5i.s`) raised three exceptions from three REAL causes —
`trap #1` → vector 33, `divu.w #0` → vector 5, `ILLEGAL 0x4AFC` → vector 4 — each
dispatched to the correct vector with the correct frame, byte-exact vs the oracle,
plus a bus-error path (`jmp` below the sandbox origin → vector 2). It is the
realisation of spec.md Architecture §(2) "68k exceptions handled in C" — Emu68's own
`EMIT_Exception`/VBR path (`src/M68k_Exception.c`) is part of the machine-owning
runtime `[J0]` re-hosts and is the documented no-op stub in `j5c_shims.c`.

### AROS side (to wire) — the frozen contract

A genuinely wild 68k access in *integrated* translated code (an `ldr`/`str` off a
corrupt `An`, or a jump to a bad PC) faults the **host** with `SIGSEGV`/`SIGBUS`.
`graft/cpu_aarch64.h` already bridges a host signal into AROS's trap machinery at the
**AArch64** level — `SAVEREGS`/`RESTOREREGS` copy `*uc->uc_mcontext` (`__ss`/`__ns`)
into `struct ExceptionContext` (the corrected AArch64 layout from
`graft/cpucontext-aarch64.h`). The 68k-level layer `[J5i]` implements is the **piece
that pairs with it**. The frozen seam:

```
{ host SIGSEGV/SIGBUS in translated code }
        --( graft/cpu_aarch64.h SAVEREGS, struct ExceptionContext )-->
{ recover the m68k PC (st->pc; Emu68 keeps it in x18 at a block boundary)
  + the fault kind/address }
        --> j5d_raise_exception(BUS/ADDRESS/ILLEGAL/DIVZERO/TRAP)
        --> { build the 68k SR+PC frame, vector through the sandbox table }
```

`[J5i]` exercises this same `j5d_raise_exception` path end-to-end via the `[J5a]`
clean-fault sandbox bounds-check (no real host SIGSEGV in the spike), so the 68k
model is fully proven; the **actual host-signal→this-path wiring** is the AROS-side
integration task — documented, not faked. A host `SIGSEGV` in translated code must
become a **68k** bus/illegal/divide-by-zero/TRAP vector, **never** AROS's native
AArch64 Guru — that is the contract this seam freezes.

---

## 5. The sandbox / pointer boundary — `An` ↔ host pointer, 32-bit big-endian

### JIT side (done)

The 68k address space is a **32-bit big-endian sandbox** (`[J4]`/`[J5a]`). The block
is entered with the sandbox base-adjust in a register; every memory op through `An`
(a) **bounds-checks** with a single unsigned compare (a host-OOB-free clean fault,
no SIGSEGV), (b) maps to host with `host = sandbox_base + An` (`add … UXTW`,
zero-extending the 32-bit `An`), and (c) **byteswaps** big-endian with `REV` around
the host `ldr`/`str` (`[J5a]` `j5d_ea_helpers.c`; `[J5g]` broadened to the full
M68000 addressing modes — `(d16,An)`, `(d8,An,Xn)`, `abs.w/.l`, PC-relative, all
verified byte-exact). The relocator already emits 32-bit big-endian pointers, so the
sandbox stays big-endian as written. The `jit68k_create(void *sandbox_base, uint32_t
sandbox_size)` boundary (spec.md `jit68k.h`) carries the base + size.

### AROS side (to wire) + the deferred case

At integration the sandbox must be a **sandbox-backed allocator**: 68k-visible memory
comes from `AllocMem` with a 31-bit-addressable constraint (`MEMF_31BIT` or
equivalent — UNVERIFIED which exact AROS flag; the H5 pool must yield ≤4 GiB-space
backing for the sandbox). The marshaller (§3) converts a pointer **argument** from a
32-bit sandbox address to a host pointer (`sandbox_base + addr`) before the native
call, and a returned host pointer that lies in the sandbox back to a 32-bit address.

**The deferred case (UNVERIFIED, honest debt):** a native AROS call that returns or
takes memory **outside** the sandbox — e.g. a freshly `AllocMem`'d host buffer the
68k program must then reach. The `[J3]` spike keeps all pointers as in-range sandbox
addresses and does **not** handle the return-pointer-outside-sandbox case. Resolving
it needs exactly the sandbox-backed allocator above (so 68k-visible allocations land
in-sandbox), and is the open production concern paired with §1's tagging. Flagged in
spec.md Risks "Endianness / 32-bit sandbox".

---

## 6. What the JIT delivers today vs. what's boot-gated (honest)

**Delivered, host-proven (done — `hosted/jit68k/`, all markers green):**

- The MAP_JIT executable-memory layer + the resolved entitlement (`[J1]`).
- The adopted Emu68 **emitter** + **real per-opcode decoders** re-hosted behind our
  dispatcher and register allocator (`[J2]`, `[J5c]`–`[J5h]`): register/ALU/move/
  shift-rotate/immediate/multiply, the X-bit multi-precision chain, the full M68000
  addressing modes, real condition codes.
- A PC-driven dispatcher with a **real 68k return stack** (nested `bsr`/`jsr`/`rts`,
  computed `jmp(An)`/`jsr(An)`, full `Bcc`/`BRA`/`BSR` widths) over a PC-keyed block
  cache (`[J5f]`); a block-scoped register allocator (`[J5e]`).
- **Cross-region block chaining + cross-block register caching** (`[J5k]`): static-target
  terminators (fall-through/`BRA`/`jmp abs.l`/`Bcc`) chain block→block with **direct
  AArch64 branches past the C dispatcher** (lazy backpatch/linking), with the 68k register
  file **pinned live in host regs across the hop** — the file spills to `struct M68KState`
  ONLY at a dispatcher boundary (`rts`/the `[J3]` LVO bridge/exception/computed jump). On the
  Mandelbrot this cut C-dispatcher round-trips **41 757 → 1 728 (95.9 % fewer)**, byte-exact.
  **This is entirely INTERNAL to the engine's block dispatcher — it does NOT touch this seam:**
  the `jit_region` API, the `struct M68KState` field layout, the `[J3]` LVO-call marshalling
  contract, and the `[J5i]` exception/SR model are all unchanged (`[J5k]` is below them).
- The **LVO-call bridge** decoded from a real `jsr` stream into a stub `exec`
  (`[J3]`, `[J5d]`).
- The **68k exception/SR model** + the `graft/cpu_aarch64.h` seam stated (`[J5i]`).
- **Nine** real vasm-assembled hunk binaries run **byte-exact** vs an independent
  from-scratch interpreter (`mul`/`fact`/`arraysum`/`libcall`/`sumsq`/`bubsort`/
  `mp64`/`j5i` + the `[J5c]` block) through the loader→relocate→sandbox→JIT path.

**Boot-gated (the owner's track — not yet done):**

- The real `LoadSeg`→`RunCommand`/`CallEntry` divert + the §1 tagging decision.
- The **real library environment** — real `exec`/`dos`/`graphics` bases passed to
  the bridge (§3), replacing the stub `exec`.
- The sandbox-backed allocator + the return-pointer-outside-sandbox case (§5).
- The host-signal→`j5d_raise_exception` wiring (§4) in the integrated runtime.
- A **real Amiga app** (vs the corpus) — gated on the boot reaching `dos.library`
  (NOTES.md: the boot halts at a cold-start trap, no `dos.library` in the minimal
  3-module kickstart).

**Honest engine scope (beyond this seam):** the engine is integer-user-mode, single-
region-flat-PC-plus-return-stack, single-thread (R-JIT-THREAD). Still deferred:
self-modifying-code / dirty-page block-cache invalidation (no `M68K_VerifyUnit`/CRC32
re-host yet); the FPU (`LINEF`) + privileged ISA; the `HUNK_OVERLAY` loader; and the
USP/SSP split + 68010+ frame formats in the exception model.

---

## 7. Split & status checklist

JIT side rows are **done (host-proven, value-asserted)**; AROS side rows are the
hooks the owner builds. The seam (§1 hook + §2 routing + §3 bridge + §4 exceptions +
§5 sandbox) is the contract both sides build against.

| # | Deliverable | Owner | Status |
|---|---|---|---|
| J1 | MAP_JIT exec-memory layer + entitlement resolved (`jit_region.{h,c}`) | JIT | ✅ `[J1]` green; `com.apple.security.cs.allow-jit` is the single key, only under `-o runtime` |
| J2 | Adopted Emu68 emitter hosted in MAP_JIT, byte-exact vs independent oracle | JIT | ✅ `[J2]` green; the `[J0]` "adopt emitter, re-host runtime" bet holds |
| J3 | LVO-call bridge: vector recognition + reverse-H3 marshaller, real ABI macros | JIT | ✅ `[J3]` green; grounded against `cpu.h`/`libcall.h:1586`/`asmcall.h:822`/`gencall.c` |
| J4 | Load→relocate→sandbox→run on a real hunk binary (`HUNK_RELOC32`) | JIT | ✅ `[J4]` green; relocation byte-identical to `internalloadseg_aos.c:292-332` |
| J5a–h | Real Emu68 decoders re-hosted: memory EA + sandbox boundary, control flow, block RA, return stack, broad ISA, X-bit chain | JIT | ✅ `[J5a]`–`[J5h]` green; nine corpus programs byte-exact |
| J5i | 68k exception/SR model + the `graft/cpu_aarch64.h` host-signal seam (stated) | JIT | ✅ `[J5i]` green; vectors 2/4/5/32+n, SR+PC frame, `rte`, S bit |
| A | **The seglist-68k-tagging mechanism** (§1) — the one new AROS data-flow change | AROS | ☐ UNVERIFIED — owner decides (options in §1); the biggest open question |
| B | The `RunCommand`/`CallEntry` divert → `jit68k_run` (§1) | AROS | ☐ — at the A4-entry point (`callentry.S:18,100,104-115`); consumes the §1 tag |
| C | Route `jit_region` primitives through `hostlib.resource` + `abishim.S`; add `allow-jit` in `bootrun.sh` (§2) | AROS | ☐ — like every other macOS call (NOTES.md H3) |
| D | Pass the **real** library bases + FD/register tables; set `jit68k_set_lvo_handler` to the AROS marshal callback (§3) | AROS | ☐ — replaces the stub `exec`; native libs unchanged |
| E | Wire host `SIGSEGV`/`SIGBUS` in translated code → `j5d_raise_exception` via `graft/cpu_aarch64.h` (§4) | AROS | ☐ — recover the m68k PC + fault kind, then the proven 68k vector path |
| F | Sandbox-backed allocator (`AllocMem` + `MEMF_31BIT`/equiv) + the return-pointer-outside-sandbox case (§5) | AROS | ☐ UNVERIFIED flag; resolves the deferred `[J3]` pointer case |
| G | A real Amiga app through the boot (vs the corpus) | both | ☐ — gated on the boot reaching `dos.library` (NOTES.md cold-start halt) |

Rows J1–J5i are the JIT side, **done**. Rows A–G are the AROS side, **to wire** —
A–B are the LoadSeg hook + tagging, C is the host-call routing, D is the LVO bridge
to real bases, E is the exception bridge, F is the sandbox allocator, G is the boot
payoff. The seam between them is this document.
