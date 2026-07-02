# Upstream bug: LLVM M68k backend schedules MOVE between a flag-def and its Bcc use (MOVE clobbers CCR)

> Status: **REPORTED upstream** (2026-07-02) — the bug was already on file as
> [llvm/llvm-project#152816](https://github.com/llvm/llvm-project/issues/152816)
> ("[M68k] Miscompile: COPY and copyPhysReg() do not respect live CCR register",
> open since 2025-08, workaround patch + a pre-RA liveness pass under discussion).
> Our Rust reproducers + the placement-luck severity data + the MOVEA observation
> were added as a supporting comment:
> <https://github.com/llvm/llvm-project/issues/152816#issuecomment-4860577187>.
> Track that issue; when it is fixed and a new nightly rebuilds the corpus, the
> `vecsum_inclusive` canary flips to 91 (`make hosted-jit68k-rust` will say so).
> The analysis below is kept as the local record behind the comment.

## Summary

The M68k backend emits `MOVE.L` register copies **between** a flag-setting
instruction (`SUB.L #imm,Dn` / `CMPI.L`) and the conditional branch that consumes
those flags. On the M68000 family, data-destination `MOVE` **sets** the condition
codes (N and Z per the moved value; V and C cleared — M68000PRM, MOVE instruction,
condition codes section). The scheduled copy therefore destroys the branch
condition, and loops either run once or never terminate. The bug is placement
(register-allocation/copy-insertion) luck: the same source loop compiles correctly
or incorrectly depending on opt-level, LTO, codegen-units, and unrelated sibling
functions in the compilation unit.

Observed with: `rustc 1.98.0-nightly (13f1859f2 2026-06-27)` (bundled LLVM),
target `m68k-unknown-none-elf`, verified on two independent M68k implementations
(a JIT and an interpreter, both PRM-grounded and cross-validated against each
other on a large corpus).

## Reproducer 1 — inclusive-range loop runs once (opt-level=1, lto=fat)

```rust
// cargo +nightly build --release --target m68k-unknown-none-elf -Zbuild-std=core,alloc
// profile: opt-level=1, lto=true, codegen-units=1, panic=abort
#[no_mangle]
pub extern "C" fn vecsum_inclusive() -> u32 {
    let mut v: Vec<u32> = Vec::with_capacity(16);
    for i in 1..=13u32 { v.push(i); }
    v.iter().sum()          // correct: 91 — observed: 1
}
```

Generated loop tail (m68k-elf-objdump):

```
526: 2004            move.l d4,d0
528: 90bc 0000 000e  sub.l  #14,d0      ; loop test: carry = (next < 14)
52e: 2604            move.l d4,d3       ; *** MOVE sets CCR: C cleared here ***
530: 65ae            bcs.s  4e0         ; never taken -> body runs exactly once
```

## Reproducer 2 — countdown loop never terminates (opt-level=3, no lto)

```rust
#[no_mangle]
pub extern "C" fn fib(n: u32) -> u32 {
    let (mut a, mut b) = (0u32, 1u32);
    for _ in 0..n { let t = a.wrapping_add(b); a = b; b = t; }
    a
}
```

```
1e: d688            add.l  a0,d3
20: d2bc ffff ffff  add.l  #-1,d1      ; counter decrement sets Z when it hits 0
26: 2002            move.l d2,d0
28: 2042            movea.l d2,a0      ; (movea does NOT set flags — fine)
2a: 2403            move.l d3,d2       ; *** MOVE sets CCR: Z destroyed ***
2c: 66f0            bne.s  1e          ; always taken -> infinite loop
```

The same function at opt-level=1 in a single-function crate compiles correctly,
because the allocator happens to place a flag-neutral `MOVEA` (not `MOVE`) before
the branch — which is what makes this placement-luck rather than a systematic
lowering rule.

## Expected behavior

Either (a) model data-`MOVE` as clobbering CCR so nothing flag-live can be
scheduled across it, or (b) use flag-neutral sequences (`MOVEA` via an address
register, or `LEA`) for copies inserted between a flag-def and its use, or
(c) re-materialize the compare adjacent to the branch.

## Notes for the fix hunt

- `MOVEA` (address-register destination) does not affect CCR; the backend already
  emits it in the lucky-correct schedules, so the machinery exists.
- Everything above reproduces with plain `-Zbuild-std=core` too (no alloc);
  the smallest trigger is any loop whose backedge condition comes from
  `SUB`/`CMP` with at least one register copy live-range-split across it.
- A `cmpi.l` variant of reproducer 1 also occurs (flags from `CMPI` clobbered by
  two `MOVE.L` copies before `BEQ`) — same root cause.

## How this repo pins it meanwhile

- Corpus binaries are committed and two-engine-verified (`make hosted-jit68k-rust`).
- `vecsum_inclusive.exe` is kept as a CANARY asserted at the wrong value 1;
  when a fixed toolchain rebuilds it to return 91, flip the expectation.
- Working envelope used for the committed corpus (luck, not guarantee):
  opt-level=1, debug=false; `fib` isolated in its own crate (fibcore/).
- rustc also SIGSEGVs compiling compiler_builtins for this target with
  `debuginfo=2` — worth a separate rust-lang/rust issue after this one.
