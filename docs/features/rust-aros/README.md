# Rust on AROS (aarch64)

Status: **scoping + an `[RS0]`/`[RS1]` starting point landed** — 2026-06-28. A
bring-up plan grounded against the upstream tree and Rust's target model. The
no_std path now has a real first commit to build from in
[`hosted/rust/`](../../../hosted/rust/): a custom `aarch64-unknown-aros` target
spec and a `#[global_allocator]` runtime crate that **cross-compile clean to
aarch64-AROS ELF today** (stage 1 verified — see below). The std path (RS3+) is
still scoping. Items not yet confirmed are marked **UNVERIFIED**.

## Goal

Make Rust target darwin-aarch64 AROS, in two stages:

1. **`#![no_std]` (core + alloc)** — the language, `Vec`/`String`/iterators, and FFI,
   with a custom target spec and an allocator wired to AROS exec. Reachable soon.
2. **Full `std`** — `fs`/`thread`/`time`/`net`/`process`/`env`/`println!` ported onto
   AROS's `posixc`/`dos`/`exec`/`bsdsocket`. A real project, the same flavor as the
   [ffmpeg](../ffmpeg-native/README.md) libc-completeness work — and it **shares the
   same substrate** (hardening `posixc` unblocks both; `bsdsocket` is already live,
   so Rust `net` comes nearly free).

Why it's more feasible than it sounds: Rust is **LLVM-based**, and aarch64 is a
tier-1 LLVM/Rust codegen backend — the *same* code generator the AROS crosstools
already use. Generating correct ARM code for AROS is not the blocker. What's
missing is a **target definition** and **std**, not the compiler.

## What it actually needs vs. what AROS has

| Rust needs | AROS status |
|---|---|
| LLVM aarch64 codegen | **tier-1** in stock rustc; no AROS work needed for codegen |
| a target spec (`aarch64-unknown-aros.json`) | **new** — a JSON file (arch, data-layout, linker, reloc model, `os`/`env`, `panic`). No rustc fork needed; nightly + `-Zbuild-std` consumes a custom JSON target |
| linker + AROS startup/crt | **present** — reuse the AROS `clang`/`ld.lld` wrapper (knows `aarch64elf_aros`) and the same C startup AROS C programs use; set the target's `linker` to it |
| `#[global_allocator]` | **bridge to write** — thin `extern "C"` shim to exec `AllocVec`/`FreeVec` (or pools); ~30 lines |
| `#[panic_handler]` / `panic=abort` | trivial for no_std; **unwinding** later needs `eh_personality` + libunwind — the crosstools LLVM build **includes libunwind** (`UPSTREAM-NOTES.md` item 4), so plausible, but wiring is **UNVERIFIED** |
| TLS (std thread-locals) | `pthread_key_create` is **present** (`posixc`); Rust TLS over it is **UNVERIFIED** |
| std `sys`: fs / thread / time / net / process / env / stdio | **buildable on existing pieces** — `fs`→`dos`/`posixc`, `thread`→AROS tasks/`pthread`, `time`→`clock_gettime`, **`net`→`bsdsocket` (already live)**, `process`→`SystemTags`/`CreateNewProc`, `stdio`→`dos` Input/Output. Completeness is the work, same as ffmpeg. |

Takeaway: codegen is free; the no_std path is small; **std is a genuine OS port**
of Rust's platform layer, but every piece it needs already exists in some form.

## Approach

- **Cross-compile from the Mac host** with stock **nightly** rustc — no custom
  rustc build. A custom JSON target + `-Zbuild-std=core,alloc` (then `std` once
  ported) is the supported path for an out-of-tree OS.
- **Reuse the AROS link toolchain**: target `linker` = the AROS cc/`ld.lld`
  wrapper, `linker-flavor = gcc`, so Rust objects link with AROS startup + libs and
  produce a normal AROS ELF. Same linker that builds AROS C programs.
- **Interop first**: earliest milestones are a Rust `staticlib` crate exposing
  `extern "C"` functions, linked into an AROS C program (C owns AROS startup, calls
  Rust). A Rust-owned `fn main()` waits for std (RS3).
- **`panic = "abort"`** initially; unwinding is a later nicety.
- **std module choice (RS3 decision):** a fresh `sys/pal/aros` platform module
  (clean, matches how Rust adds OSes — Redox/Hermit/UEFI) vs. reusing `sys/unix`
  with an AROS arm added to the `libc` crate (faster spike, leakier — `unix`
  assumes fork/exec/signals/`/dev` that AROS only partly has). Recommend a quick
  `unix`-reuse spike to prove viability, then the dedicated `pal` module.

## Phased bring-up (greppable markers `[RSn]`, each unattended)

- **[RS0]** Toolchain proof: nightly rustc + `aarch64-unknown-aros.json`; a
  `#![no_std]` **staticlib** (`-Zbuild-std=core`) exposing one `extern "C"` fn
  returning a constant; link into a minimal AROS C program; run on booted AROS →
  assert the Rust fn ran. Proves codegen + linking + startup interop.
  → **concrete code: [`hosted/rust/`](../../../hosted/rust/). Stage 1 GREEN** —
  the spec loads (`target_os="aros"`) and the crate cross-compiles to
  `elf64-littleaarch64`; the AROS-side link/run is gated on the AROS C SDK.
- **[RS1]** alloc: `-Zbuild-std=core,alloc` + a `#[global_allocator]` over
  `AllocVec`/`FreeVec`; a Rust fn builds a `Vec`/`String`, returns a checksum →
  assert. Proves the allocator bridge + collections.
  → **built & compiling** in [`hosted/rust/aros-rt`](../../../hosted/rust/aros-rt/src/lib.rs):
  the allocator does aligned-allocation-over-`AllocVec`; the `[RS1]` digest is a
  reproducible FNV-1a over a `Vec<u32>`+`String`. Same stage-1/stage-2 gate as RS0.
- **[RS2]** `aros-sys` FFI shim crate: wrap a few AROS calls (`dos` `PutStr`,
  `timer`) so no_std Rust can do I/O. Demo: Rust prints via `dos.library` and times
  a loop on booted AROS.
- **[RS3]** **std bring-up** (the big one): pick the sys approach, implement the
  minimum — `stdio` (`println!`), `time`, `thread` spawn/join, `env`. `fn main()`
  with `println!("hello from rust")` on booted AROS. Gated on `posixc`
  completeness (shared with ffmpeg).
- **[RS4]** std `net` over **`bsdsocket`** → a Rust `TcpStream` fetches a URL
  (reuses the live sockets path); std `fs` → read/write a `MacRW:` file.
- **[RS5]** Cargo workflow: `cargo build --target aarch64-unknown-aros.json
  -Zbuild-std` + packaging, so arbitrary crates build for AROS routinely.

## Risks & decisions

1. **std is the mountain** — RS0–RS2 are days; RS3+ is a Rust OS port (Redox-sized,
   though smaller pieces). Treat no_std and std as separate deliverables.
2. **std sys approach** (`pal/aros` vs `unix`+`libc`-crate) — the RS3 fork in the road.
3. **`posixc` correctness** gaps surface here exactly as in ffmpeg (the `printf`
   float bug is the prototype) — shared work, do it once.
4. **Unwinding/TLS** over AROS — `panic=abort` and FFI sidestep both early; verify
   libunwind + `pthread_key` wiring before relying on them.
5. **68k Amiga is out of scope** — LLVM's M68k backend is experimental and `m68k`
   Rust is bleeding-edge no_std only. This targets aarch64 (and trivially x86_64).

## What it unlocks

Modern Rust crates on AROS; memory-safe AROS tooling; and **FFI straight into the
native [libav\*](../ffmpeg-native/README.md)** once both exist — same runtime, two
payoffs. `bsdsocket` already done means networked Rust is one of the *earlier* std
wins, not a late one.

---

*The `[RS0]`/`[RS1]` no_std starting point is built and cross-compiles to
aarch64-AROS ELF ([`hosted/rust/`](../../../hosted/rust/)); RS2+ and all of std are
still scoping. Like [ffmpeg-native](../ffmpeg-native/README.md), this is a **native
software port**, not a `hostlib` bridge. The no_std path is nearly self-contained
(core + alloc + an exec allocator shim); the std path rides on the same `posixc`
hardening the rest of the native-stack work needs.*
