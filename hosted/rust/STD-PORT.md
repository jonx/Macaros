# Rust `std` on AROS — contributor & resume guide

How to get started on, or resume, the Rust **`std`** port for darwin-aarch64 AROS.
For the no_std `[RS0]`/`[RS1]` work and the target spec see [README.md](README.md);
for design/status see [docs/features/rust-aros](../../docs/features/rust-aros/README.md).
**Known pal defects + the fix status** (2026-07-02 review: 7 fixed in-tree awaiting
a rebuild, 7 planned) live in [FIX-PLAN.md](FIX-PLAN.md) — read it before touching
the pal or trusting `env`/`errno`/`Command::output` semantics under threads.

## TL;DR — current state (2026-07-01)

- **`[RS0]`/`[RS1]`** (no_std: codegen + the `AllocVec` allocator) — done, run on AROS.
- **`[RS2]`** (a no_std I/O shim) — **skipped on purpose**; `std` doesn't reuse it.
- **`[RS3a]`** `std` *compiles* for `aarch64-unknown-aros`.
- **`[RS3b]`** `std` *runs* on booted AROS. A live run prints:
  ```
  hello from rust std on AROS
  [RS3] Vec sum=55  HashMap aros=64 rust=2021  fmt=    ok
  RUST-AROS: STD PASS
  ```
  That is `println!`, `Vec` + iterators, `HashMap` (drives the random pal + the
  allocator), and `format!`, all through the real standard library.
- **`reserve-x18`** added to the target spec (x18 register uses 532 → 0) — Rust code
  is now immune to the platform-register clobber.
- **`[RS4]` net** — a `std`-using Rust program does a **TCP round-trip over the
  bsdsocket bridge** on booted AROS (`aros_net_glue.c` + `net-build.sh` → `C:RustNet`):
  3/3 byte-exact echoes plus a clean connect-refused error path, no crash. The bridge
  is x18-safe (glue + `bsdsocket.library` both `-ffixed-x18`).
- **The pal is functionally complete** and verified live on booted AROS: real errno,
  `env` (read+write), `args`, **`fs`** (files + metadata + `read_dir`), **`std::net`**
  (TcpStream over the bsdsocket bridge), **`std::process`** (`Command` output/status via
  dos `SystemTagList`), **`time`** (`Instant`/`SystemTime`), and **`std::thread`**
  (spawn/join + `Mutex`/`Condvar`/`RwLock` + pthread-key TLS — 4 threads sharing a
  `Mutex` counter, verified solid). `time` + `thread` were unblocked by the OS-wide
  `-ffixed-x18` rebuild. See the [pal table](#the-pal-what-std-needs-per-os) and
  [Resume map](#resume-map).

## The mental model: three trees

The port spans three source trees. The actual Rust port is small and lives in a
**clone of Rust itself**, not in this repo.

| Tree | Path | What it is |
|---|---|---|
| **1. The `std` port** | `/Users/user/Source/rust-aros` | A git clone of the Rust std source. The whole port is **9 files / ~194 lines** here = the eventual rust-lang/rust PR. **Not pushed.** |
| **2. Host glue** (this repo) | `hosted/rust/` | The test crate + link recipe that build/run the port on AROS. |
| **3. The AROS OS** | `/Users/user/Source/aros-upstream` (`aarch64-darwin-graft`) | `exec`/`kernel`/`posixc` — provides `malloc`/`write`/… that `std` links against. |

**The connection:** the toolchain's `rust-src` is a **symlink** to tree 1, so
`-Zbuild-std` compiles `std` (including our `aros` pal) straight from the clone:
```
~/.rustup/toolchains/nightly-2026-06-27-*/lib/rustlib/src/rust  ->  /Users/user/Source/rust-aros
```

## Why this is tractable (the two low-level layers)

1. **The genuinely-hard low-level layer is the AROS OS itself** — the aarch64
   context switch, MMU, exception model, and the `posixc` C runtime. That is in
   `aros-upstream` and is **already built**. Rust `std` does not re-implement any of
   it; it just calls `posixc`.
2. **Rust's platform layer (the "pal") is thin per-feature glue on top.** `std`
   factors into ~23 `sys/<feature>` modules, and ships a complete `unsupported`
   **stub** for every one — so `std` compiles for almost any target by default. You
   only *must* provide the few modules with no stub fallback, then replace stubs with
   real `posixc`-backed code one at a time. That is why the core was ~194 lines, not
   a rewrite. It is **smaller work in a lot of places**, not one mountain.

## What's done vs. what's left (the pal map)

In the clone, `library/std/src/sys/`. Real `aros` arms so far:

| Module | Status | How |
|---|---|---|
| `alloc/aros.rs` | done | `posixc` `malloc` / `posix_memalign` |
| `stdio/aros.rs` | done | `posixc` `write`/`read` on fd 0/1/2 |
| `random/aros.rs` | done | calls `posixc` `arc4random_buf` (host CSPRNG borrowed via hostlib on hosted; weak fallback on native). The entropy policy lives in AROS (`compiler/crt/posixc/arc4random.c`), not the pal |
| `io/error/aros.rs` | done | real `posixc` errno via `__stdc_geterrnoptr` + `strerror`, NetBSD `ErrorKind` map |
| `env/aros.rs` | done (read+write) | `getenv`/`setenv`/`unsetenv` verified live; only `vars()` enumeration is empty (no POSIX `environ`) |
| `args/aros.rs` | done | reads argc/argv the C harness stashes in globals — verified live |
| `fs/aros.rs` | done | files (create/write/read/seek/close), `metadata`/`stat`/`fstat`/`exists`, and `read_dir` (opendir/readdir) — all verified live; only symlinks/`set_perm`/times remain stubs |
| `time/aros.rs` | done | `Instant`/`SystemTime` over `clock_gettime` — verified live after the OS `-ffixed-x18` rebuild (was SIGBUS before). REALTIME reads ~1978 (un-host-synced RTC, UPSTREAM-NOTES #36) but no longer faults |
| `net/connection/aros.rs` | done (IPv4) | TcpStream/TcpListener/UdpSocket/lookup_host over the bsdsocket LVOs (via `aros_net_glue.c`); blocking is solid, `set_nonblocking`/timeouts not yet effective (library park model) |
| `process/aros.rs` | done (output/status) | `Command` → shell-quoted line run by dos `SystemTagList` (via `aros_process_glue.c`); `output()` captures via `MacRW:` temp files, `status()` runs synchronously — verified live. No live pipes / async child / per-command env+cwd yet |
| `thread/aros.rs` | done | `std::thread` spawn/join/sleep/yield over `pthread.library` (via `aros_thr_*` glue) — verified live (4 threads + shared Mutex) |
| `thread_local/key/aros.rs` | done | pthread-key TLS (`pthread_key_*`), the `os` key-based backend |
| `pal/unsupported/sync/*` | done | `Mutex`/`Condvar` over `pthread_mutex`/`pthread_cond` (zeroed pinned buffers, `aros_sync_glue.c`); `RwLock` = portable `queue`, `Parker` = shared `pthread` parker |
| `process pipe`, … | **stubs** | a few leftover corners fall through to `unsupported` |

## Dev environment setup (fresh machine, or if the clone/symlink is gone)

```sh
# 1. toolchain (pinned by hosted/rust/rust-toolchain.toml) + rust-src
rustup component add rust-src --toolchain nightly-2026-06-27-aarch64-apple-darwin

# 2. recreate the local clone from the exact-version rust-src, git-track it
SYSROOT=$(rustc +nightly-2026-06-27 --print sysroot)
SRC="$SYSROOT/lib/rustlib/src/rust"
cp -R "$SRC" /Users/user/Source/rust-aros
( cd /Users/user/Source/rust-aros && git init -q && git add -A && git commit -qm "vendor rust-src baseline" )

# 3. point -Zbuild-std at the clone (so edits there take effect)
mv "$SRC" "$SRC.orig"
ln -s /Users/user/Source/rust-aros "$SRC"
```
If a toolchain reinstall ever replaces the symlink with a real dir, just redo step 3.
(For a real upstream PR later, the same diffs apply to a rust-lang/rust fork at the
matching commit — `rustc +nightly-2026-06-27 -vV` prints the commit hash.)

## The build + run loop

```sh
# build the std staticlib (std compiled from the clone)
cd hosted/rust/std-probe
cargo +nightly-2026-06-27 build --release \
    -Zjson-target-spec -Zbuild-std=std,panic_abort \
    --target ../aarch64-unknown-aros.json

# link it + the C harness into C:RustStd (deploys into the AROS build tree)
../std-build.sh

# run on booted AROS, capturing std's stdout via a redirect to a host file
printf 'RustStd >MacRW:rs3.out\nEcho done >MacRW:rs3.done\n' > /tmp/rs3-start
AROS_CTL_STARTUP_FILE=/tmp/rs3-start ./graft/aros-ctl run   # from repo root
#   ... wait for ~/AROS/Shared/rs3.done, then read ~/AROS/Shared/rs3.out
```
When you change anything **in the clone's `build.rs`** (e.g. the known-target list),
run `cargo clean` first — cargo caches the std build and won't re-run `build.rs`.

## Gotchas (each of these cost real time)

- **Capture std output via the posixc redirect** (`RustStd >MacRW:file`), *not* a C
  harness's `PutStr`. posixc `write(1)` honors the shell redirect; `dos` `PutStr`
  (via `pr_COS`) does **not**, so C-side prints go to the console, not the file.
- **No `-lclang_rt.builtins`** — these crosstools don't ship it and `std` doesn't
  need it; linking it fails. (`std-build.sh` already omits it.)
- **`-Zjson-target-spec` is required** by this nightly to use a `.json` target.
- **`code-model: large`** in the target spec is mandatory (AROS loads GOT-less).
- **The boot stall**: some boots hang before the window/shell appears — a
  pre-existing flaky-boot bug, not yours. Retry. And do **not** background-boot in a
  loop while the user has AROS running; the cleanup `pkill`s their instance.

## Known risks (read before trusting a green run)

- **`x18` is not reserved in the Rust target yet.** macOS zeroes `x18` across signal
  delivery, and AROS-hosted preempts via signals, so any long-lived value the
  compiler parks in `x18` can be clobbered (this is exactly the ffmpeg h264 bug; see
  [hosted/x18probe](../x18probe/README.md) and `NOTES.md`). ffmpeg fixed
  it with `-ffixed-x18`. The Rust target should do the equivalent — add the LLVM
  **`reserve-x18`** target feature to `aarch64-unknown-aros.json`
  (`"features": "+v8a,+neon,+reserve-x18"`, verify the exact spelling) and rebuild.
  Until then a passing run is not proof of safety; the bug is intermittent and
  data/timing dependent. **This is the highest-priority correctness item.**

## Tools & tests for this port

- **`graft/rust-smoke`** — the no_std `[RS0]`/`[RS1]` end-to-end: build → link →
  boot → assert `RUST-AROS: ALL PASS`. The template for an `RS3` std smoke (writing a
  `rust-std-smoke` that boots `RustStd` and asserts `RUST-AROS: STD PASS` is a good
  early TODO).
- **`hosted/rust/std-build.sh`** — the RS3 link recipe (collect-aros). **`std-probe/`**
  is the std test crate; edit `src/lib.rs` to exercise whatever pal piece you just added.
- **`graft/aros-ctl`** — drive AROS headlessly: `run` (boot), `type`/`click`/`shot`,
  `stop`. The boot-and-capture loop above uses it. See
  [docs/features/control-harness](../../docs/features/control-harness/README.md).
- **`graft/bench-run C:<cmd>`** — boot AROS, run one `C:` command, capture output
  (what `rust-smoke` uses).
- **`graft/deploy-check`** — confirms you're running the artifacts you just built
  (stale-deploy guard).
- **Debugging a crash** ([docs/features/debug-tools](../../docs/features/debug-tools/README.md)):
  - the **trap backtrace** (free, always on) names the faulting `module function+offset`;
  - **MUNGWALL** (`AROS_HOST_ARGS=mungwall ./graft/aros-ctl run`) guards every
    `AllocMem`/pool alloc — the right tool for a suspected allocator/`std` memory bug;
  - **host lldb** (`lldb -p "$(cat /tmp/aros-cm.pid)"`) — `Macaros` is a darwin
    process, so lldb catches the fault address before AROS's guru;
  - **`hosted/x18probe`** — the host-ABI probe that proved the `x18` clobber; rerun it
    to re-check the platform on a new macOS.

## How to add a pal module (the loop)

1. In the clone, add `target_os = "aros"` to the `cfg_select!` in
   `library/std/src/sys/<feature>/mod.rs`, pointing at a new `aros` module
   (or reusing `unix`/`generic` if it fits).
2. Write `sys/<feature>/aros.rs` calling `posixc`/`dos`/`bsdsocket` through a private
   `unsafe extern "C"` block (no `libc`-crate dependency yet — see `alloc/aros.rs` /
   `stdio/aros.rs` for the pattern).
3. `cargo build … -Zbuild-std` the probe; fix errors until `std` compiles.
4. Exercise the new API in `std-probe/src/lib.rs`, `std-build.sh`, boot, read the
   redirected output. Add the marker to a smoke when it's stable.

## Resume map (do these next, roughly in order)

**Done:** `reserve-x18`; **`std::net`** (`sys/net/connection/aros.rs`:
TcpStream/TcpListener/UdpSocket/lookup_host over the bsdsocket LVOs via the
`aros_np_*` glue — verified live, `C:RustStdNet` does a real `TcpStream::connect` +
`write_all`/`read_exact` round-trip plus `local_addr`/`peer_addr`/`set_nodelay`;
needs `bsdsocket.library` built via `make workbench-libs-bsdsocket` and a TCP server
on the **host**, since the bridge is host-passthrough so AROS's `127.0.0.1` is the
Mac's loopback); the earlier **glue round-trip** (`C:RustNet`) still exists as the
lower-level RS4 proof; **real errno** (`sys/io/error/aros.rs`, NetBSD-numbered,
matches the net errno 61); **env reads** (`sys/env/aros.rs` getenv, verified); and
**args** (`sys/args/aros.rs` reads the C `main`'s argc/argv via two globals the
harness sets — verified live: `RustStd alpha beta 42` → `std::env::args()` =
`["RustStd","alpha","beta","42"]`).

**`std::net` caveats (bsdsocket bridge limits, not pal bugs):** IPv4 only (IPv6
requests return `Unsupported`); `set_nonblocking(true)` and socket timeouts are not
yet effective because the AROS library keeps host sockets `O_NONBLOCK` and emulates
blocking with a timer-poll park (`FIONBIO` is a no-op; we return `Unsupported` for a
requested timeout rather than lie). `try_clone`/`duplicate` return `Unsupported`.
See UPSTREAM-NOTES #37.

**The x18 blocker is fixed (2026-07-01).** `time` used to SIGBUS from Rust: the fault
was the **x18 clobber in OS code not built with `-ffixed-x18`** (macOS zeroes x18 on
signal delivery, AROS-hosted preempts via `SIGALRM`; `posixc` alone had 108 x18 uses).
The flag was already in `config/make.cfg.in` but the deployed modules predated it, so
the fix was to **force-recompile the Rust-runtime path** (`posixc`, `stdc`, `exec`,
`kernel`, `timer`, `dos`, `emul-handler`) — each dropped to ~2 x18 uses (compiler-rt
soft-float residual). After that, `Instant`/`SystemTime` and `std::thread` both work,
verified live. See the build doc + NOTES.md for the x18 finding. (Separately, the
hosted RTC still isn't host-synced, so REALTIME reads ~1978 — UPSTREAM-NOTES #36 — but
it no longer faults.)

**`env` writes work** (earlier "setenv fails" was a boot-stall run misread): AROS
`setenv` → `SetVar(..., LV_VAR|GVF_LOCAL_ONLY)` returns DOSTRUE for a loaded `C:`
command, so `std::env::set_var` succeeds — verified live (`set_var RUST_WROTE` reads
back `"yes-42"`). Only `std::env::vars` (full enumeration) is still empty, since AROS
has no POSIX `environ` array.

Remaining pal pieces (small corners — the core is done):

1. **Small stubs**: `fs` symlinks/`set_perm`/times; `std::process` live pipes + async
   child + per-command env/cwd (the sync `output()`/`status()` path is done);
   `std::env::vars()` enumeration.
2. **Toward a real PR** (see "PR readiness" below): built-in target spec, `libc`-crate
   AROS support.

### PR readiness (rust-lang/rust, a tier-3 `aarch64-unknown-aros` target)

What a first PR would contain, and where each piece stands:

- **Target spec → built-in.** Today `aarch64-unknown-aros.json` + `-Zjson-target-spec`
  drive the build. For upstream it becomes
  `compiler/rustc_target/src/spec/targets/aarch64_unknown_aros.rs` (os `"aros"`,
  `code-model: Large`, `features: "+v8a,+neon,+reserve-x18"`, `relocation-model` +
  linker wiring to the collect-aros/crosstools flow). Mechanical; the JSON is the spec.
- **pal modules (done, in the clone):** `alloc`, `stdio`, `io/error`, `env`
  (read+write), `args`, `fs` (files + metadata + dirs), `net` (IPv4), `process`,
  `time`, **`thread` + the full sync core** (`Mutex`/`Condvar`/`RwLock`/`Parker`) +
  pthread-key TLS, and `random` (posixc `arc4random_buf`). All verified live. This is
  the bulk of the PR.
- **`libc`-crate AROS support.** The pal currently declares its own `extern "C"`
  posixc/bsdsocket/pthread signatures. A proper PR adds an `aros` module to the `libc`
  crate (types + fn decls from `compiler/crt/posixc` + `arch/all-unix/bsdsocket` +
  `compiler/pthread`), then the pal uses `libc::*`. It would also let the sync core use
  the upstream `pthread` pal `Mutex`/`Condvar` directly instead of the byte-buffer glue.
- **CSPRNG: done, in AROS.** `posixc` now has `arc4random_buf` (borrows the host CSPRNG
  on hosted via hostlib, weak fallback on native); the pal just calls it. A native
  entropy source is the remaining AROS-side gap (asked upstream on Slack).
- **Known gaps to disclose in the PR:** the `-ffixed-x18` OS build requirement (the
  hosted OS must be built with it, else `time`/threads SIGBUS); `std::process` has no
  live pipes / async child / per-command env+cwd (sync `output()`/`status()` only);
  `std::env::vars()` enumeration is empty (no POSIX `environ`); `set_nonblocking`/socket
  timeouts are no-ops (bsdsocket park model, UPSTREAM-NOTES #37); REALTIME reads ~1978
  (un-host-synced RTC, #36). Tier-3 targets ship with documented gaps like these.

### Threads (done)

`std::thread` and the full sync core are wired and verified (4 threads incrementing a
shared `Mutex` counter to exactly 4000, over 5 runs, no race/hang). AROS's
`pthread.library` (the `-lpthread` linklib) backs all of it:

- `sys/thread/aros.rs` — `Thread` spawn/join/yield/sleep via the `aros_thr_*` glue
  (`hosted/rust/aros_thread_glue.c`), which owns the opaque `pthread_attr_t` for the
  stack size. `sleep` uses dos `Delay` (20ms granularity).
- `sys/thread_local/key/aros.rs` — pthread-key TLS (`pthread_key_*`); the library
  reserves a main-task slot at `ADD2INIT`, so keys work on the main thread too.
- **Sync core**: `sys/pal/unsupported/sync/{mutex,condvar}.rs` implement the pal
  `Mutex`/`Condvar` as **zeroed, pinned byte buffers** over `pthread_mutex`/
  `pthread_cond` (`aros_sync_glue.c`; `pthread_mutex_t` is 136 B, `pthread_cond_t`
  152 B — a zeroed buffer is a valid `PTHREAD_*_INITIALIZER`). `pal/unsupported/mod.rs`
  exposes `pub mod sync` for aros, so the shared **`pthread` `Mutex`/`Condvar`/`Parker`
  wrappers** (with their `OnceBox` movability handling) and the portable **`queue`
  `RwLock`** all resolve — aros is added to those `cfg_select` arms in `sys/sync/`.

Threads are only *solid* because of the OS-wide `-ffixed-x18` rebuild (thread execution
runs through `exec`'s scheduler under `SIGALRM` preemption, same x18 exposure as
`time`). All the std build scripts link `-lpthread` + `aros_thread_glue.o` +
`aros_sync_glue.o`.

---

*The pal lives in the clone (tree 1), under `library/std/src/sys/*/aros.rs`. This repo
holds the rig that builds and proves it. Nothing Rust-side is pushed until the
maintainer decides.*
