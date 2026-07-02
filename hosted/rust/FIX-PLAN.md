# rust-aros fix plan — std pal review findings + the 68k track

> Written 2026-07-02 after a file-by-file correctness review of the std pal
> (`rust-aros/library/std/src/sys/**/aros.rs`, 13 files) and the C glue here
> (`hosted/rust/aros_*_glue.c`), every finding verified against the AROS headers
> in `../aros-upstream`. Status doc: [STD-PORT.md](STD-PORT.md). The Rust-on-68k
> track lives in [../jit68k/rust68k/](../jit68k/rust68k/README.md).

## 1. Fixes ALREADY APPLIED (in-tree, need a rebuild + test pass to verify)

Applied to the working trees (this repo + `/Users/user/Source/rust-aros`), not yet
compiled: verify with `./x check library/std --target aarch64-unknown-aros` in
rust-aros, then rebuild std and rerun the RS harnesses on the booted AROS.
Reminder for any rebuild: the OS/world is built `-ffixed-x18` and the target JSON
reserves x18 (Apple Silicon platform register) — keep both.

| id | severity | what was wrong | fix applied |
|---|---|---|---|
| H1 | High | condvar timed-wait deadline overflowed 32-bit AROS `time_t` for large timeouts (e.g. `recv_timeout(Duration::MAX)`), pthread returned ETIMEDOUT instantly, callers busy-spun at 100% CPU | clamp the deadline to `time_t` range in `aros_sync_glue.c:aros_cond_timedwait` |
| H2 | High | `OpenOptions` read+append mapped to `O_WRONLY\|O_APPEND` (reads then fail); `create`/`create_new` without write, and `truncate`+`append`, were not rejected as std documents | `fs/aros.rs:flags()` now mirrors the unix pal's access/creation-mode rules, returns EINVAL on the invalid combos |
| M3 | Medium | `SystemTagList`'s -1 ("could not run at all") became a normal exit status; a capture temp-file that failed to open silently ran the child with inherited streams | both `spawn` and `output` in `process/aros.rs` return `Err(NotFound)` on -1; `aros_process_glue.c` now fails with -1 when a requested redirection cannot be set up |
| M6 | Medium | net glue passed `usize` lengths into bsdsocket LVOs that take a 32-bit `int`; >= 2 GiB buffers went negative/wrapped | clamp to `INT_MAX` in `aros_net_glue.c` send/recv/sendto/recvfrom (std's contract is a short read/write) |
| M7 | Medium | TLS `create(None)` transmuted `None` into a non-nullable `extern "C" fn` (language UB) | `thread_local/key/aros.rs` declares the destructor param as `Option<fn>` (null-pointer-optimized, like the libc binding) |
| L2 | Low | sleeps > ~2.7 years truncated mod 2^32 ticks (`Delay` takes a 32-bit ULONG) and woke years early | chunk the tick count in `aros_thread_glue.c:aros_thr_sleep` |
| L5 | Low | `decode_error_kind` missed ENOTDIR/EISDIR/ENOSPC/EROFS | added the NetBSD-numbered mappings in `io/error/aros.rs` |

## 2. Fixes PLANNED (need a design decision or live testing; not applied)

Ordered by suggested priority.

1. **M2 — `env::var` use-after-free (safe-code UB).** posixc `getenv` returns a
   per-name cache all threads share (one PosixCBase); two concurrent `env::var`
   calls can realloc the buffer under each other. Fix: a static env `Mutex` in
   `env/aros.rs` held across `getenv` + the copy-out (and across `setenv`/
   `unsetenv`), like std's unix env lock.
2. **M1 — process-global errno.** `__stdc_geterrnoptr` resolves to one `_errno`
   for the whole process (stdc is peropener, opened once; pthread tasks share it),
   so `Error::last_os_error()` races across threads. Pal-side locking cannot fix
   this (the write happens inside the C library). Real fix is OS-side: per-task
   errno in stdc/posixc (pertask storage). Until then, document the limitation in
   STD-PORT.md and keep pal code reading errno immediately after each failing call.
3. **M4 — `Command::output()` temp-name collisions.** The uniquifier is a
   per-process counter and every process is pid 1. Mix in entropy: `DateStamp()`
   ticks + the task pointer (or stdc `arc4random`), e.g.
   `MacRW:rustproc-<task>-<ticks>-<n>.out`.
4. **M5 — non-UTF-8 command arguments are mangled.** `quote_into` goes through
   `to_string_lossy()`; Latin-1 filenames become U+FFFD and the child sees a
   different line. Build the line as bytes (`Vec<u8>` from `OsStr::as_encoded_bytes`),
   quote at byte level, reject only interior NUL.
5. **L4 — `readdir` cannot report mid-iteration errors.** Add the errno protocol
   to `aros_fs_glue.c`: set errno 0 before `readdir`, on NULL distinguish end
   (errno still 0) from error; return -1 with errno preserved and surface
   `Some(Err(..))` in `fs/aros.rs`.
6. **L3 — `temp_dir()` returns `T:`** which `process/aros.rs` deliberately avoids
   (unassigned on a minimal boot -> blocking requester). Decide: return `MacRW:`
   like the process pal, or probe `T:` first. One decision, two lines.
7. **L1 — `aros_net_open` check-then-set race** on first concurrent socket
   creation (leaks a library reference). Guard with pthread_once or a mutex in
   the glue.
8. **NEW (found by RS3d-3, 2026-07-02) — the AROS shell returns rc 0 for an
   unknown command**, so `Command::output("NoSuchCmd")` looks like a clean
   success (`code=Some(0)`); a shell backend should surface rc >= 10 there
   (AmigaOS semantics) or fail the line. OS-side: the Shell's unknown-command
   error path in `../aros-upstream` (not a pal bug — SystemTagList returned 0,
   not -1). Until fixed, callers cannot distinguish "ran fine" from "no such
   command".

## 3. Verification checklist (after the port rebuild finishes)

1. `./x check library/std --target aarch64-unknown-aros` (syntax/type gate for
   the applied pal fixes; nothing here changes codegen).
2. Rebuild std, relink the RS harnesses, rerun them on the booted AROS
   (markers per [STD-PORT.md](STD-PORT.md)).
3. ~~New micro-tests worth adding~~ **DONE (2026-07-02): the `[RS3d]` block in
   `std-probe/src/lib.rs`**, verified on booted AROS: open-mode matrix all-true,
   `Condvar::wait_timeout(Duration::MAX)` woke by notify after 200ms with no
   spin, and the missing-command test surfaced finding §2.8 (shell rc 0).

## 4. The 68k track (separate deliverable)

- DONE: `no_std` core+alloc Rust runs as m68k hunk executables through the JIT,
  two-engine verified — corpus, pipeline, toolchain notes:
  [../jit68k/rust68k/README.md](../jit68k/rust68k/README.md)
  (`make hosted-jit68k-rust`).
- NEXT (blocking for anything bigger): file the upstream LLVM M68k CCR bug —
  draft ready in
  [../jit68k/rust68k/UPSTREAM-LLVM-CCR-BUG.md](../jit68k/rust68k/UPSTREAM-LLVM-CCR-BUG.md).
  Until it is fixed upstream, every regenerated 68k binary must re-pass the
  two-engine regression; treat the backend as untrusted.
- LATER: a `m68k-unknown-aros` target + std is gated on (a) a trustworthy backend
  and (b) a real m68k AROS library world — either the AROS-side JIT integration
  (INTERFACE.md rows A–F + the genmodule marshal tables) or a built m68k AROS
  userland under emulation. The pal itself is retargetable: it is OS-level, the
  glue is CPU-agnostic, and the Rust tree already carries m68k alignment/unwind
  cfgs. Do not start this before the backend is fixed upstream.
