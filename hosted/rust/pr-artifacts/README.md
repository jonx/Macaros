# rust-lang PR artifacts (review-ready, not built here)

Prepared pieces for the eventual tier-3 `aarch64-unknown-aros` upstreaming. They are
**not** compiled in this repo: our verified builds use the JSON target spec +
`-Zjson-target-spec` against a `rust-src` (library-only) clone, which has no
`compiler/` or `libc` tree to compile these against. Each file names its exact
upstream destination and the mechanical confirmation step.

| File | Upstream destination | What it is |
|---|---|---|
| [aarch64_unknown_aros.rs](aarch64_unknown_aros.rs) | rust-lang/rust `compiler/rustc_target/src/spec/targets/aarch64_unknown_aros.rs` | The JSON spec as a built-in `Target`. |
| [libc-aros-mod.rs](libc-aros-mod.rs) | rust-lang/libc `src/aros/mod.rs` | The `libc` crate `aros` module (types + fn decls) the pal would migrate onto. |

## What a first rust-lang/rust PR contains

- **pal (done, verified live, in the clone `../rust-aros`):** `alloc`, `stdio`,
  `io/error`, `env` (read+write+**enumeration**), `args`, `fs` (files + metadata + dirs
  + **chmod/symlink/readlink/utimes**), `net` (IPv4 + **try_clone**), `process`
  (output/status + **per-command env+cwd**), `time`, `thread` + full sync core
  (`Mutex`/`Condvar`/`RwLock`/`Parker`) + pthread-key TLS, `random`. This is the bulk.
- **built-in target spec:** drop in `aarch64_unknown_aros.rs`, then add
  `("aarch64-unknown-aros", aarch64_unknown_aros),` to the `supported_targets! { ... }`
  macro in `compiler/rustc_target/src/spec/mod.rs` (alphabetical). Add a
  `src/doc/rustc/src/platform-support/aarch64-unknown-aros.md` page (tier-3 template:
  target maintainer, requirements, the known gaps below, build/run notes pointing at
  this repo's rig).
- **`libc` support (separate rust-lang/libc PR):** add `src/aros/mod.rs`, gate it in
  `src/lib.rs`, then a follow-up std PR migrates the pal's hand-written
  `unsafe extern "C"` blocks to `use libc::*` (1:1; the module covers exactly the
  symbols the pal declares). The bsdsocket LVO surface stays behind
  `aros_net_glue.c` (library-base calling convention, not plain `extern "C"`).

## Confirming "builds clean" / byte-identical (the step not doable here)

1. `rustc +nightly-2026-06-27 -vV` -> `commit-hash`. Check out rust-lang/rust at that
   commit (the FULL tree, not the `rust-src` subset).
2. Drop in `aarch64_unknown_aros.rs` + the `mod.rs` line; `./x build`.
3. Build the std-probe with `--target aarch64-unknown-aros` (built-in) and again from
   `aarch64-unknown-aros.json`; the two object sets must be byte-identical (the JSON is
   the spec). Reconcile any `crate::spec` helper-name drift flagged by `./x build`.

## Known gaps to disclose in the PR (tier-3 targets ship with these)

- **`-ffixed-x18` OS-build requirement.** The hosted OS/world must be built
  `-ffixed-x18` (Apple Silicon zeroes x18 on signal delivery; AROS-hosted preempts via
  SIGALRM). Else `time`/threads SIGBUS. The target reserves x18 (`+reserve-x18`) so
  Rust codegen is safe; the OS side is a build-flag requirement, not a code change.
- **`std::process`:** no live bidirectional pipes / async child with a readable
  `Child.stdout` or writable `Child.stdin` (AROS has no fork/exec; the hosted boot
  mounts no `PIPE:` handler; `SystemTagList` is line-oriented). `output()`/`status()`
  and **per-command env + cwd** work. `env_clear()` is not fully honoured; `kill` is a
  no-op on the synchronous path.
- **`std::net`:** IPv4 only (`AF_INET6` numbering differs in the host-passthrough
  bridge); `set_nonblocking`/socket timeouts are no-ops (the library keeps host sockets
  `O_NONBLOCK` and emulates blocking with a timer-poll park; making them effective needs
  a `WaitSelect`-gated glue whose `fd_set` ABI must match the library exactly).
  `try_clone` works (`Dup2Socket`).
- **`std::fs`:** `File::set_times` and `set_times_nofollow` are `Unsupported` (posixc
  has no `futimes`/`lutimes`); path-based `std::fs::set_times` works (`utimes`).
  `symlink`/`readlink` work; `FileType::is_symlink()` can read false on the host-backed
  emul-handler (its `lstat` does not set `S_IFLNK`). `link`/`canonicalize`/`copy`/
  `remove_dir_all` are stubs.
- **`SystemTime::now()` (REALTIME)** reads ~1978 on hosted: the RTC is not host-synced
  (un-host-synced clock, UPSTREAM-NOTES #36). Monotonic `Instant` is correct.
- **errno is process-global** (`__stdc_geterrnoptr` resolves to one `_errno`); the pal
  reads it immediately after each failing call. Per-task errno is an AROS-side fix
  (FIX-PLAN M1).
- **CSPRNG:** `arc4random_buf` borrows the host CSPRNG on hosted; a native AROS entropy
  source (`entropy.resource`) is the remaining AROS-side item.
</content>
