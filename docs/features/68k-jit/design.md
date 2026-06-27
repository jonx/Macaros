# 68k JIT — run classic Amiga 68k software at native AArch64 speed

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-24

> **The JIT↔AROS seam is frozen in [INTERFACE.md](INTERFACE.md)** — the LoadSeg/
> RunCommand hook + seglist tagging, the `jit_region`→`hostlib.resource` routing,
> the LVO bridge to the real library bases, the exception bridge, and the sandbox/
> pointer boundary, each splitting JIT side (host-proven) vs AROS side (to wire).
> Read it before wiring the AROS side; this design.md is the rationale behind it.

## What & why

Hosted AROS on Apple Silicon can already load and run AROS's own *native* AArch64
modules (exec.library, kernel.resource — see NOTES.md, "Hosted AROS BOOTS").
What it cannot do is run a real classic Amiga binary — a 68k hunk executable off
an ADF or a Workbench disk — because there is no 68k CPU on this machine and AROS
has no 68k translator for AArch64. This feature is a host-side **68k→AArch64 JIT**:
a translator exposed as a host service so that LoadSeg can map a 68k executable and
exec/dos can run it at native ARM speed, with its library calls bridged back into
the real AArch64 AROS libraries. It is the standout, uniquely-Apple-Silicon payoff:
a MacBook Air running *Deluxe Paint* or *Lemmings* through AROS, fast, is exactly
the "loves AmigaOS + loves the MacBook + crazy project" thesis — and it leans
directly on the W^X / MAP_JIT executable-memory layer the boot push already flagged
as deferred for LoadSeg.

## Does it already exist?

**No — verified, not assumed.** There is no 68k JIT for AArch64 anywhere in the
upstream tree, and there is no JIT at all in this checkout.

- **Petunia does not exist here.** Petunia is AROS's historical x86 68k JIT (the
  "emulation"-flavour `emul.handler` / `m68k.library` path used by the
  i386/x86_64 hosted ports to run Amiga binaries). It is **absent** from
  `/Users/user/Source/aros-upstream`:

  ```
  $ grep -rli petunia --include='*.c' --include='*.h' --include='*.conf' .   # → (no output)
  $ find . -iname '*petunia*'                                                # → (no output)
  ```

  So even the x86 prior art we'd want to study isn't in this clone — design must be
  grounded on the load/dispatch contracts that *are* present, not on Petunia's code.

- **No translator / dynarec of any kind.** A sweep for translation infrastructure
  is empty:

  ```
  $ grep -rliE '(dynarec|dynamic.recompil|translat.*68k|68k.*translat|jit_compile|codegen)' rom/ arch/ workbench/libs
        # → (no output)
  $ find . -iname '*jit*'                                                    # → (no output)
  ```

- **What *does* exist for m68k is the opposite of a JIT.** `arch/.unmaintained/m68k-emul/`
  is **AROS compiled natively to m68k machine code, running on a Linux/m68k host** —
  the host CPU is a real 68k. Its `machine.h` says so outright: *"machine.h include
  file for Linux/m68k"* and *"Linux/m68k gcc has no register args capabilities"*
  (`arch/.unmaintained/m68k-emul/machine.h`). It is `.unmaintained`, it has no
  AArch64 anything, and it translates nothing — it relies on the host executing 68k
  directly. The other m68k tree, `arch/m68k-amiga/`, is the **native port for real
  Amiga hardware**. Neither helps a non-68k host run 68k code; both *are* 68k code.

- **MAP_JIT / pthread_jit_write_protect:** zero hits in upstream
  (`grep -rln MAP_JIT . → none`; `grep -rln pthread_jit_write_protect . → none`).
  The Apple-Silicon executable-memory layer this feature needs does not exist
  upstream either — it lives only in this project (`graft/`, see below).

- **The one in-tree way to launch a 68k app is *delegation*, not execution.**
  `workbench/libs/workbench/uae_integration.c` detects a 68k binary by its
  `0x000003f3` magic (`is_68k()`) and, *if an external Amiga emulator is
  running*, forwards a launch message to its execute port
  (`FindPort`); with no emulator present, the binary simply will
  not launch. That is whole-machine Amiga emulation handed to a **separate program**
  (a GPL emulator that emulates the chipset and runs its own ROM) — not AROS executing
  68k in its own address space. So on *no* host architecture does AROS itself run 68k
  code. This feature is the opposite model: an in-OS JIT that runs the 68k program as
  an AROS task, in AROS's own address space, with its library calls bridged into the
  real native AArch64 libraries (the AmigaOS 4 / MorphOS "Petunia" approach, which
  AROS has never had).

**External prior art (web-grounded, *not* in the AROS tree).** Two projects matter and
neither lives in `aros-upstream`:

- **Emu68** (`github.com/michalsc/Emu68`, MPL-2.0) — a real, fast, actively-developed
  **m68k→AArch64 JIT** (the engine behind PiStorm, "Amiga on a Raspberry Pi"). It is
  exactly the translator we'd otherwise write. The catch: it is **bare-metal** — it
  owns the whole machine (firmware on an SD card, its own MMU/exception vectors), with
  no hosted/library mode and no macOS/Apple-Silicon support. Not a drop-in, but its
  AArch64 emitter is battle-tested; the realistic move is to lift its translator *core*
  out of its bare-metal runtime and feed it our `MAP_JIT` executable memory + AROS
  library-call/trap hooks.
- **emumiga** (`github.com/moggen/emumiga`, LGPL-2.1) — an **in-OS** 68k engine for
  AROS (runs 68k as an AROS execution engine — the opposite of a separate-machine
  emulator). That is the integration shape we want, but it is effectively abandoned (2
  commits, XFree86-era), 32-bit-oriented, and unclear JIT-vs-interpreter. Useful as a
  *design reference* for the in-OS LoadSeg/library-dispatch plumbing, not as code.

So the space is **not greenfield**: Emu68 gives the translator, emumiga sketches the
AROS integration; the open question is marrying Emu68's core to AROS's in-OS model,
hosted under macOS via `MAP_JIT`. That decision is `[J0]` below.

**The precise gap for AArch64:** there is (1) no 68k translator *usable hosted inside
AROS on this Mac* — Emu68 exists but is bare-metal — and (2) no AArch64
executable-memory JIT layer here. We adapt or build the translator; we *reuse* the
real, present-and-correct AROS contracts for loading 68k hunks and for the 68k
register-arg library ABI (next section).

## Background: what AROS already has — it *loads* 68k, it can't *execute* it (grounded)

Loading ≠ executing, and the difference is the whole feature. The tree has the full,
real machinery for *getting a 68k binary into memory and relocated*, plus the exact ABI
a 68k caller uses to reach a library — but **nothing that executes the resulting 68k
instructions on a non-68k CPU**. On the m68k-native port the loaded code is simply
branched into (the CPU *is* a 68k — `arch/m68k-all/dos/callentry.S`); on AArch64 there
is no equivalent, and supplying it is exactly what this feature is. Note too that the
hunk loader is *arch-agnostic*: `rom/dos/internalloadseg.c` picks the loader by file
magic (`0x000003f3` → AOS, alongside `0x7f454c46` → ELF), so a 68k hunk can be loaded
and relocated on this Mac today — it just produces 68k code in RAM with nothing to run
it. These are the contracts the JIT must honour.

**1. Loading: the AmigaOS hunk format.** Classic Amiga executables are hunk files,
not ELF. The loader is `rom/dos/internalloadseg_aos.c` (`InternalLoadSeg_AOS`),
selected by `rom/dos/internalloadseg.c`. Hunk type constants are in
`compiler/include/dos/doshunks.h`: `HUNK_HEADER 1011`, `HUNK_CODE 1001`,
`HUNK_DATA 1002`, `HUNK_BSS 1003`, `HUNK_RELOC32 1004`, `HUNK_END 1010`, etc. The
loader allocates one buffer per hunk and chains them as a **seglist**: each hunk is
prefixed with a `BPTR` to the next, and the segment payload starts at
`BADDR(seg) + sizeof(BPTR)` (`GETHUNKPTR` macro, `internalloadseg_aos.c`).

**2. Relocation: HUNK_RELOC32, absolute.** `internalloadseg_aos.c` handles
`HUNK_RELOC32` (line ~292): for each (target-hunk, offset) pair it reads the 32-bit
big-endian value already at that offset, adds the runtime base of the target hunk,
and writes it back big-endian (`val = AROS_BE2LONG(*addr) + (IPTR)GETHUNKPTR(target)`).
`HUNK_RELOC32SHORT`/`HUNK_RELRELOC32`/`HUNK_ABSRELOC16` are also handled; `HUNK_RELOC16`/
`HUNK_RELOC8` are explicitly *not implemented* (it `bug()`s). Note: relocations are
32-bit and **big-endian** — both facts matter to the JIT (the 68k address space is a
32-bit big-endian sandbox).

**3. Entering loaded code.** A seglist is run via `rom/dos/runcommand.c`
(`RunCommand`): it computes the entry as `(IPTR)BADDR(segList) + sizeof(BPTR)`
(runcommand.c line ~134), sets up `args.Args[]`, and `NewStackSwap`s into
`CallEntry`. On m68k the actual entry thunk is `arch/m68k-all/dos/callentry.S`
(`AOS_CallEntry`), which receives the entry pointer in register **A4**
(`AROS_UFHA(LONG_FUNC, entry, A4)`) and jumps to it. **This `CallEntry`/seglist-entry
point is the single hook the JIT replaces:** instead of branching native, it hands
the entry address to the translator.

**4. The 68k library ABI: negative-offset jump table + register args.** A 68k caller
reaches library function *n* through a jump vector at a **negative** offset from the
library base. `arch/m68k-all/include/aros/cpu.h`:
- `struct JumpVec { unsigned short jmp; void *vec; }` — a 6-byte entry.
- `__AROS_ASMJMP = 0x4EF9` — the m68k opcode for `jmp <abs.l>`.
- `__AROS_GETJUMPVEC(lib,n) = (&((struct JumpVec*)(lib))[-(n)])` — vector *n* sits at
  `lib - n*6`.
- `__AROS_USE_FULLJMP` is **defined** for m68k (real executable jump code in the
  vector), in contrast to AArch64/x86_64 where vectors are data pointers
  (`arch/x86_64-all/include/aros/cpu.h`; and NOTES.md H8: "64-bit AROS library
  vectors are data pointers").

  The table is built by `rom/exec/makefunctions.c` (`MakeFunctions`) via
  `__AROS_INITVEC` / `__AROS_SETVECADDR`, called from `rom/exec/makelibrary.c`
  (`MakeLibrary`).

- Arguments arrive in **68k registers** d0–d7 / a0–a6. The portable contract is the
  `AROS_LH*` / `AROS_UFH*` macro family: a function declares each argument's register
  with `AROS_LHA(type, name, reg)` (`compiler/arossupport/include/libcall.h`,
  `__AROS_LHA`) or `AROS_UFHA(type, name, reg)` for hook/user functions
  (`compiler/arossupport/include/asmcall.h`, e.g.
  `AROS_UFHA(struct Hook *, h, A0)`). This register map (`A0`, `D0`, …) is the exact
  data the JIT's library-call thunk needs to marshal a 68k call into a native
  AArch64 `AROS_LH` call.

So the project already owns: a correct hunk loader, a correct relocator, a single
seglist-entry hook, and a fully-specified 68k register ABI. What's missing is the
thing in the middle — translation — plus the Apple-Silicon executable memory to put
it in.

## Design

### Host side (macOS / Apple Silicon)

The translator is a **host service**, consistent with the project thesis ("macOS
owns the drivers; AROS reaches them via standard exec I/O"). It does the one thing
AROS code cannot do on this OS: produce and run native machine code.

- **Executable memory under W^X.** Apple Silicon refuses a one-shot RWX anon mmap
  (the boot push hit this: "W^X refuses a one-shot RWX anon mmap even with JIT
  entitlements" — NOTES.md; the AROS RAM pool is mapped `RW`, and "executing code
  *loaded into* the pool (LoadSeg) needs the W^X-aware path later"). The JIT code
  cache is a separate region: `mmap(..., PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_ANON|MAP_JIT, ...)`,
  written only inside a `pthread_jit_write_protect_np(0)` … `pthread_jit_write_protect_np(1)`
  window, followed by `sys_icache_invalidate()` over the patched range before
  execution (I-cache/D-cache are not coherent on arm64 — see Risks). The process
  must carry the `com.apple.security.cs.allow-jit` entitlement; signing is a build
  step, verified in the loop, not a manual approval. **UNVERIFIED:** exact entitlement
  string and whether the existing run.sh ad-hoc codesign suffices — to be pinned in
  spike [J1].
- **Translation unit = basic block.** Translate one 68k basic block at a time
  (linear opcodes up to a branch/jump/return), cache it keyed by 68k PC, and chain
  blocks. A full ahead-of-time recompile of a hunk is out of scope; block-at-a-time
  is the standard dynarec shape and keeps each spike small and verifiable.
- **The translator itself is plain C** (a 68k decoder + an AArch64 emitter), built
  with the host toolchain. It is *not* AROS code and does not go through AROS's
  crosstools — it is the host-side peer of `hosted/display.c` (ImageIO) or
  `hosted/abishim.S`: native macOS code that AROS reaches through a defined boundary.

### AROS side

AROS keeps owning *policy*; the host owns the *translation mechanism*.

- **Dispatch at the seglist-entry hook.** LoadSeg still uses the real
  `InternalLoadSeg_AOS` loader (it already produces a relocated 68k seglist — we do
  not reimplement it). The change is at the entry: where `RunCommand` /`CallEntry`
  would branch to `BADDR(segList)+sizeof(BPTR)` natively (runcommand.c,
  callentry.S), the AArch64 host instead recognises a 68k seglist and hands the
  entry PC to the translator. **UNVERIFIED:** how a 68k seglist is *tagged* as 68k
  vs native AArch64 — likely a flag set by the loader path, or a magic in the
  hunk header; to be decided in the design of spike [J4]. The 68k code, its data
  and BSS hunks live in a 32-bit big-endian sandbox region (the relocator already
  produced 32-bit big-endian pointers).
- **The LVO-call bridge.** When translated 68k code performs a library call — it
  computes `libbase - n*6` and jumps to the `0x4EF9` vector (`arch/m68k-all/include/aros/cpu.h`)
  — the translator does **not** emit a native jump into a fake 68k vector. Instead it
  recognises the negative-offset call, reads the 68k registers (d0–d7/a0–a6) from the
  emulated 68k state, and marshals them into a native `AROS_LH` call on the *real*
  AArch64 library, using the per-function register map declared by
  `AROS_LHA(type,name,reg)` (libcall.h) / `AROS_UFHA` (asmcall.h). The return value
  goes back into the 68k d0. This is the same kind of cross-ABI marshalling the
  project already wrote by hand for the host-call boundary (`hosted/abishim.S`, H3) —
  here it runs the other direction (68k regs → AArch64 AROS_LH) and is data-driven by
  the FD/register table, not the variadic-stack ABI.

### The bridge

68k execution reaches host and native services strictly through the existing exec
contracts, never by calling macOS directly:

- **68k → native AROS library:** via the LVO-call bridge above — a translated 68k
  call to, say, `exec.library/AllocMem` (LVO -198) lands in the native AArch64
  `AllocMem` with d0/d1 marshalled to AArch64 args. The native library then does its
  normal thing (the H5 allocator, etc.).
- **Native AROS → host (macOS) service:** unchanged — those libraries already reach
  the host through `hostlib.resource` and the host-call shim (NOTES.md H3; the
  hosted boot uses `hostlib.resource`). So a 68k program that draws on screen ends up
  going 68k→(JIT bridge)→native graphics.library→host display driver
  (`hosted/display.c`, render-to-PNG) — three defined boundaries, no shortcuts.
- **Memory:** the 68k sandbox is plain AROS memory (`AllocMem` from the H5 pool),
  mapped RW like the rest of the pool; only the JIT *code cache* is the special
  MAP_JIT region. 68k data and AArch64 code are kept in separate maps, which is also
  what W^X wants.

## Plan — spikes in the loop

Each spike is a standalone binary that prints a unique marker and yields one
PASS/FAIL, exactly like H1–H12. Ordered smallest-risk-first; the first spike is the
executable-memory layer because it is the load-bearing Apple-Silicon unknown.

- **[J0] Build-vs-adapt decision (a spike of *reading*, not running). RESOLVED → ADAPT
  Emu68's core** (see [spec.md](spec.md)). Grounded on a file-level read of Emu68 master:
  (a) the AArch64 **emitter** `include/A64.h` is a self-contained header of inline
  instruction-encoders with no MMU/MMIO/cache coupling, and the per-opcode decoders
  (`src/M68k_LINE*.c`, EA/CC/SR, `M68k_Translator.c`/`ExecutionLoop.c`) are portable —
  only the bare-metal runtime (`src/aarch64/{start,vectors,mmu}.c`, `src/raspi`,
  `src/pistorm`, the `src/support.c` cache asm) must be *re-hosted*, not lifted;
  (b) MPL-2.0 is *file-level* copyleft, so Emu68 files in a quarantined dir (MPL headers
  kept) + an own-authored C-ABI header keep the copyleft contained and nothing in AROS
  becomes MPL (Exhibit-B check pending); (c) every translated block exits via `RET` to a
  central dispatcher `MainLoop` with the m68k PC in **x18** and state flushed to
  `struct M68KState` — a single funnel our LVO bridge + scheduler intercept, no
  block-chaining to patch. Building fresh would discard a battle-tested, ISA-complete
  emitter for no licence gain. So **[J2] is "adapt Emu68"** — verify the MAP_JIT host of
  its emitter against Emu68's own `M68K_TranslateNoCache` output.

- **[J1] MAP_JIT executable-memory layer.** `mmap` a `MAP_JIT` region; inside a
  `pthread_jit_write_protect_np(0/1)` window, write a hand-assembled AArch64 stub
  that returns a constant (e.g. `mov w0,#0x6804; ret`); `sys_icache_invalidate`;
  call it through a function pointer. **PASS:** the call returns `0x6804`. **FAIL:**
  SIGBUS/SIGSEGV/EXC_BAD_ACCESS, or wrong value (stale I-cache). Also records the
  exact entitlement/codesign incantation needed, resolving the UNVERIFIED note above.
  This is the deferred-LoadSeg wall, retired.

- **[J2] One-basic-block translator vs a reference.** Translate a tiny fixed 68k
  block (e.g. `moveq #5,d0; addq #3,d0; rts` → 0x700503080x...; hand-encoded bytes)
  to AArch64 into the [J1] cache and run it. **PASS:** the emulated 68k d0 ends == 8,
  *and* matches a reference. Reference = a bundled 68k interpreter step-function
  (Musashi is the obvious choice as a host library if vendoring is acceptable —
  **UNVERIFIED:** whether to vendor Musashi or write a minimal interpreter; decide in
  [J2]); the test runs both and asserts identical d0–d7/a0–a7/PC/CCR. **FAIL:** any
  register diverges from the reference.

- **[J3] Library-call thunk (the LVO bridge).** Translate a 68k block that performs a
  negative-offset library call into a *stub* AArch64 library whose one LVO records its
  arguments. Use a real register-arg declaration (`AROS_UFHA(ULONG, x, D0)`,
  `AROS_UFHA(APTR, p, A0)`) so the marshalling is exercised against the genuine ABI
  macros (asmcall.h / libcall.h). **PASS:** the stub sees the exact (D0,A0) values the
  68k block set, and the 68k d0 receives the stub's return. **FAIL:** wrong/garbled
  args or return — i.e. the register map is mismarshalled.

- **[J4] A real hunk binary, end to end.** Take a tiny real 68k hunk executable
  (assembled offline: load some constants, call one library LVO, return an exit code),
  feed it through the real `InternalLoadSeg_AOS` path
  (`rom/dos/internalloadseg_aos.c`, HUNK_RELOC32 relocation) to produce a relocated
  seglist, then dispatch the entry through the JIT instead of a native branch.
  **PASS:** the program's chosen exit code appears in 68k d0 *and* the library LVO it
  called was observed (marker from the stub). **FAIL:** load error, relocation error,
  bad entry, or wrong exit code. This proves load → relocate → translate → bridge →
  return as one chain.

## How we verify it unattended

No TCC, no Screen-Recording, no manual click anywhere — all observation is markers,
files, and register traces the agent reads back:

- **Serial markers:** each spike prints `[J1]`…`[J4]` with PASS/FAIL, greppable by
  the harness exactly like `[H1]`…`[H12]`.
- **Register-state diff against a reference** ([J2], the core correctness claim): run
  the JIT block and the interpreter reference over the same 68k input and assert
  d0–d7/a0–a7/PC/CCR are bit-identical. The verdict is the diff being empty, printed
  as a single marker line — not "it ran". This is the same "compare to an
  authoritative reference" discipline used throughout (ground it, don't dream it).
- **Argument/return capture** ([J3]): the stub library writes the args it received to
  a known buffer the test reads back, so the bridge is asserted on values, not on "no
  crash".
- **Exit-code readback** ([J4]): the real-hunk run's final 68k d0 is printed and
  compared to the known-good value baked into the test binary; the library-call marker
  must also be present.
- **No-crash is necessary but never sufficient:** every PASS asserts a *value*
  (returned constant, matching register set, observed args, exit code), so a silent
  mistranslation cannot pass — mirroring H7's "a file exists isn't a PASS, the pixels
  are".

## Risks & open questions

Honest debt — none of this is free:

- **Self-modifying code / dirty pages.** Amiga code that writes into its own code
  hunks (decompressors, copy-protection, overlays via `HUNK_OVERLAY 1013`) invalidates
  cached translations. Need write-protection or dirty-tracking on 68k code pages to
  flush the affected blocks. Out of scope for [J1]–[J4]; flagged as the first thing
  beyond a clean run.
- **JIT cache flushing / coherency.** Apple Silicon I-cache and D-cache are not
  coherent: after emitting a block we must `sys_icache_invalidate` the exact range, and
  the `pthread_jit_write_protect_np` toggle is **per-thread** — a multithreaded AROS
  scheduler dispatching translated code on different host-anchor threads needs the
  toggle on the right thread. The H4/H6 work assumed a single underlying thread; the
  JIT may break that assumption. **UNVERIFIED** interaction with the hosted scheduler.
- **FP/NEON and the 68881/68882.** Translating 68k FPU opcodes to AArch64 NEON/VFP is
  a separable, large sub-feature; integer-only is the [J1]–[J4] scope. The host NEON
  state is already modelled (`graft/cpu_aarch64.h`, `_STRUCT_ARM_NEON_STATE64`,
  `AARCH64_FPU_SIZE`), so the plumbing exists, but 68k↔IEEE FP semantics are work.
- **Exceptions / traps.** 68k TRAP, illegal-instruction, divide-by-zero, and the
  CCR/SR semantics must be reproduced; a host SIGSEGV during translated code must map
  back to a 68k exception, not to AROS's native Guru. The project already has a
  host-signal→AROS-trap bridge (`graft/cpu_aarch64.h` SAVEREGS/RESTOREREGS), but it
  targets AArch64 ExceptionContext, not a 68k frame — a second mapping layer is needed.
- **Endianness / address width.** 68k is 32-bit big-endian; all loads/stores the
  translator emits must byte-swap and stay inside the 32-bit sandbox. Easy to get
  subtly wrong; the [J2] reference diff is the guard.
- **Entitlement / codesign.** `com.apple.security.cs.allow-jit` and a valid signature
  are required for MAP_JIT; whether the current ad-hoc-signed run.sh path is enough is
  **UNVERIFIED** and is exactly what [J1] pins down before anything else is built.
- **Engine: adapt Emu68 vs. write fresh (the [J0] call).** Petunia is irrelevant here
  (it is AmigaOS 4 / PowerPC, not an AROS component — confirmed on the web). The real
  reference is **Emu68**, a proven m68k→AArch64 JIT — but it is bare-metal and MPL-2.0,
  so adopting it means extracting its core from a runtime that assumes it owns the
  machine, and accepting file-level copyleft on those files. Writing fresh avoids both
  but discards a battle-tested emitter. This is the biggest upfront unknown; [J0]
  resolves it before any translation code is written.

## References

Every cited path is under `/Users/user/Source/aros-upstream` unless noted.

- `arch/.unmaintained/m68k-emul/machine.h` — proves m68k-emul is AROS-compiled-to-m68k
  on a Linux/m68k host, not a translator ("machine.h include file for Linux/m68k").
- `arch/.unmaintained/m68k-emul/` — the unmaintained hosted m68k port (no AArch64, no JIT).
- `arch/m68k-amiga/` — the native m68k port for real Amiga hardware.
- `arch/m68k-all/include/aros/cpu.h` — 68k LVO jump table: `struct JumpVec`,
  `__AROS_ASMJMP 0x4EF9`, `__AROS_GETJUMPVEC`, `__AROS_USE_FULLJMP`, `__AROS_INITVEC`.
- `arch/x86_64-all/include/aros/cpu.h` — 64-bit vectors are data pointers (contrast).
- `compiler/include/dos/doshunks.h` — hunk format constants (`HUNK_HEADER/CODE/DATA/BSS/
  RELOC32/RELOC32SHORT/RELRELOC32/END/OVERLAY`).
- `rom/dos/internalloadseg_aos.c` — the AmigaOS hunk loader + HUNK_RELOC32 absolute
  relocation; `GETHUNKPTR` seglist payload at `BADDR(seg)+sizeof(BPTR)`.
- `rom/dos/internalloadseg.c` — loader dispatch (AOS vs ELF).
- `rom/dos/runcommand.c` — `RunCommand`: entry = `BADDR(segList)+sizeof(BPTR)`,
  `NewStackSwap` into `CallEntry` (the seglist-entry hook).
- `arch/m68k-all/dos/callentry.S` — `AOS_CallEntry`, entry in A4
  (`AROS_UFHA(LONG_FUNC, entry, A4)`).
- `rom/exec/makefunctions.c` — `MakeFunctions` builds the jump table.
- `rom/exec/makelibrary.c` — `MakeLibrary` allocates negative vectors and calls MakeFunctions.
- `compiler/arossupport/include/libcall.h` — `AROS_LH*` / `__AROS_LHA(type,name,reg)`
  register-arg declarations for library functions.
- `compiler/arossupport/include/asmcall.h` — `AROS_UFH*` / `AROS_UFHA(type,name,reg)`
  (e.g. `AROS_UFHA(struct Hook *, h, A0)`) register-arg hook/user functions.

Working-repo references (`/Users/user/Source/aros-aarch64`):
- `NOTES.md` — H3 host-call ABI shim; H5 allocator; H7 render-to-PNG display; H8
  64-bit vectors are data pointers; the boot push's W^X / MAP_JIT note ("executing code
  loaded into the pool (LoadSeg) needs the W^X-aware path later").
- `graft/cpu_aarch64.h` — host-signal→AROS-trap bridge, `_STRUCT_ARM_NEON_STATE64`,
  SAVEREGS/RESTOREREGS, the host CPU-context boundary.
- `graft/cpucontext-aarch64.h` — corrected AArch64 `struct ExceptionContext`.
- `hosted/abishim.S` / `hosted/abishim.c` — the existing cross-ABI marshaller (H3),
  the model for the LVO-call bridge.
- `hosted/display.c` — host display as a service (render-to-PNG), the unattended-
  observation precedent.

External prior art (web, not in the AROS tree):
- `github.com/michalsc/Emu68` — bare-metal m68k→AArch64 JIT, MPL-2.0 (the PiStorm
  engine). The translator we'd otherwise write; the [J0] adapt-vs-build candidate.
- `github.com/moggen/emumiga` — abandoned in-OS 68k engine for AROS, LGPL-2.1; a
  design reference for the AROS-integration shape, not usable code.
- AROS in-tree delegation (for contrast): `workbench/libs/workbench/uae_integration.c`
  — hands 68k binaries to an external Amiga emulator; not in-OS execution.
