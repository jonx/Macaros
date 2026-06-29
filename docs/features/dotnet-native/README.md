# Native .NET on AROS (aarch64)

Status: **design + rationale (drafted 2026-06-29)** · the full plan is in
[design.md](design.md). A **native software port** — ARM code built *for* AROS — in the same
family as [Rust on AROS](../rust-aros/README.md) and [native ffmpeg](../ffmpeg-native/README.md),
and the heavyweight cousin of the [ARexx/Regina](../arexx-host-port/README.md) language-host work.

## The one thing to know

"Native .NET on AROS" is a **runtime-port** task, not a PowerShell task. You port a .NET
*execution engine* to AROS; PowerShell (a big managed-code app) then rides on the shared .NET
class library. **The runtime is the whole job; PowerShell is the last — and riskiest — 10%.**

## The plan in five lines

1. **Runtime = Mono interpreter** (`MONO_AOT_MODE_INTERP`), single-threaded first, modeled on
   `src/mono/wasi` — *not* CoreCLR (a two-PAL + JIT + W^X-double-mapping monster) and *not*
   NativeAOT (bans `Add-Type`/`Reflection.Emit` → can't host PowerShell).
2. **The interpreter is mandatory, not optional** — it's the only W^X-safe way to run
   PowerShell's `Reflection.Emit`/script-block compilation (interpret emitted IL, don't JIT it).
3. **Honest W^X caveat:** "interpreter = zero executable memory" is half-true — the
   managed↔native **trampolines** still need executable pages, codegen-free only when
   AOT-precompiled. That tiny footprint rides the **already-proven** `MAP_JIT` layer.
4. **Most of the OS surface already exists here** — `pthread`, `mmap`/`mprotect`, a wired
   SIGSEGV→trap path, live `bsdsocket`, `posixc`. The gaps are the *same* `posixc`/unwinding
   grind Rust-std and ffmpeg already named — harden once, three payoffs.
5. **PowerShell is the stretch goal** (`[DN6]`); never run on Mono by anyone, with concrete
   known blockers (`AssemblyLoadContext`, collectible dynamic assemblies). If it doesn't land,
   the foundation still delivers **native C# on AROS** — a real win on its own.

## Milestones (`[DN*]`, each unattended, value-asserted)

`[DN0]` cross-build Mono + init · `[DN1]` execute managed IL (the headline) · `[DN2]` shared
BCL beachhead · `[DN3]` the platform shim (the mountain) · `[DN4]` sockets + managed
exceptions + threads · `[DN5]` a real C# app end-to-end · `[DN6]` PowerShell (stretch).

→ Full grounding, the facility-mapping table, effort calibration, and provenance: **[design.md](design.md)**.
