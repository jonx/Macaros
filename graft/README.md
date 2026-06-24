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

## Build status — HONEST (2026-06-24: it BOOTS)

These starter patches are no longer just a starting point — they're in, and
**hosted AROS now builds, loads, relocates, and runs on this Apple-Silicon Mac.**
`exec.library` (175KB) + `kernel.resource` (70KB) + `hostlib.resource` initialise
with valid `SysBase`/`KernelBase`; the host-signal→AROS-trap path (this very
`cpu_aarch64.h`) catches a SIGSEGV and AROS prints its AArch64 register dump and
native Guru-Meditation alert. It halts at a cold-start trap — a 3-module kickstart
has no `dos.library` to hand off to.

The full work (and the upstream-worthy friction) lives on the `aarch64-darwin-graft`
branch of `aros-upstream` and is mapped in **[WORKFLOW.md](WORKFLOW.md)** with
**[UPSTREAM-NOTES.md](UPSTREAM-NOTES.md)** (25 items). The files in *this* directory
(`cpu_aarch64.h`, `cpucontext-aarch64.h`, `configure-darwin-aarch64.diff`) are the
seed commit; the branch has many more (the bootstrap loader's AArch64 relocations,
the Apple-Silicon `mmap` fixes, the static-C-runtime force-load, `arch/aarch64-all/
exec/`, etc.).

## Run it

A self-contained signed bundle is at `~/aros-darwin/`:

```
~/aros-darwin/run.sh
```

You'll see the bootstrap map memory and enter the kernel, then AROS's own
`[KRN]` output, the Guru alert, and the AArch64 register dump. Ctrl-C to quit
(it halts in the alert loop at the cold-start trap).
