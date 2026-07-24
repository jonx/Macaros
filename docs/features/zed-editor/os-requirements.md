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

Still remaining on item 2: only the Rust `std::process` glue that connects a
child's stdio to `PIPE:` endpoints (and uses `NP_NotifyOnDeath` + `SIGF_CHILD`
for async reaping) — sequenced with the socket `set_nonblocking` work as one
coordinated std pass, sockets first.

---

## 3. Pseudo-terminal (PTY) for the integrated terminal

**Unblocks:** the integrated terminal panel (the shell-in-the-editor pane).

**Current state.** No PTY. The terminal is stubbed on our side (the terminal
backend's tty layer returns *unsupported* on AROS), so the editor builds and
runs without it.

**Contract.**
1. **A master/slave terminal pair.** A shell spawned on the slave end behaves as
   if it has a controlling terminal: interactive line editing and job-control
   behaviour work.
2. **Master endpoint** is a byte stream the editor reads (shell output) and
   writes (keystrokes), integrating with item 1's readiness.
3. **Resize.** A way to set the terminal size (rows × columns) such that the
   child is notified of the change (the `SIGWINCH`-equivalent), so full-screen
   programs reflow.

**How it surfaces to us.** It replaces the aros stub in the terminal backend's
tty layer: an open-pty equivalent, spawn-shell-on-slave, master read/write, and
set-size. We wire the terminal crate to it.

**Acceptance.** Run a shell in the pty; type a command and see its output; run a
full-screen program (e.g. an editor or pager) and have it use the whole area;
resize and have the program reflow to the new size.

**Note.** Highest effort and most terminal-specific; conceptually depends on the
pipe/child-exit work in item 2 and the readiness primitive in item 1.

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

### LSP application layer (2026-07-24) — connects, one bug left

The TCP LSP transport is built and the editor **connects to a real
rust-analyzer** over the host bridge:

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
- **Verified:** opening `MacRW:proj/src/main.rs` connects to the bridge and
  rust-analyzer starts (`[lsp-bridge] client … connected; starting server`).

**Open bug (blocks diagnostics/completions):** the instant rust-analyzer
replies, two gpui worker threads abort in `std::alloc::rust_oom`. It is **not**
memory exhaustion (reproduces with a 140 MB stripped binary and >1 GB free
heap): writes succeed (the server got our `initialize`), but the reader appears
to misframe the streamed reply — a garbage `Content-Length` → a multi-GB
allocation → abort. Suspect the socket **read path under streamed/partial
reads** (`aros_np_recv` / reactor readiness), distinct from the single-shot
`--nettest`. This is the next thing to chase; the transport and path mapping
above are proven.

Aside: the `-Zbuild-std` link emits ~1 GB of `.debug_*` sections that AROS
loads into RAM; `hosted/zed/build.sh` now `--strip-debug`s the binary
(~1 GB → ~140 MB).

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
