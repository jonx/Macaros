# System monitor — real Mac stats in the AROS SysMon (network / disk / host pages)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

AROS already ships a graphical system monitor, **`SysMon`** (a MUI/Zune app under
`workbench/system/SysMon/`). It has three tabs today — **Tasks** (a live
task/process list), **CPU** (per-core load gauges or a graph, plus per-core
frequency/model), and **System** (RAM and video-memory size/free). This feature
**extends** SysMon — it does not rebuild it — with three new pages:

- a **NETWORK** page: a throughput graph (RX / TX bytes-per-second, deltas/sec)
  built from the host's per-interface byte/packet counters;
- a **DISK** page: free/total space per AROS volume, plus host disk-I/O
  bytes-per-second;
- a **HOST** page: the Mac's *real* CPU load and memory.

It is the project thesis — *"macOS owns the drivers; AROS reaches them via standard
exec I/O"* — applied to telemetry. A hosted AROS has no NIC, no block device, and
no CPU of its own; the truthful system numbers for the guest are the **host's**. The
numbers feed in through a tiny read-only **host-stats shim** (`libhoststats.dylib`),
`dlopen`'d via `hostlib.resource` exactly like the CoreAudio, BSD-socket, clipboard
and host-volume shims this project already ships — with a graceful fallback to
AROS-native numbers when not hosted.

The one design point that *earns this doc* — and the reason host-sourcing is not
merely "nicer" but **necessary** — is grounded below: AROS's own CPU-load source,
`processor.resource` `GCIT_ProcessorLoad`, **returns 0 on darwin-aarch64**. SysMon's
CPU page is wired to a number that does not exist on this target. So the honest CPU
figure *must* come from the host. The rest of the design is about adding pages to a
MUI `Register` group, reusing SysMon's existing graph/gauge classes, and crossing
the host-call boundary the same way every other hosted feature already does.

## Does it already exist?

**The monitor exists; the three pages and a working data source do not.**

What SysMon shows today, grounded in the source
(`/Users/user/Source/aros-upstream/workbench/system/SysMon/`):

- **Three tabs**, built from `smdata->tabs[]` = `{Tasks, CPU, System, NULL}`
  (`main.c:94–97`), with `SYSMON_TABCOUNT 3` (`sysmon_intern.h:24`). The tab
  container is `RegisterGroup(smdata->tabs)` (`main.c:168`).
- **Tasks page**: a `Tasklist` custom class listview (`main.c:171`, class in
  `tasks.c`) showing ready/waiting counts.
- **CPU page**: per-core load shown as a `ProcessorGauge` or `ProcessorGraph`
  custom-class group (`main.c:199`, `ProcessorGroupObject` in `processor.c:73`),
  plus a per-core frequency/model list (`main.c:319–339`, filled by
  `UpdateProcessorInformation`/`...StaticInformation` in `processor.c:90,128`).
- **System page**: RAM size/free (`memory.c`) and video-memory size/free
  (`video.c`), no graph.

What it **lacks**: no network page, no disk/volume page, no host-stats view, and —
the load-bearing gap — **no working CPU-load number on this target** (next section).

Nothing in the repo or upstream surfaces host network/disk throughput inside an AROS
GUI. The host-side primitives we lean on are partly built already: the
[BSD-sockets shim](../bsdsocket-net/README.md) reaches host networking (where the
interface counters live) and the [CoreAudio shim](../coreaudio-audio/README.md)
established the exact `hostlib`-loaded flat-C-ABI `.dylib` pattern this feature
copies. So the work is: **one new host-stats shim** + **three new SysMon pages** +
the data-source glue. SysMon's app skeleton, timer loop, graph and gauge classes are
reused unchanged.

## Background: the SysMon page mechanism + where its numbers come from (grounded)

### How a page/tab is built — the MUI `Register` mechanism (cite the exact constructs)

SysMon's tabbed UI is a single MUI **`Register`** group. The exact constructs, all
in `main.c`:

1. **Tab labels** — a NULL-terminated `CONST_STRPTR` array assigned in
   `CreateApplication`:
   ```c
   smdata->tabs[0] = _(MSG_TAB_TASKS);   /* main.c:94  */
   smdata->tabs[1] = _(MSG_TAB_CPU);     /* main.c:95  */
   smdata->tabs[2] = _(MSG_TAB_SYSTEM);  /* main.c:96  */
   smdata->tabs[3] = NULL;               /* main.c:97  */
   ```
   The array is sized `tabs[SYSMON_TABCOUNT + 1]` (`sysmon_intern.h:33`), with
   `SYSMON_TABCOUNT 3` (`sysmon_intern.h:24`).
2. **The Register group** — `smdata->pages = RegisterGroup(smdata->tabs)`
   (`main.c:168`), used as the window's `WindowContents`.
3. **Each page is the next positional `Child` of that group.** The three children
   are added in tab order: the Tasks `VGroup` (`main.c:169–182`), the CPU `VGroup`
   (`main.c:183–203`), the System `VGroup` (`main.c:204–267`). The `RegisterGroup`
   pairs each label with the child at the same index.
4. **The visible page drives updates.** The main event loop reads
   `MUIA_Group_ActivePage` and switches on it, refreshing only the foreground page
   on each timer tick:
   ```c
   get(smdata.pages, MUIA_Group_ActivePage, &currentPage);  /* main.c:422 */
   switch (currentPage) {
       case 1: UpdateProcessorInformation(&smdata); break;  /* CPU    */
       case 2: /* every 8th tick */ UpdateMemoryInformation/UpdateVideoInformation; break;  /* System */
   }                                                          /* main.c:423–437 */
   ```
   Tick cadence is a `timer.device` `UNIT_VBLANK` request re-armed every 250 ms via
   `SignalMeAfter(250)` (`timer.c:42`, `main.c:408,441`).

**So adding a page is a known, local recipe** (spelled out fully in spec.md):
(a) bump `SYSMON_TABCOUNT`; (b) add a tab label + an `_(MSG_TAB_*)` catalog string;
(c) add a `Child` `VGroup` to the `RegisterGroup` at the matching index; (d) add a
`case` to the active-page switch calling the page's `Update...Information`; (e) add a
`struct SysMonModule` to the `modules[]` array (`main.c:387`) for Init/DeInit. This
is pure additive MUI wiring — no change to the existing pages.

### Where SysMon's numbers come from today (cite the sources)

- **CPU load %** — from **`processor.resource`** via `GetCPUInfo()` with the
  `GCIT_ProcessorLoad` tag (`processor_gauge.c:138`, `processor_graph.c:63,283`).
  The value is a **16.16 fixed-point fraction** of full load: SysMon converts it
  with `usage = ((usage >> 16) * 1000) >> 16` to tenths-of-a-percent
  (`processor_gauge.c:144`, `processor_graph.c:73,288`). The tag is defined at
  `compiler/include/resources/processor.h:47` (autodoc: `getcpuinfo.c` documents the
  range as `0..0xffffffff`).
- **CPU count / speed / model** — same `GetCPUInfo()` resource, tags
  `GCIT_NumberOfProcessors` (`processor.h:24`), `GCIT_ProcessorSpeed`
  (`processor.h:32`), `GCIT_ModelString` (`processor.h:28`)
  (`processor.c:56,108,142`).
- **Memory size/free** — **exec** `AvailMem()`: `AvailMem(MEMF_ANY|MEMF_TOTAL)` for
  size, `AvailMem(MEMF_ANY)` for free, repeated for `MEMF_CHIP`/`MEMF_FAST`
  (`memory.c:25–54`).
- **Video memory** — the gfx HIDD's `aHidd_Gfx_MemoryAttribs`
  (`video.c:68,87`).
- **Tasks** — the exec task lists, walked by the `Tasklist` class (`tasks.c`).

### The load-bearing fact: AROS has NO working CPU-load source on this target

`GCIT_ProcessorLoad` is honoured **only by the x86/`all-pc` `processor.resource`,
and only in SMP builds**: `arch/all-pc/processor/getcpuinfo.c:100–106` returns
`KrnGetSystemAttr(KATTR_CPULoad + cpu)` under `__AROSEXEC_SMP__`, else `0` with a
`/* TODO: IMPLEMENT */`. The real computation is per-core idle accounting in the
APIC heartbeat (`arch/all-pc/kernel/apic_heartbeat.c:122`). For **every other
target** the load is a stub returning **0**:

- the generic `rom/processor/getcpuinfo.c` (the default) returns `0`;
- `arch/arm-native/processor/getcpuinfo.c` returns `0` (stub);
- there is **no `processor.resource` implementation under `arch/all-darwin`,
  `arch/all-hosted`, or `arch/aarch64-all`** — the hosted darwin-aarch64 target
  falls back to the generic stub.

**Conclusion:** on darwin-aarch64 hosted, SysMon's CPU page is plotting a constant
`0.0 %`. AROS-native memory (`AvailMem`) is real but describes only the *guest's*
fixed RAM partition, not the Mac. AROS has **no** network-throughput counter at all
(no SANA-II driver/stack is up on this target — see the
[port inventory](../darwin-aarch64-port-inventory.md) §6), and no disk-I/O byte
counter. So three of the four interesting numbers (CPU%, network, disk I/O) are
**absent on the guest** and must come from the host; volume free-space and
guest-RAM are AROS-native and stay so. This asymmetry is the whole rationale for the
host-stats source.

### Reference points already de-risked in this repo

- **Host shim pattern** — a native arm64 `.dylib` exporting a flat C ABI
  (`*.exports`), built with host clang, deployed to `~/lib`, and `dlopen`'d from
  AROS through `hostlib.resource`. Proven by the
  [CoreAudio shim](../coreaudio-audio/README.md) (`hosted/coreaudio/`,
  `build/libcoreaudio.dylib`, `make coreaudio-abi`) and the
  [BSD-socket shim](../bsdsocket-net/README.md) (`hosted/bsdsocket/`,
  `build/libbsdsockhost.dylib`). The AROS side resolves it via
  `OpenResource("hostlib.resource")` → `HostLib_Open` → `HostLib_GetInterface`
  (`arch/all-unix/bootstrap/hostlib.h`, `arch/all-hosted/hostlib/` — `open.c:13`,
  `getinterface.c:15`, `getpointer.c:17`).
- **Calling host libc from AROS** — `sysctlbyname` etc. reached through
  `hostlib.resource` → `libSystem.dylib`, the exact mechanism the BSD-socket and
  host-volume shims already use; non-variadic, no `abishim.S` needed.
- **GUI verification in the loop** — the [control harness](../control-harness/README.md)
  boots SysMon windowed and screenshots the framebuffer off an offscreen oracle (no
  TCC), so the rendered page is checkable unattended.

## Design

### Host side (the host-stats shim — the truthful numbers)

A new native arm64 shim `libhoststats.dylib` (peer of `libcoreaudio.dylib`),
exposing a flat C ABI (full ABI in spec.md). It pulls **no** AROS headers and is the
only file naming Apple/Mach symbols. All sources are **read-only, no entitlement, no
TCC**:

- **CPU load (whole-machine + per-core)** — `host_statistics64(mach_host_self(),
  HOST_CPU_LOAD_INFO, …)` returns `host_cpu_load_info` with cumulative
  `cpu_ticks[CPU_STATE_{USER,SYSTEM,IDLE,NICE}]`; CPU% over an interval is
  `1 - Δidle/Δtotal`. Per-core uses `host_processor_info(…,
  PROCESSOR_CPU_LOAD_INFO, &n, …)`. `[PUB]`
- **Physical memory** — `sysctlbyname("hw.memsize", …)` for installed RAM;
  `host_statistics64(…, HOST_VM_INFO64, …)` → `vm_statistics64` for
  active/inactive/wired/free/compressed pages × `vm_page_size` for used/free. `[PUB]`
- **Per-interface byte/packet counters** — `getifaddrs()` to enumerate, then
  `sysctl(NET_RT_IFLIST2)` reading `struct if_msghdr2.ifm_data` (an `if_data64`)
  for `ifi_ibytes`/`ifi_obytes`/`ifi_ipackets`/`ifi_opackets` (monotone since boot).
  Throughput = delta between two reads ÷ elapsed seconds. `[PUB]`
- **Disk free/total** — `getmntinfo()` / `statfs()` (`f_blocks`, `f_bavail`,
  `f_bsize`) per mounted filesystem. This is the *host's* view; the AROS-volume view
  uses AROS `Info()` (below). `[PUB]`
- **Disk I/O bytes/sec (optional)** — IOKit `IOServiceMatching("IOBlockStorageDriver")`
  → the `Statistics` dictionary (`kIOBlockStorageDriverStatisticsBytesReadKey` /
  `...BytesWrittenKey`); delta/sec. This adds a CoreFoundation/IOKit link to the
  shim. `[PUB]` Flagged as the optional half of the DISK page (free-space alone is
  the must-have).

The shim is **stateless across calls for the raw counters** (it returns the
cumulative numbers); rate computation (delta ÷ elapsed) is done by the caller so the
shim has no timer and no thread. This keeps it trivially testable: feed two known
counter snapshots, assert the computed rate. (See spec.md for whether deltas are
computed in the shim or the AROS side — default: shim returns raw cumulative
counters + a monotonic timestamp; the AROS page computes the rate.)

### AROS side (three new SysMon pages + the data feed)

A new SysMon data-source module `hoststats.c` (peer of `memory.c`/`processor.c`)
plus the page wiring in `main.c`. The module:

- on `Init`, resolves the shim: `OpenResource("hostlib.resource")` →
  `HostLib_Open("libhoststats.dylib")` → `HostLib_GetInterface` for the `hs_*`
  verbs. **If the shim is absent (not hosted), `Init` records "no host stats" and
  the pages fall back to AROS-native numbers** (volume free-space via `Info()`,
  guest RAM via `AvailMem`) or hide the host-only rows. The CPU/HOST page then shows
  AROS-native values, which on a *real* AROS target are meaningful.
- exposes `UpdateNetworkInformation` / `UpdateDiskInformation` /
  `UpdateHostInformation` called from the active-page switch.

The three pages:

1. **NETWORK page** — a Zune **Graph** plotting two series (RX, TX) in
   bytes-per-second, using the *same* graph class SysMon's CPU graph uses
   (`workbench/classes/zune/graph/`, header `graph.h`). Construct with `GraphObject`
   (the macro SysMon uses at `processor_graph.c:103`), then attach two data sources
   via `MUIM_Graph_GetSourceHandle 0`/`1` and set each source's
   `MUIV_Graph_Source_ReadHook` + a distinct `MUIV_Graph_Source_PenSrc`
   (`graph.h:30,31,39,40`). The multi-series-on-one-graph capability is exactly what
   SysMon's single-graph mode already exercises (`processor_graph.c:188–191`,
   confirmed in `graph.c` draw loop). Below the graph, a small text block shows the
   per-interface absolute byte/packet totals (like the CPU page's frequency list).
   Read-hooks call `hs_net_counters` and compute the per-second delta.
2. **DISK page** — a per-volume list (free / total / % used) built from AROS's own
   volume list — `LockDosList(LDF_VOLUMES|LDF_READ)` → `NextDosEntry` →
   `UnLockDosList` (`compiler/include/dos/dosextens.h:475–496`) and `Info(lock,
   &InfoData)` per volume (`rom/dos/info.c`, `struct InfoData` in
   `compiler/include/dos/dos.h:174`): free = `(id_NumBlocks − id_NumBlocksUsed) ×
   id_BytesPerBlock`. **Precedent: `workbench/c/Info.c:301–450`** already walks the
   list this way — reuse the pattern. Optionally a second Graph plots host disk-I/O
   bytes/sec (`hs_disk_io`). The volume free-space is the **AROS-native** part (it is
   the guest's correct view of its mounted volumes, several of which are host folders
   via emul-handler); the I/O-rate graph is **host-sourced** (the guest has no I/O
   counter).
3. **HOST page** — the Mac's real CPU load (a gauge/graph reusing
   `processor_gauge.c`/`processor_graph.c` shape, but fed by `hs_cpu_load` instead
   of `GetCPUInfo`) and real physical memory (`hs_mem`: installed / used / free).
   This is the page that exists *because* `GCIT_ProcessorLoad` is 0 on this target —
   it is the only honest CPU number SysMon can show on Apple Silicon. A short note in
   the UI distinguishes "Host CPU" (real Mac) from the existing "CPU" tab (the guest
   abstraction, which on this target reads 0 — see Risks).

### The bridge (host-vs-AROS data decision — which number is truthful)

The explicit rule, per number:

| Number | Source on darwin-aarch64 hosted | Why |
|--------|---------------------------------|-----|
| **CPU load %** | **HOST** (`host_statistics64`) | AROS `GCIT_ProcessorLoad` = 0 (stub); the guest runs *on* the Mac's cores — the Mac's load is the real one. |
| **Physical memory** | **HOST** for the HOST page; **AROS** (`AvailMem`) stays on the existing System tab | The guest's RAM partition is real *to the guest*; the Mac's installed/used RAM is the machine truth. Show both, labelled. |
| **Network throughput** | **HOST** (`if_msghdr2 ifm_data`) | The guest has no NIC and no stack up; all packets flow through the Mac's interfaces. |
| **Volume free space** | **AROS** (`Info()` over the volume list) | This is the guest's *correct* view of *its* mounted volumes (incl. host-folder mounts via emul-handler). Host `statfs` would double-count and not match AROS volume names. |
| **Disk I/O bytes/sec** | **HOST** (IOKit, optional) | The guest has no block-device byte counter; the Mac's storage driver does. |

The honest framing surfaced in the UI: host-sourced rows are labelled as the Mac's
real figures; the existing CPU/System tabs keep their AROS-native semantics
(unchanged), and the new HOST page is explicitly "the Mac". This avoids pretending a
guest abstraction is a hardware truth — and it is the only way to show a non-zero
CPU figure at all on this target.

## Plan — spikes in the loop

Each marker is a standalone host binary (one-binary-per-marker, like
`hosted/coreaudio/` and the `[A?]` audio markers) with a single PASS/FAIL verdict
the agent greps — no human reads a gauge. Prefix **`[SM*]`**.

- **[SM1] host-stats shim returns plausible real values.** Pure host probe (no
  AROS): call every `hs_*` verb and assert sanity — `0 ≤ CPU% ≤ 100` for whole and
  each core; `used ≤ installed` memory and both `> 0`; each interface's byte/packet
  counters are non-negative and **monotone non-decreasing** across two reads ~200 ms
  apart; at least one mounted filesystem with `free ≤ total`. Grounds the shim +
  assertion harness, like the audio `[A1]` host probe. `[SM1]`.
- **[SM2] numeric oracle: known synthetic values round-trip the data model.** The
  primary gate. Drive the page data model with **injected, known** counter snapshots
  (a fake `hs_*` table, or an env-seeded shim mode): feed CPU=37.5%, mem
  used=4 GiB/installed=16 GiB, an interface with ibytes 1000→3000 over 2.0 s, a
  volume 100 blocks total / 40 used × 512 B/block. Assert the page model computes:
  CPU gauge = 375 (tenths), mem-used text = 4096 MiB, RX rate = 1000 B/s, disk free
  = 30720 bytes. Proves the math + the page binding without a live machine — the
  deterministic spike that gates the feature. `[SM2]`.
- **[SM3] rate computation across the delta boundary.** Assert the
  cumulative-counter → bytes/sec conversion (incl. counter wrap handling and a
  zero/negative-elapsed guard) over a scripted sequence of snapshots; assert a
  64-bit counter wrap yields a sane (non-negative, bounded) rate rather than a huge
  spike. No GUI. `[SM3]`.
- **[SM4] graft: the three pages live in SysMon.** Build the extended
  `workbench/system/SysMon/` (new `hoststats.c` + the `main.c` page wiring) for
  darwin-aarch64, boot it windowed under the control harness, switch to each new tab,
  and **screenshot** (secondary check). The numeric gate is re-asserted in-AROS:
  with the shim in synthetic mode, a tiny AROS-side probe (or a debug-printed model
  value) confirms the page read the known values. PASS = pages render + the
  in-AROS model matches the synthetic input. Rides the SysMon rebuild graft, not a
  session-sized spike. `[SM4]`.

Build/run in the existing harness style (`make hoststats-abi` → `[SM?]` markers via
`harness/run-hosted.sh`; `make sysmon-smoke` for the windowed `[SM4]` screenshot,
mirroring `make audio-smoke`), clean-exit on PASS.

## How we verify it unattended

No human reads a gauge; no TCC prompt is hit (every host-stats call is a read-only
Mach/BSD query — `host_statistics64`, `sysctl`, `getifaddrs`, `getmntinfo` — none
needs an entitlement or screen-recording/automation permission).

1. **Numeric oracle (primary gate).** `[SM2]` feeds **known synthetic values**
   through the shim and asserts the page's data model reads them back exactly; `[SM1]`
   asserts the *real* shim returns plausible values (the `0 ≤ CPU% ≤ 100`,
   `used ≤ installed`, monotone-counter invariants from the task brief). This is the
   pass/fail the agent reads.
2. **GUI render (secondary, not the gate).** `[SM4]` uses the
   [control harness](../control-harness/README.md): `aros-ctl run`, switch tabs,
   `aros-ctl shot` off the offscreen oracle, and a structural check that the page
   painted (non-blank region where the graph/list lives). A screenshot regression is
   a warning, not a hard fail — the numeric oracle is the truth.

Markers are unique per spike so a regression localises (the `[M*]`/`[H*]`/`[A*]`
discipline).

## Risks & open questions

- **Two CPU numbers could confuse.** The existing **CPU** tab reads the AROS
  `GCIT_ProcessorLoad` stub (0.0 % on this target); the new **HOST** tab reads the
  real Mac load. Showing both risks "why is one always zero?". **Mitigation
  (decision in spec.md):** label the HOST page unambiguously as the Mac's figures,
  and — *optionally, and only if the owner approves touching the CPU page* — have the
  existing CPU gauge fall back to the host whole-machine load when the AROS source is
  the known-zero stub, so the default tab is not dead. Default plan: leave the CPU
  tab untouched (purely additive) and let the HOST page carry the truth.
- **Per-interface counters: which interface(s)?** A Mac has `lo0`, `en0`, maybe
  `utun*`, `awdl0`, bridges. Summing all double-counts loopback; showing only the
  default-route interface needs a route lookup. **Open:** default to "all non-loopback
  interfaces summed", with the per-interface breakdown in the text block. **UNVERIFIED**
  whether `NET_RT_IFLIST2` ordering/availability is stable across macOS versions on
  Apple Silicon — `getifaddrs` is the portable fallback for enumeration.
- **`if_data64` field offsets / `NET_RT_IFLIST2` parsing.** Walking the `sysctl`
  buffer of `if_msghdr2` records is fiddly (alignment, message-type filtering).
  `[SM1]` asserts monotonicity to catch a misparse (garbage counters won't be
  monotone). **UNVERIFIED** exact struct layout until compiled against the SDK; the
  shim must `#include <net/if.h>`/`<net/route.h>` and trust the SDK headers, never
  hard-code offsets.
- **Volume free-space vs host `statfs`.** Decided: AROS `Info()` is the source for
  the volume list (it matches AROS volume names and includes emul-handler host-folder
  mounts). Host `statfs` is *not* used for the per-volume list (would mismatch
  names/double-count); it is only relevant if we later want a "host disk" summary
  row. **Open:** whether a host-folder AROS volume should show the *host* filesystem's
  free space (it does, because emul-handler's `ACTION_INFO` already proxies the host
  `statfs` — see [host-volume](../host-volume/design.md)) — so AROS `Info()` already
  gives the truthful host free-space for those, for free.
- **Disk I/O via IOKit adds a heavier link.** `IOBlockStorageDriver` statistics pull
  in IOKit + CoreFoundation. **Open:** ship the DISK page with free-space only
  first (`Info()`, zero new host dependency), add the host I/O-rate graph as a second
  increment. Free-space is the must-have; I/O-rate is the nice-to-have.
- **MUI string catalogs.** New tab labels need `MSG_TAB_*` entries in the locale
  catalogs (`workbench/system/SysMon/catalogs/`); missing entries fall back to the
  built-in English default, so this is non-blocking but should be done. `[AROS]`.
- **The graft, not a spike.** `[SM4]` (the real pages in the AROS tree) depends on
  SysMon rebuilding for darwin-aarch64 (`muimaster.library` + the Zune graph class
  are already in the desktop baseline per the
  [port inventory](../darwin-aarch64-port-inventory.md) §4). `[SM1]`–`[SM3]` are
  session-sized host spikes that stand alone; `[SM4]` rides the rebuild.

## References

AROS upstream (`/Users/user/Source/aros-upstream`):
- SysMon app: `workbench/system/SysMon/` — `main.c` (tabs `:94–97`,
  `RegisterGroup` `:168`, page children `:169–267`, active-page switch `:422–437`,
  modules array `:387`, timer re-arm `:408,441`), `sysmon_intern.h` (`SYSMON_TABCOUNT`
  `:24`, `tabs[]` `:33`, `struct SysMonData`, `struct SysMonModule` `:64`),
  `processor.c` (`GetCPUInfo` use `:56,108,142`, `ProcessorGroupObject` `:73`),
  `processor_gauge.c` (`GCIT_ProcessorLoad` `:138`, 16.16 decode `:144`),
  `processor_graph.c` (`GraphObject` `:103`, read-hook `:53–80`, multi-series
  `:188–191`, decode `:73,288`), `memory.c` (`AvailMem` `:25–54`), `video.c`
  (`aHidd_Gfx_MemoryAttribs` `:68,87`), `timer.c` (`SignalMeAfter` `:42`), `tasks.c`
  (Tasklist class), `mmakefile.src` (`FILES :=` `:5`).
- `processor.resource`: `compiler/include/resources/processor.h` (`GCIT_*`:
  NumberOfProcessors `:24`, ModelString `:28`, ProcessorSpeed `:32`, ProcessorLoad
  `:47`), `rom/processor/getcpuinfo.c` (generic stub returns 0),
  `arch/all-pc/processor/getcpuinfo.c:100–106` (real load, SMP only),
  `arch/all-pc/kernel/apic_heartbeat.c:122` (idle accounting),
  `arch/arm-native/processor/getcpuinfo.c` (stub returns 0). **No
  `arch/all-darwin|all-hosted|aarch64-all/processor/` exists.**
- Zune Graph class: `workbench/classes/zune/graph/{graph.c,graph.h,graph_intern.h}`
  (`MUIA_Graph_*` `graph.h:18–25`, `MUIM_Graph_GetSourceHandle`/`SetSourceAttrib`
  `:30,31`, `MUIV_Graph_Source_ReadHook`/`PenSrc` `:39,40`, ReadHook call
  `graph.c:956`, multi-source draw `graph.c:769`).
- DOS volume/disk: `compiler/include/dos/dos.h` (`struct InfoData` `:174`),
  `rom/dos/info.c` (`Info()`), `compiler/include/dos/dosextens.h` (`LDF_*`/`DLT_*`
  `:475–496`, `struct DosList`), `rom/dos/{lockdoslist.c,nextdosentry.c,
  unlockdoslist.c}`, **precedent** `workbench/c/Info.c:301–450` (volume walk +
  `Info()`/`ACTION_DISK_INFO`).
- Host-symbol mechanism: `arch/all-unix/bootstrap/hostlib.h`
  (`Host_HostLib_Open`/`GetPointer`), `arch/all-hosted/hostlib/`
  (`open.c:13`, `getinterface.c:15`, `getpointer.c:17`).

This repo (`/Users/user/Source/aros-aarch64`):
- `hosted/coreaudio/` (`coreaudio_shim.h`/`.c`, `coreaudio.exports`,
  `build/libcoreaudio.dylib`, `make coreaudio-abi`/`audio-smoke`) — the
  `hostlib`-loaded flat-C-ABI `.dylib` pattern this shim copies.
- `hosted/bsdsocket/` (`build/libbsdsockhost.dylib`) — host networking; the
  interface counters live in the same host realm.
- `graft/aros-ctl` + `hosted/cocoametal/` — the control harness for the `[SM4]`
  screenshot; `graft/deploy-check`/`run-window.sh` deploy `~/lib` shims.
- `docs/features/darwin-aarch64-port-inventory.md` — §6 (no network stack up), §4
  (muimaster/Zune in the desktop baseline), §7 (the shim/`hostlib` deploy pattern).

External prior art (web, not in the AROS tree):
- Apple Mach/BSD host-statistics APIs (all read-only, no entitlement): `host_statistics64`
  (`HOST_CPU_LOAD_INFO`, `HOST_VM_INFO64`), `host_processor_info`
  (`PROCESSOR_CPU_LOAD_INFO`), `sysctlbyname("hw.memsize")`, `sysctl(NET_RT_IFLIST2)`
  with `if_msghdr2`/`if_data64`, `getifaddrs(3)`, `getmntinfo(3)`/`statfs(2)`, and
  IOKit `IOBlockStorageDriver` statistics — published Apple developer documentation
  and the macOS SDK headers (`<mach/mach_host.h>`, `<sys/sysctl.h>`, `<net/if.h>`,
  `<net/route.h>`, `<sys/mount.h>`, `<ifaddrs.h>`). Interfaces only; no third-party
  implementation read.
- MUI/Zune `Register` group + `Graph`/`Gauge` classes — the published MUI class API
  (the same one SysMon itself targets). API surface, not third-party code.
