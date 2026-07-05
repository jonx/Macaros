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

## Build status

These starter patches were the seed. They are long since in, and **hosted AROS
now builds, boots, and runs a full Wanderer desktop** on Apple Silicon — exec /
kernel.resource / hostlib / dos.library / the whole boot module set come up, SYS:
mounts, the AmigaDOS Shell runs the standard C: command set, and the desktop
renders in a live Cocoa/Metal window. The seed's own contribution — the
host-signal→AROS-trap path in `cpu_aarch64.h` — is still what turns a guest fault
into an AArch64 register dump and a native Guru alert. Current state:
[CONTINUATION.md](CONTINUATION.md).

The full work (and the upstream-worthy friction) lives on the `aarch64-darwin-graft`
branch of `aros-upstream` (the [jonx/AROS](https://github.com/jonx/AROS/tree/aarch64-darwin-graft)
fork) and is mapped in **[WORKFLOW.md](WORKFLOW.md)** with
**[UPSTREAM-NOTES.md](UPSTREAM-NOTES.md)**. The files in *this* directory
(`cpu_aarch64.h`, `cpucontext-aarch64.h`, `configure-darwin-aarch64.diff`) are the
seed commit; the branch has many more (the bootstrap loader's AArch64 relocations,
the Apple-Silicon `mmap` fixes, the static-C-runtime force-load, `arch/aarch64-all/
exec/`, etc.).

## Build and run it

See **[GETTING-STARTED.md](../GETTING-STARTED.md)** for the newcomer path and
[docs/features/build/README.md](../docs/features/build/README.md) for the build
detail. In short: build the OS from `../aros-upstream` with
[build-darwin-aarch64.sh](build-darwin-aarch64.sh), then from this repo:

```sh
make cocoametal-dylib pasteboard-dylib coreaudio-dylib bsdsock-dylib
./aros-ctl deploy && AROS_CTL_STARTUP_MODE=desktop ./run-window.sh
```

A window titled "AROS" opens on the Workbench desktop; click it and type at the
shell. Drive it headlessly with `./aros-ctl run`.
