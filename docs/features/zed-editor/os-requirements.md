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

**Current state.** Blocking `std::net` TCP/UDP works today over the bsdsocket
bridge (an LSP `initialize` handshake round-trips in ~50 ms). What is missing:
`FIONBIO` is a no-op, socket read/write timeouts are no-ops, and there is no
readiness primitive — so no async runtime can drive sockets, only one blocking
call at a time on a dedicated thread.

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

## Smaller items (already on your list)

- **Per-task errno.** Threads must not clobber each other's errno; `rustix` and
  parts of std assume per-task errno. Needed for correctness once the async
  stack runs multi-threaded.
- **Host-synced RTC.** So file timestamps and log times are correct on the
  hosted boot (currently drifts / defaults).

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
