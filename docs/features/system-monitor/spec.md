# Implementation spec — System monitor host-stats pages (network / disk / host)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent,
driver, or otherwise — was read, searched, or consulted in producing it, and any
resemblance to existing implementations is coincidental.** Implement only from this
spec + the approved sources cited by tag: `[PUB]` Apple framework / Mach / BSD /
POSIX docs and the macOS SDK headers, published MUI/Zune class API; `[AROS]` in-tree
AROS headers and the live SysMon / DOS / `processor.resource` / `hostlib` source
(paths given; APL/LGPL — ours); `[OURS]` this project's spikes and shims
(`hosted/*`, `graft/*`, the H/A-series). `[DERIVED]` items are independently-derived
requirements flagged for extra verification; each stands solely on its cited
`[PUB]`/`[AROS]`/`[OURS]` justification — implement from that justification, never
from any reference. No identifier name, call sequence, file layout, or algorithm in
this spec derives from any third-party implementation.

## Scope

**In.** Extend the existing AROS `SysMon` MUI/Zune app
(`workbench/system/SysMon/`) with **three new tabbed pages** — **Network** (a
bytes/sec throughput graph from host per-interface counters), **Disk** (free/total
per AROS volume, plus optional host disk-I/O rate), and **Host** (the Mac's real CPU
load and physical memory) — fed by a new read-only host-stats shim
**`libhoststats.dylib`** loaded through `hostlib.resource`, with a **graceful
fallback to AROS-native numbers when not hosted**. Ship a numeric oracle (known
values in → page model out) as the unattended gate, and a control-harness screenshot
as a secondary check.

**Decision (host-vs-AROS data — the truthful source per number).** On darwin-aarch64
hosted: **CPU% = HOST** (because AROS `GCIT_ProcessorLoad` returns 0 — the stub, see
below), **network throughput = HOST**, **disk I/O rate = HOST**; **volume free-space
= AROS `Info()`** (the guest's correct view of its own volumes), **guest RAM = AROS
`AvailMem`** (kept on the existing System tab). The new Host page additionally shows
the Mac's installed/used/free RAM. Host-sourced rows are labelled as the Mac's
figures. Full rationale in [design.md](design.md).

**Decision (additive only).** The three pages and the data module are **purely
additive**; the existing Tasks / CPU / System pages and their data sources
(`processor.c`, `memory.c`, `video.c`, `tasks.c`) are **not modified**. (One
*optional*, owner-gated exception: making the existing CPU gauge fall back to the
host whole-machine load when the AROS source is the known-zero stub — see R-CPUFALL,
off by default.)

**Out (non-goals, this spec).** Recording/writing any host state (read-only only);
per-process network/disk attribution; historical logging/export; a network *stack*
or SANA-II driver (the guest has none — we read host counters, we do not move
packets); temperature/fan/power sensors (SMC — needs more than a read-only sysctl);
changing the Tasks page; a settings UI for which interfaces to sum (default policy
below).

## Architecture

Two layers joined by a flat hand-written C ABI (ours, ASCII, independent), exactly
as the CoreAudio and BSD-socket shims are joined to their AROS sides.

```
AROS side (aarch64, AROS crosstools)              Host side (Apple toolchain)
┌──────────────────────────────────────┐          ┌──────────────────────────────┐
│ SysMon  (MUI/Zune app)  [AROS]        │          │ libhoststats.dylib  [OURS]   │
│  · RegisterGroup pages (Tasks/CPU/Sys)│          │  · host_statistics64 (CPU/VM)│
│  · NEW Network/Disk/Host pages        │ hostlib  │  · sysctl(hw.memsize,        │
│  · NEW hoststats.c module ────────────┼──Open──► │      NET_RT_IFLIST2)         │
│      hs_* verbs via HostLib_GetIface  │ ◄────────┤  · getifaddrs / getmntinfo   │
│  · Zune Graph (reused)   [AROS]       │  hs_* C  │  · (opt) IOKit disk stats    │
│  · DOS Info()/DosList for volumes     │   ABI    │  read-only · no entitlement  │
└──────────────────────────────────────┘          └──────────────────────────────┘
   volume free-space = AROS Info()   |   CPU% / net / disk-IO / phys-mem = HOST shim
```

- **Host shim** `[OURS]` — native arm64 C built with host clang (NOT AROS
  crosstools), peer of `hosted/coreaudio/coreaudio_shim.c`. Owns every Mach/BSD/IOKit
  call; pulls **no** AROS headers; exposes the `hs_*` C ABI below; reached via
  `hostlib.resource` (`HostLib_Open` of the dylib + `HostLib_GetInterface`) `[AROS]`,
  exactly as the CoreAudio/BSD-socket shims resolve theirs.
- **AROS side** `[AROS]` — additive SysMon code: a `hoststats.c` data module + the
  page wiring in `main.c`. The only file naming Mach symbols is the shim.
- Spike-phase paths: shim in `hosted/hoststats/`; at graft, the AROS side lands in
  `workbench/system/SysMon/`.

## The C ABI (`hoststats_shim.h`)

Hand-authored, neutral. Verbs mirror the *role* of the in-tree shims' opaque-handle
APIs (`ca_*`, the bsdsock verbs). `[PUB]` Mach/BSD calls under the hood. The shim is
**stateless for raw counters** (it returns cumulative values + a timestamp; the
caller computes rates — keeps the shim thread-free and trivially testable). One
exception: a build/run **synthetic mode** for the numeric oracle (R-SYNTH).

```c
typedef struct HSContext HSContext;

/* Open the shim. No host resources held beyond the handle; returns NULL on
   failure. `synthetic` != 0 selects the deterministic oracle mode (R-SYNTH). */
HSContext *hs_open(int synthetic);
void       hs_close(HSContext *);

/* CPU load — cumulative tick counters (NOT a precomputed percentage; caller
   deltas two reads). `whole` gets the machine-wide ticks; `perCore` (may be NULL)
   gets up to `maxCores` per-core entries; `*outCores` returns the core count.
   Tick fields: user, system, idle, nice (the CPU_STATE_* order). Returns 0 on
   success. [PUB] host_statistics64(HOST_CPU_LOAD_INFO) / host_processor_info. */
typedef struct { unsigned long long user, system, idle, nice; } HSCpuTicks;
int hs_cpu_ticks(HSContext *, HSCpuTicks *whole,
                 HSCpuTicks *perCore, int maxCores, int *outCores);

/* Physical memory snapshot in bytes. installed = hw.memsize; used/free derived
   from HOST_VM_INFO64 (active+inactive+wired+compressed vs free) × page size.
   Returns 0 on success. [PUB] sysctlbyname + host_statistics64(HOST_VM_INFO64). */
typedef struct { unsigned long long installed, used, free; } HSMem;
int hs_mem(HSContext *, HSMem *out);

/* Per-interface cumulative byte/packet counters since boot, plus a monotonic
   host timestamp in nanoseconds (mach_absolute_time-derived) so the caller can
   compute per-second rates. Fills up to `maxIfs`; `*outCount` = interfaces seen.
   `flags` selects loopback inclusion (HS_IF_SKIP_LOOPBACK default). Counters are
   monotone non-decreasing (modulo 64-bit wrap). Returns 0 on success.
   [PUB] getifaddrs + sysctl(NET_RT_IFLIST2) if_msghdr2.ifm_data (if_data64). */
typedef struct {
    char name[16];
    unsigned long long ibytes, obytes, ipackets, opackets;
} HSIface;
int hs_net_counters(HSContext *, HSIface *out, int maxIfs, int *outCount,
                    unsigned long long *outTimestampNs, unsigned flags);

/* Host disk-I/O cumulative bytes since boot (summed over block storage drivers),
   plus the same monotonic timestamp. Returns 0 on success; returns nonzero (and
   leaves *out zero) if IOKit stats are unavailable — the Disk page then shows
   free-space only. [PUB] IOKit IOBlockStorageDriver Statistics (optional half). */
typedef struct { unsigned long long bytesRead, bytesWritten; } HSDiskIO;
int hs_disk_io(HSContext *, HSDiskIO *out, unsigned long long *outTimestampNs);

/* Host filesystem free/total (advisory; the AROS Disk page uses AROS Info() for
   the per-volume list — this is only for an optional host-summary row). [PUB]
   getmntinfo/statfs. May be omitted in the first cut. */
typedef struct { unsigned long long total, free; char mount[64]; } HSFs;
int hs_host_fs(HSContext *, HSFs *out, int maxFs, int *outCount);
```

`hs_open(synthetic=0)` is the real path; `hs_open(synthetic=1)` returns the
deterministic fixtures defined in R-SYNTH so `[SM2]` can assert the page math without
a live machine. The header is shared source, hand-written, independent. The shim
must not include AROS headers; the AROS side must not include Mach/IOKit headers.

## The host stats — required behaviour (`[PUB]`)

Each is a `[PUB]` requirement (Apple/Mach/BSD/POSIX API + the macOS SDK headers).
The shim implements only to the published interface; no offsets are hard-coded.

**R-CPU `[PUB]`.** Whole-machine CPU ticks via `host_statistics64(mach_host_self(),
HOST_CPU_LOAD_INFO, (host_info64_t)&cpuload, &count)` → `cpuload.cpu_ticks[
CPU_STATE_{USER,SYSTEM,IDLE,NICE}]`. Per-core via `host_processor_info(mach_host_self(),
PROCESSOR_CPU_LOAD_INFO, &n, &info, &infoCnt)` (then `vm_deallocate` the returned
array). Return the **cumulative** ticks; the caller computes
`pct = (1 − Δidle/Δtotal) × 100`. Headers: `<mach/mach_host.h>`,
`<mach/processor_info.h>`.

**R-MEM `[PUB]`.** `installed = sysctlbyname("hw.memsize")`. Used/free from
`host_statistics64(…, HOST_VM_INFO64, &vmstat, &count)` (`vm_statistics64`):
`used = (active + inactive + wired + compressor) × pageSize`,
`free = free_count × pageSize`, `pageSize` from `host_page_size`/`vm_page_size`.
Headers: `<sys/sysctl.h>`, `<mach/mach_host.h>`, `<mach/vm_statistics.h>`.

**R-NET `[PUB]`.** Enumerate via `getifaddrs(3)`; read cumulative counters by
walking the `sysctl([CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0], …)` buffer,
filtering `if_msghdr2` records (`ifm_type == RTM_IFINFO2`) and reading
`ifm_data` (a `struct if_data64`): `ifi_ibytes`, `ifi_obytes`, `ifi_ipackets`,
`ifi_opackets`. **Trust the SDK struct layout — never hard-code offsets** (R-NET2).
Default `flags` skips loopback (`IFF_LOOPBACK`). Headers: `<net/if.h>`,
`<net/route.h>`, `<net/if_dl.h>`, `<ifaddrs.h>`, `<sys/sysctl.h>`.

**R-NET2 `[DERIVED]`, restated `[PUB]`.** The shim must parse the `NET_RT_IFLIST2`
message stream defensively: advance by `ifm_msglen`, skip non-`RTM_IFINFO2` records,
bounds-check against the buffer end. Justification: `[PUB]` the routing-message
buffer is a heterogeneous variable-length record stream; a fixed stride or
unchecked cast is the classic parse bug. We independently determined the
defensive-walk requirement; implement from the message-format docs, not from any
reference. `[SM1]`'s monotonicity assertion catches a misparse (garbage is not
monotone).

**R-DISKIO `[PUB]`, optional.** Sum `kIOBlockStorageDriverStatisticsBytesReadKey` /
`...BytesWrittenKey` from each `IOServiceMatching("IOBlockStorageDriver")` service's
`Statistics` dict (`IORegistryEntryCreateCFProperty`). Adds an IOKit +
CoreFoundation link. If unavailable, return nonzero so the Disk page shows
free-space only. Headers: `<IOKit/IOKitLib.h>`, `<IOKit/storage/IOBlockStorageDriver.h>`.

**R-TS `[PUB]`.** All cumulative-counter calls return a monotonic timestamp from
`mach_absolute_time()` converted to ns via `mach_timebase_info`, so the caller's
rate denominator is the true elapsed host time, independent of the SysMon 250 ms
tick jitter.

**R-RO `[PUB]`.** Every call is read-only and prompt-free: `host_statistics64`,
`host_processor_info`, `sysctl`, `getifaddrs`, `getmntinfo`, and IOKit
registry-property reads need **no entitlement and trigger no TCC prompt** (not
camera/mic/screen-recording/automation). This is what keeps the feature inside the
unattended loop.

**R-SYNTH `[OURS]`.** `hs_open(1)` returns a fixed fixture set so `[SM2]` is
deterministic: CPU whole = ticks chosen so the interval percentage is exactly 37.5%;
`hs_mem` = `{installed=16 GiB, used=4 GiB, free=12 GiB}`; one interface `en0` with
`ibytes` that advances by exactly 2000 between two reads spaced exactly 2.0 s of the
returned timestamp (→ 1000 B/s); `hs_disk_io` advancing deterministically. The exact
fixtures are ours.

## AROS side — the page wiring (`[AROS]`, contract from [design.md](design.md))

### Adding the pages to the `Register` group — the exact recipe

The SysMon page mechanism is a MUI `RegisterGroup` whose positional children are the
pages (grounded in design.md §"How a page/tab is built"). The additive steps,
each citing the construct it mirrors:

**R-PAGE1 `[AROS]`.** Bump `SYSMON_TABCOUNT` from `3` to `6`
(`sysmon_intern.h:24`) so `tabs[SYSMON_TABCOUNT + 1]` (`sysmon_intern.h:33`) holds
the new labels + the terminating NULL.

**R-PAGE2 `[AROS]`.** Add three tab labels after the existing three, before the NULL
(mirror `main.c:94–97`):
```c
smdata->tabs[3] = _(MSG_TAB_NETWORK);
smdata->tabs[4] = _(MSG_TAB_DISK);
smdata->tabs[5] = _(MSG_TAB_HOST);
smdata->tabs[6] = NULL;
```
Add the `MSG_TAB_NETWORK/DISK/HOST` strings to the SysMon catalog descriptor
(`workbench/system/SysMon/catalogs/`); missing entries fall back to the built-in
English default (non-blocking). `[AROS]`.

**R-PAGE3 `[AROS]`.** Add three `Child` `VGroup`s to `RegisterGroup(smdata->tabs)`
(`main.c:168`) **immediately after the System page child** (`main.c:267`), in the
same positional order as the labels. Each page child is a `VGroup` with
`MUIA_CycleChain, 1` like the existing pages.

**R-PAGE4 `[AROS]`.** Extend the active-page `switch` in the event loop
(`main.c:423–437`) with cases `3`/`4`/`5` calling
`UpdateNetworkInformation`/`UpdateDiskInformation`/`UpdateHostInformation`. Keep the
"every 8th tick" throttle for the disk/volume scan (volume `Info()` is heavier than a
counter read); update the network/host graphs every tick (250 ms) so the graph is
live.

**R-PAGE5 `[AROS]`.** Add a `struct SysMonModule hoststatsmodule` to the `modules[]`
array (`main.c:387`) with `Init`/`DeInit` (mirror `memorymodule`, `memory.c:57`).
`Init` resolves the shim (R-SHIM); `DeInit` `HostLib_Close`es it. Add the new files
to `FILES :=` in `mmakefile.src:5` (`hoststats`).

### Resolving the shim — `[AROS]` + `[OURS]`

**R-SHIM `[AROS]`.** In `InitHostStats`: `OpenResource("hostlib.resource")`, then
`HostLib_Open("libhoststats.dylib", &err)`, then `HostLib_GetInterface(handle,
hs_symbols, &unresolved)` for the `hs_*` table (mirror the in-tree `hostlib` open +
the CoreAudio/BSD-socket shim resolution; `arch/all-hosted/hostlib/` —
`open.c:13`, `getinterface.c:15`). Host calls are serialised under
`HostLib_Lock`/`Unlock` around each `hs_*` call (the established host-call
discipline). The dylib is found in `~/lib` (deployed by `run-window.sh`/`aros-ctl`,
the CoreAudio precedent).

**R-FALLBACK `[AROS]`/`[OURS]`.** If `HostLib_Open` fails or any required symbol is
unresolved (not hosted, or shim absent), `Init` **still returns TRUE** with an
"no host stats" flag set. Then:
- the **Network** page shows "no host network counters" (the guest has no native
  source — design.md);
- the **Disk** page works fully from AROS `Info()` (no shim needed for free-space);
- the **Host** page falls back to AROS-native `AvailMem` for memory and the AROS
  `GetCPUInfo`/`GCIT_ProcessorLoad` for CPU (which is real on a non-hosted AROS, 0 on
  this hosted target — see R-CPUNOTE).
The app never fails to start because the shim is missing. `[OURS]` (graceful-degrade
discipline from the other shims).

### Network page — `[AROS]` Graph reuse

**R-NETPAGE `[AROS]`.** Build the throughput graph with the **same Zune Graph
class** the CPU page uses (`workbench/classes/zune/graph/`, header `graph.h`). Use
`GraphObject … End` (the macro at `processor_graph.c:103`) with `MUIA_Graph_InfoText`,
a `MUIA_Graph_ValueCeiling` sized to the rate scale, `MUIA_Graph_ValueStep`,
`MUIA_Graph_PeriodCeiling/Step/Interval` (`graph.h:18–25`). Attach **two data
series** — RX and TX — via `MUIM_Graph_GetSourceHandle 0` and `1`
(`graph.h:30`), each set with its own `MUIV_Graph_Source_ReadHook` and a distinct
`MUIV_Graph_Source_PenSrc` (`graph.h:39,40`) — exactly the multi-series pattern
SysMon's single-graph mode uses (`processor_graph.c:188–191`; multi-source draw
confirmed `graph.c:769`). The ReadHook receives a storage pointer (it writes the
value, per `graph.c:956`) and the hook's `h_Data`; it calls `hs_net_counters`,
computes the per-second delta against the previous snapshot stored in the module's
instance data, and writes the rate (scaled to the graph ceiling). Below the graph, a
text block lists per-interface absolute totals (mirror the CPU frequency list,
`main.c:319–339`).

**R-NETSUM `[DERIVED]`, restated `[PUB]`/`[AROS]`.** Default interface policy: sum
all non-loopback interfaces for the RX/TX series; show the per-interface breakdown in
the text block. Justification: `[PUB]` a host has several interfaces (en0, utun*,
awdl0, bridge*) and loopback would double-count; summing non-loopback is the simplest
truthful aggregate; we independently chose "sum non-loopback" as the default.
`[DERIVED]` flag because the "right" interface (default-route only?) is a judgement
call — owner may revisit.

### Disk page — `[AROS]` `Info()` + DosList

**R-DISKPAGE `[AROS]`.** The per-volume list comes from AROS, not the shim. Walk the
volume list: `LockDosList(LDF_VOLUMES | LDF_READ)` → loop `NextDosEntry(dl,
LDF_VOLUMES)` → `UnLockDosList(LDF_VOLUMES | LDF_READ)`
(`compiler/include/dos/dosextens.h:475–496`; precedent `workbench/c/Info.c:301,450`).
For each volume, obtain a lock (`Lock("<volname>:", SHARED_LOCK)`) and call
`Info(lock, &infodata)` (`rom/dos/info.c`); free space (bytes) =
`(id_NumBlocks − id_NumBlocksUsed) × id_BytesPerBlock`, total =
`id_NumBlocks × id_BytesPerBlock` (`struct InfoData`, `compiler/include/dos/dos.h:174`).
Render a row per volume: name (`%b` of `dol_Name`), free, total, % used (mirror the
field-list rendering in `Info.c:680–703`). Host-folder volumes (emul-handler) report
the **host** filesystem's free space automatically, because emul-handler's
`ACTION_INFO` already proxies host `statfs` (see [host-volume](../host-volume/design.md))
— so AROS `Info()` is already the truthful host free-space for those, with zero shim
work. `[AROS]`.

**R-DISKIO `[AROS]`+`[PUB]`, optional.** A second Graph (same class) plots host disk
I/O bytes/sec from `hs_disk_io` (R-DISKIO host requirement), delta/sec. Shipped as a
**second increment** — the free-space list is the must-have; the I/O-rate graph is
the nice-to-have and brings the IOKit dependency. If the shim/IOKit is unavailable,
omit the graph (free-space list still works). `[OURS]` increment policy.

### Host page — `[AROS]` gauge/graph reuse, host-fed

**R-HOSTPAGE `[AROS]`.** Real Mac CPU load + memory. CPU: reuse the visual shape of
`ProcessorGauge`/`ProcessorGraph` (`processor_gauge.c`/`processor_graph.c`) — a
gauge/graph driven by a value 0..1000 (tenths of a percent) — but feed it from
`hs_cpu_ticks` (whole + per-core) and the `pct = (1 − Δidle/Δtotal) × 100` interval
math (R-CPU), **not** `GetCPUInfo`. Memory: three text rows (installed / used / free)
from `hs_mem` (R-MEM), formatted in MiB/GiB. The page is labelled unambiguously as
the Mac's real figures. `[AROS]` for the widget classes, `[PUB]` for the math.

**R-CPUNOTE `[AROS]`/`[OURS]`.** The existing **CPU** tab (untouched) reads
`GCIT_ProcessorLoad`, which is the **unimplemented stub returning 0** on
darwin-aarch64 (`rom/processor/getcpuinfo.c`; no `arch/all-darwin|aarch64-all/processor`
exists; the real load lives only in `arch/all-pc/processor/getcpuinfo.c:100` under
SMP). This is *why* the Host page exists. Document the distinction in the UI so the
two CPU readings are not confusing.

**R-CPUFALL `[OURS]`, OPTIONAL, owner-gated, default OFF.** Only if the owner
approves touching the existing CPU page: when the AROS CPU source is the known-zero
stub *and* the shim is present, the existing CPU gauge may fall back to the host
whole-machine load so the default tab is not dead. Default plan keeps the CPU page
purely as-is (additive-only). `[OURS]`.

## Verification (unattended — `[OURS]` discipline)

No human reads a gauge; no TCC prompt (R-RO). The oracle is **numeric**: known
values in → page model out; plus an invariant check on the real shim. The MUI render
is a secondary control-harness screenshot.

**The assertions** (every marker asserts *values*, never "it didn't crash"):

- **Plausibility (real shim):** `0 ≤ CPU% ≤ 100` whole and per-core; `0 < used ≤
  installed` memory; each interface counter non-negative and **monotone
  non-decreasing** across two reads; `≥ 1` mounted filesystem with `free ≤ total`.
- **Round-trip (synthetic shim):** the page data model computes the exact values the
  R-SYNTH fixtures imply (CPU 37.5% → gauge 375; mem used → "4096 MiB"; RX
  2000 B / 2.0 s → 1000 B/s; volume math from a known `InfoData`).
- **Rate math:** cumulative→bytes/sec correct over a scripted snapshot sequence,
  incl. 64-bit wrap → bounded non-negative rate, and a zero/negative-elapsed guard.

**Markers** (one host binary per marker, `[SM?]` PASS/FAIL via
`harness/run-hosted.sh`, clean-exit on PASS):

- **[SM1] host-stats shim returns plausible real values.** Pure host probe (no
  AROS): call every `hs_*` verb on the real machine, assert the plausibility
  invariants above. Grounds the shim + assert harness (like audio `[A1]`). `[SM1]`.
- **[SM2] numeric oracle — synthetic values round-trip the page model.** **The
  gate.** Drive the page-model code (the rate/format/gauge math, factored so it links
  without MUI) from `hs_open(1)` synthetic fixtures (R-SYNTH); assert the exact
  outputs. Proves the binding without a live machine. `[SM2]`.
- **[SM3] rate computation across the delta boundary.** Assert cumulative→bytes/sec
  over a scripted sequence incl. wrap and zero-elapsed guard. No GUI. `[SM3]`.
- **[SM4] graft — the three pages live in SysMon, rendered.** Build the extended
  `workbench/system/SysMon/` for darwin-aarch64; boot windowed under the control
  harness, switch to each new tab, screenshot (secondary). Re-assert the numeric gate
  in-AROS with the shim in synthetic mode (a debug-printed model value or a tiny
  probe confirms the page read the known values). PASS = pages render **and** the
  in-AROS model matches the synthetic input. Rides the SysMon rebuild graft. `[SM4]`.

The render check (`[SM4]`) uses the [control harness](../control-harness/README.md)
(`aros-ctl run` / `wait` / `shot` off the offscreen oracle, TCC-free) and is a
structural "the page painted" check — a screenshot regression is a warning, the
numeric oracle is the truth.

## Build / integration

- Shim `libhoststats.dylib` links `CoreFoundation` and (for R-DISKIO only) `IOKit`;
  the CPU/mem/net paths need **no** framework beyond libSystem (Mach + sysctl are in
  libSystem). Built with host clang `-arch arm64`, ad-hoc codesigned (confirm vs.
  the existing `harness/run*.sh`/`coreaudio-dylib` signing path — **UNVERIFIED**),
  loaded via `hostlib.resource`. Exports listed in `hosted/hoststats/hoststats.exports`
  (the `_hs_*` symbols), mirroring `hosted/coreaudio/coreaudio.exports`. `[OURS]`.
- Add `make hoststats-dylib` / `make hoststats-abi` (host-side `[SM1]`/`[SM2]`/`[SM3]`
  through the dylib boundary, like `make coreaudio-abi`) and `make sysmon-smoke` (the
  windowed `[SM4]` screenshot, like `make audio-smoke`) to the `Makefile`; deploy the
  dylib to `~/lib` via `run-window.sh`/`aros-ctl`/`deploy-check` and bundle into
  `Daedalos.app/Contents/Frameworks/` via `make-aros-app.sh` (the CoreAudio
  precedents). `[OURS]`.
- AROS side: new files `hoststats.c` (+ any `hoststats.h`) added to
  `workbench/system/SysMon/mmakefile.src` `FILES :=` (`:5`); the new MUI catalog
  strings in `catalogs/`. Built by the AROS crosstools as part of SysMon — **not**
  host clang. `[AROS]`.
- The C ABI header is shared, hand-written, independent work. The shim must not link
  or include AROS headers; the AROS side must not include Mach/IOKit headers.

## Open questions / UNVERIFIED

- Exact `if_msghdr2`/`if_data64` layout and `NET_RT_IFLIST2` walk — trust the SDK
  headers, never hard-code offsets (R-NET2); `[SM1]` monotonicity catches a misparse.
  **UNVERIFIED** until compiled against the SDK.
- Interface aggregation policy (sum-non-loopback vs default-route-only) — default
  sum-non-loopback (R-NETSUM), owner may revisit.
- Whether to compute rates in the shim or the AROS side — default: shim returns raw
  cumulative + timestamp, AROS computes the rate (keeps the shim thread-free).
- IOKit disk-I/O is the optional half — ship free-space-only Disk first, add the
  I/O-rate graph as a second increment (R-DISKIO).
- The CPU-page fallback (R-CPUFALL) — off by default; owner decision before touching
  the existing CPU page.
- Codesign/entitlement for a `dlopen`'d stats dylib in the hosted process — confirm
  vs. `harness/run.sh`/`coreaudio-dylib`. **UNVERIFIED.**
- New MUI catalog string IDs (`MSG_TAB_NETWORK/DISK/HOST`) — pick free IDs, confirm
  vs. the existing `catalogs/` descriptor at graft. **UNVERIFIED.**
- `Lock("<vol>:")` per volume for `Info()` may transiently fail for a busy/unvalidated
  volume; fall back to `DoPkt(dol_Task, ACTION_DISK_INFO, …)` as `Info.c:700` does.

## Provenance summary

`[PUB]` Apple Mach/BSD host-statistics APIs — `host_statistics64`
(`HOST_CPU_LOAD_INFO`/`cpu_ticks[CPU_STATE_*]`, `HOST_VM_INFO64`/`vm_statistics64`),
`host_processor_info(PROCESSOR_CPU_LOAD_INFO)`, `host_page_size`/`vm_page_size`,
`mach_absolute_time`/`mach_timebase_info`, `sysctlbyname("hw.memsize")`,
`sysctl(NET_RT_IFLIST2)` over `if_msghdr2`/`if_data64`, `getifaddrs(3)`,
`getmntinfo(3)`/`statfs(2)`, IOKit `IOBlockStorageDriver` `Statistics`
(`...BytesRead/WrittenKey`) — all read-only, no entitlement, no TCC; the macOS SDK
headers named above; the published MUI/Zune `Register`/`Graph`/`Gauge` class API. ·
`[AROS]` SysMon (`workbench/system/SysMon/{main.c,sysmon_intern.h,processor.c,
processor_gauge.c,processor_graph.c,memory.c,video.c,timer.c,tasks.c,mmakefile.src}`
— `RegisterGroup` `main.c:168`, `tabs[]` `:94–97`/`sysmon_intern.h:33`, `SYSMON_TABCOUNT`
`:24`, active-page switch `main.c:423–437`, `modules[]` `:387`, `GetCPUInfo`/
`GCIT_ProcessorLoad` `processor_gauge.c:138,144`, `AvailMem` `memory.c:25–54`);
`compiler/include/resources/processor.h` (`GCIT_*` `:24,28,32,47`),
`rom/processor/getcpuinfo.c` + `arch/all-pc/processor/getcpuinfo.c:100` +
`arch/arm-native/processor/getcpuinfo.c` (the load stub picture);
`workbench/classes/zune/graph/{graph.c,graph.h}` (`MUIA_Graph_*` `:18–25`,
`MUIM_Graph_GetSourceHandle`/`SetSourceAttrib` `:30,31`, `MUIV_Graph_Source_ReadHook`/
`PenSrc` `:39,40`, ReadHook call `graph.c:956`, multi-source `:769`);
`compiler/include/dos/{dos.h (InfoData :174),dosextens.h (LDF_*/DLT_* :475)}`,
`rom/dos/{info.c,lockdoslist.c,nextdosentry.c}`, `workbench/c/Info.c:301–450,680–703`
(volume-walk precedent); `arch/all-unix/bootstrap/hostlib.h` +
`arch/all-hosted/hostlib/{open.c:13,getinterface.c:15,getpointer.c:17}`. ·
`[OURS]` the `hostlib`-loaded flat-C-ABI dylib pattern proven by
`hosted/coreaudio/{coreaudio_shim.h,coreaudio.exports}` + `build/libcoreaudio.dylib`
+ `make coreaudio-abi`/`audio-smoke`, and `hosted/bsdsocket/` (host networking, where
the interface counters live); the control harness (`graft/aros-ctl` +
`hosted/cocoametal/`) for the `[SM4]` screenshot; `graft/{deploy-check,run-window.sh,
make-aros-app.sh}` for `~/lib`/bundle deploy; `harness/run-hosted.sh` marker harness;
the graceful-degrade-when-not-hosted discipline. ·
`[DERIVED]` independently-derived points flagged for extra verification:
(a) the defensive `NET_RT_IFLIST2` message-walk (R-NET2), and (b) the
sum-non-loopback interface aggregation default (R-NETSUM) — both restated above from
Apple's published message-format/interface docs `[PUB]`; no third-party code,
identifiers, or call sequence used.
