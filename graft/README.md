# graft/ — the first concrete AArch64-darwin patch set for the AROS tree

These are the **starter patches** that turn [GRAFT.md](../GRAFT.md)'s map into real
code against `aros-upstream`. They are faithful translations of *proven* templates,
not speculation:

- `cpu_aarch64.h` is `arch/all-darwin/kernel/cpu_arm.h` re-expressed for AArch64,
  using the exact macOS `mcontext` fields our H2/H4 spikes already save/restore
  live (`uc_mcontext->__ss.{__x,__fp,__lr,__sp,__pc,__cpsr}`, FP via `__ns`),
  grounded against the SDK (`mach/arm/_structs.h`, `arm/_mcontext.h`).
- `cpucontext-aarch64.h` fixes AROS's broken `struct ExceptionContext` so its
  layout matches Darwin's `_STRUCT_ARM_THREAD_STATE64` — required because the host
  kernel's `SAVEREGS`/`RESTOREREGS` `CopyMemQuick` straight between them.
- `configure-darwin-aarch64.diff` is the `*aarch64*` case for the darwin hosted
  flavour, modelled on the existing `*x86_64*`/`*arm*` cases.

## Build status — HONEST

**These are NOT built or run.** They cannot be, yet: AROS's darwin hosted backend
needs its full (old-leaning) build environment stood up first (see GRAFT.md's live
grounding — `configure` accepts `aarch64-darwin` but then walks a chain of GNU
build-tool prerequisites), and the backend itself is bit-rotted to ~2010 Xcode.

What *is* verified: every `mcontext` field these files touch is the same code that
compiles and runs in the H2/H4/H6 spikes on this Mac. So the host-glue layer is
grounded; what remains unproven until the build stands up is the AROS-side
integration (genmodule, the `kernel.resource` wiring, the stale backend).

Treat these as a reviewed, grounded *starting point* for the integration — the
thing a person (or a future session) picks up to make AROS itself build, not a
finished port.

## Apply (once the environment exists)

```
cp graft/cpu_aarch64.h        $AROS/arch/all-darwin/kernel/cpu_aarch64.h
cp graft/cpucontext-aarch64.h $AROS/arch/aarch64-all/include/aros/cpucontext.h
# then fold configure-darwin-aarch64.diff into configure's darwin target_cpu case
```
