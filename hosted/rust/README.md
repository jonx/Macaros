# `hosted/rust/` — Rust on AROS (no_std `[RS0]`/`[RS1]` + `std` `[RS3]`)

The Rust-on-AROS work, behind the [Rust on AROS](../../docs/features/rust-aros/README.md)
plan: a custom Rust **target spec** for darwin-aarch64 AROS plus a `#![no_std]`
**runtime crate** whose `#[global_allocator]` bridges to exec `AllocVec`/`FreeVec`.

Like [ffmpeg-native](../../docs/features/ffmpeg-native/README.md), this is a
**native software port**, not a `hostlib` bridge: the artifact is ARM code *for
AROS*, compiled by Rust's own LLVM, not a macOS dylib AROS dlopens. Developed
standalone (outside mmake), the same convention as the other `hosted/` shims.

> **`std` now runs on AROS too (`[RS3]`).** `println!`/`Vec`/`HashMap`/`format!` work
> through the real standard library, via a fresh `sys/pal/aros` developed in a local
> Rust clone. For the std port, the dev loop, the tools/tests, the resume map, and a
> key `x18` risk, **start at [STD-PORT.md](STD-PORT.md)**. The rest of *this* file
> covers the no_std `[RS0]`/`[RS1]` runtime, which `std` builds on.

## Status — **PROVEN LIVE on AROS** (`[RS0]` + `[RS1]`)

Rust cross-compiled on the Mac runs on booted darwin-aarch64 AROS. `graft/rust-smoke`
builds, links, deploys, boots, and asserts; the program prints, on AROS:

```
aros-rt: [RS0] rust selftest ran
[RS0] rust selftest magic 0x52533020 PASS
[RS1] alloc checksum 0xe5889f2d PASS (Vec<u32>+String round-trip)
RUST-AROS: ALL PASS
```

Two stages, both now green:

- **Stage 1 — cross-compile to the custom target** (`build.sh`). Stock nightly +
  `-Zbuild-std=core,alloc` compiles `core`, `alloc`, and `aros-rt` for
  `aarch64-unknown-aros`: the spec loads (`target_os="aros"`, `panic="abort"`) and
  every object in `libaros_rt.a` is `elf64-littleaarch64` with **GOT-free**
  relocations (`MOVW_UABS_*`/`CALL26`/`ABS64`) — the exact set every AROS C: command
  uses, so AROS's LoadSeg relocates them by construction.
- **Stage 2 — link into a real AROS C: command + deploy** (`aros-build.sh`). The
  AROS crosstools compile the flat-C glue + harness against real proto headers;
  `collect-aros` links them with the staticlib into an **ET_REL** program (zero
  undefined symbols) and deploys it. `[RS1]` PASS means the `#[global_allocator]`
  handed back live memory that survived 1000+ `Vec` pushes and the `String` growth
  on AROS, and the FNV digest matched.

The single bug between "links" and "runs" was the **startup object**: the program
must link `startup.o` (defines `__startup_main` + the `PROGRAM_ENTRIES` set), *not*
`elf-startup.o` — a binary missing it loads as "filesystem action type unknown".

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
| `aros_rt_glue.c` | the flat-C boundary → exec `AllocVec`/`FreeVec`, dos `PutStr`; header-clean (no host libc) | **AROS crosstools cc** |
| `rs0_main.c` | AROS C program that owns `startup.o` and calls Rust; one PASS/FAIL line per milestone | **AROS crosstools cc** |
| `build.sh` | stage 1: cross-compile the crate (+ ABI dump) | — |
| `aros-build.sh` | stage 2: compile glue+harness, `collect-aros`-link into an AROS C: command, deploy. Auto-discovers the AROS build tree | — |

## Build & run

```sh
./build.sh --build              # stage 1: cross-compile libaros_rt.a
./aros-build.sh                 # stage 2: link the AROS C: command + deploy
../../graft/rust-smoke          # all of the above + boot AROS + assert ALL PASS
```

Stage 1 needs nightly + `rust-src` (`rustup component add rust-src --toolchain
nightly`). Stage 2 needs an AROS build tree (crosstools + SDK + `collect-aros`);
`aros-build.sh` finds it the way `graft/deploy-check` finds the boot dir, or set
`$AROS_BUILD`.

## Notes on the target spec (grounded against the AROS C objects)

The `.json` is derived from the in-tree `aarch64-unknown-none` spec, so the
`data-layout` matches rustc's own LLVM exactly. The choices that the live run
pinned down:

- **`code-model: large`** — *required*. AROS loads modules GOT-less, and clang
  routes even non-PIC weak-symbol access through a GOT under the small model
  (`make.cfg` item: every AROS C: command is built `-mcmodel=large`). Large model
  emits absolute `MOVW_UABS_*` instead — confirmed: `libaros_rt.a` has **no GOT
  relocations**, matching a working `C:` command exactly.
- **`relocation-model: static`** — matches the AROS C objects; the live link and run
  confirm no relocation-model mismatch.
- **`linker` / `pre-link-args`** — inert for `[RS0]`/`[RS1]` (the staticlib is linked
  by `collect-aros` on the AROS side, see `aros-build.sh`); they're pre-wired for
  RS3+ when Rust owns the final link. The `--allow-multiple-definition` for the
  `AROS_LIBREQ` duplicate marker (`graft/UPSTREAM-NOTES.md` item 18) is applied at
  the `collect-aros` link.
- **`features: +v8a,+neon`** — dropped the bare-metal `+strict-align`; AROS at EL0
  under macOS allows unaligned access. The live run confirms no alignment traps.

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

`[RS1]` PASS on booted AROS (it does — `0xe5889f2d`) means the allocator handed back
live memory that survived 1000+ `Vec` pushes and the `String` growth — i.e. the
AllocVec bridge and the aligned-allocation header math are correct end to end.

---

*Provenance: independent work. No third-party Rust-on-AROS / OS-port
implementation source was read, searched, or consulted. The allocator is the
textbook aligned-allocation-over-malloc trick, written from
`core::alloc::GlobalAlloc` and the AROS `exec.library` autodocs; the target spec is
derived from Rust's own `aarch64-unknown-none`. Any resemblance is coincidental.*
