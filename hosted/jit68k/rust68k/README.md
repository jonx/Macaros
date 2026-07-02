# rust68k — REAL Rust compiled for m68k, run through the 68k JIT

> Regression: `make hosted-jit68k-rust` (marker `[rust68k] PASS`) — runs the
> **committed** `bin/*.exe` under **both engines** (the JIT and the independent
> reference interpreter, `run68k --interp`) and asserts the exit codes.
> Rebuild the binaries from source: `tools/build-rust68k.sh` (see "Toolchain").

This directory proves and regresses the second compiled language on the JIT after
vbcc C ([../apps68k/](../apps68k/README.md)): **Rust**, `no_std` with `core` +
`alloc` (Vec and friends over a bump allocator), compiled by stock nightly rustc
for `m68k-unknown-none-elf`, linked into real AmigaOS **hunk** executables, and run
byte-agreeing through both engines.

## The pipeline (all tools already on this machine)

```
rustc nightly (-Zbuild-std=core,alloc, pinned profile)      # LLVM M68k backend, Tier 3
  -> m68k ELF staticlib
  -> m68k-elf-objcopy -R .comment -R .note.GNU-stack -R .eh_frame   (per object)
  -> m68k-elf-as entry_<prog>.s                              # tiny _start: jsr <fn>; rts
  -> vlink -bamigahunk -s -gc-all -mtype -e _start           # ELF objects -> hunk .exe
  -> build/run68k bin/<prog>.exe            (exit code = the program's D0)
```

- `-gc-all` prunes the unused compiler_builtins roots a staticlib keeps (273 KiB ->
  a few KiB; run68k's sandbox is 256 KiB); `-mtype` merges rustc's one-section-per-
  function output into one hunk per type; `.comment` must be stripped because its
  odd-sized payload trips run68k's strict hunk-size check.
- The Rust m68k ABI passes arguments on the stack; the entry stubs push constants
  and `jsr` the `#[no_mangle]` function, so D0 (= the return value) becomes the
  shell exit code. `abort` is a trapping stub (compiler_builtins references it).
- Alternative hunk route (proven, not used here): `m68k-elf-ld -r -T <merge script>`
  + AROS's `tools/elf2hunk`. It works but `ld -r` cannot garbage-collect, so the
  binaries do not fit the sandbox.

## The corpus (source `src/lib.rs` + `fibcore/`, committed `bin/*.exe`)

| program | exercises | exit (both engines) |
|---|---|---|
| `fib.exe` | `core` only: iterative Fibonacci, fib(10), stack-arg ABI | **55** |
| `allocprobe.exe` | the `GlobalAlloc` bump allocator raw: two allocations, write/read through both | **10** |
| `vecsum.exe` | `alloc`: Vec::with_capacity + push loop + indexed sum (exclusive range) | **91** |
| `vecsum_inclusive.exe` | **the CCR canary** — same Vec sum with an INCLUSIVE range (`1..=13`); correct answer 91 | **1** (miscompiled) |
| `hello.exe` | **prints** through the stub OS: `puts`/decimal printing over `putchar68k`, the 68k library call (`jsr -30(a6)`, PutChar) via a 12-byte asm thunk; output asserted byte-exact | **0** + 3 lines on stdout |

## Writing your own 68k Rust program

1. Copy `hello/` (own crate: keeps your codegen luck independent of the corpus
   while the CCR bug is open) and put your code in a `#[no_mangle] pub extern "C"`
   entry function. `core` is free; for `alloc` add the bump allocator from
   `src/lib.rs` and build with `-Zbuild-std=core,alloc`.
2. Copy `entry_hello.s`, point the `jsr` at your function. A0/D0 hold the AmigaDOS
   argument string if you want CLI args (see [../run68k.md](../run68k.md)); A6 is
   the stub library base for PutChar/AllocMem/FreeMem calls.
3. Add the crate + program name in `tools/build-rust68k.sh`, run it, then run
   `build/run68k bin/<prog>.exe` — and ALWAYS cross-check with `--interp`
   (two engines agreeing is your defense against the open compiler bug).

## The upstream compiler bug this corpus documents (and canaries)

rustc's experimental LLVM M68k backend **schedules register copies between a
flag-setting instruction and the branch that consumes the flags**. On a real 68000
(and in both of our engines, which follow the PRM) `move.l` **sets** CCR (N/Z per
the value, V/C cleared), so the scheduled copy destroys the branch condition.
Full analysis + report draft: [UPSTREAM-LLVM-CCR-BUG.md](UPSTREAM-LLVM-CCR-BUG.md).

Consequences baked into this directory:

- **It is scheduling luck, not a source pattern.** The same source loop compiles
  correctly or not depending on opt-level, LTO, and even the *sibling functions in
  the crate* (register allocation changes the copy placement). That is why `fib`
  lives alone in [fibcore/](fibcore/Cargo.toml): that exact crate shape + profile is
  the combination verified to schedule correctly on the pinned nightly.
- **`vecsum_inclusive` is a canary, asserted at the WRONG value 1** under both
  engines. Both engines agreeing on 1 is what proves the defect is in the compiled
  code, not in the JIT. When upstream fixes the backend and the binaries are
  rebuilt, it will return 91 and the `hosted-jit68k-rust` row must be flipped.
- **A regenerated binary is only trusted after the two-engine regression passes.**
  `tools/build-rust68k.sh` prints what it built; `make hosted-jit68k-rust` is the
  verdict. The committed binaries are the known-good (or known-miscompiled-canary)
  artifacts; behavior is deterministic for a fixed binary.
- Also hit while probing: `debug = 2` SIGSEGVs rustc itself on this target
  (hence `debug = false` in the pinned profile), and opt-level 3 miscompiles more
  aggressively than 1 (hence `opt-level = 1`). Neither is a safety guarantee; the
  regression is.

## Toolchain — what `tools/build-rust68k.sh` needs

- **rustup nightly + rust-src** (`rustup +nightly component add rust-src`). Stock
  nightly: its LLVM already includes the experimental M68k backend, and
  `m68k-unknown-none-elf` is a Tier 3 target built on demand via `-Zbuild-std`.
  Pinned-good nightly: 2026-06-27 (`rustc 1.98.0-nightly 13f1859f2`).
- **Homebrew m68k-elf binutils** (`m68k-elf-as/-ar/-objcopy`, in `/usr/local/bin`).
- **vlink** from [../apps68k/.toolchain/](../apps68k/README.md) (build once with
  `../apps68k/tools/build-vbcc.sh`).
- No AROS SDK, no crosstools, no booted AROS: everything runs on the host against
  run68k's stub OS (AllocMem/FreeMem/PutChar via the real [J3] LVO bridge).

Note for the OTHER Rust target in this project: the aarch64-AROS port reserves
x18 (`+reserve-x18`, OS built `-ffixed-x18` — Apple Silicon platform register).
That constraint is aarch64-specific; it does not apply to m68k *programs*, and the
JIT's generated AArch64 code already respects the platform conventions.

## Scope and the road to `std`

This corpus is deliberately `no_std` core + alloc: it needs nothing from an OS, so
it runs (and is byte-verified) under the stub library today. Rust `std` for
m68k-AROS is a separate, later effort: it needs a real m68k AROS library world to
call (either the AROS-side JIT integration per
[../../../docs/features/68k-jit/INTERFACE.md](../../../docs/features/68k-jit/INTERFACE.md)
rows A–F with the typed marshalling of
[../../../docs/features/68k-marshalling/](../../../docs/features/68k-marshalling/README.md),
or a built m68k AROS userland under emulation) — and a much more trustworthy
compiler backend than today's. The staged plan lives in
[../../rust/FIX-PLAN.md](../../rust/FIX-PLAN.md).
