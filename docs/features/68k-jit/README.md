# 68k JIT (`run68k`)

**Status: built.** A host **68k → AArch64 translator** that runs self-contained
classic-Amiga **68k** binaries — integer *and* 68881/68882 hardware
floating-point — natively on Apple Silicon, each byte-exact-verified against an
independent interpreter.

## Run it

```sh
make run68k                                          # -> build/run68k
build/run68k hosted/jit68k/apps68k/bin/mandel.exe    # a 68k Mandelbrot, exit 0
build/run68k hosted/jit68k/apps68k/bin/j5t.exe       # a vbcc hardware-FP program
```

Output goes to stdout, the exit code is the program's own `D0`, and any fault
writes a self-contained crash-bundle `.tar.gz`. It runs *system-friendly* 68k
software — a CPU+FPU JIT with a stub OS, not a full-chipset emulator.

## Provenance — the one non-clean-room part of this repo

The JIT's 68k decoders and AArch64 emitter are **adopted from
[Emu68](https://github.com/michalsc/Emu68) (MPL-2.0)**, vendored verbatim in
`hosted/jit68k/emu68/` behind a documented license boundary; the engine, loader,
and OS bridge link to them but copy no Emu68 code. Full disclosure:
[THIRD-PARTY-NOTICES.md](../../../THIRD-PARTY-NOTICES.md).

## Docs

- [design.md](design.md) — the translator, the OS-bridge, the marshalling seam
- [spec.md](spec.md) — implementation spec
- [INTERFACE.md](INTERFACE.md) — the engine/host API
- [68k-marshalling](../68k-marshalling/README.md) — the big-endian-on-LE
  library-call boundary
- CLI + sample programs: [hosted/jit68k/run68k.md](../../../hosted/jit68k/run68k.md) ·
  [apps68k](../../../hosted/jit68k/apps68k/README.md)
