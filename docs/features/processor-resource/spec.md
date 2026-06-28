# Implementation spec — processor.resource darwin/AArch64 backend (host-sourced CPU facts)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent,
driver, or otherwise — was read, searched, or consulted in producing it, and any
resemblance to existing implementations is coincidental.** Implement only from this
spec + the approved sources cited by tag: `[PUB]` Apple `sysctl`/Mach `host_info`
docs and the macOS SDK headers / the Arm Architecture Reference Manual (register
semantics) / POSIX; `[AROS]` in-tree AROS headers and the live `processor.resource`,
its arch backends, and `hostlib` source (paths given; APL/LGPL — ours); `[OURS]`
this project's spikes and shims (`hosted/*`, the H/A-series, the `CPUInfo` host-query
spike). `[DERIVED]` items are independently-derived requirements flagged for extra
verification; each stands solely on its cited `[PUB]`/`[AROS]`/`[OURS]` justification
— implement from that justification, never from any reference. No identifier name,
call sequence, file layout, or algorithm in this spec derives from any third-party
implementation.

## Scope

**In.** A hosted **arch backend for AROS's `processor.resource`** on
`aarch64-darwin` that fills `GetCPUInfo()` from the host: CPU **identity**
(`GCIT_ModelString`), **topology** (`GCIT_NumberOfProcessors`, P/E split),
**architecture/endianness** (`GCIT_Architecture`/`GCIT_Endianness`), **caches**,
**features** (`GCIT_SupportsNeon`/`SupportsFPU`/`SupportsAES`), and **load**
(`GCIT_ProcessorLoad`) — read from `libSystem` `sysctl` + Mach `host_*` via
`hostlib.resource`. It **folds in** the private host query that
`workbench/c/CPUInfo/cpuinfo_arch.c` currently carries (deleting that hack), and it
verifies headless/numeric: real values are plausible, and `GetCPUInfo` returns the
**same** values the raw `sysctl` returns, so `CPUInfo`/`ShowConfig` print the real
model instead of "Unknown".

**Decision (the arch-module seam — confirmed pattern).** Use **Pattern A** — a darwin
arch `getcpuinfo.c` that replaces the generic `GetCPUInfo` LVO (mirroring
`arch/all-pc/processor/` and `arch/arm-native/processor/`), with the host query in a
`hostlib`-loaded helper split (`processor_hostlib.c` + `processor_hostcpu.c`,
mirroring `arch/all-linux/processor/`). Rationale: self-contained, lowest blast radius
on the shared `rom/processor` getter, full control of the tag set. The alternative —
Pattern B′, a model/feature/load override hook added to
`rom/processor/getcpuinfo.c` fed by the arch helper (the "model-override hook to
rom/processor" `cpuinfo_arch.c:71` names) — is the documented fallback if Pattern A's
getter-shadowing fights the build. **Owner confirms the pattern before implementation.**
Full rationale in [design.md](design.md) §"The arch-module seam".

**Decision (architecture constant).** Report **`PROCESSORARCH_ARM`** for
`GCIT_Architecture` — the value the generic `defaults.h:24-28` already uses for
`__aarch64__` and the only one `CPUInfo`'s `arch_name()` knows
(`cpuinfo_arch.c:33-43`). Adding a `PROCESSORARCH_ARM64` public constant is **out of
scope** unless the owner approves a public-header change (see R-ARCH / UNVERIFIED).

**Out (non-goals, this spec).** A new `PROCESSORARCH_ARM64`/`CPUFAMILY_ARM_*` public
constant (owner-gated); per-AArch64-feature `GCIT_*` tags beyond the few that already
exist (most `hw.optional.arm.FEAT_*` have no destination tag); reading EL1 ID
registers (they trap at EL0 — the whole reason for the host route); a CPU-frequency
number (`hw.cpufrequency` is absent on Apple Silicon — R-SPEED leaves it 0);
modifying the generic `rom/processor/getcpuinfo.c` (Pattern A keeps it untouched);
any change to non-darwin arch backends.

## Architecture

Two layers joined by the host-call boundary, exactly as `CPUInfo` and the other
hosted shims cross it. No new RT thread, no SPSC ring — `GetCPUInfo` is pure pull.

```
AROS side (aarch64, AROS crosstools)              Host side (Apple, via libSystem)
┌──────────────────────────────────────┐          ┌──────────────────────────────┐
│ processor.resource  (kernel-processor)│          │ libSystem.dylib   [PUB]      │
│  · rom/processor base + LVO   [AROS]  │  hostlib │   sysctlbyname(...)           │
│  · NEW arch/all-darwin/processor/     │  Open +  │   (static facts, like CPUInfo)│
│      getcpuinfo.c  (GetCPUInfo LVO) ──┼──Lock──► │                              │
│      processor_hostcpu.c (queries)    │ ◄────────┤ libhostcpu.dylib  [OURS]     │
│      processor_hostlib.c (resolve)    │  values  │   mach_host_self /            │
│  · ProcessorBase->Private1 = cached   │          │   host_statistics64 /        │
│      DarwinProcessorInformation       │          │   host_processor_info (load) │
└──────────────────────────────────────┘          └──────────────────────────────┘
   GetCPUInfo(tagList)  ──►  read cached facts + live load delta  ──►  fill out-params
   CPUInfo / ShowConfig / SysMon  ──►  GetCPUInfo()  ──►  real "Apple M…", real load
```

- **Static facts** (identity/topology/endianness/caches/features) are queried **once
  at init** via plain `sysctlbyname` resolved through `HostLib_GetPointer` over
  `libSystem.dylib` — the exact chain `CPUInfo` uses (`cpuinfo_arch.c:99-143`) `[OURS]`
  — and cached in `ProcessorBase->Private1`.
- **Load** is a *delta*, so it is read live on each `GetCPUInfo(GCIT_ProcessorLoad)`
  via Mach `host_statistics64`/`host_processor_info`, cleaner behind a tiny native
  **`libhostcpu.dylib`** `[OURS]` (the CoreAudio/host-stats shim precedent) than
  resolving each Mach symbol individually.
- The darwin `getcpuinfo.c` LVO **shadows** the generic one for the darwin arch
  (Pattern A); only this file names the host facts on the AROS side. The shim is the
  only file naming Mach symbols.
- Spike-phase paths: `libhostcpu.dylib` in `hosted/hostcpu/`; at graft, the AROS side
  lands in `arch/all-darwin/processor/`.

## The C ABI (`hostcpu_shim.h`)

Hand-authored, neutral. Verbs mirror the *role* of the in-tree shims' flat-C APIs
(`ca_*`, the bsdsock verbs, `hs_*`). `[PUB]` `sysctl`/Mach under the hood. The shim is
**stateless for the static facts** (it just forwards `sysctlbyname`) and keeps the
**load snapshot** (the one stateful fact). The header is shared source, hand-written,
independent; the shim pulls **no** AROS headers, the AROS side pulls **no** Mach
headers.

```c
typedef struct HCContext HCContext;

/* Open: resolve sysctl + Mach entry points; no host resources held beyond the
   handle. Returns NULL on failure (not hosted / not darwin). */
HCContext *hc_open(void);
void       hc_close(HCContext *);

/* STATIC IDENTITY — filled once, cached by the AROS side.
   brand: machdep.cpu.brand_string (e.g. "Apple M5"). Returns 0 on success;
   nonzero (and brand[0]='\0') if the OID is absent. [PUB] sysctlbyname. */
int hc_brand(HCContext *, char *brand, int brandLen);

/* STATIC TOPOLOGY — hw.ncpu / hw.physicalcpu / hw.logicalcpu, and the P/E split
   hw.perflevel0.physicalcpu (performance) + hw.perflevel1.physicalcpu (efficiency).
   Any field the host lacks is left 0 and reported via the bitmask `*haveMask`
   (bit per field). Returns 0 on success. [PUB] sysctlbyname. */
typedef struct {
    int ncpu, physical, logical, pcores, ecores;
} HCTopology;
int hc_topology(HCContext *, HCTopology *out, unsigned *haveMask);

/* STATIC MISC — hw.byteorder (1234=LE/4321=BE), hw.cachelinesize and the cache
   sizes in BYTES (caller converts to kB for GCIT_*CacheSize). Absent fields 0. */
typedef struct {
    int byteorder, cachelineBytes;
    unsigned long long l1iBytes, l1dBytes, l2Bytes, l3Bytes;
} HCMisc;
int hc_misc(HCContext *, HCMisc *out, unsigned *haveMask);

/* STATIC FEATURES — query a named hw.optional[.arm] OID DEFENSIVELY: returns 1 if
   the OID exists and is nonzero, 0 if it exists and is zero, and -1 if the OID is
   ABSENT (never assume a name exists — names vary per chip/OS). [PUB] sysctlbyname. */
int hc_feature(HCContext *, const char *oidName);   /* e.g. "hw.optional.neon" */

/* LIVE LOAD — the one stateful fact. Returns cumulative CPU ticks (NOT a percentage)
   for the whole machine and up to maxCores per-core, plus a monotonic timestamp in ns
   (mach_absolute_time/mach_timebase_info) so the AROS side computes
   load = 1 - dIdle/dTotal over the true elapsed host time. *outCores = core count.
   Returns 0 on success. [PUB] host_statistics64(HOST_CPU_LOAD_INFO) /
   host_processor_info(PROCESSOR_CPU_LOAD_INFO); vm_deallocate the per-core array. */
typedef struct { unsigned long long user, system, idle, nice; } HCTicks;
int hc_cpu_ticks(HCContext *, HCTicks *whole, HCTicks *perCore, int maxCores,
                 int *outCores, unsigned long long *outTimestampNs);
```

The static-fact verbs (`hc_brand`/`hc_topology`/`hc_misc`/`hc_feature`) may, at the
owner's option, be resolved **directly** as `sysctlbyname` via `HostLib_GetPointer`
on the AROS side (exactly as `CPUInfo` does, no shim needed for them); only the Mach
**load** path (`hc_cpu_ticks`) needs the native dylib. **Default: ship one
`libhostcpu.dylib` for all of it** so the AROS side has a single resolution path and
the load snapshot lives in the shim. See R-RESOLVE.

## The host facts — required behaviour (`[PUB]`)

Each is a `[PUB]` requirement (Apple `sysctl`/Mach API + the macOS SDK headers + the
Arm ARM for what is EL0-unreadable). All read-only; no offsets hard-coded.

**R-BRAND `[PUB]`.** `GCIT_ModelString` ← `sysctlbyname("machdep.cpu.brand_string",
…)`. On this Mac this returns "Apple M5" (verified live). On absent OID, fall back to
the constant `"Unknown"` (the generic stub's value, `getcpuinfo.c:160`) so behaviour
degrades to today's. The returned string is owned by the resource (cached copy);
`GetCPUInfo` returns a `CONST_STRPTR` into it (read-only, per the LVO doc
`getcpuinfo.c:50`).

**R-TOPO `[PUB]`.** `GCIT_NumberOfProcessors` keeps `ProcessorBase->cpucount` (=
`KrnGetCPUCount()`, `init.c:21`) as the authoritative count, **cross-checked** against
`hw.logicalcpu`; if they disagree, prefer `cpucount` (it is what the rest of exec
sees) and note the divergence in a debug line. `hw.physicalcpu`/`hw.logicalcpu` and
the P/E split (`hw.perflevel0.physicalcpu` = performance, `hw.perflevel1.physicalcpu`
= efficiency; live: 4 P + 6 E on a 10-core M5) are cached for the model detail and the
`[CP1]` `P+E == physical` invariant. Headers: `<sys/sysctl.h>`.

**R-ENDIAN `[PUB]`.** `GCIT_Endianness` ← `hw.byteorder`: `1234 → ENDIANNESS_LE`,
`4321 → ENDIANNESS_BE` (`processor.h:154-156`). On absent OID, fall back to
`ENDIANNESS_DEF` (`defaults.h:4-5`, = LE for this build). Live: 1234.

**R-ARCH `[DERIVED]`, restated `[AROS]`.** `GCIT_Architecture` ← `PROCESSORARCH_ARM`
(`processor.h:151`). Justification: `[AROS]` the generic `defaults.h:24-28` already
maps `__aarch64__ → PROCESSORARCH_ARM` ("AArch64 is the 64-bit execution state of the
ARM architecture; there is no separate PROCESSORARCH_ value"), and `arm-native`'s real
backend returns `PROCESSORARCH_ARM` too (`arm-native/getcpuinfo.c:87`). We
independently confirm reporting ARM (not inventing a constant) is the consistent
choice; the 64-bit distinction is carried by R-BRAND + the CPUInfo AArch64-facts
block. **`[DERIVED]` flag:** whether a `PROCESSORARCH_ARM64` should be *added* is an
owner call on a public header — **UNVERIFIED / out of scope here**.

**R-VEC `[PUB]`/`[AROS]`.** `GCIT_VectorUnit` ← `VECTORTYPE_NEON` (`processor.h:143`)
because NEON/Advanced-SIMD is mandatory in the AArch64 AROS ABI
(`cpuinfo_arch.c:226`) and `hw.optional.neon == 1` (verified live).

**R-FEAT `[PUB]`.** Boolean feature tags (`GCIT_FeaturesBase < tag ≤
GCIT_FeaturesLast`, `processor.h:52,96`) handled as a group like
`arm-native/getcpuinfo.c:111`. Map only the tags that **exist**:
`GCIT_SupportsFPU`/`GCIT_SupportsNeon`/`GCIT_SupportsVFP` ← `hw.optional.neon`
(NEON implies FP on AArch64); `GCIT_SupportsAES` ← `hw.optional.arm.FEAT_AES`. **Query
each OID defensively** (R-FEAT2). Every other feature tag returns `FALSE` (the stub
default, `getcpuinfo.c:149`) — most AArch64 features (`FEAT_LSE`, `FEAT_SHA*`,
`FEAT_CRC32`, …) have **no** `GCIT_*` tag, so they are simply not surfaced. Headers:
`<sys/sysctl.h>`.

**R-FEAT2 `[DERIVED]`, restated `[PUB]`/`[OURS]`.** Feature-OID names **vary by chip
and OS version** — verified live: this M5 exposes 79 `hw.optional.arm.FEAT_*` OIDs but
`hw.optional.arm.FEAT_FP` is *absent*, and `hw.optional.AdvSIMD` does not exist while
`hw.optional.neon` does. So the shim must treat a missing OID as "feature FALSE"
(`hc_feature` returns -1) and **never** assume a name is present. Justification:
`[PUB]` Apple documents `hw.optional.*` as capability flags that are added over
releases; `[OURS]` the live enumeration above. We independently derived the
defensive-query rule; implement from the OID-absence behaviour, not from any
reference. `[CP1]` asserts `hw.optional.neon == 1`.

**R-CACHE `[PUB]`.** `GCIT_CacheLineSize` ← `hw.cachelinesize` (bytes);
`GCIT_L1DataCacheSize`/`GCIT_L1InstructionCacheSize`/`GCIT_L2CacheSize`/
`GCIT_L3CacheSize` ← `hw.l1dcachesize`/`hw.l1icachesize`/`hw.l2cachesize`/
`hw.l3cachesize` **converted bytes→kB** (the LVO doc states cache sizes are in kB,
`getcpuinfo.c:81`; `CacheLineSize` is in bytes, `:83`). `GCIT_L1CacheSize` = L1D + L1I
(matching `all-pc/getcpuinfo.c:72-74`). Absent OID → 0 (the stub default). On Apple
Silicon some cache OIDs differ per perflevel; absent ones stay 0 (honest), not faked.

**R-LOAD `[PUB]`.** `GCIT_ProcessorLoad` is the **only stateful** tag. Whole-machine
ticks via `host_statistics64(mach_host_self(), HOST_CPU_LOAD_INFO,
(host_info64_t)&load, &count)` → `load.cpu_ticks[CPU_STATE_{USER,SYSTEM,IDLE,NICE}]`;
per-core via `host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n,
&info, &cnt)` then `vm_deallocate(mach_task_self(), info, …)`. The shim returns
**cumulative** ticks + a monotonic ns timestamp; the AROS side computes the interval
fraction `f = 1 − Δidle/Δtotal` (per selected core, or whole machine) and converts to
the **16.16 fixed-point** the tag wants (range `0..0xffffffff`, `getcpuinfo.c:70`):
`*data = (ULONG)(f * 65536.0) << 16` — equivalently `(ULONG)(f * 0xffffffff)` clamped
— such that SysMon's decode `((v>>16)*1000)>>16` (`processor_gauge.c:144`,
`system-monitor/design.md`) yields tenths-of-a-percent in `0..1000`. Headers:
`<mach/mach_host.h>`, `<mach/processor_info.h>`.

**R-LOADSTATE `[DERIVED]`, restated `[PUB]`/`[OURS]`.** The backend caches the
previous tick snapshot (per core + whole) and timestamp in `Private1`. The **first**
`GCIT_ProcessorLoad` call has no prior snapshot → seed the cache and return **0** (a
single-sample load is undefined). Subsequent calls return the delta over the true
elapsed host time (the ns timestamp, not the AROS clock). Guard: Δtotal == 0 (two
reads same instant) → return the last value, not a divide-by-zero; counter values are
monotone (kernel cumulative) so no wrap handling beyond an unsigned-subtract is
needed. Justification: `[PUB]` `HOST_CPU_LOAD_INFO` is documented cumulative;
`[OURS]` SysMon polls at 250 ms so steady-state is accurate from the 2nd sample. We
independently derived the seed-and-zero rule; implement from the cumulative-counter
semantics. `[CP2]` asserts the math + guards.

**R-SPEED `[PUB]`.** `GCIT_ProcessorSpeed` ← `hw.cpufrequency` **if the OID exists**;
verified live it is **absent on Apple Silicon** (`hw.cpufrequency` = unknown OID;
`hw.tbfrequency` = 24 MHz is the timebase, *not* the core clock). So on Apple Silicon
this honestly stays **0** (the stub default, `getcpuinfo.c:192`). Do **not** report
`hw.tbfrequency` as the CPU speed. `GCIT_FrontsideSpeed` stays 0.

**R-FAMILY `[PUB]`/`[AROS]`.** `GCIT_Family` ← `CPUFAMILY_UNKNOWN`
(`processor.h:99`): the `CPUFAMILY_ARM_*` enum stops at `ARM_7` (`processor.h:115-123`)
and has no Apple-Silicon value; inventing one is out of scope (R-ARCH rationale).
`GCIT_Vendor` may be `ARM_VENDOR_*` if derivable, else left as the stub default;
brand-string-derived vendor is **UNVERIFIED** — default leave untouched.

**R-RO `[PUB]`.** Every call — `sysctlbyname`, `host_statistics64`,
`host_processor_info`, `mach_host_self`, `mach_absolute_time` — is **read-only and
prompt-free**: no entitlement, no TCC (not camera/mic/screen-recording/automation).
This is what keeps the feature inside the unattended loop. (Same posture `CPUInfo`
already relies on, `cpuinfo_arch.c:61-63`.)

## AROS side — the arch backend (`[AROS]`, contract from [design.md](design.md))

A new directory `arch/all-darwin/processor/`, files mirroring the in-tree precedents:
`getcpuinfo.c` + `processor_arch_intern.h` (from `arch/arm-native/processor/`),
`processor_hostlib.c` + `processor_hostcpu.c` (from `arch/all-linux/processor/`), and
`mmakefile.src` (the build seam).

### The `GetCPUInfo` LVO (`getcpuinfo.c`) — Pattern A, mirror `arm-native`

**R-LVO `[AROS]`.** Provide the darwin `GetCPUInfo` with the exact LVO signature
`AROS_LH1(void, GetCPUInfo, AROS_LHA(struct TagItem *, tagList, A0), struct
ProcessorBase *, ProcessorBase, 1, Processor)` (`arm-native/getcpuinfo.c:23-26`,
declared `rom/processor/processor.conf:17`). Read the cached
`struct DarwinProcessorInformation *` from `ProcessorBase->Private1`
(`processor_intern.h:25`); clamp `GCIT_SelectedProcessor` to `[0, cpucount)`
(`arm-native/getcpuinfo.c:37-44`); walk the tags; route the feature range
(`tag > GCIT_FeaturesBase && tag ≤ GCIT_FeaturesLast`) to a `ProcessFeaturesTag`
helper (`arm-native/getcpuinfo.c:49-54`). Fill per the R-* requirements above. The
file structure is copied from `arm-native/getcpuinfo.c`; the *values* come from the
host facts, not from `/proc` or CPUID. `[AROS]` for the LVO shape.

### The cached arch info (`processor_arch_intern.h`) — mirror `arm-native`

**R-PRIV `[AROS]`.** Define `struct DarwinProcessorInformation` (the analogue of
`struct ARMProcessorInformation`, `arm-native/processor_arch_intern.h`): brand string
buffer, `HCTopology` (ncpu/physical/logical/pcores/ecores), `HCMisc`
(byteorder/cacheline/cache sizes), a feature bitmask (NEON/AES/…), and the **load
snapshot** (`HCTicks whole + per-core array + timestamp`). `ProcessorBase->Private1`
points at a per-CPU array of these (or one shared struct + per-core load), allocated
in init. `[AROS]`.

### Init — resolve the host + cache the static facts (`processor_hostlib.c`)

**R-INIT `[AROS]`.** An `ADD2INITLIB` hook (mirror `all-linux/processor_hostlib.c:71-93`):
`HostLibBase = OpenResource("hostlib.resource")`; if NULL (not hosted) **leave the
generic-stub values** (do not fail — degrade to today's behaviour). Else
`HostLib_Open("libhostcpu.dylib", &err)` (or `"libSystem.dylib"` for the static
verbs); resolve the `hc_*` (or `sysctlbyname`) pointers into a function table
(`HostLib_GetPointer`, `all-linux/processor_hostlib.c:58-66`). Then, **once**, run the
static queries (R-BRAND/R-TOPO/R-ENDIAN/R-CACHE/R-FEAT) under `HostLib_Lock`/`Unlock`
and cache into `Private1` (allocated here). `ADD2EXPUNGELIB` closes the handle
(`all-linux/processor_hostlib.c:83-91`). The static facts are queried under one lock
batch, then released — the same "gather under one lock, no AROS I/O while held"
discipline `CPUInfo` uses (`cpuinfo_arch.c:114-140`). `[AROS]`.

**R-HOSTLOCK `[AROS]`/`[OURS]`.** Every host call (init batch and the live load read)
is bracketed `HostLib_Lock(); … ; HostLib_Unlock();` (the established serialisation,
`cpuinfo_arch.c:116,140`). `sysctlbyname` is **non-variadic**, so **no `abishim.S`
variadic shim** is needed (`[OURS]`: `hosted/abishim.S` is only for true varargs host
calls; `sysctl` takes a fixed arg count, as `CPUInfo` relies on,
`cpuinfo_arch.c:87`). The Mach calls are likewise fixed-arg. `AROS_HOST_BARRIER` is
**not** used on aarch64 (per the project memory "no AROS_HOST_BARRIER on aarch64").

### The host queries (`processor_hostcpu.c`)

**R-QUERY `[AROS]`+`[PUB]`.** Implements the static-fact and load reads against the
resolved function table — the darwin analogue of `all-linux/processor_hostcpu.c` (the
`/proc/cpuinfo` reader). For the static facts, thin `sysctlbyname` wrappers
(R-BRAND/R-TOPO/R-ENDIAN/R-CACHE/R-FEAT). For load, the `host_statistics64`/
`host_processor_info` delta (R-LOAD/R-LOADSTATE), updating the snapshot in `Private1`.
If the host facts live behind `libhostcpu.dylib`, this file just calls the `hc_*`
verbs; if the static facts are resolved as raw `sysctlbyname`, this file builds the
OID strings and parses the results. `[AROS]` for the file role, `[PUB]` for the calls.

### The build seam (`mmakefile.src`) — the cross-cutting change

**R-BUILD `[AROS]`, the load-bearing wiring.** Register the backend for the darwin
arch, mirroring `all-linux/processor/mmakefile.src:11`:

```make
include $(SRCDIR)/config/aros.cfg
USER_INCLUDES := -I$(SRCDIR)/rom/processor
FILES := getcpuinfo processor_hostcpu processor_hostlib
%build_archspecific mainmmake=kernel-processor modname=processor maindir=rom/processor \
    arch=darwin files=$(FILES)
%common
```

`arch=darwin` matches `configure`'s `aros_target_arch="darwin"`
(`configure:10807`). `%build_archspecific` (`config/make.tmpl:3252-3263`) registers
`kernel-processor-darwin`, whose objects merge into the `kernel-processor` module
(`rom/processor/mmakefile.src:10`). Including `getcpuinfo` here makes the darwin
`GetCPUInfo` **replace** the generic one (Pattern A). **UNVERIFIED until a link**
(R-BUILD-VERIFY): that the darwin metatarget pulls `kernel-processor-darwin`; that an
`arch/all-darwin/processor/` dir is scanned for this target; that the darwin
`getcpuinfo.o` shadows `rom/processor/getcpuinfo.o` without a duplicate-symbol clash.
`[CP4]` is exactly this assertion (link map shows darwin objects, not the generic
getter). If shadowing clashes, fall back to **Pattern B′**: drop `getcpuinfo` from
`FILES`, keep the generic getter, and add a model/feature/load **override hook** to
`rom/processor/getcpuinfo.c` (a `WeakAlias`/function-pointer the arch helper fills) —
documented in design.md; a change to the shared getter, so higher review bar.

### Folding in the CPUInfo hack — `[AROS]`/`[OURS]`

**R-FOLD `[OURS]`.** Once the backend serves R-BRAND, **delete**
`print_host_cpu_info()` and its `#if defined(__aarch64__)` host block from
`workbench/c/CPUInfo/cpuinfo_arch.c` (the function `:89-161`; the call `:231-232`; the
`#include <proto/hostlib.h>` `:85` and the `sysctlbyname_t` typedef `:87`). The
**unconditional** `processor.resource` block (`:213-216`) then prints the real
`GCIT_ModelString`; the AArch64 ABI-facts block (`:219-226`) stays. CPUInfo's deps
shrink to `dos.library` + `processor.resource` (no direct `hostlib`). This is the
"this block can move into it unchanged" outcome the comment predicts
(`cpuinfo_arch.c:77-79`). `[OURS]` — CPUInfo's host query is this project's spike; we
own moving it home. Verified by `[CP5]` (CPUInfo prints the model from the resource
block, ShowConfig no longer says "Unknown").

## Verification (unattended — `[OURS]` discipline)

No human reads a value; no TCC prompt (R-RO). The oracle is **numeric**: real values
plausible, and `GetCPUInfo` equals the raw `sysctl`. The markers, one host binary
each, `[CP?]` PASS/FAIL via `harness/run-hosted.sh`, clean-exit on PASS:

**The assertions** (every marker asserts *values*, never "it didn't crash"):

- **Plausibility (real host):** `hw.physicalcpu ≤ hw.logicalcpu`, both `> 0`;
  `perflevel0.physicalcpu + perflevel1.physicalcpu == hw.physicalcpu`;
  `machdep.cpu.brand_string` non-empty; `hw.byteorder == 1234` (LE);
  `0 ≤ load ≤ 100` over a ~200 ms interval, per-core count `== hw.logicalcpu`;
  `hw.optional.neon == 1`; address width 64 (`sizeof(void*)*8`).
- **Equality across the boundary:** `GetCPUInfo` `GCIT_ModelString` byte-equals
  `machdep.cpu.brand_string`; `GCIT_NumberOfProcessors == hw.logicalcpu`;
  `GCIT_Endianness == ENDIANNESS_LE`.
- **Load math:** cumulative→fraction→16.16 correct over a scripted tick sequence;
  all-idle → ~0, all-busy → ~full; SysMon decode lands in `0..1000`; first-sample → 0
  (R-LOADSTATE); zero-elapsed guard holds.

**Markers:**

- **[CP1] host CPU facts are plausible (pure host probe).** No AROS: call every
  `sysctl`/Mach query directly, assert the plausibility invariants. Grounds the query
  layer + assert harness (like audio `[A1]`, SysMon `[SM1]`). `[CP1]`.
- **[CP2] load delta + 16.16 scaling.** Drive the cumulative→fraction→16.16 conversion
  over a scripted tick sequence; assert range, idle/busy extremes, SysMon-decode
  bounds, first-sample-zero, zero-elapsed guard. No AROS. `[CP2]`.
- **[CP3] same values through the `hostlib` boundary.** Build `libhostcpu.dylib`,
  resolve it the way AROS will, call the `hc_*` verbs across the dylib boundary, assert
  the values **equal** the raw `sysctl`/Mach values from `[CP1]` (the brief's "same
  values the raw sysctl returns", at the ABI boundary, pre-AROS). Mirrors
  `coreaudio-abi`/`hoststats-abi`. `[CP3]`.
- **[CP4] graft: the darwin arch backend builds and links.** Build `kernel-processor`
  for darwin-aarch64 with `arch/all-darwin/processor/` present. PASS = the link map
  shows the darwin `getcpuinfo.o`/`processor_hostcpu.o`/`processor_hostlib.o` and
  **not** the generic `rom/processor/getcpuinfo.o` (grep the map — the host-volume
  `[V0]` overlay-not-dummy check). Rides the crosstools graft. `[CP4]`.
- **[CP5] graft: real values in booted AROS.** Boot windowed; run a probe (or
  `CPUInfo`/`ShowConfig`) from `S/Startup-Sequence`; assert two-sided that
  `GCIT_ModelString` equals `machdep.cpu.brand_string` read independently host-side,
  `GCIT_NumberOfProcessors == hw.logicalcpu`, `GCIT_ProcessorLoad` decodes to `0..100`,
  arch name "ARM", endianness LE; and that `CPUInfo` prints the model in its
  `processor.resource` block (post-R-FOLD) and `ShowConfig` no longer prints
  "Unknown". Full thesis end-to-end. `[CP5]`.

The render check (CPUInfo/ShowConfig output) uses the
[control harness](../control-harness/README.md) (`aros-ctl run`/`shot` off the
offscreen oracle, TCC-free); the numeric serial assertions are the gate, the
screenshot a secondary confirmation. Markers unique per spike.

## Build / integration

- `libhostcpu.dylib` links **CoreFoundation only if** the optional cache-per-perflevel
  path needs it; the CPU/load/static paths need **no** framework beyond libSystem
  (Mach + `sysctl` are in libSystem). Built with host clang `-arch arm64`, ad-hoc
  codesigned (confirm vs. the existing `harness/run*.sh` / `coreaudio-dylib` signing
  path — **UNVERIFIED**), loaded via `hostlib.resource`. Exports in
  `hosted/hostcpu/hostcpu.exports` (the `_hc_*` symbols), mirroring
  `hosted/coreaudio/coreaudio.exports`. `[OURS]`.
- Add `make hostcpu-dylib` / `make hostcpu-abi` (host-side `[CP1]`–`[CP3]` through the
  dylib boundary, like `make coreaudio-abi`) to the `Makefile`; deploy the dylib to
  `~/lib` via `run-window.sh`/`aros-ctl`/`deploy-check` and bundle into
  `Daedalos.app/Contents/Frameworks/` via `make-aros-app.sh` (the CoreAudio
  precedents). `[OURS]`.
- AROS side: new files in `arch/all-darwin/processor/{getcpuinfo.c,
  processor_hostcpu.c,processor_hostlib.c,processor_arch_intern.h,mmakefile.src}`,
  built by the **AROS crosstools** as part of `kernel-processor` — **not** host clang.
  The CPUInfo fold-in edits `workbench/c/CPUInfo/cpuinfo_arch.c`. `[AROS]`.
- The C ABI header is shared, hand-written, independent work. The shim must not link
  or include AROS headers; the AROS side must not include Mach headers.

## Open questions / UNVERIFIED

- **`PROCESSORARCH_ARM64`** — none exists (`processor.h:147-151`); default report
  `PROCESSORARCH_ARM` (R-ARCH). Adding a public constant is owner-gated and out of
  scope. **UNVERIFIED.**
- **The arch-module build wiring (biggest risk)** — that `kernel-processor` for
  darwin-aarch64 pulls `arch/all-darwin/processor/` (`arch=darwin`) and a darwin
  `getcpuinfo` cleanly shadows the generic getter. Asserted by `[CP4]`; Pattern B′
  (generic override hook) is the fallback. **UNVERIFIED until a link.**
- **`GCIT_ProcessorSpeed`** — `hw.cpufrequency` absent on Apple Silicon (verified
  live); default 0, do not report `hw.tbfrequency`. **UNVERIFIED** whether any per-perf-
  state OID gives a usable max freq; default leave 0.
- **Feature-OID names** — vary per chip/OS (`FEAT_FP` absent here, 79 others present;
  `hw.optional.neon` reliable). Query defensively (R-FEAT2). **UNVERIFIED** the full
  OID→`GCIT_*` map; only NEON/FP/AES have destination tags.
- **`CPUFAMILY_ARM_*` / `GCIT_Vendor`** — no Apple-Silicon family enum; default
  `CPUFAMILY_UNKNOWN`, vendor left as stub. **UNVERIFIED.**
- **Resolve static facts as raw `sysctlbyname` vs. via `libhostcpu.dylib`** — default
  one dylib for all (single resolution path + load snapshot in the shim); raw
  `sysctlbyname` (CPUInfo-style) is the lighter alternative for the static facts only.
  R-RESOLVE.
- **Codesign/entitlement for a `dlopen`'d dylib** in the hosted process — confirm vs.
  `harness/run.sh` / the CoreAudio dylib path. **UNVERIFIED.**
- **De-dup with SysMon** — default: SysMon's Host-page CPU% consumes
  `processor.resource` `GCIT_ProcessorLoad` (this backend) once it lands, so load is
  computed in one place. Owner confirms (design.md §"Division of responsibility").

## Provenance summary

`[PUB]` Apple `sysctl(3)`/`sysctlbyname` OIDs — `machdep.cpu.brand_string`,
`hw.ncpu`/`hw.physicalcpu`/`hw.logicalcpu`, `hw.perflevel0/1.physicalcpu`,
`hw.byteorder`, `hw.cachelinesize`/`hw.l{1i,1d,2,3}cachesize`, `hw.optional.neon`,
`hw.optional.arm.FEAT_*`, `hw.cpufrequency` (absent on Apple Silicon),
`hw.tbfrequency`; Mach `host_statistics64(HOST_CPU_LOAD_INFO)` /
`host_processor_info(PROCESSOR_CPU_LOAD_INFO)` / `mach_host_self` / `vm_deallocate` /
`mach_absolute_time`+`mach_timebase_info` (`<sys/sysctl.h>`, `<mach/mach_host.h>`,
`<mach/processor_info.h>`) — all read-only, no entitlement, no TCC; the Arm ARM for
the EL1 ID-register semantics (`MIDR_EL1`, `ID_AA64*`) that are EL0-unreadable and so
motivate the host route; POSIX. ·
`[AROS]` `rom/processor/{getcpuinfo.c (GetCPUInfo LVO :23-135, stub model :160 / load
:195 / speed :192 / feature-range :146, load-range doc :70, cache-kB doc :81),
init.c (cpucount=KrnGetCPUCount :21), processor_intern.h (struct ProcessorBase +
Private1 :20-26), defaults.h (PROCESSORARCH_DEF=ARM :24-28, ENDIANNESS_DEF :4-5),
processor.conf (LVO :17), mmakefile.src (kernel-processor module :10)}`;
`arch/arm-native/processor/{getcpuinfo.c (LVO shape :23, selectedproc clamp :37,
feature group :49, PROCESSORARCH_ARM :87), processor_arch_intern.h (struct
ARMProcessorInformation)}`; `arch/all-pc/processor/getcpuinfo.c (Private1 :32,
ProcessFeaturesTag :121, L1=L1D+L1I :72)`; `arch/all-linux/processor/{processor_hostlib.c
(HostLib_Open :53, GetPointer :58, ADD2INITLIB :93, expunge :83), processor_hostcpu.c
(host-fact reader shape), mmakefile.src (arch=linux, %build_archspecific :11)}`;
`compiler/include/resources/processor.h` (`GCIT_*` :24-48, feature range :52-96,
`PROCESSORARCH_*` :147-151 — **no ARM64**, `ENDIANNESS_*` :154-156, `VECTORTYPE_NEON`
:143, `CPUFAMILY_ARM_*` :115-123, `ARM_VENDOR_*` :173-178); `config/make.tmpl`
(`%build_archspecific` :3252-3307); `configure` (darwin arch/family :10807-10808,
aarch64 :~10905); `arch/all-unix/bootstrap/hostlib.h` + `arch/all-hosted/hostlib/`
(`HostLib_Open`/`GetPointer`/`Lock`). ·
`[OURS]` `workbench/c/CPUInfo/cpuinfo_arch.c` (this project's host-query spike —
`print_host_cpu_info` :89-161, the "proper home" comment :65-79, the
`HostLib_Open("libSystem.dylib")` chain :103-143 — folded in by R-FOLD); the host-call
memory ("hostlib.resource → libSystem.dylib → sysctlbyname"; "no AROS_HOST_BARRIER on
aarch64"); `hosted/abishim.S` (variadic shim — *not* used for non-variadic `sysctl`);
the flat-C-ABI dylib pattern (`hosted/coreaudio/{coreaudio.exports}`,
`hosted/bsdsocket/`, `make *-abi`, `~/lib` deploy via `graft/deploy-check`/
`run-window.sh`/`make-aros-app.sh`); the [control harness](../control-harness/README.md)
for the windowed `[CP5]` screenshot; `harness/run-hosted.sh` marker harness; the
two-sided unattended-loop discipline. ·
`[DERIVED]` independently-derived points flagged for extra verification:
(a) reporting `PROCESSORARCH_ARM` rather than inventing a constant (R-ARCH),
(b) the defensive feature-OID query (R-FEAT2), and (c) the load seed-and-zero +
zero-elapsed guard (R-LOADSTATE) — each restated from Apple's `sysctl`/Mach docs
`[PUB]` + the in-tree generic/arm-native defaults `[AROS]` + the live OID enumeration
`[OURS]`. No third-party code, identifiers, or call sequence used.
