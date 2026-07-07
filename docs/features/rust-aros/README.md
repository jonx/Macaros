# Rust on AROS (aarch64)

Status: **`[RS0]`..`[RS3]` PROVEN LIVE on AROS** — 2026-07-01. Real Rust **`std`**
cross-compiled on the Mac **runs on booted darwin-aarch64 AROS**. A booted-AROS run
prints, all through the real standard library:

```
hello from rust std on AROS
[RS3] Vec sum=55  HashMap aros=64 rust=2021  fmt=    ok
RUST-AROS: STD PASS
```

That exercises `println!`, `Vec` + iterators, `HashMap` (drives the random pal + the
allocator), and `format!`. The target reserves `x18` (`+reserve-x18`), so Rust code is
immune to the platform-register clobber. `[RS0]`/`[RS1]` (no_std codegen + the
`AllocVec` allocator) came first; **`[RS2]` (a no_std I/O shim) was skipped** — `std`
doesn't reuse it.

**Verified live since (2026-07-01), all through real `std`:**

- **`std::net`** — `TcpStream::connect` + `write_all`/`read_exact` round-trip over the
  bsdsocket bridge, plus `local_addr`/`peer_addr`/`set_nodelay` (`C:RustStdNet`). IPv4.
  `try_clone`/`duplicate` now work (`Dup2Socket`).
- **`fs`** — `File` create/write/**read**/seek + `metadata` (size/type/perms) +
  `read_dir` on the `MacRW:` host share, plus **`set_permissions`** (chmod/fchmod),
  **`symlink`+`readlink`**, and path **`set_times`** (utimes) — all verified live
  2026-07-07.
- **`env`** — `getenv` **and** `setenv` (`set_var` reads back) + `unsetenv`, plus
  **`std::env::vars()` enumeration** (walks `pr_LocalVars`).
- **`args`** — `std::env::args()` from the shell command line.
- **`process`** — `Command::output()`/`status()` runs a real `C:` command and captures
  stdout/exit code (dos `SystemTagList`), now honouring **per-command `env` + `cwd`**
  (injected `CD`/`Set` script via `Execute`) — verified live 2026-07-07.
- **`time`** — `Instant`/`SystemTime` via `clock_gettime` (unblocked by the OS
  `-ffixed-x18` rebuild; used to SIGBUS).
- **`std::thread`** — 4 threads incrementing a shared `Mutex<u64>` to exactly 4000,
  joined, over 5 runs. Full sync core (`Mutex`/`Condvar`/`RwLock`/`Parker`) on
  `pthread.library`, plus pthread-key TLS.

**The pal is functionally complete.** The authoritative per-module status + the
build/run loop live in [hosted/rust/STD-PORT.md](../../../hosted/rust/STD-PORT.md);
this page is the design/why.
`std` is brought up the upstream way: a fresh `sys/*/aros.rs` per module calling
`posixc`/`bsdsocket`/`pthread` directly, developed in a **local rust clone**
(`../rust-aros`, not pushed) so it is PR-able to rust-lang/rust later.

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
  → **DONE, PROVEN LIVE** ([`hosted/rust/`](../../../hosted/rust/)): the crate
  cross-compiles to `elf64-littleaarch64` (GOT-free relocs), `collect-aros` links it
  into an ET_REL AROS `C:` command (`startup.o`, not `elf-startup.o` — the one
  gotcha), and on booted AROS the selftest returns the magic. Code model **must** be
  `large` (AROS loads GOT-less), confirmed against a working `C:` command.
- **[RS1]** alloc: `-Zbuild-std=core,alloc` + a `#[global_allocator]` over
  `AllocVec`/`FreeVec`; a Rust fn builds a `Vec`/`String`, returns a checksum →
  assert. Proves the allocator bridge + collections.
  → **DONE, PROVEN LIVE** in [`hosted/rust/aros-rt`](../../../hosted/rust/aros-rt/src/lib.rs):
  the allocator does aligned-allocation-over-`AllocVec`; on booted AROS the `[RS1]`
  FNV-1a digest over a `Vec<u32>`+`String` matches the host value (`0xe5889f2d`) —
  the `#[global_allocator]` round-trips real exec memory. `graft/rust-smoke` asserts it.
- **[RS2]** ~~`aros-sys` no_std I/O shim~~ — **skipped**: `std` calls `posixc`
  directly, not a hand-written shim, and `[RS0]` already proved linking/startup, so
  this was a detour. Went straight to `std`.
- **[RS3]** **std bring-up** — **DONE (core), PROVEN LIVE.** `println!`, `Vec`,
  `HashMap`, `format!` run on booted AROS via a fresh `sys/pal/aros` (see
  [The `std` port](#the-std-port-rs3)). Remaining surface (real `time`, `thread`
  spawn/join, `env`/`args`, `fs`) is RS3c. Gated on `posixc` completeness (shared
  with ffmpeg).
- **[RS4]** std `net` over **`bsdsocket`** → a Rust `TcpStream` fetches a URL
  (reuses the live sockets path); std `fs` → read/write a `MacRW:` file.
- **[RS5]** Cargo workflow: `cargo build --target aarch64-unknown-aros.json
  -Zbuild-std` + packaging, so arbitrary crates build for AROS routinely.

## The `std` port (RS3)

`std` is brought up the **upstream way** so it can become a rust-lang/rust PR: a
fresh `sys/pal/aros` that calls `posixc` directly (no `libc`-crate AROS support
needed yet), developed in a **local clone of the Rust std source**
(`../rust-aros` — the toolchain's `rust-src` for
`nightly-2026-06-27`, git-tracked, symlinked into the toolchain so `-Zbuild-std`
builds from it). **Not pushed**, kept local until ready.

What makes `std` compile + run for `aarch64-unknown-aros` (all in the clone):

| Piece | Where | How |
|---|---|---|
| System allocator | `sys/alloc/aros.rs` | `posixc` `malloc` / `posix_memalign` |
| stdio | `sys/stdio/aros.rs` | `posixc` `write`/`read` on fd 0/1/2 (= dos Input/Output) |
| errno → ErrorKind | `sys/io/error` → `generic` | **stub** (errno=0); TODO real `posixc` errno |
| randomness | `sys/random/aros.rs` | **weak** SplitMix64 (ASLR seed); TODO real CSPRNG |
| TLS | `sys/thread_local` → `no_threads` | single-thread statics; pthread keys when `std::thread` lands |
| known target | `library/std/build.rs` | adds `aros` so std is not `restricted_std` |

Build + run: `hosted/rust/std-probe` is a `std` staticlib; `hosted/rust/std-build.sh`
links it + `rs3_main.c` through `collect-aros` into `C:RustStd` (the RS0/RS1 recipe,
minus `-lclang_rt.builtins` — these crosstools don't ship it and `std` doesn't need
it). Run on booted AROS with output redirected to a `MacRW:` file: posixc `write(1)`
honors the shell redirect, but `dos` `PutStr`/`pr_COS` does **not**, so capture std
output via the redirect, not a C harness's own prints.

TODO before a real PR: real errno, a real CSPRNG, the `time`/`thread`/`env`/`args`/
`fs` pal pieces (RS3c), `net` via `bsdsocket` (RS4), and eventually `libc`-crate AROS
support so the pal can drop its private `extern "C"` declarations.

**Hands-on dev loop, tools/tests, the `x18` risk, and the full resume map:**
[`hosted/rust/STD-PORT.md`](../../../hosted/rust/STD-PORT.md).

## Risks & decisions

1. **std is the mountain** — RS0–RS2 are days; RS3+ is a Rust OS port (Redox-sized,
   though smaller pieces). Treat no_std and std as separate deliverables.
2. **std sys approach** (`pal/aros` vs `unix`+`libc`-crate) — the RS3 fork in the road.
3. **`posixc` correctness** gaps surface here exactly as in ffmpeg (the `printf`
   float bug is the prototype) — shared work, do it once.
4. **Unwinding/TLS** over AROS — `panic=abort` and FFI sidestep both early; verify
   libunwind + `pthread_key` wiring before relying on them.
5. **68k: no_std WORKS, std stays out of scope.** Rust `core`+`alloc` compiled for
   `m68k-unknown-none-elf` runs as hunk executables through the 68k JIT, two-engine
   verified — the corpus, pipeline, and the open LLVM M68k CCR miscompile it
   canaries live in [`hosted/jit68k/rust68k/`](../../../hosted/jit68k/rust68k/README.md)
   (`make hosted-jit68k-rust`). `std` for m68k stays out of scope until there is a
   real m68k AROS library world to call (see the plan in
   [`hosted/rust/FIX-PLAN.md`](../../../hosted/rust/FIX-PLAN.md)). This port targets
   aarch64 (and trivially x86_64).

## What it unlocks

Modern Rust crates on AROS; memory-safe AROS tooling; and **FFI straight into the
native [libav\*](../ffmpeg-native/README.md)** once both exist — same runtime, two
payoffs. `bsdsocket` already done means networked Rust is one of the *earlier* std
wins, not a late one.

---

*`[RS0]`/`[RS1]` are built and **run on AROS** (`graft/rust-smoke`,
[`hosted/rust/`](../../../hosted/rust/)); RS2+ and all of std are still scoping. Like
[ffmpeg-native](../ffmpeg-native/README.md), this is a **native software port**, not
a `hostlib` bridge. The no_std path is self-contained (core + alloc + an exec
allocator shim — now proven); the std path rides on the same `posixc` hardening the
rest of the native-stack work needs.*
