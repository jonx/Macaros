# Native modules — disk-loadable AArch64 AROS code under W^X executable memory

> Status: started (partial) - the native W^X LoadSeg path is largely landed in the AROS tree (commit 71f75760); darwin verify-and-reconcile pending · Target: aarch64-darwin hosted · Drafted 2026-06-28

> **The W^X executable-memory layer is a SHARED foundation with the 68k JIT.** Its
> host primitives + the per-thread toggle discipline are owned by `[J1]` in
> [../68k-jit/design.md](../68k-jit/design.md), and the routing seam is frozen in
> [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2. This doc covers the
> **native (non-translated) module-loading path** that sits on top of that layer —
> the JIT loads *translated* 68k code into the W^X region, native `LoadSeg` loads
> *native aarch64* ELF code into it. Where the memory layer is already specified by
> `[J1]`, we cross-reference and defer; we do not restate or change it.

## What & why

Hosted AROS on Apple Silicon can already load and run AROS's own *native* AArch64
modules **that are baked into the kickstart** (exec.library, kernel.resource — see
`graft/CONTINUATION.md`, "Hosted AROS BOOTS"). What this feature is about is the
*disk-loadable* path: taking a natively-compiled AArch64 AROS module **off disk** —
`DEVS:Printers/<name>`, `DEVS:Midi/<name>`, an AHI sub-driver, `LIBS:<x>.library` — via
`dos.library` `LoadSeg`, mapping its code sections into **Apple-Silicon W^X executable
memory**, applying the AArch64 ELF relocations, flushing the instruction cache, and
running it.

This is a **foundation**, not a leaf feature. It is the real dependency under the
printer, MIDI, fonts and audio-sub-driver work: those features all ship as loadable
modules, and on this target a loaded module's code lands in pages that — by default —
are **not executable** (the AROS RAM pool is mapped R/W only; Apple Silicon refuses a
one-shot RWX mapping). Until the loaded code is in correctly-mapped W^X-aware executable
memory and the I-cache is made coherent, those drivers *compile* but cannot *run*. The
printer case makes it vivid: the printer driver's magic word (`pmh_Magic`,
`compiler/include/aros/printertag.h`) is the **first instruction executed** in a loaded
driver — it depends entirely on the loaded code being executable.

It is the native sibling of the `MAP_JIT` work the boot push already flagged as
deferred for `LoadSeg`: `graft/CONTINUATION.md` records that "executing code loaded
*into* the pool via LoadSeg needs the W^X-aware path", and the port inventory
(`darwin-aarch64-port-inventory.md` §2, §9) lists the "W^X / `MAP_JIT` executable-memory
policy" / "MAP_JIT/W^X allocation layer" as the deferred item LoadSeg needs.

## Does it already exist?

**Yes — far more than the JIT doc's "deferred wall" framing implies. This must be
stated honestly:** a substantial native-LoadSeg W^X path is *already implemented in this
project's `aros-upstream` checkout* (the work-tree, not stock AROS), and the design
below is largely about *verifying* and *reconciling* it, not building it from nothing.
Concretely, all `../aros-upstream` and attributed to this project's
commits `71f75760` / the all-unix kernel additions:

- **The native ELF loader already requests executable memory.**
  `rom/dos/internalloadseg_elf.c:257-260` tags any `SHF_EXECINSTR` section with
  `MEMF_EXECUTABLE` and allocates the hunk with it:
  `if (sh->flags & SHF_EXECINSTR) memflags |= MEMF_EXECUTABLE;` then
  `hunk = ilsAllocMem(hunk_size, memflags | MEMF_PUBLIC | …)`.
- **`LoadSeg`'s `AllocFunc` routes executable allocations to host pages.**
  `rom/dos/loadseg.c:47-76` (`AllocFunc`, = `funcarray[1]`): for
  `flags & MEMF_EXECUTABLE` it `OpenResource("kernel.resource")` and calls
  `KrnAllocPages(NULL, length, flags)`, falling back to `AllocMem` if the resource is
  absent. `FreeFunc` mirrors it: `TypeOfMem(buffer)` ? `FreeMem` : `KrnFreePages`
  (`loadseg.c:78-103`). This is *required* because exec's allocator does **not** honour
  `MEMF_EXECUTABLE` specially — it is masked out of `MEMF_PHYSICAL_MASK`
  (`compiler/include/exec/memory.h:102-106`) before the MemHeader-attribute compare
  (`rom/exec/memory_nommu.c:24,37`; `rom/exec/memory.c:959,1016`), so a plain R/W pool
  satisfies it. The dedicated host-page route is the only way to get executable backing.
- **The hosted kernel.resource backs those pages with `mmap`/`mprotect`.**
  `arch/all-unix/kernel/allocpages.c` (`KrnAllocPages`, LVO 27) `mmap`s **R/W only**
  (`allocpages.c:66,79` — `prot = PROT_READ | PROT_WRITE`); `(void)flags` (the MEMF
  flags are intent-only). `arch/all-unix/kernel/setprotection.c` (`KrnSetProtection`,
  LVO 21) maps `MAP_Readable/Writable/Executable` → `PROT_READ/WRITE/EXEC` and calls the
  host `mprotect` (`setprotection.c:25-32`). `arch/all-unix/kernel/freepages.c` →
  `munmap`.
- **The loader already flips populated exec hunks to R/X.**
  `internalloadseg_elf.c:1235-1252`: after all sections are loaded and relocated, for
  each hunk with `!TypeOfMem(hunk)` (i.e. a page-allocated hunk outside any MemHeader)
  it calls `KrnSetProtection(hunk, hunk->size, MAP_Readable | MAP_Executable)`.
- **The AArch64 ELF relocations are implemented.**
  `internalloadseg_elf.c:756-824` handles `R_AARCH64_ABS64/ABS32/PREL64/PREL32`, the
  `MOVW_UABS_G0..G3` movz/movk family, `ADR_PREL_PG_HI21` (ADRP), the
  `ADD/LDST*_ABS_LO12_NC` imm12 family, `JUMP26`/`CALL26` (b/bl), and `NONE`, with the
  AArch64 reloc-type constants defined locally (`:306-328`).

**So the load + relocate + populate-then-protect chain is present.** What is *not*
proven, *not* reconciled, or genuinely *gappy*:

1. **The cache-maintenance is effectively a no-op on this target.** `DOCACHECLEAR` is
   defined for all archs (`rom/dos/mmakefile.src:24`), and the loader calls
   `ils_ClearCache(hunk->data, hunk->size, CACRF_ClearD | CACRF_ClearI)`
   (`internalloadseg_elf.c:1223`) → `CacheClearE` (`internalloadseg_support.c:124-128`).
   But **there is no hosted/aarch64 `CacheClearE`** (no `cachecleare.c` under
   `arch/all-unix`/`all-hosted`/`all-darwin`; the arch-specific ones are ppc/arm-native/
   i386/m68k/riscv only), so it resolves to the **generic empty stub**
   (`rom/exec/cachecleare.c:72-77`, "Replace when needed"). On AArch64 the I-cache and
   D-cache are **not coherent for freshly-written code** — this needs a real
   `IC IVAU`/`DC CVAU`/`DSB`/`ISB` (or `__builtin___clear_cache`) or it can fetch stale
   instructions (a **wrong value / crash**, not a clean failure). Today coherency relies
   *implicitly* on the `mprotect` R/W→R/X transition; whether that is sufficient on
   Apple Silicon is **UNVERIFIED** and is the sharpest correctness question (see Risks).
2. **It uses a DIFFERENT W^X strategy than `[J1]`.** The native path is
   **populate-R/W-then-`mprotect`-R/X** under the
   `com.apple.security.cs.allow-unsigned-executable-memory` /
   `disable-executable-page-protection` entitlements (`allocpages.c:11-16` says so
   outright). The 68k JIT's `[J1]` uses **`MAP_JIT` + `pthread_jit_write_protect_np`**
   under `com.apple.security.cs.allow-jit` (`hosted/jit68k/jit_region.c`). These are two
   different, both-valid Apple W^X routes. The host entitlements plist
   (`graft/aros-host.entitlements.plist`) already carries **all** the keys, so both run.
   But "shared foundation" currently means *shared need*, not *shared code* — and that
   divergence is a real design decision to settle (see Design / Risks).
3. **Nothing is value-asserted in the loop.** None of the above has a greppable
   PASS/FAIL spike. The whole point of this doc's `[NL*]` plan is to prove, unattended,
   that a host stub round-trips through the W^X layer, that a tiny native module
   `LoadSeg`s + runs + returns a known value, and that a real loadable driver works.
4. **It can't yet run end-to-end** because the boot still halts before `dos.library` is
   up (`graft/WORKFLOW.md` F1/F2); the bare-process spikes (`[NL1]`/`[NL2]`) do not need
   the boot, the in-AROS ones (`[NL3]`+) ride the graft.

**External prior art (web-grounded, *not* in the AROS tree).** The ELF object format
and the AArch64 ELF relocation set (`R_AARCH64_*`) are published — the *AArch64 ELF
ABI* (ARM IHI 0056) and the System V ELF spec; the cache-maintenance requirement (after
writing instructions, `IC IVAU` per line, `DSB ISH`, then `ISB`) is the *Arm
Architecture Reference Manual* (the "B2.4 / cache maintenance" instructions). Apple's
W^X rules — `MAP_JIT` pages toggled per-thread with `pthread_jit_write_protect_np`, the
`com.apple.security.cs.allow-jit` entitlement, the alternative
`allow-unsigned-executable-memory` route, and the empirical fact that a one-shot RWX
anon `mmap` is refused even with the entitlement — are Apple developer documentation +
this project's own boot-push observation (`arch/all-unix/bootstrap/memory.c:57-66`).
None of this is third-party *implementation* source.

## Background: the AROS native-module contracts (grounded)

Loading a native module on this target is four steps, each grounded in a real file. The
loader is **arch-agnostic at the top** and **arch-specific in the relocations + the
W^X/cache plumbing** — the latter is what this feature owns.

### 1. Loader dispatch — picked by file magic `[AROS]`

`rom/dos/internalloadseg.c` reads the first longword and dispatches by magic
(`internalloadseg.c:85-115`):

```c
static const segfunc_t funcs[] = {
    SEGFUNC(0x7f454c46, ELF),   /* "\x7fELF" — native AArch64 objects (THIS feature) */
    SEGFUNC(0x000003f3, AOS)    /* AmigaOS hunk — 68k code (the JIT's path) */
};
```

A natively-built AArch64 module is an **ELF** object (`0x7f454c46`), so it lands in
`InternalLoadSeg_ELF` (`internalloadseg_elf.c`). A 68k hunk (`0x000003f3`) lands in
`InternalLoadSeg_AOS` — the JIT's loader. **The two loaders are disjoint, which keeps
this feature and the JIT cleanly separated:** the AOS loader allocates with
`MEMF_31BIT` and **never sets `MEMF_EXECUTABLE`** (`internalloadseg_aos.c:181-207`;
`grep -c MEMF_EXECUTABLE` → 0) because 68k code is never natively executed — it goes to
the translator. So **only the ELF loader requests executable memory**, and only it needs
the W^X/cache machinery this doc covers.

### 2. Section allocation — `MEMF_EXECUTABLE` → `KrnAllocPages` `[AROS]`

`load_hunk` (`internalloadseg_elf.c:205-304`) allocates one hunk per loadable section.
The executable-memory decision is `:257-260`:

```c
/* Executable code must come from executable memory (matters on W^X hosts). */
if (sh->flags & SHF_EXECINSTR)
    memflags |= MEMF_EXECUTABLE;
hunk = ilsAllocMem(hunk_size, memflags | MEMF_PUBLIC | (sh->type == SHT_NOBITS ? MEMF_CLEAR : 0));
```

`ilsAllocMem` (`internalloadseg.h:39-46`) calls `funcarray[1]` — under `LoadSeg` that is
`AllocFunc` (`loadseg.c:148-153`), whose `MEMF_EXECUTABLE` branch calls
`KrnAllocPages(NULL, length, flags)` (`loadseg.c:61-73`). **This is load-bearing**
because exec's own allocator cannot serve executable memory: `MEMF_EXECUTABLE`
(bit 4, `exec/memory.h:69-70`, value `0x10`) is **not** in `MEMF_PHYSICAL_MASK`
(`exec/memory.h:102-106`), so it is masked off before the MemHeader-attribute match
(`memory_nommu.c:24,37`) — a plain R/W pool "satisfies" it, which would yield
**non-executable** backing. The `KrnAllocPages` detour is the whole point.

`KrnAllocPages` on this target (`arch/all-unix/kernel/allocpages.c:79`) `mmap`s the
region **R/W only** — the executable transition is deferred to a later
`KrnSetProtection` (step 4). The header comment states the rationale verbatim
(`allocpages.c:11-16`): "a page may not be writable and executable at the same time…
hand back *writable* memory… leave the transition to executable to a subsequent
`KrnSetProtection()` call (host `mprotect`)… With the process holding the
`com.apple.security.cs.allow-unsigned-executable-memory` entitlement, that R/W → R/X
flip is permitted without `MAP_JIT`." Data and BSS sections (no `SHF_EXECINSTR`) come
from the ordinary R/W `AllocMem` pool and stay writable.

### 3. AArch64 ELF relocations — applied while writable `[AROS]` + `[PUB]`

`relocate` (`internalloadseg_elf.c:331-840`) walks each `SHT_REL`/`SHT_RELA` section and
patches the target in place. The AArch64 cases (`:756-824`) implement the standard
AArch64 ELF relocation encodings `[PUB]` (ARM IHI 0056):

- absolute/PC-relative data: `R_AARCH64_ABS64/ABS32` (`*p = s + addend`),
  `PREL64/PREL32` (`s + addend − p`) (`:758-768`);
- `movz`/`movk` immediates `R_AARCH64_MOVW_UABS_G0..G3` — replace instruction bits 5-20
  with the 16-bit slice (`:772-786`);
- `R_AARCH64_ADR_PREL_PG_HI21` (ADRP) — the 21-bit page offset split into immlo
  (bits 29-30) / immhi (bits 5-23) (`:789-794`);
- `R_AARCH64_ADD_ABS_LO12_NC` + `LDST{8,16,32,64,128}_ABS_LO12_NC` — the imm12 field
  (bits 10-21), scaled by the access size (`:797-811`);
- `R_AARCH64_JUMP26`/`CALL26` (b/bl) — the 26-bit signed branch offset `>> 2`
  (`:815-821`); `R_AARCH64_NONE` (`:823`).

These run **while the hunk is still R/W** (before the step-4 flip), which is the correct
ordering: relocations are in-place writes into code, so they must precede the
write-protect. The relocation maths is a published-spec requirement (`[PUB]`); the
in-tree code is `[AROS]` and is reused unchanged by this feature.

### 4. The R/W → R/X flip — `KrnSetProtection` `[AROS]`

After load + relocate, `internalloadseg_elf.c:1235-1252` walks the seglist and flips the
populated executable hunks:

```c
struct KernelBase *KernelBase = OpenResource("kernel.resource");
if (KernelBase) {
    curr = hunks;
    while (curr) {
        struct hunk *hunk = BPTR2HUNK(BADDR(curr));
        BPTR next = hunk->next;
        if (!TypeOfMem(hunk))                                   /* page-allocated, not pool */
            KrnSetProtection(hunk, hunk->size, MAP_Readable | MAP_Executable);
        curr = next;
    }
}
```

`!TypeOfMem(hunk)` distinguishes a `KrnAllocPages` hunk (outside any MemHeader → `0`)
from a pooled data/bss hunk (`!= 0`, left writable). `KrnSetProtection`
(`arch/all-unix/kernel/setprotection.c:25-32`) issues the host `mprotect`. The
`MAP_Readable`/`MAP_Writable`/`MAP_Executable` flags are
`compiler/include/aros/kernel.h:35-37` (`0x100`/`0x200`/`0x400`).

### 5. The cache-maintenance step — the real gap `[PUB]` + `[OURS]`

The loader's cache-clear (`internalloadseg_elf.c:1215-1226`, `DOCACHECLEAR`) calls
`ils_ClearCache` → `CacheClearE`. **On hosted darwin-aarch64 `CacheClearE` is the
generic empty stub** (`rom/exec/cachecleare.c:72-77`; no arch override exists for this
target). Per the Arm ARM, after writing instructions to memory you must
`DC CVAU`/`IC IVAU` the affected lines + `DSB`/`ISB` before executing them, or the CPU
may fetch stale instructions `[PUB]`. The JIT's `[J1]` layer does exactly this via
`sys_icache_invalidate` (`hosted/jit68k/jit_region.c:76`). The native path currently
relies on the `mprotect` transition for coherency, which is **UNVERIFIED** as sufficient.
This is the part the `[NL*]` spikes must value-assert.

### Reference points already de-risked in this repo `[OURS]`

- **The `[J1]` W^X layer** — `hosted/jit68k/jit_region.{h,c}`: `MAP_JIT` alloc +
  `pthread_jit_write_protect_np` toggle + `sys_icache_invalidate`, green and entitlement-
  resolved. The shared substrate (frozen in [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2).
- **The kickstart's own populate-then-protect** — `arch/all-unix/bootstrap/memory.c`
  `AllocateRO`/`SetRO` (`mmap` R/W → `mprotect(R|X)`, `memory.c:78-92`) is the *same*
  strategy the native LoadSeg path uses, already proven for the RO kickstart region.
- **The host-call discipline** — `KrnAllocPages`/`KrnSetProtection` already use
  `AROS_HOST_BARRIER` after the host call (`allocpages.c:80`, `setprotection.c:33`),
  matching H3's host-call boundary.
- **The entitlement/codesign path** — `graft/aros-host.entitlements.plist` already
  carries `allow-jit`, `allow-unsigned-executable-memory`,
  `disable-executable-page-protection`; the signing step is in `graft/bootrun.sh:35-40`
  (`codesign -s - -f -o runtime --entitlements …`), `aros-ctl`, `run-window.sh`.

## Design

The work is **verification + reconciliation + closing the cache gap**, not a greenfield
build. AROS keeps owning *policy* (the loader, what is executable); the host owns the
*executable-memory mechanism*. The shared W^X layer is `[J1]`'s; this feature wires the
native ELF loader through it (or, equivalently, settles that the existing
`KrnAllocPages` + `KrnSetProtection` route *is* the native side of that layer).

### Host side (the shared W^X executable-memory substrate)

Two W^X strategies coexist in the tree today, and the first design decision is which one
the native LoadSeg path should use. **Both are valid Apple routes; they must not be
conflated, and the choice is the owner's.**

- **Strategy A — `mprotect` populate-then-protect (what the native path uses today).**
  `KrnAllocPages` `mmap`s R/W; the loader writes + relocates; `KrnSetProtection` flips
  R/X via `mprotect`. Needs `allow-unsigned-executable-memory` /
  `disable-executable-page-protection`. Simple, page-granular, persistent R/X (no
  per-thread toggle), and matches the kickstart's `AllocateRO`/`SetRO`. **This is the
  natural fit for LoadSeg'd modules** — a driver's code is written once at load and then
  executed many times from many tasks/threads; a persistent R/X mapping has no
  per-thread-toggle hazard. The cost: it relies on an entitlement Apple frames as a
  compatibility escape hatch.
- **Strategy B — `MAP_JIT` + `pthread_jit_write_protect_np` (what `[J1]`/the JIT uses).**
  A `MAP_JIT` region, writable only inside a per-thread toggle window, executable
  otherwise. Needs only `allow-jit`. This is Apple's *preferred* W^X path, but the
  **per-thread** toggle (`[J1]` R-JIT-THREAD, frozen in
  [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2) is a poor fit for *persistent*
  module code that any task may call: the page is only writable on the toggling thread,
  but it is executable on **all** threads once the window closes — so for write-once-
  execute-forever module code it works, but the toggle discipline buys nothing over a
  one-shot `mprotect`.

**Recommendation to evaluate (owner decides):** keep **Strategy A** for native LoadSeg
(persistent R/X module code is exactly its sweet spot; it already exists and is
grounded), and frame the "shared foundation" as a *shared host page-allocator contract*
— `KrnAllocPages` (R/W) + `KrnSetProtection` (R/X) — that *also* underlies what `[J1]`
needs for any future pool-resident translated code, rather than forcing native modules
through the JIT's per-thread `MAP_JIT` toggle. The two strategies remain **interface-
compatible at the AROS seam** (both yield executable hunks the loader can run), so the
choice is contained behind `kernel.resource`. **The `[J1]` `jit_region` layer is
unchanged by this decision** — this doc does not touch it. The deferral to `[J1]` is:
if/when the JIT and native loader must share *one* code region (e.g. a single executable
arena), that unification is the JIT track's call, made against
[../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2, not here.

### AROS side (the native ELF loader, wired through the W^X layer)

The AROS side is the loader chain of Background §2-§5, **reused as-is** — the design
work is to (a) prove it value-asserts, and (b) close the cache-maintenance gap:

- **Section allocation through the executable allocator.** Unchanged: `load_hunk`'s
  `MEMF_EXECUTABLE` → `AllocFunc` → `KrnAllocPages` path (`loadseg.c:61-73`). The
  *contract* this feature pins (in spec.md) is that **any executable section is backed by
  page-granular host memory that can later be flipped to R/X**, and data/bss stay in the
  R/W pool.
- **Relocate while writable, then flip.** Unchanged ordering: relocations
  (`relocate`, AArch64 cases `:756-824`) run before the `KrnSetProtection` flip
  (`:1235-1252`). The spec freezes this as a hard requirement: **no executable hunk may
  be flipped to R/X before all its relocations are applied**, and **no relocation may
  write into an already-R/X hunk** (it would fault).
- **Close the cache gap (the one genuinely-new code).** Insert an AArch64
  instruction-cache maintenance step over each executable hunk **after** relocation and
  **after/with** the R/X flip, before first execution. Two implementation options
  (decided in spec.md / `[NL1]`):
  1. provide a real hosted/aarch64 `CacheClearE` (so the existing
     `ils_ClearCache(...CACRF_ClearI)` call at `:1223` does the right thing) that calls
     `__builtin___clear_cache(start, end)` or `sys_icache_invalidate` on the host; or
  2. add the I-cache invalidate inside the `KrnSetProtection` flip path for
     `MAP_Executable` (so the flip is atomically "make R/X + sync I-cache").
  **UNVERIFIED:** whether the `mprotect` R/W→R/X transition already orders the I-cache
  sufficiently on Apple Silicon; `[NL1]` is designed to *prove or disprove* it by
  asserting the returned value (a stale I-cache yields a **wrong value**, the H/J
  discipline).

### The bridge (how the AROS loader reaches host executable memory)

Native module loading reaches the host strictly through the existing exec/kernel
contracts, never by calling macOS directly:

- **Loader → executable pages:** `LoadSeg` → `AllocFunc` → `KrnAllocPages`
  (`kernel.resource` LVO) → host `mmap`. The `kernel.resource` host implementation
  (`arch/all-unix/kernel/*`) reaches the host through its `KernelInterface` table
  (`iface->mmap`/`mprotect`/`munmap`, `kernel_intern.h:32-37`) — the same `hostlib`-style
  binding every hosted port uses; the per-thread toggle (Strategy B) would route via the
  frozen [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2 routing, **not** a new
  channel.
- **Loader → R/X flip:** `KrnSetProtection` (LVO) → host `mprotect`.
- **First execution:** the module's entry (a driver `ped_*` hook, a CAMD
  `OpenPort`, a library init, or the printer magic word executed at load) is reached by
  the normal AROS call path once the hunk is R/X + I-cache-coherent. **No 68k, no
  translation** — this is native AArch64, branched into directly (the opposite of the
  JIT's translated-entry divert in [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §1).

### The ROM-resident sidestep (the interim option) — and why the general fix matters

A driver/library **linked into the kickstart** is resident in the RO kickstart region
(mapped R/X by `AllocateRO`/`SetRO` at boot, `bootstrap/memory.c:78-92`) and needs **no
`LoadSeg` and no per-module W^X path at all** — it is already executable. This is the
interim escape hatch for an urgent dependent feature: e.g. a CoreMIDI or CUPS-sink driver
could be co-built into the kickstart (resident, `residentpri`) rather than loaded from
`DEVS:`, exactly as `unixio.hidd` is (`graft/CONTINUATION.md`, "added to the kickstart
conf, residentpri 91"). The MIDI doc flags precisely this fallback ("the driver may need
to be a resident/co-built module rather than a `DEVS:Midi/` file").

**Why the general fix still matters:** the resident sidestep does not scale — it bakes
every driver into one image, forecloses third-party / user-installed `DEVS:`/`LIBS:`
modules, breaks the AmigaOS model where drivers are drop-in files, and means a driver
update is a whole-kickstart rebuild. The whole point of AROS is a loadable module system.
So the resident path is the *bring-up unblock*; disk-loadable native modules
(this feature) are the *correct* end state. The two are not in tension — a feature can
ship resident first (`[NL5-resident]`) and move to `DEVS:` once `[NL3]`/`[NL4]` are green.

## Plan — spikes in the loop

Each spike is a standalone binary (or one booted-AROS assertion) that prints a unique
`[NL*]` marker and yields one PASS/FAIL the agent greps — exactly like the H/J/A series.
Ordered smallest-risk-first; the first spike is the W^X+cache round-trip because the
I-cache coherency is the load-bearing Apple-Silicon unknown. Every PASS asserts a
**value** (a returned constant, a known exit code), never "it didn't crash" — a stale
I-cache or a bad relocation yields a *wrong value*, which a no-crash test would miss.

- **[NL1] W^X round-trip, value-asserted (pure host, the shared-layer probe).** A bare
  host spike that exercises the **native** Strategy-A path end to end without AROS:
  `mmap` R/W a page (the `KrnAllocPages` shape), write a hand-assembled AArch64 stub that
  returns a constant (`movz w0, #0x6804 ; ret` — encoded from the architecture, not a
  magic literal, cf. `jit_region.h:73-76`), `mprotect(R|X)` (the `KrnSetProtection`
  shape), do the I-cache maintenance (`__builtin___clear_cache` / `sys_icache_invalidate`),
  then call it through a function pointer. **PASS:** the call returns `0x6804`. **FAIL:**
  SIGBUS/SIGSEGV/EXC_BAD_ACCESS, or a wrong value (stale I-cache — *this is the bit `[NL1]`
  exists to catch*). Also runs the **mprotect-without-explicit-icache** variant to
  *empirically settle* whether the R/W→R/X transition alone suffices on this CPU
  (resolves the §5 UNVERIFIED). Records the exact entitlement/codesign incantation needed.
  This is the deferred-LoadSeg W^X wall, retired — and it **cross-references `[J1]`**: it
  is the Strategy-A peer of `[J1]`'s `MAP_JIT` proof; where they overlap (the
  shared-substrate claim) it defers to [../68k-jit/design.md](../68k-jit/design.md) `[J1]`.

- **[NL2] AArch64 ELF relocation correctness, asserted (pure host).** Take a tiny native
  AArch64 ELF object assembled offline that uses a representative relocation set
  (an `ADRP`+`ADD_ABS_LO12_NC` to a data symbol, a `CALL26` to another function, an
  `ABS64` data pointer), apply the **same relocation maths** as
  `internalloadseg_elf.c:756-824` in a standalone harness, place the result in `[NL1]`'s
  executable page, and call the entry. **PASS:** the entry returns the value that is only
  correct if every relocation resolved (e.g. it reads a relocated global and calls a
  relocated helper, returning their sum). **FAIL:** wrong value (mis-encoded immediate /
  wrong page-offset split). Asserts the relocation encodings against a real toolchain's
  output, not against any reference implementation.

- **[NL3] `LoadSeg` a tiny native module + call its entry (booted AROS).** Build a
  minimal native AArch64 AROS module off disk (one code section that returns a known
  longword from a published entry), `LoadSeg` it through the real
  `InternalLoadSeg_ELF` → `AllocFunc`/`KrnAllocPages` → relocate → `KrnSetProtection`
  chain, then call the seglist entry. **PASS:** the entry returns the baked-in constant
  *and* (asserted via debug/`bug()` or a sentinel) the hunk was page-allocated
  (`TypeOfMem == 0`) and flipped R/X. **FAIL:** load error, relocation error, non-exec
  page (SIGSEGV on call), or wrong value. This proves load → page-alloc → relocate →
  flip → I-cache → execute as one chain on the real loader. *(Rides the graft; gated on
  the boot reaching `dos.library`, `graft/WORKFLOW.md` F2.)*

- **[NL4] a real loadable driver, loaded and used (booted AROS).** Take a real dependent
  module — the cheapest is the **printer** path once `[PR0]` lands the `__aarch64__`
  magic: `printer.device` `LoadSeg`s `DEVS:Printers/PostScript`, validates
  `ps_runAlert == AROS_PRINTER_MAGIC` (the magic word being the **first instruction
  executed** — the direct proof that loaded code is executable), and drives one
  `CMD_WRITE`. **PASS:** the driver loads, the magic validates, and a print produces the
  expected spool bytes (the printing doc's `[PR2]` oracle). **FAIL:** load/validate
  failure, or a crash entering the driver. Equivalently a `DEVS:Midi/coremidi` CAMD
  driver (the MIDI doc's `[MD5]`) or an AHI sub-driver (`[A5]`) — whichever dependent
  feature is ready first. This is the foundation proven by a feature that *uses* it.

- **[NL5-resident] (interim, parallel) the ROM-resident sidestep.** Co-build one
  dependent driver into the kickstart (resident, `residentpri`) so it runs **without**
  `LoadSeg`, unblocking that feature before `[NL3]`/`[NL4]` are green. **PASS:** the
  resident driver is reachable and works (same per-feature oracle as `[NL4]`). Not a
  W^X proof (resident code is already R/X via `SetRO`); it is the documented escape hatch,
  and the marker exists so the unblock is greppable and the migration to `DEVS:` is
  tracked.

Build/run in the existing harness style (`make hosted-native-modules` → `[NL1]`/`[NL2]`
host markers; `graft/native-modules-smoke` for the booted legs), clean-exit on PASS.

## How we verify it unattended

No TCC, no Screen-Recording, no manual step — all observation is markers and values the
agent reads back, the exact discipline of H7/J1/A1:

- **Returned-value assertions** (`[NL1]`/`[NL2]`/`[NL3]`): every PASS asserts a *constant*
  the called code returns, so a stale I-cache (wrong instruction fetched) or a bad
  relocation (wrong immediate) produces a **wrong value**, not a silent pass — mirroring
  `[J1]`'s "assert `0x6804`, because skipping the I-cache flush is a wrong value not a
  crash" (`jit_region.c:73-77`).
- **The cache-coherency negative control** (`[NL1]`): run the path **with** and
  **without** an explicit I-cache invalidate and compare — this *empirically settles* the
  §5 UNVERIFIED (does `mprotect` R/W→R/X alone order the I-cache on Apple Silicon?)
  instead of guessing.
- **Page-state assertions** (`[NL3]`): assert the executable hunk is page-allocated
  (`TypeOfMem(hunk) == 0`) and was flipped (a debug sentinel / `bug()` the loader already
  emits), so "it ran" is backed by "it ran *from an R/X page we allocated and flipped*".
- **Two-sided / feature oracle** (`[NL4]`): the dependent feature's own value oracle (the
  printing doc's DSC-PostScript assertion, MIDI's byte-exact loopback, audio's RMS/FFT)
  re-read on the host side — the H11 two-sided rule.
- **Markers** are unique per spike (`[NL1]`…`[NL5-resident]`); a hung load is reaped by
  the existing bash watchdog; `harness/run-hosted.sh` greps the marker and emits the
  uniform `result=(PASS|FAIL)` block.

## Risks & open questions

Honest debt — and most of the *mechanism* is already in-tree, so the risk is correctness
+ reconciliation, not absence:

- **I-cache coherency (the headline risk).** `CacheClearE` is a **no-op** on hosted
  darwin-aarch64 (`rom/exec/cachecleare.c:72-77`; no arch override), so freshly-loaded
  code's I-cache is invalidated only *implicitly* by the `mprotect` R/W→R/X transition.
  Whether that is sufficient on Apple Silicon is **UNVERIFIED** — if not, loaded code can
  execute stale instructions (a wrong-value / crash that is hard to localise). Mitigation
  = a real hosted/aarch64 I-cache step (`__builtin___clear_cache` / `sys_icache_invalidate`)
  and the `[NL1]` negative-control that proves the question either way. This is the first
  thing to settle.
- **Two W^X strategies in one tree.** Native LoadSeg uses `mprotect`-populate-then-protect
  (`allow-unsigned-executable-memory`); the JIT `[J1]` uses `MAP_JIT` +
  `pthread_jit_write_protect_np` (`allow-jit`). Both work, but "shared foundation" is
  currently *shared need*, not *shared code*. The Design recommends keeping Strategy A for
  persistent module code and deferring any unification to the JIT track
  ([../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2). **Open:** does the owner want
  one arena, or two routes behind `kernel.resource`? **UNVERIFIED** — owner decides.
- **Entitlement posture.** Strategy A leans on
  `com.apple.security.cs.allow-unsigned-executable-memory` /
  `disable-executable-page-protection`, which Apple frames as compatibility escape
  hatches (vs. `allow-jit`, the supported W^X path). They are already in
  `graft/aros-host.entitlements.plist`, and `allocpages.c` confirms RWX is refused even
  *with* them (populate-then-protect is mandatory). **Open:** is a non-MAP_JIT exec
  mapping durable across macOS releases, or should native modules move to `MAP_JIT` (the
  supported path) long-term? **UNVERIFIED.**
- **Relocation completeness.** The implemented AArch64 set (`:756-824`) covers the common
  encodings a `-fno-pic`/typical AROS module emits, but **GOT/PLT/TLS** relocations are
  **not** handled and `R_AARCH64_*` types outside the list hit the `default` →
  `ERROR_BAD_HUNK` (`:832-835`). The module build flags must avoid emitting unsupported
  relocations (e.g. `-fno-plt`, no TLS); `[NL2]` asserts the implemented set, and the
  *build* side (spec.md) constrains the toolchain output. **UNVERIFIED** which exact
  reloc types a real driver's `-O2` build emits.
- **`KrnAllocPages` per-hunk page waste.** Each executable section becomes its own
  page-granular `mmap` (16 KiB native page on Apple Silicon). Small drivers waste most of
  a page per code section. Acceptable for correctness; a future arena allocator (shared
  with the JIT arena question above) is the optimisation, not the first cut.
- **`TypeOfMem` discrimination.** The R/X flip + the `FreeFunc` both rely on
  `TypeOfMem(hunk) == 0` to mean "page-allocated, outside any MemHeader". This is correct
  *iff* `KrnAllocPages` memory is never inside a MemHeader and pool memory always is — a
  load-bearing invariant the spec must freeze and `[NL3]` must assert.
- **Gated on the boot.** `[NL3]`+ need `dos.library` + the boot module set, which the
  kickstart does not yet reach (`graft/WORKFLOW.md` F1/F2). `[NL1]`/`[NL2]` are
  session-sized host spikes that stand alone; the in-AROS legs ride the graft — same
  gating as host-volume `[V4]`+, printing `[PR2]`+, MIDI `[MD5]`.
- **Self-modifying / re-loaded modules.** A module unloaded + reloaded, or one that
  patches its own code post-load, re-opens the I-cache question. Out of scope for
  `[NL1]`-`[NL4]`; flagged. (The JIT's SMC/dirty-page concern,
  [../68k-jit/design.md](../68k-jit/design.md) Risks, is the translated-code analogue.)

## References

Every cited path is under `../aros-upstream` unless noted.

- `rom/dos/internalloadseg.c` — loader dispatch by magic (`0x7f454c46` ELF /
  `0x000003f3` AOS), `:85-115`.
- `rom/dos/internalloadseg_elf.c` — the native ELF loader: `load_hunk` section alloc +
  `MEMF_EXECUTABLE` for `SHF_EXECINSTR` (`:205-304`, esp. `:257-260`); the AArch64 ELF
  relocations (`:306-328` constants, `:756-824` cases); the R/W→R/X flip via
  `KrnSetProtection` (`:1235-1252`); the `DOCACHECLEAR`/`ils_ClearCache` step
  (`:1215-1226`).
- `rom/dos/internalloadseg.h` — the `ils*` macros (`ilsAllocMem` → `funcarray[1]`,
  `:39-46`).
- `rom/dos/internalloadseg_support.c` — `ils_ClearCache` → `CacheClearE` (`:124-128`).
- `rom/dos/internalloadseg_aos.c` — the 68k hunk loader (contrast/the JIT's path);
  allocates `MEMF_31BIT`, never `MEMF_EXECUTABLE` (`:181-207`).
- `rom/dos/loadseg.c` — `LoadSeg` + the `FunctionArray`; `AllocFunc`'s
  `MEMF_EXECUTABLE` → `KrnAllocPages` (`:47-76`), `FreeFunc`'s `TypeOfMem` split
  (`:78-103`).
- `rom/dos/mmakefile.src` — `-DDOCACHECLEAR` for all archs (`:24`).
- `rom/exec/cachecleare.c` — the generic empty `CacheClearE` stub the hosted aarch64
  target falls through to (`:72-77`).
- `compiler/include/exec/memory.h` — `MEMF_EXECUTABLE` (bit 4, `:69-70`); it is **not**
  in `MEMF_PHYSICAL_MASK` (`:102-106`).
- `rom/exec/memory_nommu.c` / `rom/exec/memory.c` — the MemHeader-attribute match that
  masks `MEMF_EXECUTABLE` out (`memory_nommu.c:24,37`; `memory.c:959,1016`).
- `arch/all-unix/kernel/allocpages.c` — `KrnAllocPages`: host `mmap` R/W only, the W^X
  rationale comment (`:11-16`, `:66`, `:79`).
- `arch/all-unix/kernel/setprotection.c` — `KrnSetProtection`: `MAP_*` → `PROT_*` →
  host `mprotect` (`:25-32`).
- `arch/all-unix/kernel/freepages.c` — `KrnFreePages` → host `munmap`.
- `arch/all-unix/kernel/kernel_intern.h` — the `KernelInterface` host bindings
  (`mmap`/`mprotect`/`munmap`/`__error`, `:32-37`).
- `compiler/include/aros/kernel.h` — `MAP_Readable`/`MAP_Writable`/`MAP_Executable`
  (`:35-37`).
- `rom/kernel/kernel.conf` — `KrnSetProtection` (LVO 21), `KrnAllocPages` (27),
  `KrnFreePages` (28).
- `arch/all-unix/bootstrap/memory.c` — the AROS RAM pool mapped R/W only on darwin
  (`:68-76`, `:119`); `AllocateRO`/`SetRO` populate-then-protect for the kickstart
  (`:78-92`); the "RWX refused, MAP_32BIT fails, LoadSeg needs the W^X-aware path" note
  (`:57-66`).
- `compiler/include/aros/printertag.h` — `AROS_PRINTER_MAGIC` (per-arch, **no
  `__aarch64__` branch** — the `[PR0]` gap, `:16-30`); `pmh_Magic` is the first
  instruction of the loaded driver (`AROS_PRINTER_TAG`, `:32-43`).
- `workbench/libs/camd/openmididevice.c` — camd's `LoadSeg` + seglist-scan driver
  discovery (the MIDI dependent-feature gate, `:70-155`).

Working-repo references (`.`):
- `hosted/jit68k/jit_region.{h,c}` — the `[J1]` `MAP_JIT` W^X layer (the SHARED
  foundation): `mmap MAP_JIT` (`jit_region.c:37-40`), `pthread_jit_write_protect_np`
  toggle (`:58-68`), `sys_icache_invalidate` (`:76`).
- `graft/aros-host.entitlements.plist` — `allow-jit`,
  `allow-unsigned-executable-memory`, `disable-executable-page-protection`,
  `disable-library-validation`, `allow-dyld-environment-variables`.
- `graft/bootrun.sh` (`:35-40`), `graft/aros-ctl`, `graft/run-window.sh`,
  `graft/make-aros-app.sh` — the `codesign -o runtime --entitlements …` signing path.
- `graft/CONTINUATION.md` — commit `71f75760` "executable memory is W^X-correct"
  (populate-then-protect, `TypeOfMem`-recognised page hunks, `KrnFreePages`); the
  `KrnAllocPages`/`KrnFreePages` host allocator; `unixio.hidd` resident bring-up
  (the resident-sidestep precedent).
- `graft/WORKFLOW.md` — boot status F1/F2 (the in-AROS-spike gating).
- `docs/features/68k-jit/{design.md,INTERFACE.md}` — `[J1]` the W^X layer; §2 the frozen
  `jit_region`→`hostlib.resource` routing (the shared seam this doc respects and defers
  to).
- `docs/features/darwin-aarch64-port-inventory.md` — §2 / §9 the deferred W^X /
  `MAP_JIT` allocation layer this feature retires.

External prior art (web, not in the AROS tree):
- AArch64 ELF ABI (ARM IHI 0056) + System V ELF spec — the `R_AARCH64_*` relocation
  encodings the loader implements.
- Arm Architecture Reference Manual — instruction-cache maintenance after writing code
  (`DC CVAU` / `IC IVAU` / `DSB` / `ISB`; `__builtin___clear_cache`).
- Apple developer documentation — `MAP_JIT` + `pthread_jit_write_protect_np`, the
  `com.apple.security.cs.allow-jit` vs `allow-unsigned-executable-memory` /
  `disable-executable-page-protection` entitlements, code signing / hardened runtime; and
  that a one-shot RWX anon `mmap` is refused even with the entitlement (also observed
  in-tree, `bootstrap/memory.c:57-66`).
