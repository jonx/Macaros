# Benchmarks

Status: **exec + clib micro-benchmarks build and run on darwin-aarch64**
(2026-06-28). They flushed out — and we fixed — a real C-library bug (`printf`
floats), and now produce live numbers on booted AROS. A second axis — **68k
Dhrystone through the JIT** — also runs now, after fixing two real translator bugs
it surfaced (see [the JIT Dhrystone section](#the-other-dhrystone--68k-through-the-jit)).

This wires AROS's own in-tree micro-benchmark suite to the hosted darwin-aarch64
target and adds a one-command runner. It also documents the floating-point
`printf` fix the suite surfaced, and the benchmarks that don't yet run cleanly.
Two complementary perf axes live here: **native aarch64** benchmarks (the suite
below, via `bench-run`, measuring the M5/AROS) and the **68k→AArch64 JIT**
(`run68k`, measuring the translator against real-Amiga figures — last section).

## What it is

AROS ships micro-benchmarks under
`/Users/user/Source/aros-upstream/developer/debug/test/benchmarks/` in four groups
(`exec`, `clib`, `boopsi`, `graphics`). `exec` and `clib` are headless (they
`printf` to stdout) and so run unattended; `graphics` needs the display and
`boopsi` needs BOOPSI. We build and run the headless groups.

Built binaries land in `SYS:Developer/Debug/Tests/benchmarks/<group>/<name>`
(i.e. `bin/darwin-aarch64/AROS/Developer/Debug/Tests/benchmarks/...`, which the
emul-handler exposes as `SYS:`).

Implemented pieces:

- host runner: `graft/bench-run` / `make bench`
- mmake targets (built from the AROS build dir): `test-benchmarks-exec-quick`,
  `test-benchmarks-clib-quick`
- the `printf` float fix in
  `/Users/user/Source/aros-upstream/compiler/crt/stdc/__vcformat.c`
  (see [Float printf fix](#float-printf-fix) and `graft/UPSTREAM-NOTES.md` item 34)

## Build

From the AROS build dir, with the crosstools on `PATH`
(`/tmp/graft-tools:/opt/homebrew/bin:/tmp/aros-xtools/bin`):

```sh
make -C "$BUILD" test-benchmarks-exec-quick
make -C "$BUILD" test-benchmarks-clib-quick
```

`$BUILD` is the (ephemeral) mmake tree — find it with
`find /private/tmp/claude-* -maxdepth 6 -type d -name arosbuild`. The `-quick`
variants skip the includes/linklib/fetch chain (see the `aros-c-commands-quick-build`
project note). `clib/stdio` also needs `stdcio.library` built
(`make -C "$BUILD" compiler-stdcio-quick`).

> Float output additionally requires the printf fix below to be built into
> `posixc.library` **and** `stdcio.library`. Without it every `%f`/`%lf`/`%g`
> prints the literal specifier.

## Run

```sh
make bench                              # default known-good set
./graft/bench-run                       # same
./graft/bench-run exec/allocvec clib/memset
./graft/bench-run clib/dhrystone        # once Dhrystone is built into the suite
BENCH_DRYRUN=1 ./graft/bench-run        # print the generated Startup-Sequence only
make bench BENCH="clib/dhrystone"       # via make
```

`bench-run` boots windowed AROS headlessly through `graft/aros-ctl`, feeds a
generated `S/Startup-Sequence` that runs each benchmark with stdout redirected to
a writable host volume (`MacRW:`), polls for the `=== DONE ===` marker (or a
crash / process exit), then prints the captured output and saves it, a screenshot,
and the host log under `run/darwin-aarch64/bench-<timestamp>*`. Each argument is a
benchmark: a name with `:` is a literal AROS path (`C:Dhrystone`, `SYS:...`),
otherwise it is relative to `$BENCH_BASE`
(default `SYS:Developer/Debug/Tests/benchmarks`), e.g. `clib/memset`.

Knobs (env): `BENCH_BASE`, `BENCH_TIMEOUT` (s, default 180), `BENCH_WAIT` (warmup
s, default 5), `BENCH_DRYRUN`, `BENCH_RUN_DIR`, `AROS_CTL_BOOTD`.

## Results (darwin-aarch64, hosted on Apple M5)

| Benchmark | Result |
|---|---|
| `exec/allocvec` (AllocVec+FreeVec) | ~30.8M ops/s (10M in 0.325 s) |
| `exec/allocpooled` (pooled alloc+free) | ~59.5M ops/s (100M in 1.68 s) |
| `clib/memset` — clear | ~62.9 GB/s |
| `clib/memset` — fill pattern | ~57.2 GB/s |

### Interpreting them

These are **best-case micro-benchmarks** (same-size repeated alloc/free,
sequential `memset`) running as native aarch64, so they mostly measure the **M5,
not AROS**. The memory-bandwidth figures are decent single-threaded but well under
an M5's ceiling (so there is headroom; they are *not* hardware-limited as they
would be on an M1). The allocator latencies (~32 ns AllocVec, ~17 ns pooled, pools
~2x faster as expected) are roughly on par with a native `malloc` on a core this
fast — healthy, not exceptional. There is **no baseline** yet: to judge the real
"AROS tax" we want the same `memset`/`malloc` natively under macOS on the same
machine, plus `taskswitch2` (context-switch latency — the one metric that measures
AROS's scheduler rather than the CPU).

## Float printf fix

The suite's first run printed the literal format specifier for every
floating-point field (`Allocations per second: %f`) while integers were fine. Root
cause: AROS's shared format engine (`compiler/fmtprintf/fmtprintf.c`) gates
`%[aAeEfFgG]` behind `#ifdef FULL_SPECIFIERS`, and `__vcformat` was the only engine
that gated *that* on `#ifndef STDC_STATIC`. Because this target force-links the
freestanding `libstdc.static.a` early in every link (so compiler-emitted
`memset`/`memcpy` bind before `stdc.library` exists — `UPSTREAM-NOTES.md` item 26),
its strong, float-less `__vcformat` shadowed the float-capable `StdCBase` dispatcher
— so `printf`/`vfprintf` from `posixc.library` and `stdcio.library` (i.e. every C
program) dropped floats, while `sprintf` (via `stdc.library`, which keeps its own
float-ON copy) worked.

Fix: drop the lone `#ifndef STDC_STATIC` gate in
`compiler/crt/stdc/__vcformat.c` so float support is unconditional, matching the
sibling engines (`__vcscan`, `__vwformat`, `__vwscanf`, kernel `_vkprintf`). Rebuild
`libstdc.static.a` + `posixc.library` + `stdcio.library`:

```sh
make -C "$BUILD" linklibs-stdc-static-quick
make -C "$BUILD" compiler-posixc-quick
make -C "$BUILD" compiler-stdcio-quick
```

This fixes float output system-wide (any `printf`, not just benchmarks). Full
write-up: `graft/UPSTREAM-NOTES.md` item 34.

## Known issues

- **`clib/stdio` crashes the emul-handler** — `[KRN] Task EMU went out of stack
  limits` + unrecoverable trap during heavy formatted host-file output. Excluded
  from the default set. `UPSTREAM-NOTES.md` item 35 (open).
- **`exec/copymem` never completes** in this harness — the process exits at
  ~40–90 s with no crash trace and no output; uninvestigated. `clib/memset`
  already covers memory bandwidth.
- **`exec/taskswitch2` waits for CTRL-C** — its default build has
  `SELF_TIMED_TEST 0`, so unattended it hangs. Rebuild with `SELF_TIMED_TEST=1`
  (or signal it) to get context-switch latency. Excluded from the default set.
- **`clib/string` prints `+inf`** — those sub-tests measured 0 elapsed time
  (divide-by-zero from coarse timing / optimized loops). It is a benchmark
  artifact, *not* a printf bug — in fact it confirms the `isinf` path now works.
- **The runner uses the windowed boot**, which shares the clipboard/keyboard
  bridge with the live Mac. Interacting with the host during a run (or the AROS
  window taking focus) can disturb it. A truly headless boot path would be more
  robust for unattended runs.

## Adding a benchmark (e.g. Dhrystone)

`bench-run` runs any self-terminating program that `printf`s its results. To add
Dhrystone:

1. Build it into the suite (an `mmakefile.src` under
   `developer/debug/test/benchmarks/clib/` or its own dir) so it lands at, say,
   `SYS:Developer/Debug/Tests/benchmarks/clib/dhrystone`; or build it as a C:
   command.
2. Run it: `./graft/bench-run clib/dhrystone` (relative to `$BENCH_BASE`) or
   `./graft/bench-run C:Dhrystone` (literal path).
3. Dhrystone reports DMIPS as a float — so it depends on the printf fix above.
   It is also a good cross-checkable number (DMIPS/MHz against published 68k and
   modern figures), unlike the M5-bound micro-benchmarks.

## The other Dhrystone — 68k through the JIT

There are **two Dhrystones** worth running here, and they measure different things.
The native one above (compiled for aarch64 AROS, run by `bench-run`) measures the
**M5/AROS**. The one in this section is the **68k** Dhrystone 2.1 — a real big-endian
Amiga hunk executable, vbcc-compiled — run through the project's **68k→AArch64 JIT**
([`hosted/jit68k`](../68k-jit/design.md), tool [`run68k`](../../../hosted/jit68k/run68k.md)),
**not** native code. So it measures the **translator**: how fast the JIT executes
classic 68k integer code on this Mac, cross-checkable against published *real-Amiga*
Dhrystone figures. It is the standout "run real Amiga software, fast" payoff, narrowed
to a CPU benchmark (no chipset, no GUI — see the [68k-jit design](../68k-jit/design.md)).

### Two real engine bugs it flushed out (and we fixed)

Exactly like the `printf` float bug the native suite surfaced, running *genuine
compiler output* through the JIT immediately hit two paths the hand-built `apps68k`
corpus never exercised — both now fixed:

1. **RMW-to-memory EA faulted.** A read-modify-write ALU op with a memory destination
   (`or.l Dn,(ea)`, the immediate ops, `addq/subq`, memory shifts) resolved the dest
   address into a register then emitted a **raw** `ldr`/op/`str` straight off it — a 68k
   address treated as a host pointer, with **no sandbox rebase and no big-endian REV**.
   Doubly wrong (faults on abs.l, mis-orders bytes otherwise); latent because no corpus
   program does RMW-to-memory, but vbcc emits it for every `g |= x;` / `g += x;`. Fix: a
   new `--rmw-sandbox` transform in `hosted/jit68k/emu68_darwinize.pl` routes each such
   site through the existing `j5d_ea_mem` helper (sandbox rebase + REV + pre/post index),
   applied to `M68k_LINE0/5/8/9/B/C/D/E` via a re-darwinize step in the Makefile (the
   register-only `[J5c]` build is left raw). Same discipline as the EA `[J5d]`/`[J5g]`
   rewrites; the quarantine decoders stay byte-verbatim.
2. **`DBcc` (`DBF`/`DBRA`) was not a block terminator.** vbcc compiles its `movem`/
   struct-copy loops with `DBF`; `is_terminator` (`j5d_engine.c`) recognised `Bcc`
   (`0x6xxx`) but not `DBcc` (`0x5_C8`, in the LINE5 range), so the block decoder walked
   past the loop branch and ran away into the runaway guard. Fix: add `DBcc` to
   `is_terminator` + a dispatcher case that mirrors the existing `[J5q]` `FDBcc`
   (decrement `Dn.W`, branch unless the condition is true or the counter hit −1).

Two env knobs came with them: `JIT68K_STEP_CAP` (raise `run68k`'s 2M dispatcher-step
runaway guard for a long benchmark) and `JIT68K_NO_DIAG` (skip the `[J5n]` diagnostics so
the engine keeps **block chaining on** — the hot path; default keeps the safe, every-block-
localizable path). The genuine Dhrystone now runs **byte-perfect** (all canonical integrity
values: `Int_Glob=5`, `Bool_Glob=1`, `Arr_2_Glob[8][7] = N+10`, …), and the byte-exact
corpus regression stays green (`make hosted-jit68k-j5m` / `-j5t` / `-apps` / `-j5l`) — the
new paths are inert for the corpus, so nothing regressed.

### Results (68k Dhrystone 2.1 through the JIT, hosted on Apple M5)

Measured by the two-point method (steady-state rate from two run lengths, cancelling
fixed load/translate overhead):

| Path | Dhrystones/sec | VAX MIPS (÷1757) |
|---|---|---|
| chained hot path (`JIT68K_NO_DIAG=1`) | ~205,000 | ~117 |
| instrumented (default — diagnostics on, chaining off) | ~51,000 | ~29 |

vs published **real-Amiga** Dhrystone (chained number):

| Real machine | ~Dhry/s | this JIT is |
|---|---|---|
| A500 — 68000 @ 7 MHz | ~2,000 | ~102× |
| A3000 — 68030 @ 25 MHz | ~14,000 | ~15× |
| A4000/060 — 68060 @ 50 MHz | ~80,000 | ~3× |

### Interpreting them

The JIT now **outruns every real Amiga** — ~3× the fastest stock machine (a 50 MHz
'060) on the chained path. That said, 117 VAX-MIPS is **modest for a native-AArch64
JIT**, and Dhrystone is close to the **worst case** for this engine:

- **Calls aren't chained.** Dhrystone does ~12 `jsr`/`rts` *per iteration* (Proc_1–8,
  Func_1–3); every one returns to the C dispatcher via the real return stack — *not* a
  block→block branch. A large fraction of the run is dispatcher round-trips, not
  translated code. A loop/compute-heavy kernel (what a "MIPS test" usually is) chains
  cleanly and scores far higher on the *same* engine.
- **Byte-swap per memory access** (big-endian 68k ↔ little-endian host) and a **per-block
  68k-register frame** add fixed overhead; Dhrystone is memory/struct-heavy.

These are deliberate trade-offs — the engine is a block-at-a-time translator built to be
**byte-exact vs an independent interpreter**, correctness over peak speed. Headroom, in
bang-for-buck order: **chain across `jsr`/`rts`** (the big Dhrystone win), drop redundant
byte-swaps, superblock/trace compilation, cross-block register allocation. So ~3× the
fastest Amiga is the *floor*, on the most call-heavy workload, with no speed optimization
done yet.

### Running it

`run68k` is built from the JIT tree, not via `bench-run`:

```sh
make run68k                                   # -> build/run68k
build/run68k dhry.exe                         # safe path: diagnostics on, byte-exact, crash bundles
JIT68K_NO_DIAG=1 JIT68K_STEP_CAP=40000000000 build/run68k dhry.exe   # chained hot path, long run
```

The `dhry.exe` here is a **freestanding Dhrystone 2.1** (static records instead of
`malloc`, `PutChar` output, no in-program timer — wall time is measured externally),
compiled to a real Amiga hunk by the project's own 68k cross-toolchain
(`hosted/jit68k/apps68k/.toolchain`, vbcc→vasm→vlink). It is a **demo/validation
artifact, not vendored** into the tree (it pulls in a third-party benchmark); the
JIT engine fixes above *are* in the tree. See
[`hosted/jit68k/run68k.md`](../../../hosted/jit68k/run68k.md) for the tool itself.
