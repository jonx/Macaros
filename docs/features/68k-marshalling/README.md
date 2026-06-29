# The 68k endianness / marshalling boundary — why AROS has no in-OS m68k emulator, and the design that makes one doable on little-endian

> Status: design + rationale (drafted 2026-06-29) · Companion to the engine docs in
> [../68k-jit/](../68k-jit/) — [design.md](../68k-jit/design.md),
> [INTERFACE.md](../68k-jit/INTERFACE.md), [spec.md](../68k-jit/spec.md).
> Process: [../CLEANROOM.md](../CLEANROOM.md).

**The split.** [68k-jit/](../68k-jit/) is the **engine**: a host-proven 68k→AArch64
translator (`[J1]`–`[J5k]`, `hosted/jit68k/`) that runs real big-endian hunk binaries
byte-exact. This doc is the **boundary**: the endianness argument that has long been
used to say "an integrated m68k emulator is impossible in AROS", what that argument
actually forbids, and the typed-marshalling design that makes the doable version
real. Concretely, it designs the **content** of the FD/register table that
[68k-jit INTERFACE §3, checklist row D](../68k-jit/INTERFACE.md) requires but does not
itself produce, and a **policy** for the untyped residue that
[INTERFACE §5](../68k-jit/INTERFACE.md) defers. The engine freezes the *mechanism*
(how a 68k `jsr` becomes a native `AROS_LH`); this freezes the *data and the rules*
that mechanism runs on.

---

## The question people actually ask

> *Why is there no m68k emulator in AROS? There's already the janus/UAE delegation
> attempt — but why not implement a virtual m68k CPU and run the software directly on
> AROS? The problem is that m68k software expects big-endian data while AROS also runs
> on little-endian CPUs: the little-endian AROS core would have to work with the
> big-endian data in the emulation, and automatic conversion is impossible — e.g. a
> field in an AmigaOS structure is sometimes one ULONG and sometimes two WORDs, so you
> cannot tell how a couple of bytes in RAM are encoded.*

That argument is **correct about one specific integration model and wrong if read as
"emulation is impossible"**. Pinning down exactly which model it kills is the whole
point of this document, because the model it *doesn't* kill is the one this project
has already built the engine for.

## What the endianness argument actually forbids

The "automatic conversion is impossible" claim applies only to **transparent,
zero-copy passthrough**: an m68k binary and the native little-endian AROS sharing the
*same* memory, with native libraries reading and writing the *same* structures the
68k code does. In that model you would have to byte-swap live memory in place without
knowing the type of the bytes — and the ULONG-vs-two-WORDs ambiguity makes that
genuinely undecidable. A `union { ULONG l; UWORD w[2]; }` in RAM has no run-time tag;
swapping it as a longword and swapping it as two words give different results, and
nothing in the bytes tells you which the program meant. **Agreed: that model is
impossible on a little-endian host. Nobody should try to build it.**

But that is not the only way — nor even the usual way — to run m68k software. The
argument quietly assumes the hardest possible coupling and then rules out everything.
Three integration models exist; the FAQ kills the third one only:

1. **Whole-machine emulation beside AROS** (UAE / janus-uae). The m68k world is a
   self-contained big-endian sandbox with its own ROM/Kickstart; it shares **no**
   structures with native AROS. Endianness is internal and consistent, so the problem
   never arises. This works — but it is a VM *next to* AROS, not AROS running the app,
   and it is heavyweight (it emulates the chipset and boots a separate OS).

2. **Typed-boundary marshalling** (this doc). The m68k program lives in a big-endian
   heap and calls native AArch64 AROS libraries, but every value crossing the
   library-call boundary is swapped **according to its declared type**. This is
   tractable precisely because the Amiga library API is fully typed — so the
   "untyped bytes" problem is confined to the small set of app-private data that the
   API typing doesn't describe (§"The hard residue").

3. **Transparent zero-copy passthrough** (the impossible one). m68k and native code
   share structures with no swap at the boundary. Only this requires reinterpreting
   untyped live memory. **This is what the FAQ rules out, correctly.**

The engine in [68k-jit/](../68k-jit/) is built for **model 2** (with a fallback to
model 1's self-containment for anything that can't cross the boundary safely). The
FAQ's "impossible" verdict is about **model 3**, which we are not building.

## Why MorphOS and AmigaOS 4 "got away with" the impossible model

Petunia (AmigaOS 4) and Trance (MorphOS) run m68k apps that call straight through to
native libraries on shared structures — *exactly model 3*. They are not magic: **PPC
is big-endian.** Both sides agree on byte order, so zero-copy passthrough just works,
and the ULONG-vs-two-WORDs case is a non-issue because no swap ever happens. The free
lunch is byte-order agreement, and it evaporates on a little-endian host (x86_64, and
our aarch64). So the honest reading of the FAQ is narrower than it sounds: *"we cannot
do the MorphOS/OS4 trick on little-endian."* True — and irrelevant to model 2, which
pays for the byte-order disagreement explicitly, at a typed boundary, once per call.

| | Host endianness | m68k↔native sharing | Endianness cost |
|---|---|---|---|
| AmigaOS 4 Petunia / MorphOS Trance | **big** (PPC) | zero-copy shared structs (model 3) | none — byte orders agree |
| UAE / janus-uae | any | none — isolated VM (model 1) | none — sandbox is internally BE |
| **This project (model 2)** | **little** (aarch64) | typed marshal at the call boundary | **paid per-arg, by declared type** |
| The FAQ's "impossible" case | little | zero-copy shared structs (model 3) | undecidable — untyped live memory |

## The design — typed marshalling driven by data AROS already has

The load-bearing insight: **AROS already owns a complete, machine-readable, per-library
table of every function's C signature *and* its m68k register map** — and it is the
exact same data the OS uses to *build* those libraries, so it cannot drift out of sync
with reality.

### 1. The marshal descriptor comes from `genmodule`, not from guesswork

The source of truth is the module `.conf` files. One real line from
[rom/exec/exec.conf:106](../../../../aros-upstream/rom/exec/exec.conf):

```
APTR AllocMem(IPTR byteSize, ULONG requirements) (D0, D1)
```

That single line carries everything a marshaller needs: the **return type** (`APTR`),
the **argument types** (`IPTR`, `ULONG`), and the **m68k source registers**
(`D0`, `D1`) — and [tools/genmodule/](../../../../aros-upstream/tools/genmodule/)
already parses it to build the library's function table, its FD file
([writefd.c](../../../../aros-upstream/tools/genmodule/writefd.c)), and its inline
headers. The per-function `AROS_LHA(type,name,reg)` declarations in the C source
(e.g. [rom/exec/allocmem.c:42-43](../../../../aros-upstream/rom/exec/allocmem.c),
`AROS_LHA(IPTR, byteSize, D0)` / `AROS_LHA(ULONG, requirements, D1)`) are the same
facts, generated from the same `.conf`.

**So the marshal table is a new `genmodule` back-end, a peer of `writefd.c`** — it
emits, per library, an array of descriptors:

```c
struct lvo_desc {
    int16_t  lvo;            /* negative offset / 6, the vector index            */
    uint8_t  ret;            /* MARSH_VOID | MARSH_U32 | MARSH_PTR | ...          */
    uint8_t  nargs;
    struct { uint8_t reg;    /* M68K_D0..D7 / M68K_A0..A6 — from the (..) map     */
             uint8_t kind;   /* MARSH_U32 | MARSH_PTR | MARSH_FPTR | MARSH_TAGS   */
    } arg[8];
};
```

This is the `FD/LVO→register table` that
[68k-jit INTERFACE §3 (row D)](../68k-jit/INTERFACE.md) names as the AROS-side input to
the already-frozen LVO bridge. The bridge mechanism (`jit68k_set_lvo_handler`, the
`n = (libbase − pc)/6` recognition, the reverse-of-H3 register marshal) is **done and
host-proven** (`[J3]`/`[J5d]`); this table is the *data* it consumes. Because it is
generated from `.conf`, it covers every library AROS ships, for free, and regenerates
whenever a function's signature changes.

### 2. The byte-order rule is per-`kind`, never per-byte

With the type in hand, swapping is mechanical and total — the undecidability of model 3
never appears because we never look at untyped bytes:

- **`MARSH_U32` / `MARSH_U16` / `MARSH_U8`** — a scalar in a 68k register. The 68k
  register file already holds the architectural value (the engine's EA path byte-swaps
  on every sandbox load/store — [68k-jit INTERFACE §5](../68k-jit/INTERFACE.md),
  `REV` around `ldr`/`str`), so a scalar arg needs only width handling, no swap, to
  enter the AArch64 call.
- **`MARSH_PTR`** — a 32-bit sandbox address. Translate **direction-aware**:
  `host = sandbox_base + addr` on the way in; a returned host pointer that lands inside
  the sandbox maps back to its 32-bit address on the way out. Both directions are the
  §5 contract.
- **The structure *behind* a pointer is not swapped at the boundary.** It stays in the
  big-endian heap. Native AArch64 code that dereferences it must do so through
  byte-order-aware accessors — which is exactly what native AROS already does for
  on-disk and over-the-wire big-endian data (`AROS_BE2LONG` / `AROS_BE2WORD`,
  the same macros the hunk relocator uses,
  [internalloadseg_aos.c:292](../../../../aros-upstream/rom/dos/internalloadseg_aos.c)).
  The boundary swaps *typed scalars and pointers*; deep structure access is the
  callee's typed business, longword-by-longword, never a blind memory swap.

### 3. The m68k world stays big-endian end to end

There is no "convert the heap to little-endian" step anywhere — that step is the
impossible one. The relocator already emits 32-bit **big-endian** pointers
([internalloadseg_aos.c](../../../../aros-upstream/rom/dos/internalloadseg_aos.c)),
the engine's sandbox is a 32-bit big-endian space
([INTERFACE §5](../68k-jit/INTERFACE.md)), and app-private structures are read and
written by 68k code in big-endian throughout. Little-endian appears **only** in the
AArch64 register that a typed scalar occupies for the duration of one native call.
Endianness is a property of the *boundary crossing*, not of the data at rest — which
is the move the FAQ's model 3 cannot make and model 2 makes by construction.

## The hard residue — the genuinely difficult 5%

The typed boundary handles everything the API *describes*. What it can't see is data
the app hands across that the signature types as an opaque pointer. This is the real
engineering, and it is small, bounded, and case-by-case — not "impossible":

- **Callback hooks** (`struct Hook`, `AROS_UFHA(struct Hook *, h, A0)` —
  [asmcall.h:822](../../../../aros-upstream/compiler/arossupport/include/asmcall.h)).
  A native library that calls back into a 68k-supplied hook must re-enter the
  translator for the callback body, marshalling the *callback's* own register args
  back the other direction. The boundary is symmetric: the same descriptor machinery,
  invoked native→68k. The hook's `h_Entry` points into the sandbox, so "is this a 68k
  callback?" is answered by an address-range test, no tagging needed.
- **Tag lists** (`struct TagItem { Tag ti_Tag; IPTR ti_Data; }`,
  `MARSH_TAGS`). `ti_Data` is a textbook untyped union — sometimes a scalar, sometimes
  a pointer, sometimes a packed pair — *and its meaning depends on `ti_Tag`*. The
  resolution is exactly the one model 3 lacks: **`ti_Tag` is the run-time tag.** A
  per-attribute-domain table (the kind boopsi/MUI already need) says, for each known
  tag, whether `ti_Data` is scalar or pointer, so each item is marshalled by its
  declared kind. Unknown/app-private tags fall to the policy below.
- **App-private structures passed by reference and re-cast at run time** — the residue
  proper. Policy: **keep them in the big-endian sandbox and never let native code
  reinterpret their bytes.** If a native library only stores and returns the pointer
  (the common case — opaque handles), nothing needs swapping. If a native library must
  *read fields*, it does so through generated big-endian accessors for that specific
  struct, which requires the struct to be in the typed set — i.e. it is no longer
  "private". The honest boundary: structures that are both app-private *and*
  field-accessed by native code are **not supported by model 2** and belong to model 1
  (run that call's whole subsystem inside the self-contained BE sandbox). This is the
  same line [INTERFACE §5](../68k-jit/INTERFACE.md) draws as its "deferred case"
  (return/take memory outside the sandbox) — named here as a policy, not a surprise.

## How this maps onto the frozen engine seam

Nothing here changes the engine or its `jit68k.h` contract — it supplies the AROS-side
data and rules the engine already declared it needs:

| This doc | Fills / deepens (68k-jit) | Engine status |
|---|---|---|
| §1 marshal-descriptor `genmodule` back-end | INTERFACE §3 **row D** — "the FD/LVO→register table" | bridge mechanism ✅ `[J3]`/`[J5d]`; table is AROS-side ☐ |
| §2 per-`kind` swap + pointer direction | INTERFACE §5 — pointer ↔ host, BE sandbox | sandbox/byteswap ✅ `[J5a]`/`[J5g]` |
| §"hard residue" callbacks | INTERFACE §3 — native→68k re-entry | symmetric bridge; callback path to wire |
| §"hard residue" tag lists / private structs | INTERFACE §5 — the deferred outside-sandbox case | ☐ UNVERIFIED, owner decides |

## Honest debt & open questions

- **`MEMF_31BIT` (or equivalent) is UNVERIFIED.** The sandbox-backed allocator needs
  ≤4 GiB-addressable backing so 68k-visible allocations land in-sandbox; the exact
  AROS flag is the open item already flagged in
  [INTERFACE §5 / row F](../68k-jit/INTERFACE.md).
- **The tag-domain tables are real work.** Core domains (exec/dos/intuition/graphics)
  are finite and known; boopsi/MUI/third-party attribute spaces are large and the
  long tail is where unknown tags fall to the model-1 fallback. Scope this explicitly;
  do not pretend full coverage.
- **Symmetric (native→68k) marshalling doubles the surface.** Callbacks make the
  boundary bidirectional; the descriptor machinery is reused, but re-entrancy and the
  per-thread `MAP_JIT` write toggle (`R-JIT-THREAD`,
  [INTERFACE §2](../68k-jit/INTERFACE.md)) must hold across the nested crossing.
- **This is model 2 only.** Anything needing model-3 transparent sharing is out of
  scope by construction — and correctly so; that is the case the FAQ rules out.

## References

Grounded in the upstream tree (`/Users/user/Source/aros-upstream`) unless noted.

- [rom/exec/exec.conf:106](../../../../aros-upstream/rom/exec/exec.conf) — the
  source-of-truth line: `APTR AllocMem(IPTR byteSize, ULONG requirements) (D0, D1)`
  (return type + arg types + register map together).
- [rom/exec/allocmem.c:42-43](../../../../aros-upstream/rom/exec/allocmem.c) — the same
  facts as `AROS_LHA(IPTR, byteSize, D0)` / `AROS_LHA(ULONG, requirements, D1)`.
- [tools/genmodule/](../../../../aros-upstream/tools/genmodule/) — parses `.conf`,
  emits the function table / FD ([writefd.c](../../../../aros-upstream/tools/genmodule/writefd.c)) /
  inlines; the marshal-descriptor back-end is a peer of `writefd.c`.
- [compiler/arossupport/include/libcall.h](../../../../aros-upstream/compiler/arossupport/include/libcall.h)
  — `AROS_LH*` / `__AROS_LHA(type,name,reg)` library register-arg declarations.
- [compiler/arossupport/include/asmcall.h](../../../../aros-upstream/compiler/arossupport/include/asmcall.h)
  — `AROS_UFH*` / `AROS_UFHA(type,name,reg)` (e.g. `AROS_UFHA(struct Hook *, h, A0)`),
  the callback/hook ABI.
- [arch/m68k-all/include/aros/cpu.h](../../../../aros-upstream/arch/m68k-all/include/aros/cpu.h)
  — the negative-offset `JumpVec` table the bridge recognises (`0x4EF9`, stride 6).
- [rom/dos/internalloadseg_aos.c:292](../../../../aros-upstream/rom/dos/internalloadseg_aos.c)
  — `HUNK_RELOC32` big-endian relocation; `AROS_BE2LONG`, the precedent for
  byte-order-aware access to BE data by native code.
- [workbench/libs/workbench/uae_integration.c](../../../../aros-upstream/workbench/libs/workbench/uae_integration.c)
  — the existing **model 1** delegation (hands 68k binaries to an external emulator),
  for contrast.

Companion (this repo):

- [../68k-jit/design.md](../68k-jit/design.md) — the engine's why + spike plan.
- [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) — the frozen JIT↔AROS seam; §3 (LVO
  bridge) and §5 (sandbox pointer boundary) are the seam points this doc supplies data
  and policy for; row D / row F are the open AROS-side items.
- [../68k-jit/spec.md](../68k-jit/spec.md) — the `jit68k.h` C ABI and the `[J*]` spikes.
