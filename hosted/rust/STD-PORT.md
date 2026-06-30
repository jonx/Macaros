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
- **Next (RS3c):** the remaining pal modules — real errno, `time`, `env`/`args`,
  `fs`, `thread`, then `std::net::TcpStream` proper. See [Resume map](#resume-map).

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

In the clone, `library/std/src/sys/`. We gave a real `aros` arm to **5** of ~23:

| Module | Status | How |
|---|---|---|
| `alloc/aros.rs` | done | `posixc` `malloc` / `posix_memalign` |
| `stdio/aros.rs` | done | `posixc` `write`/`read` on fd 0/1/2 |
| `random/aros.rs` | done (**weak**) | SplitMix64 from an ASLR seed — TODO real CSPRNG |
| `io/error` → `generic` | done (**stub**) | errno=0 — TODO real `posixc` errno |
| `thread_local` → `no_threads` | done | single-thread statics — switch to pthread keys for `std::thread` |
| `time`, `thread`, `fs`, `net`, `process`, `env`, `args`, `os_str`, `pipe`, `fd`, … | **stubs** | fall through to `unsupported` — these are the remaining work |

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

**Done:** `reserve-x18`; **net** (a Rust TCP round-trip over the bsdsocket bridge,
`aros_net_glue.c` + `net-build.sh` → `C:RustNet`; needs `bsdsocket.library` built via
`make workbench-libs-bsdsocket` and a TCP server on the **host**, since the bridge is
host-passthrough so AROS's `127.0.0.1` is the Mac's loopback); **real errno**
(`sys/io/error/aros.rs`, NetBSD-numbered, matches the net errno 61); and **env reads**
(`sys/env/aros.rs` getenv, verified).

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

1. **`args`**: program args from the AROS startup (needs the C `main`'s argc/argv
   threaded to Rust; `env` reads already work).
2. **`fs`**: `sys/fs` over `posixc` `open`/`read`/`write`/`stat` (or `dos`) — read a
   `MacRW:` file from Rust.
3. **`thread`** + switch `thread_local` to pthread keys (`posixc` `pthread_*`). The
   `std::env` global lock also leans on the sync pal, so this unblocks env writes too.
4. **`std::net::TcpStream` proper**: wire `sys/net/aros.rs` onto the same bsdsocket
   calls the glue uses, so `std::net` works (the current test drives the bridge via
   the glue, not `std::net` itself yet).
7. **Toward a real PR**: real CSPRNG, `libc`-crate AROS support so the pal drops its
   private `extern "C"` decls, and upstreaming the target spec.

---

*The 194-line port lives in the clone (tree 1). This repo holds the rig that builds
and proves it. Nothing Rust-side is pushed until the maintainer decides.*
