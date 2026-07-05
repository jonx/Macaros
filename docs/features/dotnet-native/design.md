# Native .NET on AROS (aarch64) — port the Mono interpreter, then run PowerShell on top

> Status: design + rationale (drafted 2026-06-29) · Target: aarch64-darwin hosted, EL0,
> W^X-strict · A **native software port**, the heavyweight cousin of [Rust on AROS](../rust-aros/README.md)
> and [native ffmpeg](../ffmpeg-native/README.md) — ARM code built *for* AROS, not a
> `hostlib` bridge. The end-goal scripting payload (PowerShell) is the same "host a language
> runtime" theme as the [ARexx/Regina](../arexx-host-port/README.md) work, an order of
> magnitude larger. Provenance note (MIT upstream porting ≠ the clean-reimpl process): §Provenance.

**The one-sentence truth.** "Native .NET on AROS" is not a PowerShell task — it is a
**runtime-port** task: you port a .NET *execution engine* to AROS, and PowerShell (a large
managed-code app) then rides on top of the shared .NET class library like any other
assembly. The runtime is the whole job; PowerShell is the last 10%, and the riskiest 10%.

This doc picks the runtime, grounds the choice in both the .NET source tree and this port's
already-built substrate, and lays a greppable `[DN*]` spike ladder that mirrors how
[Rust `[RS*]`](../rust-aros/README.md) went from "no_std runs" to "std is the mountain."

---

## Recommendation (lead, not bury)

**Port the Mono runtime in interpreter mode (`MONO_AOT_MODE_INTERP`), single-threaded
first, modeled file-for-file on [`src/mono/wasi`](https://github.com/dotnet/runtime/blob/main/src/mono/wasi/README.md).**
Use the **SSCLI PAL service list** as the stub-vs-implement checklist and the project's
existing W^X executable-memory layer for the one piece of native codegen that even the
interpreter needs (trampolines). PowerShell is the **stretch goal**; a *generic* "real C#
runs natively on AROS" is the achievable foundation that stands on its own value.

**Not CoreCLR** (heavier: a 19-subsystem Win32-shaped PAL *plus* a second POSIX-shaped
libraries PAL, a JIT, and W^X double-mapping — months even for a slice; the Haiku CoreCLR
port had to *disable* the W^X double-mapper). **Not NativeAOT** (a hard dead-end for
PowerShell — it bans `Add-Type` and `Reflection.Emit` by construction). The reasoning is
§"Runtime choice" with sources.

---

## Why this is more tractable than "port .NET" sounds — and where it isn't

The instinct is "porting .NET is impossible on an exotic OS." Two facts move it from
impossible to *bounded-but-large*:

1. **The hardest Apple-Silicon problem is already solved in this tree.** A managed runtime's
   worst enemy on aarch64 macOS is W^X executable memory. CoreCLR's own Apple-Silicon path
   ([`minipal/Unix/doublemapping.cpp`](https://github.com/dotnet/runtime/blob/main/src/coreclr/minipal/Unix/doublemapping.cpp))
   uses **exactly** `MAP_JIT` + `pthread_jit_write_protect_np` — the same mechanism this
   project already proved for the 68k JIT
   ([hosted/jit68k/jit_region.c](../../../hosted/jit68k/jit_region.c);
   [68k-jit INTERFACE §2](../68k-jit/INTERFACE.md)) and for native module loading
   ([native-modules/design.md](../native-modules/design.md), the `KrnSetProtection` +
   `sys_icache_invalidate` path). The executable-memory substrate a .NET runtime needs is
   built and entitlement-resolved here.
2. **The OS facilities a runtime needs mostly exist already** (full audit below): `pthread`
   over exec tasks, `mmap`/`mprotect`, a **SIGSEGV→trap signal path already wired**, live
   `bsdsocket` for managed sockets, and a `posixc` C library. The gaps are the *same* grind
   [Rust](../rust-aros/README.md) and [ffmpeg](../ffmpeg-native/README.md) already named —
   `posixc` completeness, pthread-under-load, unwinding wiring — done once for all three.

Where it *isn't* easy: Mono is still ~200K LOC of C to cross-compile and bring up; a
non-POSIX-complete libc breaks Mono's own "a new Unix OS is a few days" assumption (it
assumes a full libc); and PowerShell specifically has never run on the Mono flavor and has
concrete known blockers (§PowerShell).

## Runtime choice — Mono-interp, with the W^X truth stated honestly

Three candidate runtimes; the research settles it:

| Option | OS surface to port | Runtime codegen / W^X | PowerShell? | Verdict |
|---|---|---|---|---|
| **CoreCLR (JIT)** | **Two** PALs — the Win32-shaped [`src/coreclr/pal`](https://github.com/dotnet/runtime/tree/main/src/coreclr/pal/src) (19 subsystems: threads, vmem, sync, exception, file, loader, time, env) **+** the POSIX-shaped [`System.Native`](https://github.com/dotnet/runtime/tree/main/src/native/libs) (sockets, file, crypto) | Full JIT; **W^X double-mapping** (RW view + RX view of one physical page) — Haiku port had to **disable** it (`DOTNET_EnableWriteXorExecute=0`) | yes (its native target) | **too heavy** for a first port; revisit only if JIT-class perf is required |
| **Mono (interpreter)** | **One**, lean — `mono/mini` interp + `eglib` + an `io-layer`/shim; **`src/mono/wasi` is a structural twin** of AROS (thin syscall shim + interp + shared BCL, single-thread, no JIT) | **Interpreter: no per-method codegen.** Only the managed↔native **trampolines** need executable pages — and are **codegen-free when AOT-precompiled** (`emit_trampolines()`) | **plausible** (shared BCL); never demonstrated | **recommended beachhead** |
| **NativeAOT** | compile-time only (no runtime to host) | none at runtime | **dead end** — bans `Reflection.Emit` ([IL3050](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/warnings/il3050)) and dynamic load; PowerShell needs both | **excluded** |

### The W^X truth — do not overstate "interpreter = no executable memory"

This is the subtle point the research nailed and the doc must state precisely:

- The Mono **interpreter eliminates per-method JIT codegen** — method bodies are interpreted
  IL, never emitted machine code. That removes the bulk of the RWX pressure.
- It does **not** by itself remove executable code at the **managed↔native boundary**:
  trampolines/thunks are normally generated at runtime (Mono trampolines are *"generated at
  runtime using the native code generation macros"*). The configuration with a documented
  *"no code needs to be generated at runtime"* guarantee is **interpreter + Full-AOT**
  (`MONO_AOT_MODE_INTERP`), where trampolines are **precompiled into the image**. This is the
  iOS/console model ([MAUI interpreter doc](https://learn.microsoft.com/en-us/dotnet/maui/macios/interpreter)).
- **So:** target `MONO_AOT_MODE_INTERP` so the *only* executable code is **precompiled
  trampolines**, and back those few pages with the project's existing `MAP_JIT` path. Net: a
  tiny, fixed executable-memory footprint that the substrate already handles — not a JIT.

**`Reflection.Emit` is why the interpreter is mandatory, not optional.** PowerShell compiles
every script block via `Expression.Lambda(...).Compile()`
([PS `Compiler.cs`](https://github.com/PowerShell/PowerShell/blob/master/src/System.Management.Automation/engine/parser/Compiler.cs))
and uses `Add-Type`. Under Full-AOT-*without*-interp, `Reflection.Emit` throws
`ExecutionEngineException: "Attempting to JIT compile method while running in aot-only
mode."` The Mono **interpreter re-enables it** by *interpreting the emitted IL instead of
JIT-compiling it* — the W^X-safe path, and the only one that runs PowerShell.

## What a .NET runtime needs vs. what this AROS port already has

The load-bearing table — every "have" is a real file, every gap is named. (Facility audit
against `../aros-upstream`, 2026-06-29.)

| Runtime needs (PAL/shim seam) | AROS status | Where |
|---|---|---|
| **Threads** (`CreateThread`, suspend/resume, TLS) | **PRESENT** — full `pthread` over exec tasks; map runtime threads onto AROS tasks. *Single-threaded first* (the WASI model) sidesteps the robustness risk | [compiler/pthread/](../../../../aros-upstream/compiler/pthread/); `pthread_key_create` in `posixc` |
| **Virtual memory** (reserve/commit/protect) | **PRESENT** — `mmap`/`mprotect` via `KrnAllocPages`/`KrnSetProtection` | [arch/all-unix/kernel/setprotection.c](../../../../aros-upstream/arch/all-unix/kernel/setprotection.c); [allocpages.c](../../../../aros-upstream/arch/all-unix/kernel/allocpages.c) |
| **Executable memory** (trampolines only, in interp mode) | **PRESENT & PROVEN** — `MAP_JIT` + `pthread_jit_write_protect_np` + `sys_icache_invalidate` | [hosted/jit68k/jit_region.c](../../../hosted/jit68k/jit_region.c); [68k-jit INTERFACE §2](../68k-jit/INTERFACE.md) |
| **Hardware-fault → managed exception** (SIGSEGV for null-checks / GC) | **PARTIAL — machinery wired, bridge is new code** — `sigaction` for SIGSEGV/SIGBUS/SIGILL/SIGFPE installed; SIGSEGV→trap 2 mapped; `ExceptionContext` aarch64 layout fixed. The runtime's `signal→DispatchManagedException` bridge is the .NET-side wiring | [arch/all-unix/kernel/kernel.c:408-461](../../../../aros-upstream/arch/all-unix/kernel/kernel.c); [cpu_aarch64.c:20-30](../../../../aros-upstream/arch/all-unix/kernel/cpu_aarch64.c) |
| **Sockets** (BCL `System.Native` `SocketPal`) | **PRESENT & LIVE** — `bsdsocket` proven (WaitSelect + DNS). Managed sockets come *nearly free*, exactly as Rust `net` does | [bsdsocket-net](../bsdsocket-net/README.md) |
| **File / stream I/O** | **PRESENT** — `posixc` (`open`/`read`/`write`) over `dos.library` | [compiler/crt/stdc/](../../../../aros-upstream/compiler/crt/stdc/) |
| **Time / env / exit** | **PRESENT** — `clock_gettime`, `nanosleep`, `getenv` in `posixc` | `compiler/posixc/` |
| **Dynamic loading** (`dlopen`) | **ABSENT — but not on the critical path.** Managed assemblies load via Mono's *own* loader (not `dlopen`). Native P/Invoke into host libs uses [hostlib.resource](../../../../aros-upstream/arch/all-hosted/hostlib/) (`HostLib_Open`/`GetPointer`); the runtime itself links as a native AROS module (`LoadSeg`, **code-model=large**) | hostlib + [internalloadseg_elf.c](../../../../aros-upstream/rom/dos/internalloadseg_elf.c) |
| **Unwinding** (libunwind — exceptions, both C++ and managed) | **PARTIAL/UNVERIFIED** — libunwind is in the crosstools LLVM build; wiring untested (same open item as Rust unwinding) | [rust-aros/README.md](../rust-aros/README.md) (item: `eh_personality`/libunwind) |
| **C / C++ toolchain** | **Mono is C** → fits the proven path. (CoreCLR is C++; basic C++ exceptions compile, RTTI untested — another reason Mono wins first) | [rust-aros](../rust-aros/README.md) precedent; `developer/debug/test/cplusplus/exception.cpp` |
| **C library completeness** (`posixc`) | **PARTIAL — the real grind** — the `printf`-float bug ([__vcformat.c](../../../../aros-upstream/compiler/crt/stdc/__vcformat.c)) is the prototype; expect `glob`/locale/large-file gaps the Mono PAL assumes. Shared with Rust-std + ffmpeg — harden once | [build/](../build/README.md), `UPSTREAM-NOTES.md` item 34 |
| **GOT-free codegen** (`-mcmodel=large`) | **REQUIRED & PROVEN** — AROS loads GOT-less; non-negotiable for the runtime's native objects | [rust-aros/README.md](../rust-aros/README.md) ([RS0] gotcha) |

**Reading of the table:** the *mechanisms* are largely present (W^X proven, signals wired,
sockets live, threads/vmem/posixc present); the *runtime-port work* is (a) cross-compiling
Mono, (b) writing the thin Mono↔AROS shim against these seams, (c) the `posixc`/unwinding
hardening shared with the other native ports, and (d) the SIGSEGV→managed-exception bridge.

## PowerShell on the Mono flavor — the honest stretch

PowerShell is the *goal*, and it is the part with real, named risk. State it plainly so the
foundation isn't oversold:

- **In principle loadable.** Modern .NET is "two runtimes, one BCL": PowerShell's assemblies
  (`System.Management.Automation`, `Microsoft.PowerShell.*`) target the **shared** class
  library ([src/libraries](https://github.com/dotnet/runtime/tree/main/src/libraries)), and
  the same assemblies are *designed* to bind on either the CoreCLR or Mono flavor. The .NET
  team frames the flavors as *"highly compatible"* — **not** "guaranteed interchangeable."
- **Never demonstrated.** No one has run real Microsoft PowerShell on Mono
  ([PS issue #2613](https://github.com/PowerShell/PowerShell/issues/2613) documents the
  blockers, then petered out). The only "PowerShell on Mono" ever shipped was **Pash**, a
  separate clean reimplementation, **archived 2019**.
- **Three concrete blockers to budget for** (from #2613): (i) `System.Management.Automation`
  *"relies heavily on `AssemblyLoadContext`,"* historically a CoreCLR-only surface; (ii)
  Mono's collectible dynamic assemblies (`RunAndCollect`) historically *leaked*; (iii)
  `Reflection.Emit`/`DynamicMethod` — **solved** by the interpreter (it interprets the
  emitted IL), which is precisely why the interp config is mandatory.
- **Verdict:** treat PowerShell as `[DN6]`, gated on a working generic .NET (`[DN0]`–`[DN5]`).
  If PowerShell's BCL-surface gaps prove too deep, the foundation still delivers *native C#
  on AROS* — a real payoff (memory-safe AROS tooling, FFI into native `libav*`/Rust), and a
  far better ARexx-class scripting host than the language layer the
  [ARexx/Regina](../arexx-host-port/README.md) path offers.

## The `[DN*]` spike ladder — each unattended, value-asserted

Mirrors the [Rust `[RS*]`](../rust-aros/README.md) and [native-modules `[NL*]`](../native-modules/design.md)
discipline: every PASS asserts a **returned value / known byte stream** the harness greps,
never "it didn't crash." Ordered foundation-first; PowerShell last.

- **`[DN0]` Cross-build the runtime (the C-portability beachhead).** Cross-compile Mono
  (`mini` interpreter + `metadata` + SGen GC + `eglib`, `-runtimeFlavor mono`,
  `MONO_AOT_MODE_INTERP`) with the AROS clang crosstools — **code-model=large**, the
  `startup.o`-not-`elf-startup.o` gotcha, `--allow-multiple-definition`. Link into an AROS
  `C:` command. **PASS:** on booted AROS, `mono_jit_init()` returns non-NULL and a runtime
  version string prints via `dos` Output. **FAIL:** link error, or init crash. *This is the
  ~200K-LOC port's "does it even build and initialize" gate — big but bounded; the Mono
  "new Unix OS" path, adjusted for a non-POSIX libc.*
- **`[DN1]` Execute managed IL (the headline — ".NET runs natively on AROS").** Embed Mono;
  load a trivial `Test.dll` built on the Mac with stock `csc`/`dotnet`; `mono_runtime_invoke`
  a static method returning `0x6804`. **PASS:** the managed method runs *under the
  interpreter* and returns `0x6804` on booted AROS (assert the value — a broken trampoline or
  stale I-cache yields a *wrong value*, the H/J discipline). **FAIL:** load/JIT-mode error,
  wrong value, fault. *The analog of Rust `[RS1]`.*
- **`[DN2]` BCL beachhead.** A managed program exercising the shared class library —
  `List<T>`, `string.Format`, arithmetic, `Console.WriteLine` routed through the shim to
  `dos` Output. **PASS:** the emitted bytes match the host-computed expected output exactly
  (proves the shared BCL binds on the Mono flavor *on AROS*). **FAIL:** missing-method /
  type-load / wrong bytes.
- **`[DN3]` The platform shim (the mountain — mirrors Rust `[RS3]`).** Implement the Mono↔AROS
  OS seam against the table above: memory reserve/commit/protect, file/stream I/O, time, env,
  exit, single-thread, and trampoline executable memory via the `MAP_JIT` layer. **SSCLI PAL
  service list = the stub-vs-implement checklist; `src/mono/wasi` = the template.** **PASS:**
  a managed program does buffered file I/O (write+read-back a file on `MacRW:`), times a loop,
  reads an env var — all asserted. Gated on `posixc` completeness (shared work).
- **`[DN4]` Sockets, exceptions, threads.** (a) `System.Net.Sockets` → `System.Native`
  `SocketPal` → **`bsdsocket`** (nearly free, live) — a managed `TcpClient` fetches a URL,
  assert the response bytes. (b) Managed `try/catch` over a deliberate null-deref —
  SIGSEGV→trap→`DispatchManagedException` bridge + libunwind — assert the `catch` ran. (c)
  Two managed threads over `pthread`/exec tasks with the GC's stop-the-world — assert a
  contended counter. Each leg its own marker/value.
- **`[DN5]` A real C# app, end-to-end.** A non-trivial managed program (HTTP GET over
  `bsdsocket` → JSON parse → write result to a `MacRW:` file) runs on booted AROS via the
  control harness. **PASS:** the file contains the expected parsed value. *".NET is real on
  AROS."* The foundation's terminal value, independent of PowerShell.
- **`[DN6]` PowerShell bring-up (the stretch).** Load `System.Management.Automation` on the
  Mono-flavor runtime; work the known blockers (`AssemblyLoadContext`, collectible dynamic
  assemblies, `Reflection.Emit`-via-interpreter). **PASS:** `pwsh -NoProfile -c "1+1"` prints
  `2` on booted AROS, and a script block with a variable + pipeline evaluates. **FAIL (graceful):**
  a documented BCL-surface gap — recorded, foundation unaffected. *Explicitly high-risk; the
  ladder's value does not depend on it.*

Build/run in the existing harness style (host build legs + `graft/dotnet-smoke` for the
booted assertions); `[DN1]`+ ride the graft and are gated on the boot reaching `dos.library`
([graft/WORKFLOW.md](../../../graft/WORKFLOW.md) F1/F2), as with the other in-AROS spikes.

## Effort & honest debt

- **Scale.** This is the **largest** native-software undertaking proposed in the repo —
  comparable to or beyond the [68k JIT](../68k-jit/design.md). Calibration from real ports:
  Mono "a few days for a new Unix OS *if* POSIX+libc are complete" (AROS is **not** complete
  → that assumption breaks); a CoreCLR **interpreter** bring-up *"1–2 months for an engineer
  familiar with the codebase"*; the Haiku CoreCLR GSoC was ~3–4 months **and** had to
  hand-roll an epoll substitute, UNIX datagram sockets, fix ~6 `mmap`/`mprotect` bugs, and
  disable W^X. Budget in **engineer-months**, not weeks, even for Mono-interp.
- **`posixc` is the gating substrate.** The `printf`-float bug is the prototype of a class:
  Mono's PAL will assume libc surface AROS only partly has (`glob`, locale, large-file seek,
  `mmap(MAP_FIXED)` semantics). This is the **same** hardening Rust-std and ffmpeg need — do
  it once, three payoffs.
- **The trampoline executable footprint is small but real.** Even interp+AOT needs *some*
  executable pages; they ride the proven `MAP_JIT` layer, but the per-thread toggle
  discipline ([68k-jit INTERFACE §2](../68k-jit/INTERFACE.md), R-JIT-THREAD) must hold for
  whichever thread executes trampolines. **UNVERIFIED** until `[DN1]`.
- **The SIGSEGV→managed-exception bridge is new code** on top of the wired signal path — and
  must coexist with AROS's own trap handling and the [crash-handling](../crash-handling/) Guru
  path without fighting over the handler. **UNVERIFIED.**
- **PowerShell-on-Mono is unproven by anyone.** `AssemblyLoadContext` and collectible-assembly
  behavior are the likely walls; flagged, not solved.
- **Threading-under-load is UNVERIFIED** (the same "FF2 #1 risk" the ffmpeg doc flags). Start
  single-threaded; earn concurrency at `[DN4]`.
- **GC over exec memory.** SGen reserve/commit/decommit maps onto `KrnAllocPages`/`mmap`; the
  Rust `#[global_allocator]`-over-`AllocVec` proof shows exec-backed managed heaps work in
  principle, but SGen's write-barrier/card-table and decommit assumptions are **UNVERIFIED**
  on this target.

## Provenance — this is upstream porting, not the clean-reimpl process

Important distinction from the rest of `docs/features/`: **.NET and Mono are MIT-licensed
open source.** Porting them means legitimately **building and modifying their own source**
(adding an AROS shim/PAL target), the way the FreeBSD/Haiku ports did. That is ordinary
upstream porting under MIT — it is **not** governed by [CLEANROOM.md](../CLEANROOM.md), whose
"read no third-party implementation source" rule exists to keep the *clean reimplementations*
(emulators, drivers) independent. Here the third-party source **is** the deliverable's
upstream. The AROS-side shim code this project writes is its own work; the .NET/Mono code it
builds on carries its own MIT terms and attribution. No emulator or competitor
implementation is consulted — the runtime being ported is itself the licensed upstream.

## References

AROS tree (`../aros-upstream`) and this repo, confirmed 2026-06-29.

Project substrate this leans on:
- [hosted/jit68k/jit_region.{h,c}](../../../hosted/jit68k/) — the `MAP_JIT` +
  `pthread_jit_write_protect_np` + `sys_icache_invalidate` W^X layer (proven); frozen seam in
  [../68k-jit/INTERFACE.md](../68k-jit/INTERFACE.md) §2.
- [../native-modules/design.md](../native-modules/design.md) — `KrnAllocPages`/`KrnSetProtection`
  executable-memory + AArch64 ELF `LoadSeg` (code-model=large, the I-cache gap).
- [../rust-aros/README.md](../rust-aros/README.md) — the native-port trajectory this mirrors:
  no_std proven, std-as-OS-port, the `posixc`/unwinding/code-model gotchas, exec-backed allocator.
- [../bsdsocket-net/README.md](../bsdsocket-net/README.md) — live host sockets (managed
  `System.Net.Sockets` rides this).
- [../ffmpeg-native/README.md](../ffmpeg-native/README.md) — the shared `posixc`-hardening grind.
- [../arexx-host-port/README.md](../arexx-host-port/README.md) — the language-runtime-host
  theme (Regina); PowerShell is its heavyweight cousin.
- AROS facility cites: [arch/all-unix/kernel/setprotection.c](../../../../aros-upstream/arch/all-unix/kernel/setprotection.c),
  [allocpages.c](../../../../aros-upstream/arch/all-unix/kernel/allocpages.c),
  [kernel.c:408-461](../../../../aros-upstream/arch/all-unix/kernel/kernel.c),
  [cpu_aarch64.c:20-30](../../../../aros-upstream/arch/all-unix/kernel/cpu_aarch64.c),
  [compiler/pthread/](../../../../aros-upstream/compiler/pthread/),
  [compiler/crt/stdc/__vcformat.c](../../../../aros-upstream/compiler/crt/stdc/__vcformat.c),
  [arch/all-hosted/hostlib/](../../../../aros-upstream/arch/all-hosted/hostlib/),
  [rom/dos/internalloadseg_elf.c](../../../../aros-upstream/rom/dos/internalloadseg_elf.c).

.NET / Mono / PowerShell upstream (web, MIT source — the port's basis, not consulted as
competitor implementation):
- CoreCLR PAL — [src/coreclr/pal](https://github.com/dotnet/runtime/tree/main/src/coreclr/pal/src),
  [README](https://github.com/dotnet/runtime/blob/main/src/coreclr/pal/README.md),
  [guide-for-porting](https://github.com/dotnet/runtime/blob/main/docs/design/coreclr/botr/guide-for-porting.md).
- W^X double-mapping / Apple-Silicon path — [minipal/Unix/doublemapping.cpp](https://github.com/dotnet/runtime/blob/main/src/coreclr/minipal/Unix/doublemapping.cpp),
  [executableallocator.cpp](https://github.com/dotnet/runtime/blob/main/src/coreclr/utilcode/executableallocator.cpp),
  [Apple: porting JITs to Apple Silicon](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon).
- Hardware-fault → managed exception — [signal.cpp](https://github.com/dotnet/runtime/blob/main/src/coreclr/pal/src/exception/signal.cpp),
  [seh-unwind.cpp](https://github.com/dotnet/runtime/blob/main/src/coreclr/pal/src/exception/seh-unwind.cpp).
- Mono runtime / interpreter / AOT — [src/mono README](https://github.com/dotnet/runtime/blob/main/src/mono/README.md),
  [Mono interpreter](https://www.mono-project.com/news/2017/11/13/mono-interpreter/),
  [AOT](https://www.mono-project.com/docs/advanced/runtime/docs/aot/),
  [trampolines](https://www.mono-project.com/docs/advanced/runtime/docs/trampolines/),
  [embedding](https://www.mono-project.com/docs/advanced/embedding/),
  [mini-porting](https://www.mono-project.com/docs/advanced/runtime/docs/mini-porting/).
- The WASI analog (closest structural twin) — [src/mono/wasi/README.md](https://github.com/dotnet/runtime/blob/main/src/mono/wasi/README.md),
  [WASM features](https://github.com/dotnet/runtime/blob/main/src/mono/wasm/features.md).
- Shared BCL ("two runtimes, one BCL") — [System.Private.CoreLib README](https://github.com/dotnet/runtime/blob/main/src/libraries/System.Private.CoreLib/src/README.md).
- PowerShell on Mono — [PS issue #2613](https://github.com/PowerShell/PowerShell/issues/2613),
  [Pash (archived)](https://github.com/Pash-Project/Pash),
  [PS Compiler.cs](https://github.com/PowerShell/PowerShell/blob/master/src/System.Management.Automation/engine/parser/Compiler.cs).
- NativeAOT limits — [NativeAOT overview](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/),
  [IL3050](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/warnings/il3050).
- Port-effort datapoints — [Haiku .NET GSoC final report](https://www.haiku-os.org/blog/trungnt2910/2023-08-20_gsoc_2023_dotnet_port_final_report/),
  [SSCLI PAL (Ch.9)](https://www.oreilly.com/library/view/shared-source-cli/059600351X/ch09.html).
