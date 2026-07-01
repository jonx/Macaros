# Rust `std` on AROS — contributor & resume guide

How to get started on, or resume, the Rust **`std`** port for darwin-aarch64 AROS.
For the no_std `[RS0]`/`[RS1]` work and the target spec see [README.md](README.md);
for design/status see [docs/features/rust-aros](../../docs/features/rust-aros/README.md).

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
- **RS3c done:** real errno, `env` reads, `args`, **`fs`** (create/write/**read**
  round-trip on a real host file, verified live), and **`std::net`** (TcpStream
  connect/read/write/addr/nodelay round-trip, verified live over the bsdsocket
  bridge). `time` is written but blocked on the OS x18 rebuild; `thread` is staged.
  See [Resume map](#resume-map).

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
| `random/aros.rs` | done (**weak**) | SplitMix64 from an ASLR seed — TODO real CSPRNG |
| `io/error/aros.rs` | done | real `posixc` errno via `__stdc_geterrnoptr` + `strerror`, NetBSD `ErrorKind` map |
| `env/aros.rs` | reads done | `getenv` verified; `setenv` fails (AROS `SetVar`) — see blockers |
| `args/aros.rs` | done | reads argc/argv the C harness stashes in globals — verified live |
| `fs/aros.rs` | done (files) | `File` create/write/read/seek/close over `posixc` — full round-trip verified live; metadata/dirs/symlinks are stubs |
| `time/aros.rs` | written | correct `timespec` layout; Rust path faults on x18 until the OS `-ffixed-x18` rebuild |
| `net/connection/aros.rs` | done (IPv4) | TcpStream/TcpListener/UdpSocket/lookup_host over the bsdsocket LVOs (via `aros_net_glue.c`); blocking is solid, `set_nonblocking`/timeouts not yet effective (library park model) |
| `thread_local` → `no_threads` | done | single-thread statics (pthread-key backend written + staged, see below) |
| `thread` | **staged** | `sys/thread/aros.rs` + `key/aros.rs` complete (pthread spawn/join + TLS) but **not wired** — needs the sync core; see "Threads (staged)" |
| `process`, `pipe`, … | **stubs** | fall through to `unsupported` — remaining work |

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
  - **host lldb** (`lldb -p "$(cat /tmp/aros-cm.pid)"`) — `Daedalos` is a darwin
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

**Two AROS-side blockers found (not pal bugs — fix AROS, then the pal just works):**

- **`time` faults from Rust (but `clock_gettime` works in C).** `sys/time/aros.rs`
  is written and the `timespec` layout is correct (a C `ClockTest` command confirmed
  `sizeof(timespec)=16`, `sizeof(long)=8`, and `clock_gettime` returns rc=0 with a
  sane MONOTONIC uptime). So this is **not** an AROS `clock_gettime` bug; the Rust
  path faulting (SIGBUS) is the **x18 clobber in the not-yet-`-ffixed-x18`
  timer/posixc code** (intermittent in C, hit consistently from Rust's deeper call
  path). The pending OS-wide `-ffixed-x18` rebuild (TODO.md) should unblock it; the
  pal needs no change. (Separately, the hosted RTC isn't host-synced, so REALTIME is
  ~1978 — see UPSTREAM-NOTES #36.)
- **`env` writes fail.** AROS `setenv` returns -1 (`SetVar(..., LV_VAR|GVF_LOCAL_ONLY)`
  fails for a `C:` command's process), so `std::env::set_var` panics by design.
  Reads work. Fix AROS `SetVar`/local-vars for loaded commands, or back writes with
  a different mechanism.

Remaining pal pieces, roughly in order:

1. **`fs`**: `File` create/write/**read**/seek/close over posixc all work now
   (verified live: Rust wrote **and read back** a real `MacRW:` host file). metadata/
   dirs/symlinks are still stubs. **Gotcha that cost a while:** AROS's access-mode
   flags are **not** the near-universal `O_RDONLY=0` -- they are `O_RDONLY=0x1`,
   `O_WRONLY=0x2`, `O_RDWR=0x3` (`O_ACCMODE=0x3`, from
   `compiler/crt/posixc/include/fcntl.h`). A read-only open with `flags=0` gives an
   empty access mode, which AROS `open` rejects with EINVAL -- that was the
   "read-open bug", a wrong constant in the pal, not a posixc bug. The create/misc
   flags (`O_CREAT=0x40`, `O_TRUNC=0x200`, `O_APPEND=0x400`) do match. off_t=i64,
   mode_t=u16.
2. **`thread` (staged — foundation done, sync core remains).** See below.
3. **Toward a real PR**: real CSPRNG, `libc`-crate AROS support so the pal drops its
   private `extern "C"` decls, and upstreaming the target spec.

### Threads (staged)

AROS has a full `pthread.library` (the `-lpthread` linklib: `libpthread.a` is already
in the build tree), so the **foundation is written and correct, but deliberately not
wired**:

- `sys/thread/aros.rs` — `Thread` spawn/join/yield/sleep via the `aros_thr_*` glue
  (`hosted/rust/aros_thread_glue.c`), which owns the opaque `pthread_attr_t` so we can
  set the stack size. `sleep` uses dos `Delay` (20ms granularity).
- `sys/thread_local/key/aros.rs` — pthread-key TLS (`pthread_key_create`/`setspecific`/
  `getspecific`/`key_delete`). The library reserves a slot for the main task at
  `ADD2INIT`, so keys work on the main thread too.

**Why it's not turned on:** `std::thread` can't be shipped half-done. The moment real
threads exist, std's own internals (e.g. the `Stdout` lock) use `sys` `Mutex`/`Condvar`,
which for AROS still fall through to the single-threaded `no_threads` stubs — two
threads doing `println!` would race. Wiring threads therefore also requires a real
**sync core**: `Mutex`+`Condvar` (buildable on AROS `pthread_mutex`/`pthread_cond`,
whose structs are `SignalSemaphore`-based with all-zero const initializers + lazy init,
so a zeroed buffer is a valid static lock), a `RwLock` (the portable `queue` impl), and
a `Parker` (the portable `pthread` parker) — the last two need a `sys::pal::sync` for
AROS, which the `unsupported` pal doesn't provide yet. On top of that, thread execution
shares the OS-wide **x18 exposure** (same root cause as `time`), so threads aren't
*solid* until the `-ffixed-x18` rebuild (TODO.md). To wire it up once the sync core
lands: re-add `target_os = "aros"` arms in `sys/thread/mod.rs` and the `key`
`cfg_select` (and drop aros from the `no_threads` TLS arm), then add `-lpthread` +
`aros_thread_glue.o` to the std build scripts.

---

*The pal lives in the clone (tree 1), under `library/std/src/sys/*/aros.rs`. This repo
holds the rig that builds and proves it. Nothing Rust-side is pushed until the
maintainer decides.*
