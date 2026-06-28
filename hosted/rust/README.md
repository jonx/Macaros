# `hosted/rust/` — the no_std Rust-on-AROS runtime shim (`[RS0]`/`[RS1]`)

The concrete first commit behind the [Rust on AROS](../../docs/features/rust-aros/README.md)
plan: a custom Rust **target spec** for darwin-aarch64 AROS plus a `#![no_std]`
**runtime crate** whose `#[global_allocator]` bridges to exec `AllocVec`/`FreeVec`.
This is the `[RS0]` (codegen + link + startup interop) and `[RS1]` (allocator +
collections) starting point — a real thing to build from, not a sketch.

Like [ffmpeg-native](../../docs/features/ffmpeg-native/README.md), this is a
**native software port**, not a `hostlib` bridge: the artifact is ARM code *for
AROS*, compiled by Rust's own LLVM, not a macOS dylib AROS dlopens. Developed
standalone (outside mmake), the same convention as the other `hosted/` shims.

## Status — stage 1 **GREEN (verified)**, stage 2 gated

Split by what the toolchain can do *today*:

- **Stage 1 — cross-compile to the custom target (real, runs now).** Stock nightly
  + `-Zbuild-std=core,alloc` compiles `core`, `alloc`, and `aros-rt` for
  `aarch64-unknown-aros`. **Verified:** the spec loads (`target_os="aros"`,
  `panic="abort"`), the build succeeds, and every object in the resulting
  `libaros_rt.a` is `file format elf64-littleaarch64` — genuine little-endian
  aarch64 ELF. The ABI boundary is exactly the four flat-C symbols by design
  (below). No AROS crosstools or sysroot needed: a no_std staticlib links nothing.
- **Stage 2 — AROS-side link + run (gated on `$AROS_CC`).** Compiling
  `aros_rt_glue.c` + `rs0_main.c` and linking the staticlib into an AROS ELF needs
  the AROS C SDK / proto headers, which aren't up yet on this target
  (`graft/build-darwin-aarch64.sh` "current wall"). `build.sh` builds + links it
  automatically once `$AROS_CC` points at a working AROS cc; until then it's
  skipped with a note. The real on-AROS `[RS0]`/`[RS1]` PASS/FAIL waits on that.

So: codegen, the target definition, and the allocator/collections are **proven to
compile**; only the final AROS link/run is pending — and it's pending on the C
SDK, not on anything Rust.

## The ABI boundary (the only contact surface with AROS)

`aros-rt` (Rust) never touches AROS headers or the library-base register
convention. It imports exactly four flat-C symbols, provided by `aros_rt_glue.c`
(compiled by the AROS crosstools, where exec/dos are real) — the same shape the
host shims use, just forwarding to AROS instead of macOS:

```
Rust EXPORTS (T)            the C harness calls these
  aros_rust_selftest         -> [RS0] returns AROS_RS0_MAGIC ("RS0 ")
  aros_rust_alloc_checksum   -> [RS1] Vec<u32>+String round-trip, returns FNV digest

Rust IMPORTS (U)            aros_rt_glue.c provides these
  aros_exec_allocvec         -> AllocVec(size, MEMF_ANY)
  aros_exec_freevec          -> FreeVec(p)
  aros_rt_puts               -> PutStr() on Output()
  aros_rt_abort              -> abort()  (panic = "abort", no unwinding)
```

`build.sh` dumps these straight out of the built `.a` with `llvm-nm` so the
contract is grounded in the actual object, not just asserted here.

## Files

| File | Role | Built by |
|------|------|----------|
| `aarch64-unknown-aros.json` | the Rust **target spec** (arch, data-layout, `os=aros`, `panic=abort`, lld `aarch64elf_aros` + `--allow-multiple-definition`) | derived from `aarch64-unknown-none` (`rustc --print target-spec-json`) + AROS deltas |
| `aros-rt/src/lib.rs` | `#![no_std]` crate: `#[global_allocator]` over AllocVec/FreeVec, `#[panic_handler]`, the `[RS0]`/`[RS1]` selftests | nightly rustc (`-Zbuild-std`) |
| `aros-rt/Cargo.toml` | `staticlib`+`rlib`, `panic = "abort"` | — |
| `aros_rt_glue.c` | the flat-C boundary → exec `AllocVec`/`FreeVec`, dos `PutStr` | **AROS crosstools cc** |
| `rs0_main.c` | AROS C program that owns startup and calls Rust; one PASS/FAIL line per milestone | **AROS crosstools cc** |
| `build.sh` | stage 1 always (real); stage 2 when `$AROS_CC` is set | — |

## Build

```sh
./build.sh            # stage 1 (+ stage 2 if $AROS_CC is set), with ABI dump
./build.sh --build    # stage 1 only
AROS_CC=/path/to/aros-clang ./build.sh   # also compile + link rs0_test for AROS
```

Stage 1 needs nightly + the `rust-src` component
(`rustup component add rust-src --toolchain nightly`).

## Notes on the target spec (the three lines to re-verify in `[RS0]`)

The `.json` is derived from the in-tree `aarch64-unknown-none` spec, so the
`data-layout` matches rustc's own LLVM exactly. Three fields are deliberate choices
to confirm against the AROS C objects once the AROS link is live:

- **`relocation-model: static`** — must match what the AROS crosstools emit for C
  objects; a mismatch shows up as relocation errors at the C-side link (flip to
  `pic` if so).
- **`linker` / `pre-link-args`** — only exercised when Rust owns the final link
  (RS3+, a Rust `fn main()`); for `[RS0]`/`[RS1]` the AROS crosstools cc links the
  staticlib, so these are inert today. The lld emulation (`-maarch64elf_aros`) and
  `--allow-multiple-definition` (the `AROS_LIBREQ` duplicate-symbol class, see
  `graft/UPSTREAM-NOTES.md` item 18) are pre-wired for that day.
- **`features: +v8a,+neon`** — dropped the bare-metal `+strict-align`; AROS at EL0
  under macOS allows unaligned access. Revisit if anything traps.

## Reproducing the `[RS1]` digest

`aros_rust_alloc_checksum()` folds a `Vec<u32>` (1000 Knuth-hashed words, forcing
several reallocs) and a grown `String` with FNV-1a. It's plain wrapping u32
arithmetic — **target-independent** — so the expected value is authoritative from
the host and baked into `rs0_main.c` as `AROS_RS1_EXPECTED`:

```python
M=0xFFFFFFFF; h=2166136261
for i in range(1000): h=((h^((i*2654435761)&M))*16777619)&M
for b in (b"rust-on-aros\n"*100): h=((h^b)*16777619)&M
print(hex(h))   # 0xe5889f2d
```

When stage 2 runs on booted AROS, `[RS1]` PASS means the allocator handed back live
memory that survived 1000+ pushes and the `String` growth — i.e. the AllocVec
bridge and the aligned-allocation header math are correct end to end.

---

*Provenance: independent work. No third-party Rust-on-AROS / OS-port
implementation source was read, searched, or consulted. The allocator is the
textbook aligned-allocation-over-malloc trick, written from
`core::alloc::GlobalAlloc` and the AROS `exec.library` autodocs; the target spec is
derived from Rust's own `aarch64-unknown-none`. Any resemblance is coincidental.*
