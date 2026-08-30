# emu68k portability and x86-64 assessment

> Assessment written 2026-08-19. This document describes the current system and
> a proposed portability path; it does not record an implementation decision.
> Here, “x86” means x86-64. A 32-bit x86 target would be a separate project.

## Summary

Most of the 68k compatibility system is portable in concept and implementation:
the DOS launch routing, hunk handling, guest-memory model, typed marshalling,
object facades, library policy, guest-library loading, and test corpus are not
inherently tied to Apple Silicon.

The current execution backend is tied to AArch64 and macOS. It emits AArch64
instructions using Emu68's decoder/emitter model, allocates executable memory
with Apple's `MAP_JIT` APIs, and is loaded into hosted AROS as a Mach-O dylib.
The reference interpreter is portable C, but it remains a corpus-driven subset
rather than a complete CPU backend.

The shortest credible route to x86-64 is therefore:

1. define a stable CPU-backend and platform-backend interface;
2. complete the portable interpreter behind that interface;
3. move the generic AROS library out of `arch/all-darwin`;
4. bring up x86-64 hosted AROS with the interpreter; and
5. build an x86-64 JIT only if measured performance justifies it.

A production-quality interpreter-backed x86-64 hosted port is plausibly a
two-to-three-month project for one experienced engineer. A new x86-64 JIT is a
separate, substantially larger project.

## Current component map

| Component | Current location | Portability assessment |
|---|---|---|
| Public launch contract | [`compiler/include/libraries/emu68k.h`](../../../../aros-upstream/compiler/include/libraries/emu68k.h) | Generic AROS API |
| Hunk-image retention and launch routing | [`rom/dos/emu68k_route.c`](../../../../aros-upstream/rom/dos/emu68k_route.c), `runcommand.c`, `createnewproc.c`, and `internalloadseg*` | Mostly generic AROS; should remain in the AROS fork |
| `emu68k.library`, native calls and marshalling | [`arch/all-darwin/libs/emu68k/`](../../../../aros-upstream/arch/all-darwin/libs/emu68k/) | Predominantly portable C, but placed under Darwin and bound to `libemu68k.dylib` through `hostlib.resource` |
| Host runtime, guest address space, contexts and guest libraries | [`hosted/emu68k/`](../../../hosted/emu68k/) | Mostly reusable logic with POSIX, macOS and current-engine assumptions mixed in |
| JIT dispatcher, hunk loader and diagnostics | [`hosted/jit68k/`](../../../hosted/jit68k/) | Loader and dispatcher concepts are reusable; generated-code paths are AArch64-specific |
| Adopted Emu68 sources | [`hosted/jit68k/emu68/`](../../../hosted/jit68k/emu68/) | MPL-2.0 boundary; its decoders and register model are coupled to the AArch64 emitter |
| Reference interpreter | [`j5d_interp.c`](../../../hosted/jit68k/j5d_interp.c) | Portable C and a strong differential oracle, but incomplete as a general backend |
| Bridge generator and reviewed policy | [`graft/gen-emu68k-bridge`](../../../graft/gen-emu68k-bridge), [`graft/emu68k-bridge-policy.json`](../../../graft/emu68k-bridge-policy.json) | Portable, except that generated output paths currently assume `arch/all-darwin` |
| Structure-layout generator | [`graft/gen-struct-layouts`](../../../graft/gen-struct-layouts) | Reusable; must generate and verify layouts for every target ABI |
| Engine and bridge fixtures | [`hosted/jit68k/apps68k/`](../../../hosted/jit68k/apps68k/), [`hosted/emu68k/nativelib/`](../../../hosted/emu68k/nativelib/) | Portable validation assets |
| Real-program corpus and reports | `graft/68k-corpus`, `graft/68k-legacy-suite`, Bridge Lab | Portable in purpose; launch and collection adapters are currently Macaros-oriented |
| Macaros packaging | [`graft/make-aros-release.sh`](../../../graft/make-aros-release.sh) | Correctly macOS-specific; packages both `emu68k.library` and `libemu68k.dylib` |

The current implementation is well beyond the original design-only state. The
seven waterline libraries have 738 public vectors classified, guest-side tail
libraries load, and complex application paths have been exercised. The open
work is application breadth, focused device/callback cases, and turning the
existing checks into a repeatable fresh-build gate. See [HANDOVER.md](HANDOVER.md)
for the detailed implementation status.

## Boundaries that need to be made explicit

### CPU backend

The host runtime currently enters the AArch64 `j5d_run()` path directly. A
portable engine needs a backend contract covering at least:

- backend and instance creation;
- register, FPU and exception state;
- run-for-a-bounded-quantum;
- yield, block, redirect, completion and fault results;
- native-to-guest callback re-entry;
- child guest contexts and process groups;
- kill/safe-point handling;
- statistics and diagnostic state; and
- teardown of code caches and guest state.

The library bridge should consume this contract without knowing whether the
implementation is an interpreter, AArch64 JIT or future x86-64 JIT.

### Platform services

Executable memory, fault collection, symbolization, environment access and
dynamic loading must be separated from CPU semantics:

| Platform | Engine loading | Executable memory |
|---|---|---|
| Hosted macOS | dylib via `hostlib.resource` | `MAP_JIT`, per-thread write protection, cache invalidation |
| Hosted Linux | shared object via `hostlib.resource` | `mmap`/`mprotect`; normal architecture-specific cache maintenance |
| Native AROS | linked directly or as an AROS module | AROS memory/cache APIs |
| Interpreter on any target | linked or host-loaded | None |

The report schema should remain common even when signal/fault collection and
native symbolization differ by platform.

### AROS integration

The generic portions of `emu68k.library` should move from `arch/all-darwin` to
an `all-hosted` or common location. Thin target files should select the host
engine name and binding mechanism. Generated source destinations must become
parameters rather than paths embedded in the generator.

Native x86-64 AROS is a later target than hosted x86-64. It has no host process
providing `dlopen`, POSIX mappings, signals or filesystem-backed crash helpers.
For native AROS the engine must either compile directly into the library or be
loaded as an AROS-native module, and platform facilities must be supplied by
AROS itself.

## Interpreter-first x86-64 port

The independent interpreter is the best portability floor because it already
uses the same register state, sandbox, LVO callback and byte-order model as the
JIT. It also avoids executable-memory policy entirely.

It is not yet sufficient to run arbitrary applications. It explicitly rejects
out-of-subset opcodes and still has incomplete effective-address, 68020,
exception and 68881/68882 cases. Turning it into a production backend requires:

- complete instruction and effective-address coverage for the supported CPU;
- precise CCR/X behavior and exception frames;
- required FPU instructions and formats, with documented precision limits;
- the same redirect/block/callback semantics as the JIT dispatcher;
- bounded quanta, child contexts and safe cancellation;
- hardware-window classification and guest-memory protection without relying
  on a host fault as the normal control path; and
- differential validation against the AArch64 JIT before relying on the
  interpreter alone on x86-64.

The existing instruction corpus, real-program corpus, negative controls and
byte-exact JIT/interpreter comparisons are a major advantage. They should become
a backend-neutral conformance suite rather than remain Makefile targets tied to
one Mac build.

## Why an x86-64 JIT is not a direct emitter swap

Emu68's decoder source emits AArch64 operations directly and assumes its fixed
host-register map. The current re-host preserves much of that model. x86-64 has
fewer general-purpose registers, different condition-code behavior, different
calling conventions and different executable-memory rules.

A production x86-64 backend must address:

- a new register allocator with spills or memory-backed guest registers;
- translation of 68k `N/Z/V/C/X` semantics to and from x86 flags;
- big-endian guest loads/stores using `MOVBE`, byte swaps or helpers;
- sandbox bounds and hardware-window checks;
- block exits, patching, chaining and safe-point polling;
- SysV, Windows and any AROS-native ABI differences;
- x87/SSE choices and 68881 precision/exception behavior; and
- W^X code-cache management for each operating system.

The maintainable design would introduce a small backend-neutral intermediate
representation and lower it to AArch64 or x86-64. Writing another decoder that
directly emits x86 instructions would duplicate too much of the current
architecture coupling.

There is also AArch64 debt to isolate while doing this. The adopted register map
uses `x18` for the translated PC. Apple reserves `x18`, and asynchronous hosted
execution must not depend on a live value surviving there. A portable backend
boundary should make this mapping private to the AArch64 implementation, and
the Apple backend should ultimately remap or memory-back the PC rather than
rely on `x18`.

## Recommended sequence

1. Freeze the current behavior with one backend-neutral conformance command.
   Include the engine corpus, generated bridge checks, guest-library sweep,
   negative controls and selected real applications.
2. Define versioned CPU and platform interfaces without changing AArch64
   behavior.
3. Move common AROS integration and make generator destinations target-neutral.
4. Complete the interpreter on Apple Silicon and differential-test it against
   the existing JIT.
5. Bring up x86-64 hosted Linux using the interpreter and the same corpus.
6. Port to native x86-64 AROS only after the hosted backend is stable.
7. Measure interpreter performance on real applications before deciding whether
   an x86-64 JIT is justified.

Porting should not start by copying `arch/all-darwin/libs/emu68k` into another
target directory. That would create two large bridge implementations before the
shared boundary is understood.

## Planning ranges

These are order-of-magnitude estimates for one engineer familiar with the
current code and AROS, not delivery commitments:

| Work | Estimate |
|---|---:|
| Backend interfaces and source/build reorganization | 2–4 weeks |
| Complete and integrate the interpreter | 4–8 weeks |
| x86-64 hosted Linux bring-up and corpus validation | 2–4 weeks |
| Native x86-64 AROS integration after that | 4–8 additional weeks |
| Production x86-64 JIT | 3–6 additional months |

An interpreter-backed hosted x86-64 release is therefore plausibly a
two-to-three-month project. A polished native port is more likely a
three-to-five-month program. The x86-64 JIT should be estimated and scheduled
separately.

## Repository organization

Supporting a second CPU and operating system is the point at which a dedicated
`emu68k` repository becomes useful. A reasonable ownership split would be:

- **emu68k repository:** engine contract, interpreter and JIT backends,
  platform adapters, generator, policy, third-party Emu68 boundary, fixtures,
  conformance suite and third-party notices;
- **AROS fork:** public launch API, DOS execution hook, `emu68k.library` shell
  and build integration; and
- **Macaros:** Apple-specific packaging, signing, user-facing configuration,
  reporting integration and release tests.

The split should preserve reproducible version pairing between the engine,
generated bridge and AROS integration. A tagged source dependency or vendored
release snapshot is safer than three repositories implicitly depending on the
latest branch heads.

As with the rest of this project, nothing should be submitted to AROS upstream
without discussing the exact proposal first and receiving explicit approval.

