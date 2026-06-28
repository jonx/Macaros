# System monitor — real Mac stats in the AROS SysMon

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

AROS already ships a MUI/Zune system monitor — `SysMon`
(`workbench/system/SysMon/`) — with CPU per-core gauges/graph, memory size/free,
and a live task/process list. This feature **extends** it (does not rebuild it)
with three new pages:

- **NETWORK** — throughput graph (RX/TX bytes-per-second) from the host's
  per-interface byte counters;
- **DISK** — free/total space per AROS volume plus host disk I/O bytes/sec;
- **HOST** — the Mac's real CPU load and memory, because for a *hosted* guest
  those are the honest numbers.

The twist that makes this the project thesis ("macOS owns the drivers; AROS
reaches them via standard exec I/O") is sharp here: a hosted AROS has **no NIC, no
disk, no real CPU of its own** — the truthful system numbers are the host's. And
critically, AROS's own CPU-load source (`processor.resource` `GCIT_ProcessorLoad`)
**returns 0 on darwin-aarch64** (it is the unimplemented stub for this target — see
design.md), so the host-stats path is not just "more truthful", it is the *only*
way SysMon's CPU page shows a real number on Apple Silicon.

The numbers arrive through a small read-only host-stats shim
(`libhoststats.dylib`), dlopen'd via `hostlib.resource` exactly like the CoreAudio,
BSD-sockets, clipboard and host-volume shims. No entitlement, no TCC: the Mach/BSD
stats calls used (`host_statistics64`, `sysctl`, `getifaddrs`, `getmntinfo`) are
all read-only and prompt-free. When not hosted, SysMon falls back to its
AROS-native numbers unchanged.

## How it verifies (unattended)

Primary oracle is numeric, not visual: feed **known synthetic values** through the
shim and assert the page's data model reads them back; and assert the real shim
returns plausible values (0 ≤ CPU% ≤ 100, used ≤ physical memory, monotone byte
counters). One greppable host binary per marker, prefix **`[SM*]`**. The MUI render
is confirmed with a [control-harness](../control-harness/README.md) screenshot —
secondary, not the gate.

## Links

- [design.md](design.md) — why, the grounded SysMon hook points + AROS contracts,
  the host APIs, the spike plan.
- [spec.md](spec.md) — implementation spec (the host-stats shim ABI + the AROS-side
  page wiring), under the [independent-work process](../CLEANROOM.md).
- Reused: [CoreAudio shim](../coreaudio-audio/README.md) (the `hostlib`-loaded flat
  C ABI dylib pattern), [BSD sockets](../bsdsocket-net/README.md) (host networking,
  where the interface counters live), [control harness](../control-harness/README.md)
  (the screenshot used for GUI verification).
- Source extended: `workbench/system/SysMon/` (in `aros-upstream`).
