# `run68k` — run a 68k Amiga executable through the JIT, from the terminal

`run68k` is a real command-line tool that runs a self-contained 68k Amiga **hunk
executable** through the JIT (CPU + FPU) on Apple Silicon and pipes the program's
output to your terminal. It is a **usability wrapper** over the existing 68k-JIT
engine — it adds no emulation. It composes the already-proven pieces: the `[J4]`
hunk loader/relocator, the stub OS (`AllocMem`/`FreeMem`/`PutChar` over the `[J3]`
LVO bridge), the full `[J5d]` engine (Emu68's real per-opcode decoders for the
integer ISA + the LINE-F 68881/68882 FPU, block chaining, the `[J5i]` exception
model), and the `[J5n]` crash-bundle diagnostics.

## Build

```
make run68k          # -> build/run68k
```

Optional: put it on your PATH, e.g. `cp build/run68k /usr/local/bin/run68k`
(or add `build/` to `$PATH`), so you can call `run68k prog.exe` from anywhere.

## Usage

```
run68k [options] <program.exe> [program-args...]
```

- The **program's PutChar output goes to stdout** (clean, so it pipes:
  `run68k prog.exe | grep ...`). All of run68k's own messages — load errors, the
  crash banner, the `-v` stats — go to **stderr**, so they never pollute stdout.
- The **exit code is the program's exit code**: the 68k `D0` at the top-level RTS,
  clamped to a shell-meaningful `0..255` (the full 32-bit `D0` is shown under `-v`).
  So it composes in pipelines: `run68k prog.exe; echo $?`.
- On a **fault**, the `[J5n]` diagnostics write a self-contained crash bundle
  (`.tar.gz`) and print a "send this file" banner to stderr; run68k exits nonzero.

### Options

| option            | meaning                                                                 |
| ----------------- | ----------------------------------------------------------------------- |
| `-h`, `--help`    | usage and exit                                                          |
| `-v`              | verbose: print engine stats (blocks / insns / cache / FP / …) to stderr |
| `--diff`          | request the `[J5n]` differential lockstep checker (see *Limitations*)   |
| `--crash-dir DIR` | where crash bundles go (default `$JIT68K_CRASH_DIR` or `./crash`)        |

`[program-args...]` are **accepted but not yet passed into the 68k program**.
Passing an Amiga CLI argument string into the program (through the crt0/stub) is a
documented follow-on; the args are parsed off the command line but do not reach the
program.

## Examples

```sh
# Run the Mandelbrot fractal; output to the terminal, exit 0.
build/run68k hosted/jit68k/apps68k/bin/mandel.exe

# A vbcc-compiled hardware-FP program (Newton sqrt, Taylor exp, stats, sin table).
build/run68k hosted/jit68k/apps68k/bin/j5t.exe

# Pipe-able: the program output is clean on stdout.
build/run68k hosted/jit68k/apps68k/bin/j5m.exe | tail -1     # -> checksum=...

# Exit code is the program's D0 (clamped 0..255):
build/run68k hosted/jit68k/apps68k/bin/mul.exe; echo $?      # -> 42

# Verbose engine stats (to stderr; stdout stays clean):
build/run68k -v hosted/jit68k/apps68k/bin/j5m.exe 2>stats.txt
```

## What it can run

- **Self-contained / stub-OS 68k programs**, integer **and** 68881/68882 **hardware
  floating-point**, including **vbcc-compiled C** (the `[J5m]` integer and `[J5t]`
  FP capstones are exactly this — real compiler output, byte-exact through the JIT).
- The stub OS provides `AllocMem` / `FreeMem` / `PutChar` via the `[J3]` LVO bridge.
  `PutChar` is the program's output sink → stdout.

The committed corpus all runs:
`apps68k/bin/{mandel,j5m,j5t,bubsort,sumsq,mul,fact,arraysum,j5l,j5o,j5p,j5q,j5r,j5s}.exe`.

## What it can't run yet

- **Programs that call real AmigaOS/AROS libraries** (dos.library, intuition,
  graphics, …): needs the AROS integration — the host-side LVO bridge into a running
  AROS, not the stub OS. That is the next milestone, not a `run68k` change.
- **Hardware-banging games / demos** (direct chipset, copper, blitter, custom
  registers, self-modifying decompressors): needs a full-chipset emulator (UAE).
  `run68k` is a CPU+FPU JIT with a stub OS, not a machine emulator.

## Crash bundles

On a fault — an out-of-sandbox read/write, a bad/misaligned PC, an illegal
instruction, a divide-by-zero, or a genuine host signal in translated code — the
`[J5n]` diagnostics write a shareable `.tar.gz` bundle to the crash dir and print a
banner pointing at it:

```
============================================================================
  !!  AROS 68k JIT FAULT  —  the program faulted; a crash bundle was written.
  !!  ./crash/jit68k-crash-<UTCstamp>-<faultkind>.tar.gz
  !!  SEND THIS FILE to the developer.
  !!  (open README.txt inside if you're not sure what this is.)
============================================================================
```

The bundle contains a human report (the faulting instruction, 68k + host registers,
the 68k call stack named via `HUNK_SYMBOL`, a flight recorder of the last executed
instructions), a reloadable core snapshot, the exact program, and a deterministic
replay command. run68k exits nonzero (70) on a fault.

Graceful errors (no raw crash, exit nonzero, one-line stderr message): file not
found, not a hunk file (bad magic), an unsupported/undecodable opcode.

## Limitations

- **`--diff` (whole-program lockstep)** is **not** supported by the current engine
  driver: `j5d_run_diff`'s block-boundary comparison is validated for the single-
  injected-divergence micro-case (`make hosted-jit68k-j5n`), but on real multi-block
  programs it reports block-boundary *false positives*. Rather than write a
  misleading "divergence" bundle on a correct program, `run68k --diff` prints a
  notice and runs the program normally. The genuine byte-exact JIT-vs-interpreter
  check for the whole corpus is the **regression itself** (`make hosted-jit68k-j5m`,
  `-j5t`, … — each asserts byte-exact registers / memory / output / exit per program).
- **Diagnostics-on means chaining-off.** `run68k` always registers the `[J5n]`
  diagnostics so any fault is caught and bundled; with diagnostics active the engine
  keeps block chaining off (every block returns to the C dispatcher for exact fault
  localization). This is correctness-over-speed and is invisible except in `-v` stats
  (`chaining: 0 …`). The chained hot path is exercised by the corpus regression.
