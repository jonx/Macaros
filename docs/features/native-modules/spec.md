# Implementation spec — Native modules (disk-loadable AArch64 code under W^X)

> Status: started (partial) - the native W^X LoadSeg path is largely landed in the AROS tree (commit 71f75760); darwin verify-and-reconcile pending · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).
> Shared foundation: [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2 (`[J1]`).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent,
driver, loader, or otherwise — was read, searched, or consulted in producing it, and
any resemblance to existing implementations is coincidental.** Implement only from this
spec + the approved sources cited by tag: `[PUB]` Apple W^X / code-signing docs
(`MAP_JIT`, `pthread_jit_write_protect_np`, the `allow-jit` /
`allow-unsigned-executable-memory` entitlements), POSIX `mmap`/`mprotect`, the ELF +
AArch64 ELF relocation specs (ARM IHI 0056, System V ELF), and AArch64 cache-maintenance
from the Arm ARM (`IC IVAU`/`DC CVAU`/`DSB`/`ISB`); `[AROS]` in-tree AROS paths;
`[OURS]` this project's spikes (`hosted/mem.c`, `hosted/jit68k/jit_region.{h,c}`, the
boot push that hit W^X, `graft/*`); `[DERIVED]` independently-derived requirements
flagged for extra verification, each standing solely on its cited `[PUB]`/`[AROS]`/
`[OURS]` justification. No identifier name, call sequence, file layout, or relocation/
cache algorithm here derives from any third-party implementation. The relocation
encodings and the cache-maintenance sequence are dictated by published standards, not by
any implementation's expression.

## Scope

**This feature is in progress, not greenfield.** A substantial native-LoadSeg W^X path
**already exists in the work-tree** (`../aros-upstream`, this project's
commit `71f75760` + the all-unix kernel additions): the ELF loader requests executable
memory, `LoadSeg`'s `AllocFunc` routes it to `KrnAllocPages`, the kernel backs that with
host `mmap`, relocations are applied, and `KrnSetProtection` flips the pages R/X. So the
bulk of the *mechanism* is reused verbatim; this spec covers the **contract that
mechanism must satisfy**, the **cache-maintenance gap that must be closed**, the
**toggle/ordering discipline**, the **build/codesign/entitlement story**, and the
**unattended value-assertion** that proves it.

**In.** A loop-verifiable, value-asserted native-module load path for `aarch64-darwin`
that: backs `SHF_EXECINSTR` ELF sections with page-granular host-executable memory;
applies the AArch64 ELF relocations while the code is writable; flips populated
executable hunks to R/X; makes the freshly-written code **I-cache-coherent before first
execution**; runs the module's native entry; and is signed with the entitlements the W^X
path needs. Plus the bare-host spikes (`[NL1]`/`[NL2]`) that prove the W^X round-trip and
relocation correctness without AROS, and the in-AROS spikes (`[NL3]`/`[NL4]`) that prove
`LoadSeg`→run on the real loader and a real dependent driver.

**Out (non-goals, this spec).**
- The 68k (AOS-hunk) loader and translated-code execution — that is the
  [68k JIT](../68k-jit/design.md); this spec is the **native ELF** path only.
- The `MAP_JIT` per-thread-toggle layer itself — owned by `[J1]`
  (`hosted/jit68k/jit_region.{h,c}`), frozen in
  [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2; this spec cross-references it.
- GOT/PLT/TLS relocations (the module build must not emit them — see R-RELOC-SET).
- An arena/sub-page executable allocator (page-granular `KrnAllocPages` is the first cut).
- Self-modifying / hot-reloaded module code (the I-cache re-sync case — flagged, deferred).
- Any change to the loader dispatch (`internalloadseg.c`) or the relocation maths beyond
  closing the cache gap and constraining the build.

## The decision the owner must make first (W^X strategy)

**Two valid Apple W^X routes coexist in the tree and the choice is the owner's.** They
are interface-compatible at the AROS seam (both yield runnable executable hunks behind
`kernel.resource`), so the choice is contained.

- **Strategy A — `mprotect` populate-then-protect (the native path today, recommended).**
  `KrnAllocPages` `mmap`s R/W; loader writes + relocates; `KrnSetProtection` flips R/X.
  Persistent R/X, page-granular, no per-thread hazard — the natural fit for write-once-
  execute-from-any-task module code. Needs `allow-unsigned-executable-memory` /
  `disable-executable-page-protection`. `[AROS]` `arch/all-unix/kernel/{allocpages,
  setprotection}.c`; `[OURS]` already in `graft/aros-host.entitlements.plist`.
- **Strategy B — `MAP_JIT` + `pthread_jit_write_protect_np` (the `[J1]`/JIT route).**
  Apple's preferred path, but the **per-thread** write toggle ([../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md)
  §2, R-JIT-THREAD) buys nothing for persistent module code (the page is executable on
  all threads once the window closes anyway). Needs only `allow-jit`.

**Recommendation:** Strategy A for native LoadSeg; defer any single-arena unification with
the JIT to the JIT track ([../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2). The
requirements below are written **strategy-neutral at the AROS seam** and note where a
choice bites; **this spec does not modify the `[J1]` layer.**

## Architecture

Three layers; the AROS loader and the host page-allocator are joined by the existing
`kernel.resource` LVO contract (no new shim). The shared substrate is `[J1]`'s.

```
AROS side (aarch64, AROS crosstools)              Host side (Apple, via kernel.resource iface)
┌──────────────────────────────────────┐         ┌────────────────────────────────────┐
│ dos.library LoadSeg          [AROS]   │         │ kernel.resource (all-unix)  [AROS]  │
│   InternalLoadSeg_ELF                 │         │   KrnAllocPages → iface->mmap R/W   │
│    · load_hunk: SHF_EXECINSTR         │ Krn LVO │   KrnSetProtection → iface->mprotect│
│      → MEMF_EXECUTABLE ──────────────►│────────►│   KrnFreePages → iface->munmap      │
│    · relocate (AArch64 R_AARCH64_*)   │         │     (host mmap/mprotect/munmap)     │
│      [applied while R/W]              │         └────────────────────────────────────┘
│    · KrnSetProtection R/X ────────────│──────── flip after relocate
│    · I-CACHE MAINTAIN  (NEW)  ────────│──────── __builtin___clear_cache / sys_icache_invalidate
│   call native entry (R/X, coherent)   │
└──────────────────────────────────────┘     SHARED W^X substrate: [J1] jit_region.{h,c} (MAP_JIT, [J1]'s)
```

- **The only genuinely-new code** is the **I-cache maintenance step** (R-ICACHE) — the
  existing `CacheClearE` is a no-op on this target. Everything else is the in-tree chain,
  reused.
- Spike-phase: the W^X+cache round-trip and the relocation harness live in
  `hosted/native-modules/` (host clang, H-series style); at graft the AROS legs run the
  real loader.

## Requirements

### R-EXECMEM — executable sections come from page-granular executable memory `[AROS]`

Any ELF section with `SHF_EXECINSTR` MUST be backed by host memory that can be made
executable; data/BSS sections MUST stay in the ordinary R/W pool.

- **Tag executable sections.** `load_hunk` sets `memflags |= MEMF_EXECUTABLE` for
  `sh->flags & SHF_EXECINSTR` and allocates with it (`internalloadseg_elf.c:257-260`)
  `[AROS]`.
- **Route to host pages.** `MEMF_EXECUTABLE` allocations MUST go through `KrnAllocPages`,
  not the exec pool, because exec masks `MEMF_EXECUTABLE` out of the MemHeader-attribute
  match (`exec/memory.h:102-106`; `memory_nommu.c:24,37`) — a pool would yield
  non-executable backing. The in-tree route is `AllocFunc` (`loadseg.c:61-73`):
  `if (flags & MEMF_EXECUTABLE) { KernelBase = OpenResource("kernel.resource"); if
  (KernelBase) { APTR p = KrnAllocPages(NULL,length,flags); if (p) return p; } } return
  AllocMem(length, flags);` `[AROS]`.
- **`KrnAllocPages` hands back R/W.** It MUST NOT attempt RWX (refused on Apple Silicon —
  see R-NO-RWX): `arch/all-unix/kernel/allocpages.c:66,79` `mmap`s
  `PROT_READ|PROT_WRITE` only `[AROS]`.
- **Invariant (R-TYPEOFMEM).** Page-allocated (executable) hunks live **outside** any
  MemHeader, so `TypeOfMem(hunk) == 0`; pool hunks return `!= 0`. The R/X flip and the
  `FreeFunc` both depend on this discriminator (`loadseg.c:88`,
  `internalloadseg_elf.c:1246`) `[AROS]`. The spec FREEZES this invariant; `[NL3]`
  asserts it.

### R-NO-RWX — never map writable+executable simultaneously `[PUB]` + `[OURS]`

A page MUST NOT be R/W and R/X at the same time. `[PUB]` Apple Silicon refuses a one-shot
RWX anon `mmap` even with `allow-unsigned-executable-memory` /
`disable-executable-page-protection`; `[OURS]` the boot push observed exactly this
(`arch/all-unix/bootstrap/memory.c:57-66`: "a one-shot RWX anonymous mmap is refused
(EACCES) even with the … entitlements"; `allocpages.c:11-16,70-76`). Therefore the path
is **populate-R/W → relocate → flip-R/X** (Strategy A) or **MAP_JIT toggle window**
(Strategy B, `[J1]`), never RWX.

### R-RELOC-ORDER — relocate while writable, flip after `[AROS]` + `[DERIVED]`

The order is load-bearing and FROZEN:

1. allocate executable hunks **R/W** (R-EXECMEM);
2. apply **all** relocations into them while R/W (`relocate`,
   `internalloadseg_elf.c:1184-1201`, AArch64 cases `:756-824`) `[AROS]`;
3. **only then** flip each populated executable hunk to R/X (R-FLIP);
4. then I-cache-maintain (R-ICACHE), then execute.

A relocation written into an already-R/X hunk would fault; an executable hunk run before
its relocations resolve would execute wrong code. `[DERIVED]` (the *ordering requirement*
is restated from `[AROS]` — the loader already loads-then-relocates-then-flips
(`:1158-1201` load, `:1235-1252` flip) — and from `[PUB]` W^X semantics; implement from
that order, not from any reference). `[NL2]`/`[NL3]` assert it by value.

### R-RELOC-SET — the AArch64 ELF relocations + a build constraint `[PUB]` + `[AROS]`

The loader MUST resolve the AArch64 ELF relocation encodings a typical AROS module emits.
The implemented set (`internalloadseg_elf.c:756-824`) `[AROS]`, each a published-spec
encoding `[PUB]` (ARM IHI 0056):

| Reloc | Action | File:line |
|-------|--------|-----------|
| `R_AARCH64_ABS64`/`ABS32` | `*p = S + A` | `:758-762` |
| `R_AARCH64_PREL64`/`PREL32` | `*p = S + A − P` | `:764-768` |
| `MOVW_UABS_G0..G3` | replace movz/movk imm (bits 5-20) with the 16-bit slice | `:772-786` |
| `ADR_PREL_PG_HI21` (ADRP) | 21-bit page offset, immlo bits 29-30 / immhi 5-23 | `:789-794` |
| `ADD_ABS_LO12_NC`, `LDST{8,16,32,64,128}_ABS_LO12_NC` | imm12 (bits 10-21), scaled by access size | `:797-811` |
| `JUMP26`/`CALL26` (b/bl) | 26-bit signed offset `>> 2` (bits 0-25) | `:815-821` |
| `R_AARCH64_NONE` | no-op | `:823` |

- Any relocation type **outside** this set hits the `default` → `SetIoErr(ERROR_BAD_HUNK)`
  (`:832-835`) `[AROS]`. **GOT/PLT/TLS are NOT handled.**
- **R-RELOC-BUILD (the build constraint).** The native-module toolchain MUST be configured
  to emit **only** the implemented relocation set — no GOT/PLT/TLS. In practice: build
  modules as the AROS crosstools build other relocatable modules (the same flags
  `%build_module` uses), avoid `-fPIC` GOT-indirection and `__thread`/TLS, and prefer
  direct `CALL26`/`ADRP+ADD` addressing. `[DERIVED]` (restated from the `[AROS]`
  implemented-set + `[PUB]` ABI: implement the build flags from the implemented set, then
  let `[NL2]` assert; do not assume a reloc type the loader lacks). **UNVERIFIED:** the
  exact reloc types a real driver's `-O2` build emits — `[NL2]` pins it.

### R-FLIP — flip populated executable hunks to R/X `[AROS]`

After relocation, walk the seglist and, for each hunk with `!TypeOfMem(hunk)`, call
`KrnSetProtection(hunk, hunk->size, MAP_Readable | MAP_Executable)` — exactly
`internalloadseg_elf.c:1235-1252` `[AROS]`. `KrnSetProtection` maps `MAP_*` → `PROT_*` →
host `mprotect` (`arch/all-unix/kernel/setprotection.c:25-32`); `MAP_Readable` =`0x100`,
`MAP_Writable` =`0x200`, `MAP_Executable` =`0x400` (`compiler/include/aros/kernel.h:35-37`)
`[AROS]`. Data/BSS hunks (`TypeOfMem != 0`) MUST be left R/W untouched.

### R-ICACHE — make freshly-written code I-cache-coherent before execution `[PUB]` + `[OURS]` (NEW)

This is the **one genuinely-new requirement** and the sharpest correctness point.
`[PUB]` On AArch64 the instruction and data caches are **not coherent for self-written
code**: after writing instructions you MUST clean the D-cache to PoU (`DC CVAU`),
invalidate the I-cache (`IC IVAU`), `DSB ISH`, then `ISB` before executing — equivalently
`__builtin___clear_cache(start,end)` or `sys_icache_invalidate(start,len)` (Arm ARM;
Apple uses `sys_icache_invalidate`, as `[J1]` does at `hosted/jit68k/jit_region.c:76`)
`[OURS]`. **On hosted darwin-aarch64 the in-tree `CacheClearE` is a no-op** (the generic
empty stub `rom/exec/cachecleare.c:72-77`; no arch override under `arch/all-unix`/
`all-hosted`/`all-darwin`), so the loader's `ils_ClearCache(...CACRF_ClearI)` call
(`internalloadseg_elf.c:1223`) currently does nothing `[AROS]`.

**Requirement:** the load path MUST I-cache-maintain every executable hunk's range after
relocation and before first execution. Two implementation options (the implementer picks
one; `[NL1]` validates):

1. **Real hosted/aarch64 `CacheClearE`** — provide an `arch/all-unix` (or aarch64) impl
   of `CacheClearE`/`CachePreDMA` that calls `__builtin___clear_cache(addr, addr+len)`
   (or `sys_icache_invalidate`) on the host, so the existing `ils_ClearCache(...
   CACRF_ClearI)` at `:1223` becomes effective. Most localised; fixes it for every
   `LoadSeg`. **Preferred.**
2. **Fold the I-cache into the flip** — invalidate inside the `KrnSetProtection`
   `MAP_Executable` path so "flip to R/X" is atomically "make R/X + sync I-cache".
   Couples cache to protection (acceptable, slightly less general).

**R-ICACHE-VERIFY (the negative control).** `[NL1]` MUST run the path **with** and
**without** the explicit I-cache step and assert the returned value, to **empirically
settle** whether the `mprotect` R/W→R/X transition alone orders the I-cache on Apple
Silicon (the §5 UNVERIFIED). A stale I-cache yields a **wrong value, not a crash**
(`[OURS]` the `[J1]` lesson, `jit_region.c:73-77`), so the value assert is mandatory.
`[DERIVED]`: the *need* is restated from `[PUB]` (Arm cache architecture) + `[OURS]`
(`CacheClearE` is a no-op here; the `[J1]` icache lesson); implement from that, not from
any reference.

### R-THREAD — toggle discipline (only if Strategy B is chosen) `[OURS]`

If the owner chooses Strategy B (`MAP_JIT`), the per-thread write toggle is FROZEN by
`[J1]` R-JIT-THREAD ([../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2): all writes
to a `MAP_JIT` page happen inside `pthread_jit_write_protect_np(0/1)` on **one** host
thread, paired on the same thread; execution is unrestricted once the window closes. For
*module* code this is a non-issue (write-once at load on the loader's thread, execute from
any task thereafter). **Strategy A has no per-thread toggle** (persistent R/X via
`mprotect`), so R-THREAD does not apply to it. This spec does not change the `[J1]`
toggle contract either way.

### R-ENTITLE — codesign + entitlements `[PUB]` + `[OURS]`

The hosted process MUST be code-signed with the entitlements the chosen strategy needs.
`[OURS]` `graft/aros-host.entitlements.plist` already carries all of them:
`com.apple.security.cs.allow-jit` (Strategy B), `allow-unsigned-executable-memory` +
`disable-executable-page-protection` (Strategy A), plus
`disable-library-validation` and `allow-dyld-environment-variables`. The signing step is
`codesign -s - -f -o runtime --entitlements <plist> <binary>` under the hardened runtime
(`graft/bootrun.sh:35-40`, `graft/aros-ctl:374`, `graft/run-window.sh:316`,
`graft/make-aros-app.sh:145`) `[OURS]`. `[PUB]` without the hardened runtime (plain ad-hoc
signature) the entitlements are not needed; `[NL1]` records the exact incantation and
which keys are load-bearing for the chosen strategy. **UNVERIFIED:** the long-term
durability of the Strategy-A entitlements across macOS releases (Apple frames them as
compatibility escape hatches) — flagged, not blocking.

## The native module build (the toolchain side) `[AROS]` + `[DERIVED]`

A disk-loadable native module is a relocatable AArch64 ELF object the loader can
`LoadSeg`. The build MUST:

- emit **ELF** (magic `0x7f454c46`) so the dispatcher picks `InternalLoadSeg_ELF`
  (`internalloadseg.c:87`) `[AROS]`;
- place code in `SHF_EXECINSTR` sections (so R-EXECMEM tags them) — the default `.text`
  `[AROS]`/`[PUB]`;
- emit **only** the R-RELOC-SET relocations (no GOT/PLT/TLS) — built with the AROS
  crosstools the same way `%build_module` builds in-tree modules `[AROS]`; `[DERIVED]`;
- for a printer driver, carry the `.tag.printer` section with `pmh_Magic` =
  `AROS_PRINTER_MAGIC` (the **first instruction executed** — needs the `__aarch64__`
  branch from printing `[PR0]`, `compiler/include/aros/printertag.h:16-43`) `[AROS]`;
- for a CAMD driver, carry the `MidiDeviceData` blob with `Magic == MDD_Magic`
  (`compiler/include/midi/camddevices.h`) so camd's seglist scan finds it
  (`workbench/libs/camd/openmididevice.c:70-155`) `[AROS]`.

The build is **AROS crosstools**, not host clang (the module is AROS code). The
spike-phase host harnesses (`[NL1]`/`[NL2]`) are host clang; the module under
`[NL3]`/`[NL4]` is crosstools.

## Verification (unattended — `[OURS]` H7/J1 discipline)

No TCC, no Screen-Recording, no human. Every PASS asserts a **value** (a returned
constant, an exit code, a feature oracle) so a stale I-cache (wrong instruction) or a bad
relocation (wrong immediate) is caught as a **wrong value**, not missed by a no-crash
test — the `[J1]`/`[A1]` rule.

**Markers** (one host binary per host marker, one booted-AROS assertion per AROS marker;
`make hosted-native-modules` for `[NL1]`/`[NL2]`, `graft/native-modules-smoke` for the
booted legs; `harness/run-hosted.sh` greps `[NL?]` and emits the uniform
`result=(PASS|FAIL)` block; clean-exit on PASS):

- **[NL1] W^X round-trip + cache, value-asserted (pure host, the shared-layer probe).**
  `mmap` R/W a page (the `KrnAllocPages` shape), write `movz w0,#0x6804 ; ret`
  (architecturally encoded, not a magic literal — cf. `jit_region.h:73-76` `[OURS]`),
  `mprotect(R|X)` (the `KrnSetProtection` shape), I-cache-maintain
  (`__builtin___clear_cache`/`sys_icache_invalidate`), call through a fn-ptr. **PASS** =
  returns `0x6804`. **FAIL** = SIGBUS/SIGSEGV, or wrong value. **R-ICACHE-VERIFY:** also
  run the **no-explicit-icache** variant and report whether it still returns `0x6804`
  (settles the §5 UNVERIFIED). Records the entitlement/codesign incantation (R-ENTITLE).
  Cross-references `[J1]`: the Strategy-A peer of `[J1]`'s `MAP_JIT` proof; defers to
  [../68k-jit/design.md](../68k-jit/design.md) `[J1]` for the shared-substrate claim. `[NL1]`.
- **[NL2] AArch64 ELF relocation correctness, asserted (pure host).** A tiny native ELF
  object (assembled offline) exercising `ADRP`+`ADD_ABS_LO12_NC`, `CALL26`, and `ABS64`;
  apply the R-RELOC-SET maths (mirroring `internalloadseg_elf.c:756-824`), place in
  `[NL1]`'s page, call the entry. **PASS** = returns the value that is correct only if
  every reloc resolved (reads a relocated global + calls a relocated helper → their sum).
  **FAIL** = wrong value. Also asserts that an **unsupported** reloc type is rejected
  (the `default`→`ERROR_BAD_HUNK` contract, R-RELOC-SET). `[NL2]`.
- **[NL3] `LoadSeg` a tiny native module + call its entry (booted AROS).** A minimal
  crosstools-built AArch64 module (one `.text` returning a known longword from a published
  entry) `LoadSeg`'d through the real `InternalLoadSeg_ELF` →
  `AllocFunc`/`KrnAllocPages` → relocate → `KrnSetProtection` → (R-ICACHE) → call.
  **PASS** = entry returns the baked-in constant **and** the hunk was page-allocated
  (`TypeOfMem == 0`, R-TYPEOFMEM) and flipped R/X (loader `bug()`/sentinel). **FAIL** =
  load/reloc error, non-exec page (SIGSEGV), or wrong value. Proves
  load→page-alloc→relocate→flip→icache→execute on the real loader. *(Rides the graft;
  gated on the boot reaching `dos.library`, `graft/WORKFLOW.md` F2.)* `[NL3]`.
- **[NL4] a real loadable driver, loaded and used (booted AROS).** The cheapest real
  dependent: once printing `[PR0]` lands the `__aarch64__` magic, `printer.device`
  `LoadSeg`s `DEVS:Printers/PostScript`, validates `ps_runAlert == AROS_PRINTER_MAGIC`
  (the magic = first executed instruction — direct proof loaded code is executable), and
  one `CMD_WRITE` produces the expected spool. **PASS** = loads + magic validates + the
  printing `[PR2]` DSC-PostScript oracle holds. **FAIL** = load/validate failure or a
  crash entering the driver. Equivalently MIDI `[MD5]` (`DEVS:Midi/coremidi`) or audio
  `[A5]` — whichever dependent feature is ready. The foundation proven by a feature that
  uses it. `[NL4]`.
- **[NL5-resident] the ROM-resident sidestep (interim, parallel).** Co-build one
  dependent driver into the kickstart (resident, `residentpri`, the `unixio.hidd`
  precedent, `graft/CONTINUATION.md`) so it runs **without** `LoadSeg`, unblocking that
  feature before `[NL3]`/`[NL4]`. **PASS** = the resident driver works (same per-feature
  oracle as `[NL4]`). Not a W^X proof (resident code is R/X via `SetRO`); the documented
  escape hatch, marked so the unblock is greppable and the migration to `DEVS:` is
  tracked. `[NL5-resident]`.

**The assertions** (every marker asserts values, never "it didn't crash"):
- a **returned constant** (`[NL1]`/`[NL2]`/`[NL3]`) — wrong-value catches stale I-cache /
  bad reloc;
- the **with-vs-without-icache** comparison (`[NL1]`, R-ICACHE-VERIFY);
- **page-state** (`[NL3]`): `TypeOfMem(hunk)==0` and flipped (R-TYPEOFMEM, R-FLIP);
- the **dependent feature's own oracle** re-read host-side (`[NL4]`, the H11 two-sided
  rule).

## Build / integration

- Host spikes (`[NL1]`/`[NL2]`) compile to Mach-O via the existing `Makefile` pattern
  (`make hosted-native-modules` → `build/host-nl*` → `harness/run-hosted.sh '[NL?] …'`)
  `[OURS]`. Add them to `harness/test-hosted.sh`.
- The R-ICACHE fix (a hosted/aarch64 `CacheClearE`, or the folded-into-flip variant) lands
  in the **AROS crosstools** build (`arch/all-unix` or aarch64 kernel), **not** host clang.
- The native module under `[NL3]`/`[NL4]` is built by the AROS crosstools
  (`%build_module`), constrained per R-RELOC-BUILD.
- Signing per R-ENTITLE — reuse `graft/bootrun.sh` / `run-window.sh` / `aros-ctl`; no new
  signing path. `[NL5-resident]` needs no per-module signing (resident code is in the
  already-signed kickstart RO region).
- No new host dylib, no Cocoa: the host side is the existing `kernel.resource`
  `iface->{mmap,mprotect,munmap}` (`arch/all-unix/kernel/kernel_intern.h:32-37`).

## Open questions / UNVERIFIED

- Does the `mprotect` R/W→R/X transition alone make the I-cache coherent on Apple
  Silicon, or is an explicit `IC IVAU`/`__builtin___clear_cache` required? — `[NL1]`
  R-ICACHE-VERIFY settles it empirically (default: assume explicit is required).
- W^X strategy: Strategy A (`mprotect`) vs Strategy B (`MAP_JIT`) for native modules, and
  whether to unify with the JIT arena — owner decides; deferred to
  [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2 if unified.
- The exact AArch64 reloc types a real `-O2` driver build emits, vs the implemented set
  (R-RELOC-SET) — `[NL2]` pins it; the build avoids GOT/PLT/TLS (R-RELOC-BUILD).
- Long-term durability of the Strategy-A entitlements
  (`allow-unsigned-executable-memory` / `disable-executable-page-protection`) across macOS
  releases (Apple frames them as compatibility escape hatches).
- Per-hunk page waste from page-granular `KrnAllocPages` (16 KiB native page) — an arena
  allocator is the optimisation, not the first cut.
- camd's seglist-scan (`openmididevice.c:70-155`) over a *native aarch64* seglist — the
  MIDI doc flags it UNVERIFIED; `[NL3]`/`[MD5]` exercise it; the resident sidestep
  (`[NL5-resident]`) is the fallback if the scan can't walk a native seglist.

## Provenance summary

`[PUB]` Apple W^X / hardened-runtime / code-signing docs (`MAP_JIT`,
`pthread_jit_write_protect_np`, `com.apple.security.cs.allow-jit` vs
`allow-unsigned-executable-memory` / `disable-executable-page-protection`,
`sys_icache_invalidate`; one-shot RWX `mmap` refused); POSIX `mmap`/`mprotect`/`munmap`;
the ELF format + AArch64 ELF relocation ABI (ARM IHI 0056, System V ELF) — the
`R_AARCH64_*` encodings; AArch64 cache maintenance from the Arm ARM (`DC CVAU`/`IC IVAU`/
`DSB`/`ISB`, `__builtin___clear_cache`). ·
`[AROS]` `rom/dos/internalloadseg.c` (dispatch :85-115),
`rom/dos/internalloadseg_elf.c` (`load_hunk`/`MEMF_EXECUTABLE` :205-304/:257-260, AArch64
relocations :306-328/:756-824, R/X flip :1235-1252, cache-clear :1215-1226),
`rom/dos/internalloadseg.h` (`ilsAllocMem`→`funcarray[1]` :39-46),
`rom/dos/internalloadseg_support.c` (`ils_ClearCache`→`CacheClearE` :124-128),
`rom/dos/internalloadseg_aos.c` (the contrast 68k loader, never `MEMF_EXECUTABLE`),
`rom/dos/loadseg.c` (`AllocFunc`/`KrnAllocPages` :47-76, `FreeFunc`/`TypeOfMem` :78-103),
`rom/dos/mmakefile.src` (`-DDOCACHECLEAR` :24), `rom/exec/cachecleare.c` (the generic
no-op :72-77), `compiler/include/exec/memory.h` (`MEMF_EXECUTABLE` :69-70,
`MEMF_PHYSICAL_MASK` :102-106), `rom/exec/{memory_nommu.c:24,37, memory.c:959,1016}` (the
attribute mask), `arch/all-unix/kernel/{allocpages.c, setprotection.c, freepages.c,
kernel_intern.h}` (host `mmap`/`mprotect`/`munmap` page APIs),
`compiler/include/aros/kernel.h` (`MAP_*` :35-37), `rom/kernel/kernel.conf` (Krn LVOs),
`arch/all-unix/bootstrap/memory.c` (RAM R/W-only :68-76, `AllocateRO`/`SetRO` :78-92, the
RWX-refused note :57-66), `compiler/include/aros/printertag.h` (the `[PR0]` magic
:16-43), `workbench/libs/camd/openmididevice.c` (seglist scan :70-155). ·
`[OURS]` `hosted/jit68k/jit_region.{h,c}` (the `[J1]` `MAP_JIT` W^X layer — the shared
substrate; `mmap MAP_JIT`, `pthread_jit_write_protect_np`, `sys_icache_invalidate`),
`hosted/mem.c` (the H5 R/W pool allocator — no executable memory), the boot push that hit
W^X, `graft/aros-host.entitlements.plist` (the entitlements), `graft/{bootrun.sh,aros-ctl,
run-window.sh,make-aros-app.sh}` (the codesign path), `graft/CONTINUATION.md` (commit
`71f75760` W^X-correct exec memory; the `unixio.hidd` resident precedent),
`graft/WORKFLOW.md` (boot gating F1/F2), `harness/run-hosted.sh` (marker harness). ·
`[DERIVED]` independently-derived points flagged for extra verification: (a) the
relocate-then-flip-then-icache ordering [R-RELOC-ORDER], (b) the build constraint to emit
only the implemented relocation set [R-RELOC-BUILD], and (c) that an explicit AArch64
I-cache step is needed because `CacheClearE` is a no-op on this target [R-ICACHE] — each
restated from `[PUB]` (Arm cache architecture / AArch64 ELF ABI) + `[AROS]` (the in-tree
loader chain + the no-op `CacheClearE`) + `[OURS]` (the `[J1]` icache lesson). No
third-party code, identifiers, relocation algorithm, or cache sequence used.
