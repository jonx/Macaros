# Memory protection / multi-instance — implementation spec (POC-first)

> Status: spec (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-29
> Governed by the independent-work process in [../CLEANROOM.md](../CLEANROOM.md): written
> from public APIs (Apple/POSIX VM + threads), published structure layouts, this project's
> `aros-upstream` tree, and this repo's own spikes — never from third-party MP/RT/VM source.
> Design rationale: [design.md](design.md).

This spec scopes the **POCs in `hosted/`** that the design's two buildable routes rest on.
Each is one greppable spike with a single **PASS/FAIL** line, in the unattended-loop style
(`build → run → observe → one verdict`, no TCC / Screen-Recording prompt). A fresh
implementer should be able to build each from this file alone.

Two POC tracks, independent, either order:

- **POC-A** — intra-AROS diagnostic protection (`[MP1]`–`[MP3]`). Smallest diff; proves the
  headline "memory protection" mechanism.
- **POC-C** — multi-instance isolation (`[MI1]`–`[MI2]`). Proves the process-per-app
  architecture; reuses two already-built host channels.

---

## POC-A — intra-AROS diagnostic protection

**Goal:** prove that a `PROT_NONE` host page placed by the AROS allocator/scheduler turns a
class of silent corruption into an **immediate, attributed** fault, using only primitives the
port already has.

**Where it lives:** the bring-up harness in this repo — it already `mmap`s its own AROS RAM
([hosted/mem.c:208](../../../hosted/mem.c)) and per-task stacks
([hosted/exec.c:127](../../../hosted/exec.c), [hosted/execboot.c](../../../hosted/execboot.c)), and already
installs a SIGSEGV/SIGBUS path. The POC adds guard pages + a fault decoder there, so it proves
the mechanism **without** rebuilding the full OS tree. The real-OS landing (kernel/exec) is a
follow-up, not part of the POC.

### Contracts this uses (with provenance)

- **`mmap`/`mprotect(PROT_NONE)`** — POSIX; on Darwin/aarch64 the port already calls
  `mprotect` via `KrnSetProtection`
  ([setprotection.c:15-44](../../../../aros-upstream/arch/all-unix/kernel/setprotection.c)).
  The POC may call `mprotect` directly in the harness.
- **Fault → decode.** The fault address arrives in the host `ucontext`/`siginfo`; the upstream
  handler already decodes it and walks a frame chain
  ([kernel.c:111-266](../../../../aros-upstream/arch/all-unix/kernel/kernel.c)). The POC's
  harness handler must record: faulting address, which guard region it hit, the running task,
  and a backtrace. *Do not* call async-signal-unsafe functions in the handler (no `OpenLibrary`,
  no allocation) — mirror crash-handling's read-only-list discipline
  ([crash-handling](../crash-handling/design.md)).
- **Task stacks are `mmap`'d** ([hosted/exec.c:127](../../../hosted/exec.c)) — guard pages attach by
  reserving one extra page below (overflow) the stack and `mprotect(PROT_NONE)`-ing it.

### Spikes

- **`[MP1]` Stack-overflow guard.** Allocate a task stack as `[GUARD page | stack]`, guard =
  `PROT_NONE`. A test task recurses/writes past its stack bottom.
  **PASS:** the harness prints `[MP1] PASS guard hit: task=<name> addr=<a> (stack guard) — overflow caught`
  and exits cleanly; **FAIL:** silent corruption or an unattributed crash.
- **`[MP2]` NULL-page trap.** Map page 0 `PROT_NONE`. A test task writes through a NULL pointer.
  **PASS:** `[MP2] PASS null-write trapped: addr=0x0 caller=<lr>`; **FAIL:** the write
  succeeds (write went to mapped low memory) or is unattributed.
- **`[MP3]` Read-only-structure write.** `mprotect(PROT_READ)` a region standing in for
  `ExecBase`; a test task writes to it. **PASS:** `[MP3] PASS RO-write trapped: addr=<a> caller=<lr>`;
  **FAIL:** the write silently succeeds.

Each spike must also confirm the **negative control**: with the guard *disabled*, the same
access does *not* fault (proving the guard, not an unrelated bug, is what fired) — print
`[MPn] (control) no-guard: access completed` on that path.

### Out of scope for POC-A

Quarantine/UAF (`[MP4]`), panic-save (`[MP5]`), and the real-OS landing (read-only ExecBase in
the live kernel, opt-in RT) — all follow once the mechanism is proven here.

---

## POC-C — multi-instance isolation

**Goal:** prove that running two full AROS instances as separate host processes gives
kernel-enforced isolation and a shared filesystem, with copy/paste across them — i.e. the
process-per-app robustness model — reusing already-built host channels.

**Where it lives:** a host-side launcher + test driver in `hosted/` (alongside the existing
[control harness](../control-harness/README.md), which already boots and drives a windowed
instance and can spawn/headless-boot AROS).

### Contracts this uses (with provenance)

- **An AROS instance = one Darwin process.** The harness already cold-boots AROS headless
  (`arguments econsole nomonitors`, SIGKILL watchdog — see
  [crash-handling](../crash-handling/design.md) "Verifying"); two of them = two processes,
  isolation enforced by macOS.
- **Shared filesystem** — both instances mount the same host volume:
  **built**, [host-volume](../host-volume/design.md).
- **Shared clipboard** — host `NSPasteboard` bridge: **built**,
  [clipboard-bridge](../clipboard-bridge/design.md) (host→AROS verified byte-exact — see the
  memory note *Clipboard bridge*).

### Spikes

- **`[MI1]` Crash containment + shared FS.** Launch instance **A** and instance **B**, both
  mounting host volume `V:`. A writes a known payload to `V:poc.dat` and `flush`es; then A is
  hard-killed (`SIGKILL`) mid-session. B reads `V:poc.dat` and checksums it.
  **PASS:** `[MI1] PASS B alive after A killed; V:poc.dat checksum OK (<sum>)`; **FAIL:** B
  dies/hangs, or the file is absent/corrupt. (Proves: A's death cannot touch B, and the host
  FS is the shared, surviving substrate — i.e. RT-by-host-reclamation.)
- **`[MI2]` Cross-instance copy/paste.** A copies a known string to the clipboard; B reads the
  clipboard. **PASS:** `[MI2] PASS B pasted "<s>" copied by A` (byte-exact); **FAIL:** mismatch
  or empty. (Proves: cooperating apps *can* still share data across instances — through host
  channels, not AROS pointers.)

### Explicitly out of scope for POC-C

Shared public screen / host compositor, and message-port / ARexx / AppWindow bridging across
instances (the "tight cooperation" cases). MVP display model is **one top-level window per
instance**. ARexx-over-arena bridging is **Route B**, specced separately as the `[B0]`–`[B3]`
ladder in [design.md](design.md#route-b--shared-public-arena-single-system-image-cooperation-b)
— it depends on this POC-C launcher and on verifying the deployed `rexxsyslib.library` works on
aarch64 (`[B0]`); it does **not** need a Rexx interpreter (none ships on this port).

---

## Build & run

- **POC-A**: extend the harness's existing build (`hosted/` mmake/clang spike target, same
  pattern as `hosted/jit68k/j1_test.c` / `hosted/printing` — a standalone `*_test.c` with a
  `main` that runs the spikes and prints the PASS/FAIL lines). Wire into the in-tree spike
  runner the way `[J1]`/`[CP*]` are.
- **POC-C**: a launcher script/binary under `hosted/` that boots two instances headless via
  the control-harness path, drives them, and asserts the `[MI*]` lines; run under the existing
  SIGKILL watchdog so the loop is bounded.

Greppable markers `[MP1]`/`[MP2]`/`[MP3]`/`[MI1]`/`[MI2]` so the runner can scrape one verdict
each, exactly like `[H*]`/`[J*]`/`[D*]`/`[CP*]` elsewhere in the tree.

## Acceptance

POC-A passes when `[MP1]`–`[MP3]` each print PASS **and** their no-guard control path shows the
access completing (the guard is proven causal). POC-C passes when `[MI1]`–`[MI2]` print PASS.
Neither POC requires a GUI session or any macOS permission prompt.

## Provenance

Independent work under [../CLEANROOM.md](../CLEANROOM.md). Built from POSIX/Apple VM + thread
APIs, published AROS structure layouts, this project's `aros-upstream` tree and `hosted/`
spikes. No third-party memory-protection, resource-tracking, virtual-memory, or
multi-instance/sandbox implementation source was read or consulted; any resemblance is
coincidental.
