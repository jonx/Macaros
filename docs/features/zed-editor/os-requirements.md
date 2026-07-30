# OS capabilities the editor needs from AROS

A handoff from the editor/Rust side to the AROS OS side. Each item is a
**capability contract** — what the editor layer needs to be true, and how we
verify it — not an API prescription. Implement each the AROS-idiomatic way;
where a POSIX name appears it only names the behaviour, not the mechanism.

This formalises the direction your core devs already gave (mount `PIPE:`,
`WaitSelect` + real `FIONBIO`, per-task errno, host-synced RTC). It is ordered
by leverage: item 1 unblocks the most, item 3 is the most specialised.

None of these block the editor's **core editing** path, which already runs
(open/edit/save real files on AROS, see [README](README.md)). They unblock the
surrounding IDE: networking, live tools, and the terminal.

Everything here also benefits the other Rust-on-AROS work (aros-editor,
Feraille, the std port) — the demand is general, only the contracts below are
phrased from the editor's needs. Cross-reference the std-side view in
[hosted/rust/STD-PORT.md](../../../hosted/rust/STD-PORT.md).

---

## 1. Non-blocking sockets + readiness notification (the async reactor)

**Unblocks:** the HTTP client, networked and high-throughput LSP, the AI/agent
panel — and structurally the entire `tokio` / `mio` / `async-io` stack, which is
pulled in by most of Zed's networked features.

**Current state (updated 2026-07-24).** Blocking `std::net` TCP/UDP works over the
bsdsocket bridge (an LSP `initialize` handshake round-trips in ~50 ms). The
non-blocking path now works too:
- **`FIONBIO` is real** (2026-07-24). A socket put in non-blocking mode returns
  `EWOULDBLOCK` (`EINPROGRESS` for connect) immediately instead of parking, so an
  async reactor can drive it. Default sockets keep blocking behaviour. Verified
  live (`nettest` `[NB]`: FIONBIO connect -> write-ready -> recv EWOULDBLOCK).
- **Readiness wait already exists**: `WaitSelect` (LVO 21) over the kqueue pump,
  and the out-of-band wakeup is expressible today by passing a signal in its
  sigmask and `Signal()`-ing the reactor task (`notify()`).
Still missing: socket read/write **timeouts** are no-ops (the blocking park uses
an infinite wait), and the readiness wakeup is a ~20 ms poll tick rather than
signal-driven (ms-latency via unixio.hidd is the follow-up).

**Contract.**
1. **Real non-blocking mode.** Putting a socket into non-blocking mode (however
   AROS spells it) must make an operation that would block return a distinct
   *would-block* status immediately, rather than parking the task.
2. **Readiness wait.** A primitive that, given a set of sockets, blocks until at
   least one is readable/writable **or** a timeout elapses, **and** can be
   interrupted by an out-of-band wakeup so another task can unblock the reactor
   on demand. `WaitSelect` + `SetSocketSignals` over an exec signal is the
   natural AROS shape; the wakeup is the important part (a self-pipe/self-signal
   the reactor also waits on).
3. **Timeouts** on blocking read/write, or the readiness primitive is enough to
   synthesise them on our side.

**How it surfaces to us.** This backs the vendored `polling`-crate AROS backend
(its socket-readiness "Phase B"), and through it `async-io` → `mio` → `tokio`.
We do the Rust plumbing; we need the three behaviours above underneath.

**Acceptance.** A Rust program using `async-io` marks a TCP socket non-blocking,
registers it, and its read future completes when data arrives (within a few ms
of arrival, not on a fixed poll tick); a concurrent `notify()` from another
thread wakes the reactor promptly with no socket ready.

---

## 2. Live bidirectional child-process pipes + child-exit notification

**Unblocks:** running a language server or build tool **directly on AROS** over
stdio (today we bounce LSP through a host-side TCP bridge), streamed task/tool
output, and it is a prerequisite for item 3 (the terminal).

**Current state.** `std::process` `output()` / `status()` work by redirecting
the child through temp files — correct for "run to completion, then read", but
there is no *live* streaming: you cannot read a child's stdout while it is still
running, nor write to its stdin interactively. `async-process` is routed through
a wait-thread reaper as a result. The `PIPE:` handler exists in the tree
(`workbench/fs/pipe`) but is not mounted on the hosted boot, and there is no
child-exit signal (the reaper polls).

**Contract.**
1. **Streaming pipes.** Spawn a child with its stdin/stdout/stderr connected to
   endpoints the parent can read/write **incrementally**, without blocking the
   whole process, and integrating with item 1's readiness (so a reactor can wait
   on a child pipe and a socket together).
2. **Child-exit notification.** The parent learns a child has exited via an exec
   signal (not a busy-poll), and can retrieve the exit status.
3. Mount `PIPE:` (or the idiomatic equivalent) on the hosted boot so the above
   is available out of the box.

**How it surfaces to us.** `std::process::{Child, Stdio::piped}`, `async-process`,
and the LSP stdio transport. Once this exists, the LSP host-TCP bridge becomes
optional — the server can run on AROS itself.

**Acceptance.** Spawn a child; write a line to its stdin; read a line of its
stdout **while it is still running** (before exit); receive an exit signal and
read the status. A reactor waiting on both a child pipe and a socket wakes for
whichever is ready first.

**Delivered (2026-07-24) — the pipe readiness + non-blocking read primitives.**
`PIPE:` mounts on aarch64; readiness→signal is level-triggered and composes with
`WaitSelect`; non-blocking reads return would-block on empty; blocking reads now
return available bytes (POSIX stream semantics, fixes a block-until-full hang).
Verified live. The contract the Rust std wires to (all on the pipe filehandle's
`fh_Type` port, `fh_Arg1` as the key):

- `set_nonblocking(pipe)` → `DoPkt(fh_Type, ACTION_PIPE_SET_NONBLOCK /*0x50534E42*/, fh_Arg1, enable, 0)`
- register readiness → `DoPkt(fh_Type, ACTION_PIPE_READ_NOTIFY /*0x50524E31*/, fh_Arg1, sigmask, task)` — signals `task` whenever readable, and immediately if already readable.
- read → normal `Read()`; on a non-blocking empty pipe it returns `-1` with `IoErr() == ERROR_PIPE_WOULD_BLOCK /*0x50574F42*/` → map to `WouldBlock`.

`PIPE:` now mounts on every boot (2026-07-24, console + desktop + release
recipes).

**Child-exit signal — already provided by the OS (verified by existing use).**
No new OS work was needed: a child created with `NP_NotifyOnDeath = TRUE`
signals its parent's `SIGF_CHILD` on exit (exec delivers the child's ETask to
the parent's `et_TaskMsgPort`, whose signal bit is `SIGB_CHILD`). `SIGF_CHILD`
is a fixed exec signal, so it drops straight into `WaitSelect`'s sigmask beside
socket and pipe readiness — the reactor waits on all three in one call. To reap:
`ChildStatus(tid)` polls (`CHILD_EXITED`), `ChildWait(tid)` returns the ETask,
`et->et_Result1` is the exit code, then `ChildFree(et->et_UniqueID)`. This is the
same path posixc `wait()`/`waitpid()` and Rust `std::process::status()` already
use on hosted aarch64, so it is proven working; the reactor just consumes it.

**Item 2 is DONE (2026-07-25).** The Rust side now streams.
`hosted/rust/aros_proc_glue.c` spawns with each stream on a `PIPE:` endpoint,
and `std::process` gained a real `Stdio::piped()`, a `wait()` that blocks on the
child's exit signal, and an `output()` on the same path (the temp-file capture is
gone). Verified live by two reproducers: **`C:ProcProbe`** (pure C, the OS
contract) and **`C:RustProc`** (`RUST-AROS: PROC PASS`, the std API). Both read a
child's stdout while it is still running, round-trip a line through its stdin,
and get the exact exit code back.

Two things differ from what was planned here, both found by building it:

- **No `NP_NotifyOnDeath`/`SIGF_CHILD` needed.** `SystemTagList` passes
  `NP_ExitCode`/`NP_ExitData` through to `CreateNewProc` unfiltered, so the exit
  hook runs in the dying child with its return code and signals the parent
  directly. Simpler, and no trampoline process.
- **The exit code needs `cli_ReturnCode`.** AROS's Shell ends with
  `return error ? RETURN_FAIL : RETURN_OK`, so every failure reaches the parent
  as 20. The command's real code survives in `cli->cli_ReturnCode`, which
  `pr_CLI` still points at when the exit hook runs.

Remaining gaps on the Rust side: `Child::kill()` (AROS has no safe way to tear
down a running Process) and duplicating a pipe endpoint (the handler allows one
reader plus one writer).

---

## 3. Pseudo-terminal (PTY) for the integrated terminal

**Unblocks:** full-screen terminal programs. **A working terminal panel no
longer needs this** (2026-07-26): a shell runs, is typed at, and answers, over
the pipes from item 2.

**Current state.** Still no PTY, and the terminal panel works anyway. AROS does
not have a terminal device to give a child, but it does have the two pieces that
matter: a child whose stdio is on live `PIPE:` endpoints, and a shell that can
be asked to keep reading its input. Two things a tty's line discipline would
have done are done in the tty layer instead: what is typed is echoed back, and
newlines are translated in both directions (`\r` to `\n` towards the shell,
`\n` to `\r\n` towards the screen).

Asking AROS for the right kind of shell is the load-bearing part.
`SystemTagList` starts a shell that runs one command line and exits, which is
what running a command means and is not a terminal; `SYS_Background = FALSE`
starts a new CLI that goes on reading its input, which is. See
[STD-PORT.md](../../../hosted/rust/STD-PORT.md); `C:RustShell` checks it.

**What is still missing without a PTY.**
1. **No terminal size.** The child is never told how big the window is, and
   cannot be notified when it changes, so `on_resize` is inert and full-screen
   programs have nothing to reflow to.
2. **No line discipline in the OS.** No job control, and no interrupt key: a
   running command cannot be stopped from the terminal.
3. **No terminal size for the shell either**, so `Prompt` cannot show anything
   width-dependent and the shell cannot lay out in columns.

**Pipe buffering: fixed on the AROS side (2026-07-27).** dos line-buffers a
handle it considers interactive and fully buffers every other one, and only
con-handler ever set `fh_Interactive`. So a shell on a pipe had its output held
until the buffer filled, and printed no prompt, having concluded from its input
that nobody was there. The pipe handler now marks its filehandles as the streams
they are (`workbench/fs/pipe/pipecreate.c`), which gives the terminal both live
output and a prompt.

**File-change notification: solved from the host side (2026-07-30), so AROS
does not need `ACTION_ADD_NOTIFY` for this.** Rather than teach emul-handler to
report changes, the editor asks the host directly: one `kqueue` `EVFILT_VNODE`
watch per directory through `hostlib.resource`, drained on a 250 ms timer,
never a blocking host call (`hosted/rust/aros_fswatch_glue.c`, proven first by
`C:KqProbe`). Volume-to-host-path mapping arrives in `AROS_FSW_ROOTS` from the
boot harness; unmapped volumes fall back to polling.

An `ACTION_ADD_NOTIFY` in emul-handler would still be the better answer for
AROS programs generally -- it would serve everything, not just a program that
can reach the host -- but it is no longer on this port's critical path.

**Interrupt: done without the PTY (2026-07-30).** The break convention covers
it: the terminal consumes ^C and signals SIGBREAKF_CTRL_C to the shell's
process, found by CLI number -- `SYS_CliNumPtr`, which dostags.h had promised
and dos never implemented until now (fork commit, upstream candidate). Proven
by `C:BrkProbe`, live in the editor's terminal. `Child::kill` in the Rust
runtime sends the same signal, so the editor's task-stop buttons work too.
What break cannot do: stop a program that never polls for it; there is no
SIGKILL to escalate to.

**Still needing the real PTY:** terminal size and its change notification
(full-screen programs), raw char-at-a-time input, and a shell that knows it is
interactive without our pipe-handler patch.

**Contract, if a PTY is ever added.**
1. **A master/slave terminal pair.** A shell spawned on the slave end behaves as
   if it has a controlling terminal: interactive line editing and job-control
   behaviour work.
2. **Master endpoint** is a byte stream the editor reads (shell output) and
   writes (keystrokes), integrating with item 1's readiness.
3. **Resize.** A way to set the terminal size (rows x columns) such that the
   child is notified of the change (the `SIGWINCH`-equivalent), so full-screen
   programs reflow.

**How it would surface to us.** It replaces the pipe-backed tty layer for AROS:
an open-pty equivalent, spawn-shell-on-slave, master read/write, and set-size.

**Acceptance.** Run a full-screen program (e.g. an editor or pager) and have it
use the whole area; resize and have it reflow.

---

## 4. Unified fd space for the async socket stack (the LSP/HTTP blocker)

**Unblocks:** `async-io` / `mio` / `tokio` driving TCP sockets — i.e. networked
LSP over the host bridge, the HTTP client, and the agent panel.

**The problem (discovered 2026-07-24).** The reactor's Phase B (WaitSelect
socket readiness) is built and compiles, and non-blocking sockets work. But
`async-io`/`socket2`/`tokio` do not go through std's AROS net pal (which drives
`bsdsocket` via the `aros_np_*` glue). They create and drive sockets as **raw
libc fds**: `socket()` → `connect()` → `fcntl(F_SETFL, O_NONBLOCK)` →
`read()`/`write()` → `close()`. On AROS that breaks two ways:
- `socket()` / `connect()` are **not libc symbols** — sockets are `bsdsocket`
  LVOs, so those references are undefined at link.
- Even if provided, AROS keeps **sockets and files in separate fd spaces**
  (`bsdsocket` closes with `CloseSocket`, dos files with `Close`), and their
  small-integer fd ranges **overlap**. posixc `read`/`write`/`close`/`fcntl`
  operate on the *file* space, so calling them on a socket fd is wrong.

So the async socket stack assumes a **unified fd space** AROS does not have.

**Two ways forward (a real fork):**
1. **Unified-fd shim.** Provide `socket()`/`connect()`/… over `bsdsocket`,
   returning fds in a high non-overlapping range, and override
   `read`/`write`/`close`/`fcntl` (link-time, `--allow-multiple-definition`) to
   dispatch socket-range fds to `bsdsocket` and the rest to posixc. Unblocks the
   **entire** async networking stack (HTTP, LSP, agent). Substantial, delicate
   C-layer work; more libc socket calls (`setsockopt`/`bind`/… ) will surface as
   the async path goes live.
2. **Blocking-`std::net` LSP transport.** Skip the async socket stack for LSP:
   connect with blocking `std::net::TcpStream` (works today via the net pal) plus
   reader/writer threads bridging into the editor's async channels — the model
   `~/Source/aros-editor` already proved live. Targeted to LSP; needs a custom
   transport in Zed's `lsp` crate (which is built around async subprocess stdio),
   not a general networking fix.

**RESOLVED (2026-07-24) — option 1, the unified-fd shim, works.**
`hosted/rust/aros_fd_shim.c` provides `socket`/`connect`/`bind`/`listen`/
`accept`/`send`/`recv`/`setsockopt`/… over `bsdsocket`, returns socket fds
**tagged** with a high bit (`0x40000000`) so they never collide with posixc file
fds, and overrides `read`/`write`/`close`/`fcntl`/`ioctl` to dispatch (tagged →
bsdsocket, else → the real posixc `__*_PosixCBase_wrapper`, since posixc's are
weak symbols). The bsdsocket errno is copied into the C errno so `EWOULDBLOCK →
WouldBlock` is seen. The std net pal glue (`aros_np_*`) and the WaitSelect glue
strip the tag, so a fd created by the shim (socket2) and later driven through
std's `TcpStream` works too. **Verified live: an `async-io` TCP round-trip to a
host echo server passed.** This unblocks the whole `tokio`/`mio`/`async-io`
stack (HTTP, networked LSP, agent).

### LSP application layer (2026-07-25) — WORKING: live diagnostics

The editor gets **live rust-analyzer diagnostics over the host bridge**: opening
`MacRW:proj/src/main.rs` underlines a genuine error and marks it in the gutter,
and the server reaches `PrimeCaches(End)` (a full analysis pass). Pieces:

- `lsp::LanguageServer::new_tcp` reaches a server over a socket instead of
  spawning a local process (shares `new_internal`, so all framing/handling is
  common). `lsp_store` takes this path on AROS (`$ZED_AROS_LSP_ADDR`, default
  `127.0.0.1:9257`); a minimal Rust `LspAdapter` registers `rust-analyzer`.
- **Path translation** was the missing piece and is done: `url`'s
  path↔file-URL conversion is `cfg(unix/windows)`-gated and **absent on AROS**,
  so `file://` URIs never built and servers silently never started. Vendored
  `lsp-types` now converts manually and maps the AROS host-shared volume
  (`MacRW:`) ↔ the real host path a host language server reads
  (`vendor-aros/lsp-types/src/uri.rs`, `util::UrlExt::to_file_path_ext`).
- **The `readlink` runaway** was the last blocker, and it was *not* in the LSP
  or socket code at all. Every worker thread aborted in `std::alloc::rust_oom`
  the moment a language server started. The chain:
  `RealFs::watch` asks `read_link` whether a watched path is a symlink →
  unix `DoReadLink` answers `-2` when the host `readlink` fills the buffer
  ([emul_host.c:1000](https://github.com/aros-development-team/AROS/blob/master/arch/all-unix/filesys/emul_handler/emul_host.c)) →
  posixc `readlink` turns `-2` into `bufsize`
  ([readlink.c:80](https://github.com/aros-development-team/AROS/blob/master/compiler/crt/posixc/readlink.c)) →
  which POSIX defines as "may be truncated, retry bigger" → the std pal doubled
  its buffer forever (256 B → 1 GB) and the allocation failed.
  Some paths answer that way unconditionally (`readlink("/")`, and the `SYS:/…`
  form Rust produces from `home_dir().join(..)`), so the loop never terminated.
  **Fixed** in the std pal by bounding the growth and failing with `InvalidData`
  past a sane symlink length (`rust-aros` `library/std/src/sys/fs/aros.rs`).
  An OS-side fix (not answering "buffer too small" for a non-symlink) would be
  the cleaner root-cause repair and is still worth doing.

Debugging notes worth keeping:

- The one-frame AROS trap backtrace is what made this expensive. Building with
  `-C force-frame-pointers=yes` (now set for the AROS target) makes the trap
  backtrace walk the full chain and name the culprit immediately.
- Socket-stack reproducers (`hosted/rust` `RustStream`, `hosted/zed` `SockProbe`)
  read real rust-analyzer output byte-exact — blocking, non-blocking, cross-task,
  multi-threaded, and through the editor's own
  `BufReader`/`read_until`/`read_exact`. They exonerated the socket layer; keep
  them for the next "is it the network?" question.
- The `-Zbuild-std` link emits ~1 GB of `.debug_*` sections that AROS loads into
  RAM; `hosted/zed/build.sh` now `--strip-debug`s the binary (~1 GB → ~140 MB).
- **Editing the std pal does not rebuild `std`** (cargo does not fingerprint the
  `rust-src` symlink) — see the gotcha in `hosted/rust/STD-PORT.md`.

---

## Smaller items (already on your list)

- **Per-task errno.** *Done (2026-07-24).* C `errno` used to live in the
  per-process stdc libbase, so threads clobbered each other's errno. It is now
  per-task: stdc consults an optional hook that pthread installs, giving each
  thread its own errno slot. Non-threaded programs are unchanged (default hook is
  off). Verified live (`developer/debug/test/misc/pthreaderrno`: two threads keep
  distinct errno across 40 interleaved yields). Note the socket errno (`Errno()`)
  was already per-task via the bsdsocket TaskBase.
- **Host-synced RTC.** *Done (2026-07-24, dev/release boot).* The boot now runs
  `SetClock LOAD`, seeding the clock from the Mac via the battclock bridge, so
  `SystemTime::now()` / file timestamps read the real date. Cleaner follow-up:
  seed `timer.device` REALTIME at hosted timer init so it is right from the first
  tick without the boot command.

- **`mkdir` leaves errno 0 on failure.** AROS `mkdir` returns -1 without setting
  `errno`, so a caller cannot tell "already exists" from "parent missing". Rust's
  `create_dir_all` needs exactly `AlreadyExists` to treat the call as a success;
  the std pal works around it by re-`stat`ing the path and the parent. A real
  errno would remove the guesswork.
- **`readlink` reports "buffer too small" for non-symlinks.** A caller that grows
  its buffer until the call fits (which is the documented POSIX idiom, and what
  Rust's `fs::read_link` did) never terminates. The pal now caps the loop at
  64 KB, but the correct answer is `EINVAL` for a path that is not a symlink.
- **`stat` fills `st_ino` from a path hash.** Fine for identity, but hard links to
  one file get different values, so inode-based "same file?" checks are wrong on
  AROS. Worth knowing before anything relies on it.

## Explicitly *not* asks for the OS team (our side)

- **wasm extensions.** Blocked by no JIT/mmap; the path is wasmtime's Pulley
  interpreter, a build-config choice on our side, not OS work. Built-in
  tree-sitter grammars already work; only *extension* grammars need wasm.
- **mmap.** We back `mmap`/`munmap` with the heap in the editor binary
  (`hosted/zed/aros_mman_stub.c`); no OS VM mmap is required for the editor.

## Input capture (mostly our side, one possible OS touchpoint)

Full keyboard capture — every modifier combination and shortcut reaching the
editor window — is mostly `gpui_aros` input-translation work on our side. The
one place it may touch the OS: the editor's Intuition window needs to receive
**all** `RAWKEY` events, including qualifier combinations and any
system-reserved shortcuts, rather than having them intercepted (e.g. by
commodities or the console) before they arrive. If some combinations are
swallowed before the active window sees them, we would need a way to opt the
focused window into raw/exclusive keyboard input. Flagging it here so it is on
the radar; we will scope it precisely once we hit it.
