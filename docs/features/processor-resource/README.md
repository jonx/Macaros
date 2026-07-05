# processor.resource (darwin/AArch64) — host-sourced CPU facts

> Status: started (partial) - host CPU shim (hosted/hostcpu/) built, green, tested; AROS-side processor.resource backend pending · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

Give the hosted AROS the truth about the CPU it runs on. `processor.resource` is the
architecture-neutral source every CPU query in AROS flows through — `ShowConfig`,
`SysMon`'s CPU page, `CPUInfo`, any `GetCPUInfo()` caller. On darwin-aarch64 today it
is the **generic stub**: model "Unknown", speed/cache/load 0, architecture ARM only
because `defaults.h` hard-codes it. The reason is architectural — on bare metal those
facts come from EL1 system registers (`MIDR_EL1`, the `ID_AA64*` registers, the kernel
idle accounting), but the hosted port runs at **EL0** where every one of them traps.

So the facts must come from the host, which is the project thesis ("macOS owns the
drivers; AROS reaches them via standard exec I/O"): macOS owns the silicon; AROS reads
the CPU facts through `hostlib.resource` → `libSystem.dylib` → `sysctlbyname` /
`host_statistics64`. This feature is a **darwin/AArch64 arch backend** for
`processor.resource` that fills `GetCPUInfo()` from the host — model string, core
counts and P/E split, endianness, caches, NEON/FP/crypto features, and CPU load — so
*every* caller sees the real CPU, not just the one command that hacks it today.

It is the proper home for what `CPUInfo` already does privately:
`workbench/c/CPUInfo/cpuinfo_arch.c` carries its own `OpenResource("hostlib.resource")`
→ `HostLib_Open("libSystem.dylib")` → `sysctlbyname` block, and its own comment names
the fix — "a darwin processor.resource backend … so that GetCPUInfo() and hence
ShowConfig and every other caller would see the real model too." This doc turns that
paragraph into a buildable plan and **deletes the hack** once the backend serves the
model.

## Division of responsibility vs. SysMon

This doc OWNS the `processor.resource` layer (CPU identity / features / topology /
load); the [system monitor](../system-monitor/README.md) OWNS the GUI (its new
Network/Disk/Host pages + the `libhoststats.dylib` shim). They share **CPU load**:
once this backend lands, `GCIT_ProcessorLoad` returns a real value on this target, and
SysMon's existing CPU tab just works. De-dup rule (default): SysMon's Host-page CPU%
consumes `processor.resource`, so load is computed in **one** place — see design.md.

## How it verifies (unattended)

Numeric, no human, no TCC (every call — `sysctlbyname`, `host_statistics64`,
`host_processor_info` — is a read-only Mach/BSD query needing no entitlement). One
greppable host binary per marker, prefix **`[CP*]`**:

- **[CP1]** real host facts are plausible — core counts consistent (`physical ≤
  logical`, `P+E == physical`), brand non-empty, `0 ≤ load ≤ 100`, LE byteorder,
  NEON present, 64-bit address width.
- **[CP2]** the load delta → 16.16 scaling (idle/busy extremes, SysMon-decode bounds,
  guards).
- **[CP3]** the same values across the `hostlib`/dylib boundary (== the raw `sysctl`).
- **[CP4]** graft: the darwin arch backend builds and links (link map shows the darwin
  `getcpuinfo.o`, not the generic getter).
- **[CP5]** graft: `GetCPUInfo` returns the real values in booted AROS, and
  `CPUInfo`/`ShowConfig` print the real model instead of "Unknown".

## Biggest unknown

The **arch-module build wiring** — that `kernel-processor` for darwin-aarch64 actually
pulls a new `arch/all-darwin/processor/` (`arch=darwin`, mirroring how
`arch/all-linux/processor/` registers `arch=linux`) and that a darwin `getcpuinfo`
cleanly replaces the generic getter — is **UNVERIFIED until a link** (the cross-cutting
change `CPUInfo`'s comment warns about). And there is **no `PROCESSORARCH_ARM64`**
constant (`processor.h` stops at `PROCESSORARCH_ARM`); default is to report ARM, not
invent one. Both are owner-gated; see design.md / spec.md "Open questions".

## Links

- [design.md](design.md) — why, the EL0-unreadable-vs-host-derivable map, the grounded
  `processor.resource` contract, the two arch-module seams (Pattern A replace-getter vs
  Pattern B′ generic-override-hook), the build wiring, the spike plan.
- [spec.md](spec.md) — implementation spec (the `hostcpu_shim.h` ABI + the AROS-side
  arch backend + the CPUInfo fold-in), under the
  [independent-work process](../CLEANROOM.md).
- Folds in: `workbench/c/CPUInfo/cpuinfo_arch.c` (the private host-query hack — deleted
  once this lands).
- Reused: the host-call chain `CPUInfo` already uses (`hostlib.resource` →
  `libSystem.dylib` → `sysctlbyname`); the flat-C-ABI dylib pattern from
  [CoreAudio](../coreaudio-audio/README.md) / [BSD sockets](../bsdsocket-net/README.md)
  (for the Mach load path); the [control harness](../control-harness/README.md) (the
  `[CP5]` `ShowConfig`/`CPUInfo` screenshot).
- Consumed by: [system monitor](../system-monitor/README.md) (CPU load / Host page).
- Source to add: `arch/all-darwin/processor/` (in `aros-upstream`); shim
  `hosted/hostcpu/` (in this repo).
