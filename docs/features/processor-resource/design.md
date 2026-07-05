# processor.resource (darwin/AArch64) — host-sourced CPU identity, features, topology & load

> Status: started (partial) - the host CPU shim (hosted/hostcpu/) is built, green and tested (cp_abi_test); the AROS-side processor.resource arch backend is the remaining graft step. · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

Give the hosted AROS the **truth about the CPU it is actually running on**. On bare
metal an AROS `processor.resource` arch backend reads the CPU identity, feature set,
topology and load out of EL1 system registers (`MIDR_EL1`, the `ID_AA64*` ID
registers, the per-core idle accounting in the kernel heartbeat). On the hosted
Apple-Silicon port AROS runs as an ordinary **EL0 process**, so those registers are
**not readable** — every one of them traps. The facts have to come from the host
instead, through `sysctl` / Mach `host_*` calls, which is exactly the project thesis:
*"macOS owns the drivers; AROS reaches them via standard exec I/O."* macOS owns the
silicon; AROS reaches the CPU facts through `hostlib.resource` → `libSystem.dylib` →
`sysctlbyname` / `host_statistics64`.

This is a **foundation** feature, not a gadget. `processor.resource` is the
architecture-neutral source every CPU query in AROS flows through: `ShowConfig`,
`SysMon`'s CPU page, `CPUInfo`, and any app that calls `GetCPUInfo()`. Today, on this
target, that source is the **generic stub** in `rom/processor/getcpuinfo.c` — it
returns `"Unknown"` for the model, `0` for speed/cache/load, and `PROCESSORARCH_ARM`
only because `defaults.h` hard-codes it from the compiler `#define`. So `ShowConfig`
prints "Unknown", `SysMon`'s CPU load plots a flat `0.0 %`, and only the private hack
inside `CPUInfo` (see below) shows the real "Apple M…". This doc plans the proper home
for that hack: a **darwin/AArch64 arch backend for `processor.resource`** that fills
the getters from the host, so *every* caller — not just `CPUInfo` — sees the real CPU.

The one design point that earns this its own doc is the **arch-module seam**: AROS's
`processor.resource` is built by *replacing or extending* `rom/processor` with an
arch-specific module wired into the `kernel-processor` mmake target. There are **two
distinct in-tree patterns** for doing that (all-pc/arm-native *replace* `GetCPUInfo`;
all-linux only *adds* a host helper and keeps the generic getter) — and the existing
`CPUInfo` comment explicitly warns this is "a cross-cutting change to a core
resource". Picking the right pattern, and wiring it for `arch=darwin`, is the bulk of
the work below.

## Division of responsibility vs. the System monitor doc

This overlaps deliberately with [system-monitor](../system-monitor/design.md); the
line between them is firm:

- **This doc OWNS the `processor.resource` layer** — CPU *identity* (model string,
  vendor, family), *features* (NEON/FP, crypto, the `GCIT_Supports*` set), *topology*
  (`GCIT_NumberOfProcessors`, P/E split), *speed*, and *load*
  (`GCIT_ProcessorLoad`). It fixes the data **at the source** so `GetCPUInfo()`
  returns real values to *all* callers (`ShowConfig`, `CPUInfo`, `SysMon`).
- **SysMon OWNS the GUI** — the new Network/Disk/Host pages and the `libhoststats.dylib`
  shim that feeds whole-machine + per-interface telemetry into a MUI graph. SysMon's
  design notes the load-bearing fact that `GCIT_ProcessorLoad` returns 0 on this
  target and routes its **Host page** around it with its own host-stats shim.

**De-duplication rule (decided).** CPU **load** is wanted in both places and is read
two ways from the *same* Mach API (`host_statistics64(HOST_CPU_LOAD_INFO)` /
`host_processor_info(PROCESSOR_CPU_LOAD_INFO)`). To avoid two shims computing the
same number:

- Once **this** backend lands, `processor.resource`'s `GCIT_ProcessorLoad` returns a
  real value on darwin-aarch64. SysMon's existing **CPU** tab (which already reads
  `GCIT_ProcessorLoad`, `processor_gauge.c:138`) then *just works* with no SysMon
  change — the cleanest outcome, and the reason this is "correctness, not polish".
- The host-call that produces load should live behind **one** shim. The proposal:
  a small **`libhostcpu.dylib`** (this feature) exposes the CPU primitives
  (identity, features, topology, load); SysMon's broader `libhoststats.dylib` either
  **links the same source** or, simpler, SysMon's Host-page CPU% is sourced from
  `processor.resource` (`GetCPUInfo` `GCIT_ProcessorLoad`) once this backend exists,
  so there is exactly one place computing CPU load. The spec states the seam; the
  owner picks "one shim with two consumers" vs "SysMon consumes processor.resource".
  Default: **SysMon's Host CPU consumes `processor.resource`** so load lives in one
  place (this backend), and `libhoststats` keeps only what this backend does *not*
  serve (network counters, disk I/O, host physical-memory).

## Does it already exist?

**No darwin/AArch64 `processor.resource` backend exists** — in this repo or upstream.

In-tree arch backends (`../aros-upstream/arch/*/processor/`):

- `arch/all-pc/processor/` — the **full-replacement** pattern: its own
  `getcpuinfo.c` (a complete `GetCPUInfo` LVO reading a CPUID-derived
  `struct X86ProcessorInformation` from `ProcessorBase->Private1`), plus
  `processor_init.c`/`processor_util.c`/`processor_frequency.c`. Registered with
  `%build_archspecific … arch=i386 files="getcpuinfo processor_init processor_util
  processor_frequency"` (`arch/all-pc/processor/mmakefile.src:17`).
- `arch/arm-native/processor/` — same full-replacement shape for 32-bit ARM
  (`struct ARMProcessorInformation`, `GCIT_Architecture → PROCESSORARCH_ARM`,
  endianness from a feature bit; `getcpuinfo.c:87,90`). Registered `arch=raspi-arm`
  (`mmakefile.src:16`).
- `arch/all-linux/processor/` — **THE hosted precedent**, and a *different* pattern:
  it does **not** replace `getcpuinfo`. It adds only `processor_hostlib.c` +
  `processor_hostcpu.c` (a `hostlib.resource` → `libc.so.6` → `/proc/cpuinfo` reader
  exposing `GetHostProcessorFrequency`) via `ADD2INITLIB`
  (`processor_hostlib.c:93`), registered `%build_archspecific … arch=linux
  files="processor_hostcpu processor_hostlib"` (`mmakefile.src:11`). The generic
  `rom/processor/getcpuinfo.c` getter is kept.
- `arch/all-mingw32/processor/` — the Windows-hosted sibling, same add-a-helper
  shape (`processor_hostlib` + a native `wincpu.dll`), `arch=mingw32`.

There is **no** `arch/all-darwin/processor/`, `arch/all-hosted/processor/`, or any
`aarch64`/`arm64` processor dir (verified: those paths do not exist). So
darwin-aarch64 falls through to the **generic `rom/processor`**, whose `GetCPUInfo`
serves: `GCIT_NumberOfProcessors` = `KrnGetCPUCount()` (real, from `init.c:21`),
`GCIT_Architecture` = `PROCESSORARCH_DEF` = `PROCESSORARCH_ARM` (from `defaults.h:28`,
hard-coded from `__aarch64__`), `GCIT_Endianness` = `ENDIANNESS_LE`
(`defaults.h:4-5`), and **everything else stubbed**: `GCIT_ModelString` = `"Unknown"`
(`getcpuinfo.c:160`), `GCIT_ProcessorSpeed` = `0` (`:192`), `GCIT_ProcessorLoad` = `0`
(`:195`), all caches `0`, `GCIT_Family` = `CPUFAMILY_UNKNOWN`, every feature `FALSE`
(`:149`).

### The existing private hack to fold in — `CPUInfo`

The `CPUInfo` command **already** does the host query this backend should own — and
its own comments name this doc's plan. In
`workbench/c/CPUInfo/cpuinfo_arch.c`, `print_host_cpu_info()` (`:89-161`,
`#if defined(__aarch64__)`):

```c
HostLibBase = OpenResource("hostlib.resource");   /* :99  not hosted -> bail */
libc        = HostLib_Open("libSystem.dylib", …); /* :103 not darwin -> bail */
sc          = HostLib_GetPointer(libc, "sysctlbyname", …);   /* :107 */
HostLib_Lock();                                   /* :116  one lock, then print */
sc("machdep.cpu.brand_string", brand, …);         /* :119  "Apple M…" */
sc("hw.physicalcpu", &phys, …);                    /* :123 */
sc("hw.logicalcpu", &logical, …);                  /* :127 */
sc("hw.perflevel0.physicalcpu", …);                /* :131  P-cores */
sc("hw.perflevel1.physicalcpu", …);                /* :135  E-cores */
HostLib_Unlock();
```

Its header comment (`:65-79`) is explicit that this is a stop-gap and names the fix:

> "The architecturally *proper* home would be a darwin processor.resource backend
> (cf. arch/all-linux/processor/, which reads /proc/cpuinfo via the host libc), so
> that GetCPUInfo() — and hence ShowConfig and every other caller — would see the
> real model too. But the generic rom/processor only serves GCIT_ModelString as the
> constant "Unknown"; surfacing a host-derived model through it means adding a
> model-override hook to rom/processor (getcpuinfo.c + processor_intern.h) AND a new
> arch-specific module wired into the kernel-processor build — a cross-cutting change
> to a core resource. … if a darwin processor.resource backend is added later, that
> line fills in and this block can move into it unchanged."

**This doc turns exactly that paragraph into a buildable plan.** Once the backend
lands, `print_host_cpu_info()` is **deleted** and the `processor.resource` block
above it (which prints `GCIT_ModelString` etc.) automatically shows the real values —
the two layers stay honest, with the host query in its proper home.

### What is EL0-unreadable vs. host-derivable

| Fact | Bare-metal source (EL1) | EL0 hosted? | Host source |
|------|-------------------------|-------------|-------------|
| Model / part / implementer | `MIDR_EL1` | **traps** | `machdep.cpu.brand_string` ("Apple M5") |
| Feature flags (FP/SIMD/crypto) | `ID_AA64ISAR*`, `ID_AA64PFR*` | **traps** | `hw.optional.arm.FEAT_*`, `hw.optional.neon` |
| Core count | `MPIDR_EL1` + firmware | partial | `hw.ncpu`/`hw.physicalcpu`/`hw.logicalcpu` |
| P/E topology | firmware tables | no | `hw.perflevel0.*` / `hw.perflevel1.*` |
| Endianness | `SCTLR_EL1.EE` | inferable from ABI | `hw.byteorder` (1234 = LE) |
| Address width | `ID_AA64MMFR0_EL1` | inferable (`sizeof(void*)`) | — (ABI: 64-bit) |
| CPU load | kernel idle accounting | no | `host_statistics64(HOST_CPU_LOAD_INFO)` |
| CPU frequency | `CNTFRQ`/PMU | no | `hw.cpufrequency` **absent on Apple Silicon** (see Risks) |

The architectural facts that are *true by the ABI this binary is built for* (64-bit
address width, AArch64 execution state, mandatory NEON) need no host call and are the
honest fallback when the host chain is absent — exactly what `CPUInfo` already prints
unconditionally (`cpuinfo_arch.c:219-226`).

## Background: the `processor.resource` contract (grounded)

`processor.resource` is a `.resource` with a single public LVO, `GetCPUInfo()`,
documented at `rom/processor/getcpuinfo.c:23-135` and declared in
`rom/processor/processor.conf:17` (`VOID GetCPUInfo(struct TagItem * tagList) (A0)`).
The base struct is `struct ProcessorBase { struct Library pb_LibNode; unsigned int
cpucount; APTR kernelBase; APTR Private1; }` (`rom/processor/processor_intern.h:20-26`)
— `Private1` is the documented hook for **arch-specific implementation data**, which
the all-pc/arm-native backends use to hang their per-CPU info array.

`GetCPUInfo(tagList)` walks a `TagItem` array; each tag is an out-param. The control
tag is `GCIT_SelectedProcessor` (pick a core; default 0). The tag set this backend
must serve, with values from `compiler/include/resources/processor.h`:

| Tag | # | Generic stub today | This backend (darwin/AArch64) |
|-----|---|--------------------|-------------------------------|
| `GCIT_NumberOfProcessors` | `TAG_USER+1` | `cpucount` (real) | `cpucount` (keep) / cross-check `hw.logicalcpu` |
| `GCIT_ModelString` | `+4` | `"Unknown"` | `machdep.cpu.brand_string` |
| `GCIT_Architecture` | `+104` | `PROCESSORARCH_ARM` | `PROCESSORARCH_ARM` (see UNVERIFIED) |
| `GCIT_Endianness` | `+105` | `ENDIANNESS_LE` | `hw.byteorder` → `ENDIANNESS_LE` |
| `GCIT_ProcessorLoad` | `+106` | `0` | `host_statistics64`/`host_processor_info` Δidle |
| `GCIT_ProcessorSpeed` | `+8` | `0` | `hw.cpufrequency` if present, else 0 (see Risks) |
| `GCIT_Family` | `+2` | `CPUFAMILY_UNKNOWN` | `CPUFAMILY_UNKNOWN` (no AArch64 family enum — UNVERIFIED) |
| `GCIT_VectorUnit` | `+12` | `VECTORTYPE_NONE` | `VECTORTYPE_NEON` (NEON mandatory) |
| `GCIT_Vendor` | `+107` | (untouched) | `ARM_VENDOR_*` / brand-derived (see UNVERIFIED) |
| `GCIT_L1/L2/L3CacheSize` | `+9/10/11` | `0` | `hw.l1icachesize`/`hw.l1dcachesize`/`hw.l2cachesize`/`hw.l3cachesize` (per perflevel) |
| `GCIT_CacheLineSize` | `+14` | `0` | `hw.cachelinesize` |
| `GCIT_Supports*` (feature range `+200..+499`) | `FALSE` | `GCIT_SupportsFPU`/`SupportsNeon` → TRUE; crypto via `hw.optional.arm.FEAT_*` |

Tag-range rule (load-bearing): tags in `(GCIT_FeaturesBase, GCIT_FeaturesLast]` =
`(TAG_USER+200, TAG_USER+499]` are **boolean feature** queries handled as a group
(`getcpuinfo.c:146-151`, `all-pc` routes them to `ProcessFeaturesTag`,
`all-pc/getcpuinfo.c:51-54`). The AArch64 feature tags that already exist are
`GCIT_SupportsFPU`, `GCIT_SupportsVFP*`, `GCIT_SupportsNeon`, `GCIT_SupportsAES`
(`processor.h:53,83-94`).

### The arch-module seam — two in-tree patterns (the decision)

How an arch backend hooks into `processor.resource` is set by `%build_archspecific`
(`config/make.tmpl:3252`), which registers a sub-target `kernel-processor-<arch>`
that contributes object files to the `kernel-processor` module built from
`rom/processor` (`rom/processor/mmakefile.src:10`, `files="init getcpuinfo"`,
`archspecific=yes`). Two shapes exist:

- **Pattern A — replace the getter (all-pc, arm-native).** The arch `mmakefile.src`
  lists `files="getcpuinfo processor_init processor_util processor_frequency"`; the
  arch `getcpuinfo.c` provides a complete `GetCPUInfo` LVO that **shadows** the
  generic one, and an arch `processor_init` fills `ProcessorBase->Private1` with a
  per-CPU info array. Full control of every tag.
- **Pattern B — add a helper, keep the generic getter (all-linux, all-mingw32).** The
  arch `mmakefile.src` lists only host-helper files (`processor_hostlib`,
  `processor_hostcpu`); they register via `ADD2INITLIB`
  (`all-linux/processor_hostlib.c:93`) and export a helper
  (`GetHostProcessorFrequency`). The generic `rom/processor/getcpuinfo.c` getter is
  retained.

**Honest finding (important).** Pattern B as it stands in all-linux is **incomplete
as a model for us**: the generic `getcpuinfo.c` it keeps **never calls**
`GetHostProcessorFrequency` — that helper is dead weight from the generic getter's
point of view. So the Linux backend, today, does *not* actually surface a
host-derived model or speed through `GetCPUInfo`; it only makes the helper *available*.
To make `GetCPUInfo` return host facts we must either (B′) add a **model/feature/load
override hook** into `rom/processor/getcpuinfo.c` that the arch helper fills (the
"model-override hook to rom/processor" the `CPUInfo` comment names — a change to the
generic getter), or (A) ship a darwin `getcpuinfo.c` that replaces the getter
outright.

**Decision (proposed, owner to confirm in spec).** Use **Pattern A** — a darwin
arch `getcpuinfo.c` mirroring all-pc/arm-native — because it is self-contained (no
edit to the shared generic getter, lowest blast radius on a core resource), matches
the closest *structural* precedents, and gives full control of the tag set. Pattern
B′ (a generic override hook) is the documented alternative; it is more invasive to
`rom/processor` but means *every* future hosted target shares one host-fact plumbing.
Either way the **host query lives in a `hostlib`-loaded helper** (the all-linux
`processor_hostlib.c`/`processor_hostcpu.c` split), and only the *getter wiring*
differs. See spec.md for the file-by-file plan.

### Reference points already de-risked in this repo

- **Host-call mechanism** — `OpenResource("hostlib.resource")` → `HostLib_Open(
  "libSystem.dylib")` → `HostLib_GetPointer("sysctlbyname")`, serialised under
  `HostLib_Lock`/`Unlock`. Proven *for this exact data* by `CPUInfo`
  (`cpuinfo_arch.c:99-143`) and by the project memory "Calling host libc from AROS".
  The all-linux backend uses the same `HostLib_Open`/`GetPointer` over `libc.so.6`
  (`processor_hostlib.c:53-66`). Non-variadic `sysctlbyname` needs **no** `abishim.S`
  variadic shim (`hosted/abishim.S` is only for true varargs host calls).
- **The flat-C-ABI `.dylib` shim pattern** — CoreAudio/BSD-socket/host-stats shims
  (`hosted/coreaudio/`, `hosted/bsdsocket/`) show the `make *-abi` build + `~/lib`
  deploy. The Mach `host_*` calls for load are cleaner behind a tiny native
  `libhostcpu.dylib` than resolving each Mach symbol via `HostLib_GetPointer`
  (`mach_host_self`, `host_statistics64`, `host_processor_info`, `vm_deallocate`),
  exactly as SysMon proposes `libhoststats.dylib`.
- **GUI verification** — the [control harness](../control-harness/README.md) boots
  windowed AROS and screenshots `ShowConfig`/`SysMon` off the offscreen oracle (no
  TCC) to confirm the model string renders.

## Design

### Host side (the CPU facts — read-only, no entitlement, no TCC)

All facts come from `libSystem` calls that are read-only and prompt-free. Two groups:

**Plain `sysctlbyname` (resolvable directly via `HostLib_GetPointer`, like CPUInfo):**

- **Identity** — `machdep.cpu.brand_string` → `GCIT_ModelString` (live on this Mac:
  "Apple M5").
- **Topology** — `hw.ncpu`, `hw.physicalcpu`, `hw.logicalcpu` (live: 10/10/10);
  `hw.perflevel0.physicalcpu` / `hw.perflevel1.physicalcpu` (live: 4 P + 6 E). These
  refine `GCIT_NumberOfProcessors` and feed the P/E split the model string implies.
- **Endianness** — `hw.byteorder` (live: 1234 → `ENDIANNESS_LE`).
- **Caches** — `hw.cachelinesize`, `hw.l1icachesize`, `hw.l1dcachesize`,
  `hw.l2cachesize`, `hw.l3cachesize` (sizes in bytes; `GCIT_*CacheSize` wants kB —
  divide by 1024).
- **Features** — `hw.optional.neon` (live: 1) → `GCIT_SupportsNeon`/`SupportsFPU`;
  the `hw.optional.arm.FEAT_*` family (live: 79 of them — `FEAT_AES`, `FEAT_SHA256`,
  `FEAT_LSE`, `FEAT_CRC32`, …) → `GCIT_SupportsAES` and any AArch64 feature tags we
  add. **Note (verified live): `hw.optional.arm.FEAT_FP` is *absent* on this Mac** —
  the exact feature OID names vary by chip/OS, so query defensively (missing OID =
  feature reported FALSE), never assume a name exists.
- **Speed** — `hw.cpufrequency` (**absent on Apple Silicon** — see Risks);
  `hw.tbfrequency` (live: 24000000) is the timebase, not the core clock.

**Mach `host_*` (cleaner behind a tiny `libhostcpu.dylib`):**

- **Load** — `host_statistics64(mach_host_self(), HOST_CPU_LOAD_INFO, …)` returns
  cumulative `cpu_ticks[CPU_STATE_{USER,SYSTEM,IDLE,NICE}]`; whole-machine load over
  an interval is `1 − Δidle/Δtotal`. Per-core via `host_processor_info(…,
  PROCESSOR_CPU_LOAD_INFO, &n, …)` (then `vm_deallocate` the array).
  `GCIT_ProcessorLoad` wants a 16.16 fixed-point fraction (range `0..0xffffffff`,
  `getcpuinfo.c:70`; SysMon decodes it `((v>>16)*1000)>>16`,
  `system-monitor/design.md`), so scale the fraction to 16.16. This is the only
  *stateful* fact: load is a delta between two reads, so the backend keeps the
  previous snapshot (per-core) and a monotonic timestamp (`mach_absolute_time` ÷
  `mach_timebase_info`).

The host helper is **stateless for the static facts** (identity/topology/features are
queried once at init and cached) and keeps only the load snapshot. No host thread, no
callback, no SPSC ring — unlike CoreAudio this is pure pull, so none of the
RT-thread machinery is needed.

### AROS side (the darwin/AArch64 arch backend)

A new arch directory `arch/all-darwin/processor/` mirroring `arch/all-linux/processor/`
for the host-helper split, plus (Pattern A) a darwin `getcpuinfo.c` mirroring
`arch/arm-native/processor/getcpuinfo.c`:

- `processor_hostlib.c` — `ADD2INITLIB` hook: `OpenResource("hostlib.resource")` →
  `HostLib_Open("libSystem.dylib")` (or the native `libhostcpu.dylib`) → resolve the
  `sysctlbyname` (+ Mach) pointers into a function table; on init, run the static
  queries once and cache them into an arch info struct hung off
  `ProcessorBase->Private1`. Mirror `all-linux/processor_hostlib.c:71-93`.
- `processor_hostcpu.c` — the queries themselves (`sysctlbyname` wrappers, the
  `host_statistics64` load delta), all under `HostLib_Lock`/`Unlock`. Mirror
  `all-linux/processor_hostcpu.c` (which is the `/proc/cpuinfo` reader; ours is the
  sysctl reader).
- `getcpuinfo.c` — the darwin `GetCPUInfo` LVO. Reads the cached arch info from
  `Private1`, fills `GCIT_ModelString`/`Architecture`/`Endianness`/`NumberOfProcessors`/
  caches/features, computes `GCIT_ProcessorLoad` from the live delta. Structure copied
  from `arm-native/getcpuinfo.c` (the `selectedprocessor` clamp, the feature-tag
  group, the per-tag switch).
- `processor_arch_intern.h` — a `struct DarwinProcessorInformation` (brand string,
  P/E counts, cache sizes, feature bits, last load snapshot), mirroring
  `arm-native/processor_arch_intern.h`'s `struct ARMProcessorInformation`.
- `mmakefile.src` — the build seam (next section).
- (optional) `libhostcpu.dylib` under `hosted/hostcpu/` for the Mach load calls,
  built `make hostcpu-abi`, deployed to `~/lib` (the CoreAudio precedent).

### The build wiring (the cross-cutting change `CPUInfo` warns about)

This is the load-bearing integration point. Grounded in `configure`: a darwin host
sets `aros_target_arch="darwin"`, `aros_target_family="unix"`
(`configure:10807-10808`), and for Apple Silicon `aros_target_cpu="aarch64"`,
`-arch arm64` (`configure:~10905`). The arch backend must therefore register
**`arch=darwin`**, mirroring how `arch/all-linux/processor/mmakefile.src:11` registers
`arch=linux`:

```make
# arch/all-darwin/processor/mmakefile.src  (Pattern A — replace the getter)
USER_INCLUDES := -I$(SRCDIR)/rom/processor
FILES := getcpuinfo processor_hostcpu processor_hostlib
%build_archspecific mainmmake=kernel-processor modname=processor maindir=rom/processor \
    arch=darwin files=$(FILES)
```

`%build_archspecific` (`config/make.tmpl:3252-3263`) creates the target
`kernel-processor-darwin` whose objects merge into the `kernel-processor` module
built from `rom/processor` (`rom/processor/mmakefile.src:10`). When the top-level
build selects the darwin arch, these files supply `GetCPUInfo` (Pattern A) instead of
the generic `rom/processor/getcpuinfo.c`. **UNVERIFIED until a build links**: that the
darwin metatarget actually pulls `kernel-processor-darwin` (the same
arch-merge question SysMon and host-volume flag); that an `arch/all-darwin/processor/`
dir is scanned by metamake for this target; and that a darwin `getcpuinfo` cleanly
shadows the generic one without a duplicate-symbol clash. The spike `[CP4]` asserts
exactly this (the link map shows the darwin objects, not the generic getter).

### Folding in the CPUInfo hack

Once the backend serves the model string, `print_host_cpu_info()` and the
`#if defined(__aarch64__)` host block in `cpuinfo_arch.c` are **deleted** (the
function `:89-161`, its call `:231-232`). The unconditional
`processor.resource` block (`:213-216`) then prints the real `GCIT_ModelString`,
and the architectural-facts block (`:219-226`) stays as the honest ABI-derived
detail. CPUInfo's dependency shrinks back to `dos.library` + `processor.resource`
(no direct `hostlib`), and the host query lives in one place. This is the explicit
"this block can move into it unchanged" outcome the comment predicts.

## Plan — spikes in the loop

Each marker is a standalone host binary (one-binary-per-marker, like `hosted/*`) with
a single PASS/FAIL verdict the agent greps — no human reads a number. Prefix **`[CP*]`**.

- **[CP1] host CPU facts are plausible (pure host probe).** No AROS. Call every
  `sysctl`/Mach query and assert: `hw.physicalcpu ≤ hw.logicalcpu`, both `> 0` and
  `== hw.ncpu` where expected; `perflevel0.physicalcpu + perflevel1.physicalcpu ==
  hw.physicalcpu` (P+E split); `machdep.cpu.brand_string` non-empty; `hw.byteorder ==
  1234` (LE); `host_statistics64(HOST_CPU_LOAD_INFO)` gives `0 ≤ load ≤ 100` over a
  ~200 ms interval and per-core counts match `hw.logicalcpu`; `neon == 1`. Grounds the
  query layer + assertion harness (like audio `[A1]`). `[CP1]`.
- **[CP2] the load delta and 16.16 scaling.** Drive the cumulative→fraction→16.16
  conversion over a scripted tick sequence; assert `0 ≤ result ≤ 0xffffffff`, that an
  all-idle interval → ~0 and an all-busy interval → ~full, and that SysMon's decode
  `((v>>16)*1000)>>16` of our value lands in `0..1000` tenths. Counter-wrap and
  zero-elapsed guards asserted. No AROS. `[CP2]`.
- **[CP3] same value through `hostlib` boundary.** Build `libhostcpu.dylib`, resolve
  it via `hostlib.resource` the way AROS will, call the verbs across the dylib
  boundary, and assert the values **equal** the raw `sysctl`/Mach values from [CP1]
  (the brief's "GetCPUInfo … returns the SAME values the raw sysctl returns",
  proven at the ABI boundary before AROS is in the loop). Mirrors `coreaudio-abi`.
  `[CP3]`.
- **[CP4] graft: the darwin arch backend builds and links.** Build
  `kernel-processor` for darwin-aarch64 with `arch/all-darwin/processor/` present.
  PASS = the link map shows the darwin `getcpuinfo.o`/`processor_hostcpu.o`/
  `processor_hostlib.o` (overlay) and **not** the generic `rom/processor/getcpuinfo.o`
  (grep the map — the same overlay-not-dummy check host-volume `[V0]` uses). Rides the
  crosstools graft. `[CP4]`.
- **[CP5] graft: `GetCPUInfo` returns the real values in booted AROS.** Boot windowed,
  run a tiny probe (or `CPUInfo`/`ShowConfig`) from `S/Startup-Sequence`, and assert
  the model string from `GCIT_ModelString` equals `machdep.cpu.brand_string` read
  independently host-side (two-sided, like host-volume); `GCIT_NumberOfProcessors ==
  hw.logicalcpu`; `GCIT_ProcessorLoad` decodes to `0..100`; `GCIT_Architecture` arch
  name is "ARM"; `GCIT_Endianness` is LE. Also assert `CPUInfo` now prints the model
  in its `processor.resource` block (not only the deleted host block), and `ShowConfig`
  no longer prints "Unknown" for the CPU. Full thesis end-to-end. `[CP5]`.

Build/run in the existing harness style (`make hostcpu-abi` → `[CP1]`–`[CP3]` via
`harness/run-hosted.sh`; the `[CP4]`/`[CP5]` graft rides the kernel-processor rebuild,
mirroring `make audio-smoke`/`sysmon-smoke`), clean-exit on PASS.

## How we verify it unattended

No human reads a value; no TCC prompt is hit (every call — `sysctlbyname`,
`host_statistics64`, `host_processor_info` — is a read-only Mach/BSD query needing no
entitlement or screen-recording/automation permission). The oracle is **numeric**:

1. **Plausibility (real values).** `[CP1]` asserts the host probe's invariants
   (counts consistent, brand non-empty, `0 ≤ load ≤ 100`, LE, NEON present, 64-bit
   address width). The brief's numeric checklist, one greppable verdict.
2. **Equality across the boundary.** `[CP3]`/`[CP5]` assert that `GetCPUInfo` through
   `processor.resource` returns the **same** values the raw `sysctl` returns —
   model string byte-equal, core counts equal — proving the plumbing is faithful, not
   merely non-zero.
3. **The user-visible fix.** `[CP5]` asserts `CPUInfo`/`ShowConfig` print the real
   model instead of "Unknown" — the owner's "cpuinfo" request, observed end-to-end.

Markers are unique per spike so a regression localises (the `[M*]`/`[H*]`/`[A*]`/
`[SM*]` discipline).

## Risks & open questions

- **`PROCESSORARCH_ARM64` does not exist (the headline UNVERIFIED).**
  `compiler/include/resources/processor.h:147-151` defines only
  `PROCESSORARCH_{UNKNOWN,M68K,PPC,X86,ARM}` — **no AArch64/ARM64 constant**. The
  generic `defaults.h:24-28` already maps `__aarch64__` → `PROCESSORARCH_ARM` with a
  comment ("no separate value; report it as ARM"), and `CPUInfo`'s `arch_name()`
  (`cpuinfo_arch.c:33-43`) only knows M68K/PPC/X86/ARM. **Decision (proposed): report
  `PROCESSORARCH_ARM`** (matches the generic default and CPUInfo, zero new public
  constant, the 64-bit distinction carried by the model string + the AArch64 facts
  block). Adding `PROCESSORARCH_ARM64` is the cleaner-but-API-changing alternative
  (touches a public header + every `arch_name` consumer). **Confirm with the owner
  before adding a constant.** Same question for a `CPUFAMILY_ARM_*` value for
  Apple-Silicon families (`processor.h:115-123` stops at `CPUFAMILY_ARM_7`) — default
  `CPUFAMILY_UNKNOWN`. **UNVERIFIED.**
- **No CPU frequency on Apple Silicon (`GCIT_ProcessorSpeed`).** Verified live:
  `hw.cpufrequency` is an **unknown OID** on this Mac (Apple Silicon does not expose a
  fixed core clock; `hw.tbfrequency` = 24 MHz is only the timebase). So
  `GCIT_ProcessorSpeed` honestly stays `0` (or we report `hw.tbfrequency` and label it
  — *not* the core clock; default: leave `0`). The all-linux backend's
  `GetHostProcessorFrequency` reads `/proc/cpuinfo` "cpu MHz"; there is no sysctl
  equivalent here. **Open:** whether to derive an approximate max freq from the perf
  state (overkill); default leave 0. **UNVERIFIED.**
- **Feature-OID names vary (`hw.optional.arm.FEAT_*`).** Verified live: 79 `FEAT_*`
  OIDs exist on this M5, but `FEAT_FP` is *absent* and the set differs per chip/OS.
  The backend must query each OID **defensively** (missing OID → feature FALSE), never
  hard-code a name's presence. `hw.optional.neon` is the reliable NEON/FP signal.
  **UNVERIFIED** which exact OID names map to which `GCIT_Supports*` tag — most AArch64
  features have **no** `GCIT_*` tag at all (the tag set is x86/ARMv7-centric), so most
  `hw.optional.arm.FEAT_*` have no destination tag and are simply not surfaced;
  `GCIT_SupportsNeon`/`SupportsFPU`/`SupportsAES` are the ones that map.
- **The arch-module build wiring (the cross-cutting risk `CPUInfo` named).** That
  `kernel-processor` for darwin-aarch64 actually pulls a new
  `arch/all-darwin/processor/` (`arch=darwin`) and that a darwin `getcpuinfo` cleanly
  replaces the generic getter is **UNVERIFIED until a link** — asserted by `[CP4]`.
  Pattern B′ (a generic override hook in `rom/processor/getcpuinfo.c`) is the fallback
  if Pattern A's shadowing fights the build. This is the single biggest risk; it is
  the same kernel-processor rebuild + arch-merge unknown the
  [hosted rebuild memory](../darwin-aarch64-port-inventory.md) tracks.
- **Pattern A vs B′ — a core-resource edit either way.** Pattern A keeps the change
  inside `arch/all-darwin/` (lowest blast radius) but duplicates getter boilerplate;
  Pattern B′ edits the shared `rom/processor/getcpuinfo.c` (benefits every hosted
  target, higher review bar). Default A; owner confirms in spec.
- **Load is the only stateful fact.** `GCIT_ProcessorLoad` is a delta, so the backend
  caches the previous tick snapshot + timestamp. If `GetCPUInfo` is called rarely the
  interval is long (fine) — but the *first* call has no prior snapshot, so it returns
  0 (or seeds and returns 0); SysMon polls at 250 ms so steady-state is accurate.
  De-dup with SysMon's Host page resolved above (one place computes load).
- **`hostlib`/Mach resolution.** Plain `sysctlbyname` resolves via
  `HostLib_GetPointer` over `libSystem.dylib` (proven by CPUInfo). The Mach load
  calls (`host_statistics64`, `mach_host_self`) are cleaner behind a native
  `libhostcpu.dylib` (the CoreAudio shim precedent) than resolving each Mach symbol.
  **Open:** resolve Mach symbols directly vs. ship `libhostcpu.dylib` — lean toward
  the dylib for the load path, direct `sysctlbyname` for the static facts (matching
  CPUInfo). **UNVERIFIED** codesign/entitlement for a `dlopen`'d dylib in the hosted
  process — confirm vs. `harness/run.sh` / the CoreAudio dylib path.

## References

AROS upstream (`../aros-upstream`):
- Generic resource: `rom/processor/{getcpuinfo.c (GetCPUInfo LVO :23, stub model
  :160 / load :195 / speed :192, feature-range :146), init.c (cpucount :21),
  processor_intern.h (struct ProcessorBase + Private1 :20-26), defaults.h
  (PROCESSORARCH_DEF=ARM :24-28, ENDIANNESS_DEF :4-5), processor.conf (GetCPUInfo LVO
  :17), mmakefile.src (kernel-processor module :10)}`.
- Arch backends (the two seams): `arch/all-pc/processor/{getcpuinfo.c (full
  replacement, Private1 :32, ProcessFeaturesTag :121), mmakefile.src (arch=i386,
  files :7-11, %build_archspecific :17)}`; `arch/arm-native/processor/{getcpuinfo.c
  (PROCESSORARCH_ARM :87, endian-from-feature :90, structure to copy),
  processor_arch_intern.h (struct ARMProcessorInformation), mmakefile.src (arch=raspi
  :16)}`; `arch/all-linux/processor/{processor_hostlib.c (HostLib_Open libc.so.6
  :53-66, ADD2INITLIB :93, GetHostProcessorFrequency :35), processor_hostcpu.c
  (/proc/cpuinfo reader), mmakefile.src (arch=linux, files :7-9, %build_archspecific
  :11)}`; `arch/all-mingw32/processor/` (wincpu.dll sibling).
- Tags/constants: `compiler/include/resources/processor.h` (`GCIT_*` :24-48, feature
  range :52-96, `PROCESSORARCH_*` :147-151 — **no ARM64**, `ENDIANNESS_*` :154-156,
  `VECTORTYPE_NEON` :143, `CPUFAMILY_ARM_*` :115-123, `ARM_VENDOR_*` :173-178).
- The hack to fold in: `workbench/c/CPUInfo/cpuinfo_arch.c` (`print_host_cpu_info`
  :89-161, the "proper home" comment :65-79, the host-block call :231-232, arch_name
  :33-43, the unconditional resource block :213-216, the ABI-facts block :219-226).
- Build seam: `config/make.tmpl` (`%build_archspecific` :3252-3307); `configure`
  (darwin `aros_target_arch="darwin"` / `family="unix"` :10807-10808, aarch64 / `-arch
  arm64` :~10905). Host-symbol mechanism: `arch/all-unix/bootstrap/hostlib.h`,
  `arch/all-hosted/hostlib/`.

This repo (`.`):
- The host-call memory ("Calling host libc from AROS: hostlib.resource →
  HostLib_Open('libSystem.dylib') → sysctlbyname"); `hosted/abishim.S` (variadic
  shim — *not* needed for non-variadic `sysctlbyname`); the flat-C-ABI dylib pattern
  (`hosted/coreaudio/`, `hosted/bsdsocket/`, `make *-abi`, `~/lib` deploy via
  `graft/deploy-check`/`run-window.sh`); the [control harness](../control-harness/README.md)
  for the windowed `ShowConfig`/`SysMon` screenshot.
- Sibling docs: [system-monitor](../system-monitor/design.md) (consumes this layer for
  CPU load / Host page; de-dup rule above); the
  [port inventory](../darwin-aarch64-port-inventory.md) §1 (CPU/ABI).

External prior art (web — interfaces only, no third-party implementation read):
- Apple `sysctl(3)`/`sysctlbyname` OIDs (`machdep.cpu.brand_string`, `hw.ncpu`/
  `hw.physicalcpu`/`hw.logicalcpu`, `hw.perflevel0/1.*`, `hw.byteorder`,
  `hw.optional.arm.FEAT_*`, `hw.optional.neon`, `hw.cachelinesize`/`hw.l*cachesize`,
  `hw.cpufrequency` [absent on Apple Silicon], `hw.tbfrequency`); Mach
  `host_statistics64(HOST_CPU_LOAD_INFO)`, `host_processor_info(PROCESSOR_CPU_LOAD_INFO)`,
  `mach_host_self`, `mach_absolute_time`/`mach_timebase_info`
  (`<mach/mach_host.h>`, `<mach/processor_info.h>`, `<sys/sysctl.h>`) — all read-only,
  no entitlement. The Arm ARM defines the EL1 ID registers (`MIDR_EL1`, `ID_AA64*`)
  that are EL0-unreadable and so motivate the host route. Interfaces and register
  semantics only; no third-party implementation consulted.
