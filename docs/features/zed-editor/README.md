# Zed-shaped editor on AROS

Investigation into running a Zed-shaped code editor on hosted AROS
(darwin-aarch64), building on the GPUI platform port that already runs
[Feraille](../feraille-gpui/README.md). Status and design live here; the build
rig is [hosted/zed/](../../../hosted/zed/README.md).

**State: the real `zed` binary runs.** `C:Zed` is Zed's own `main` (not a shim
crate), and it renders the actual workspace on AROS: project panel with a
recursive file tree, editor tabs, breadcrumbs, syntax highlighting, status bar,
and the outline/debugger panels. See
[The real `zed` binary](#the-real-zed-binary-2026-07-25) below.

`C:ZedAros` (the earlier `zed_aros_app` shim over editor-core) and the Apache
`gpui-component` editor remain as the two earlier milestones.

## The two candidate paths

There are two different "editors" that could run on this platform, and the
licensing (below) is the first thing that separates them.

1. **Zed's own editor crates** (`~/Source/zed-aros`): `editor` + `language` +
   `lsp` + `project` + `text`/`rope` on top of `gpui`. Maximum fidelity to real
   Zed, but GPL-3.0-or-later (see boundary below), a ~236-crate graph, and a
   single monolithic `zed` binary with no cargo-feature seam for a minimal
   build (the port spine is a custom `zed_aros_app` staticlib entry point that
   boots editor-core only).
2. **The gpui-component editor** (`~/Source/gpui-component-aros`): longbridge's
   Apache-2.0 component set, whose `ui` crate ships a code editor with
   tree-sitter highlighting and LSP-via-provider-traits. Far smaller graph,
   permissive license, already patched for AROS (`smol::channel` →
   `async-channel`). Lower fidelity to Zed, but the pragmatic path.

The compile-frontier probe (below) is the evidence that decides between them.

## Licensing boundary (read before writing any port code)

This is the load-bearing constraint. Zed is dual-licensed **per crate**, and
the split runs exactly along the line we care about.

| Group | License | Crates (what matters here) |
|---|---|---|
| GPUI platform | **Apache-2.0** | `gpui`, `gpui_aros`, `gpui_platform`, `gpui_macros`, plus `util`, `collections`, `sum_tree`, `scheduler`, `http_client` — 28 crates total |
| The editor itself | **GPL-3.0-or-later** | `editor`, `text`, `rope`, `language`, `lsp`, `project`, `multi_buffer`, `workspace`, `theme`, `ui`, `settings`, `fs`, `worktree`, `zed` — 198 crates total |

(Counts from the `zed-aros` checkout, branch `aros-platform`: 28 Apache, 198
GPL.) `gpui-component-aros` is Apache-2.0 throughout.

There are **two independent licensing axes**; keep them separate.

**Axis 1 — repo hygiene / provenance** (what source lives in which repo). AROS
practice (confirmed by AROS core devs, 2026-07-22): do **not** commit GPL source
into an AROS-official codebase; keep a ports-style **diff** applied to upstream
sources downloaded at build time (see the AROS `ports` repo). If GPL code must
be present in-tree, keep its license headers intact (e.g. AHI). This is the same
model as BSD ports / Homebrew, and it fits how we already work (rust-aros is a
diff toward an upstream PR; the zed/gpui-component forks are our changes over
pinned upstream revs).

**Axis 2 — license of the distributed binary** (copyright of the linked result).
Independent of Axis 1. Using or privately compiling GPL software is
**unrestricted** — GPL obligations attach only on **distribution** of a binary.
Static-linking Rust crates creates one combined work, so a *distributed* binary
that links GPL crates is a GPL work and must ship under GPL terms (source offer,
no incompatible-proprietary code linked into that same binary). Zed's editor
crates are GPL-3.0-or-later (plain GPL, not AGPL — network/service use triggers
nothing; only shipping the binary does).

What this means in practice:

- **The GPUI backend we wrote (`gpui_aros`) is Apache-2.0** and stays clean to
  publish, exactly like the Feraille work. Nothing about the platform port is
  encumbered.
- **A GPL Zed-crate binary can still ship in the Macaros release — by
  aggregation, not linking.** GPL's mere-aggregation clause allows a standalone
  GPL binary (e.g. `AEdit-zed`) on the same DMG as the permissive binaries;
  what's disallowed is linking GPL crates *into* one of the permissive/
  proprietary binaries. So a GPL editor path is: keep Zed's GPL source out of
  our repos (Axis 1: a ports-style patch over a downloaded Zed release), build
  it as its **own** artifact with a GPL notice + source offer, and aggregate it.
  (Earlier drafts of this doc said such a build "cannot be folded into the
  release" — that overstated it; aggregation is fine, linking-in is not.)
- **The gpui-component path avoids the GPL entirely.** An Apache-2.0 editor on
  an Apache-2.0 platform — no ports diff, no GPL notice, folds straight into
  Macaros. This is what `~/Source/aros-editor` is, and it stays the default.
- **Provenance rules still apply** ([CLEANROOM.md](../CLEANROOM.md)): whichever
  path, our own AROS glue carries no third-party product names and no AI
  attribution. The upstream licenses of the vendored crates are theirs and stay
  intact; that is orthogonal to our own-code posture.

Decision rule: **default to the gpui-component (Apache) editor.** Take the GPL
Zed-crate path only if a must-have capability lives only in Zed's
`editor`/`language` crates — and if so, structure it as a ports-style patch over
a downloaded Zed release, built as a separate GPL artifact and aggregated into
(never linked into) the permissive release.

### Dependency-graph audit (distribution-readiness)

"Top-level Apache" does not prove the whole transitive tree is safe to ship. A
full scan of `aros-editor`'s dependency graph (`cargo metadata`, 2026-07-22)
found it ~99% permissive (MIT/Apache/BSD/ISC/Zlib/Unicode-3.0/MPL-2.0 — all
distributable) with **one snag: three GPL-3.0-or-later crates in the graph —
`zlog`, `ztracing`, `ztracing_macro`** (Zed's logging/tracing crates).

- **Single choke point: `sum_tree`** (a core gpui crate) pulls all three:
  `sum_tree → ztracing → zlog`, plus the `ztracing_macro` proc-macro. Everything
  downstream inherits them — gpui, gpui_platform, gpui-component, `aros-editor`,
  **and Feraille** (same gpui fork).
- **The shipped binary contains none of it (verified: 0 `zlog`/`ztracing`
  symbols in `libaros_editor_app.a`).** `ztracing::instrument` is a profiling
  attribute macro that expands to a no-op unless a `tracy` feature is on (off by
  default); `zlog` is only called in sum_tree's `#[cfg(test)]` code; the
  `ztracing_macro` proc-macro is build-time only and never links into any target
  binary. LTO strips the dead code.
- **Fixed at the source (2026-07-22).** Rather than rely on "the optimizer
  removed it", the `zed-aros` fork's `sum_tree` now drops the `ztracing`/`zlog`
  deps and the inert `#[instrument]` attributes (`cursor.rs`, `sum_tree.rs`) plus
  the test-only `zlog::init_test()`. Verified: all three GPL crates leave the
  graph, and both `aros-editor` and Feraille still build. The gpui graph is now
  permissive at the source level, not merely after dead-code elimination.
- Two crates report no SPDX license (`gpui_shared_string`, `gpui_util`) and one
  a bare `LICENSE` file (`tree-sitter-graphql`); these are Apache/MIT in
  substance but the metadata field is unset — worth pinning down before a formal
  release, not a blocker.

Takeaway for shipping Macaros: the Apache path is distributable, but do the
`sum_tree` severance (and a `cargo-deny`/`cargo-about` pass) before treating any
gpui-based binary as formally GPL-free.

### The editor app (the chosen path)

The Apache path is scaffolded as a standalone workspace at
`~/Source/aros-editor` (kept separate from Feraille for product/license
cleanliness). Two crates mirroring `feraille-aros-app`: `aros-editor` (the GPUI
app — a window holding the gpui-component code editor) and `aros-editor-app`
(the AROS `C:AEdit` staticlib wrapper + C harness + `link-aros.sh`). Host
`cargo run -p aros-editor` shows it on macOS; the AROS build uses the pinned
toolchain + `-Zbuild-std` against this repo's target JSON. See that repo's
README for the build/run loop.

**AROS-target `cargo check` (`-Zbuild-std`): PASS (2026-07-22).** The whole
gpui-component code editor + tree-sitter grammar set + gpui_aros backend +
rust-aros std compile for `aarch64-unknown-aros` — the API surface is confirmed
before the link-and-boot cycle. (Config gotcha: the workspace `.cargo/config`
target CFLAGS must carry the AROS SDK include chain + compat `-I` +
`-DHAVE_ENDIAN_H`, else tree-sitter's vendored C fails `stdio.h not found`.)

**Boot-to-buffer: PASS (2026-07-22).** `C:AEdit` (61 MB ET_REL) boots on hosted
AROS and opens the editor window — code buffer, line numbers, live tree-sitter
syntax highlighting, and working keyboard input (typed text edits the buffer and
re-highlights). The Apache gpui-component editor runs on AROS through the
gpui_aros CPU backend. Second link gotcha (besides the CFLAGS include chain):
the workspace `.cargo/config` must set `AR_aarch64_unknown_aros` to the
crosstools `llvm-ar` — Apple's `ar` silently makes **empty** archives from ELF
objects, so every cc-rs native lib (tree-sitter runtime + grammars, psm asm)
links as undefined symbols.

**Open / edit / save real files: PASS (2026-07-22).** `C:AEdit MacRW:sample.rs`
reads a file from the AROS filesystem (rust-aros std `fs`), highlights it by
extension, and Cmd+S writes it back — verified end-to-end (the edit lands on the
host at `~/AROS/Shared/sample.rs`). It is now a real editor, not a demo buffer.

**Code intelligence over LSP** (the goal-defining step), using the
host-embedded-server architecture above. gpui-component's editor exposes LSP as
Rust **provider traits** (`CompletionProvider`, `HoverProvider`,
`DefinitionProvider`, `CodeActionProvider`, semantic tokens), so the AROS side
implements those traits backed by a JSON-RPC client over `std::net` TCP to a
host language server. Staging:

- **(3) Host bridge: PASS (2026-07-22).** `aros-editor/crates/host-lsp-bridge`
  exposes `rust-analyzer` stdio on a loopback TCP port. Handshake verified on
  the host.
- **(4) AROS-side handshake: PASS (2026-07-22).** `C:AEdit` connects from AROS
  over the bsdsocket loopback and completes `initialize`; the status bar shows
  `LSP: connected — rust-analyzer 1.98.0-nightly (50 ms)`. Prereq:
  `bsdsocket.library` built + deployed.
- **Latency: ~50 ms, and the AROS recv path is fine.** An earlier draft claimed
  the handshake took "tens of seconds" and blamed a slow blocking `recv` /
  bsdsocket park, and named an async-layer OS fix as the critical path. **That
  was wrong** — a test-harness artifact. The `PollFd` park
  (`bsdsocket_util.c`) polls every ~20 ms (`Delay(1)`) with a level-triggered
  kqueue pump (`aros-aarch64/hosted/bsdsocket/bsdsock_pump.c`) that raises the
  task signal on readiness, so recv wakes promptly. The real cause of the hangs
  was the **host bridge**: it served one connection at a time and blocked, so
  repeated test boots left half-closed connections and orphaned `rust-analyzer`
  processes that wedged it. Fixed by making `host-lsp-bridge`
  thread-per-connection and reaping the child on session end; a clean boot then
  handshakes in ~50 ms.
- **Consequence: usable LSP does NOT require the deep async-layer OS work.** A
  ~50 ms blocking round-trip plus a `std::thread` is enough for interactive
  completion/hover. So the plan does not reorder — the WaitSelect/FIONBIO work
  is a later responsiveness/throughput refinement, not a prerequisite.
- **(5) Code intelligence: PASS (2026-07-23).** The LSP client
  (`aros-editor/src/lsp.rs`) is a background-reader design: a reader thread
  demuxes request responses, pushed notifications, and server requests over one
  `Arc<TcpStream>` (bsdsocket `try_clone` is unsupported, so no half-splitting).
  Backing gpui-component's provider traits, verified live on AROS:
  - **Completions** — typing `greeting.` pops rust-analyzer's String methods.
  - **Diagnostics** — `let count: i32 = "…"` shows a red type-mismatch squiggle
    (pushed `publishDiagnostics` → a polled `DiagnosticSet`).
  - **Hover** — hovering a symbol shows its type + docs.
  A Zed-shaped editor with real code intelligence, running on AROS.
- **The language server is managed for you.** `aros-editor/run-editor.sh` starts
  rust-analyzer (via `host-lsp-bridge`), boots the editor, and tears both down on
  quit — one command, no terminal juggling. The bridge drives a graceful LSP
  `shutdown` when the editor disconnects, so the server exits cleanly.
- **Click-to-position caret: FIXED (2026-07-23).** The code-editor `Input` used
  `.size_full()` inside a flex column with the status bar, so it overflowed and
  its mouse hitbox no longer matched the rendered text — `MouseDown` never
  landed (hover, a window-level listener, still worked). Root-caused by
  instrumenting `on_mouse_down` (it never fired), fixed with `.flex_1()`.
  Clicking a line now moves the caret there. (Also `graft/aros-ctl click` now
  moves the pointer before pressing, since a button event with coordinates
  doesn't reposition.)
- **Extension settings UI: DONE (2026-07-23).** A bottom status bar hosts
  extension icons on the right: a theme toggle (light/dark) and an `⚙ LSP` icon
  that opens a floating settings panel (server address + Reconnect/Close). The
  pattern for an extension exposing settings from a status-bar affordance.
- **(6) Remaining:**
  - **Go-to-definition** (a `DefinitionProvider`, same client + a
    `textDocument/definition` request; jump the caret / open the target).
  - Diagnostics/hover/completion polish; multi-file navigation.
  - The blocking-`recv` latency (~50 ms) is fine for all of this; WaitSelect/
    FIONBIO remains a later throughput refinement.

## The Zed-crate boot (editor-core on AROS)

`C:ZedAros` boots the real GPL `editor` crate on hosted AROS and renders an
`Editor` view (buffer + line numbers + base theme) through the `gpui_aros` CPU
backend, with networking/wasm/terminal stubbed. The entry point is a minimal
staticlib crate, `zed-aros/crates/zed_aros_app` (`application()` →
`settings::init` → `theme_settings::init(JustBase)` → `release_channel::init` →
`editor::init` → open a window whose root is `Editor::multi_line`). The build +
link + boot recipe and prerequisites live in
[hosted/zed/README.md](../../../hosted/zed/README.md); run it with
`hosted/zed/build.sh`.

Five AROS-specific things had to be solved past "the crates compile", each a
durable gotcha for any Rust-GUI-on-AROS binary:

- **C cross-compile recipe for cc-rs deps** (sqlite, tree-sitter, ring). Sourced
  from `hosted/zed/aros-env.sh` (path-free `.cargo/config.toml` can't hold the
  SDK include roots). Must match the Rust target ABI: `--target=aarch64-unknown-aros
  -fno-pic -mcmodel=large -ffixed-x18` (static reloc + large code model + x18
  reserved; `-mcmodel=large` *requires* `-fno-pic`), plus `-DSQLITE_MAX_MMAP_SIZE=0`.
- **`inventory` constructors must actually run.** Zed's settings and action
  registries use `inventory`, whose life-before-main constructors register via a
  `.init_array` `link_section` gated to a fixed `target_os` list that omits
  `aros` — so on AROS nothing registered and the first setting lookup panicked
  ("unregistered setting type"). Fix: vendored `inventory` with `aros` added to
  that list (`zed-aros/vendor-aros/inventory`). collect-aros then gathers
  `.init_array` into the AROS `INIT_ARRAY` symbol set.
- **…and that symbol set must be invoked.** A bare `startup.o` + our own `main`
  runs the autoinit chain only if the program *claims* the set handler; the C
  shim (`hosted/zed/zed_aros_main.c`) does `THIS_PROGRAM_HANDLES_SYMBOLSET(INIT_ARRAY)`.
  Do **not** also call `set_call_funcs` manually — that double-runs every
  constructor (symptom: "action … already registered").
- **Stack.** The editor's init + gpui layout overflow the few-KB default CLI
  stack (SIGSEGV, "went out of stack limits"). collect-aros emits no seglist
  stack cookie for hosted ELF, so a `long __stack` global is ignored; the shim
  instead `NewStackSwap`s onto a 16 MB heap stack before calling the Rust entry.
- **mmap leaves.** sqlite and `memmap2` reference `mmap`/`munmap`/`getpagesize`,
  which AROS has no VM for. `hosted/zed/aros_mman_stub.c` backs them with the
  heap (anonymous → zeroed malloc; file-backed → malloc + `lseek`/`read`, since
  posixc has no `pread`). Also a getrandom **0.2** custom backend (arc4random_buf)
  alongside the 0.3/0.4 one, since 0.2 is still in the graph.

**Workspace, highlighting, and status bar (2026-07-24).** `C:ZedAros` now boots
the real `workspace::Workspace` rather than a bare editor: opening a file
(`ZedAros MacRW:foo.rs`) shows it in a pane with an editor **tab** and the window
title tracking the file, through a real Zed `Project`. **Syntax highlighting**
works and the **status bar** renders (a live Ln:Col cursor-position item). What
this took beyond the editor-core boot:

- **Registered tree-sitter grammars directly from the `grammars` crate** instead
  of via the `languages` crate. Depending on `languages` drags in the LSP
  adapters, and the Python one pulls Microsoft's ~25-crate `pet` suite (multiple
  AROS gaps). `grammars` gives grammar + config + queries with no adapters.
- **`LanguageRegistry::set_theme`** must be called or every token renders in the
  default foreground (grammars load but highlight captures map to no color).
- **Deferred: the file tree (`project_panel`) and the language-picker status
  item (`language_selector`).** Both transitively pull `heed → LMDB`, whose C
  needs `pread`/`pwrite`/`fdatasync`/`pthread_sigmask`/`sigwait` — POSIX calls
  AROS lacks. They return once that dependency chain is cut or LMDB is ported.
- **Vendored-crate AROS arms added for the workspace graph:** `is-terminal`
  (kept std + "not a tty"), `page_size` (fixed 4 KiB granularity). And the
  link-level one: **`ring` now compiles its aarch64 AEAD assembly** (the ELF
  `linux64` perlasm target was gated to Linux; the Rust side references those
  symbols unconditionally on aarch64, so without it the AEAD ciphers are
  undefined at link). Plus no-op `ZSTD_trace_*` leaves.

Not yet wired: keyboard input into the gpui window from the automation harness
(real keyboard works), and the file/LSP features the Apache path already has
(LSP is OS-gated on the async-socket work, item 1 of
[os-requirements.md](os-requirements.md), now landing).

## The real `zed` binary (2026-07-25)

`C:Zed` links the `zed` crate's own `main` through a staticlib entry
(`crates/zed/src/aros_entry.rs`, built by `hosted/zed/build-zed.sh`), so the
workspace, menus, panels and status bar are Zed's, not a reimplementation. The
subsystems AROS cannot host are gated off per platform *in the workspace*, the
same way Zed already gates per OS: the agent stack (LMDB), extensions (wasm),
collab (WebRTC), and crash IPC.

Three AROS-specific defects had to be fixed to get from "links" to "usable":

- **The tokio runtime is time-only.** `mio` has no poll backend for AROS, so
  `enable_all()` panics during `gpui_tokio::init`. `enable_time()` works.
- **…so the HTTP client cannot be reqwest.** reqwest/hyper drive their sockets
  through tokio's I/O driver. With no driver, `PollEvented::new` panics on the
  worker thread running the request, which on AROS ends the process (telemetry
  and the ACP registry fetch both trip it within seconds of startup). AROS uses
  Zed's own `BlockedHttpClient`. This does not affect the language server, which
  connects over `smol` (see [os-requirements.md](os-requirements.md) item 4).
- **Every file had inode 0.** `RealFs::metadata` read the inode through
  `std::os::unix::fs::MetadataExt`, absent on AROS, and the AROS arm hardcoded
  `0`. The worktree scanner skips any child whose inode is already in its
  ancestor set (its recursive-symlink guard), so with all inodes equal it
  refused to descend into *any* subdirectory: the project panel listed the top
  level and expanded nothing. Fixed in the std pal by adding
  `std::os::aros::fs::MetadataExt` (see
  [STD-PORT.md](../../../hosted/rust/STD-PORT.md)), not by working around it in
  the editor.

Known gaps: the file watcher does not deliver events (`fs_watcher` logs a watch
error per directory), so external edits are not picked up; and rust-analyzer
needs the host bridge running, otherwise the status bar reads "Failed to run
rust-analyzer".

## Compile frontier (what actually builds for AROS)

The first empirical question is which crate in each path breaks first against
`aarch64-unknown-aros`, and whether that break is trimmable (a cfg-gate, a dep
swap) or load-bearing (needs an OS capability that does not exist yet). The rig
runs a per-crate `cargo check` ladder and records pass/fail per crate:

```sh
hosted/zed/frontier-check.sh <workspace> <crate> [crate...]
```

Results land in `frontier-logs/frontier-results.txt`. See
[hosted/zed/README.md](../../../hosted/zed/README.md) for the probe recipe and
the pinned-toolchain prerequisites.

**Update (2026-07-23): the whole `editor` graph now compiles AND boots.** The
"first ladder" table below was the starting map; every FAIL in it has since been
cleared by vendoring the offending crate with an AROS arm (the
vendor-and-add-aros-arm loop, ~50 crates in `zed-aros/vendor-aros/`) and
stubbing the three goal capabilities: **networking** (rustls on the `ring`
provider, no aws-lc; `mio`/`tokio`/`socket2` given AROS arms), **wasm** (drop
tree-sitter's `wasm` feature → no wasmtime/JIT/mmap; gate the `WasmStore` sites
in `language`), **terminal** (an alacritty `tty` stub). `editor` and every crate
under it (`project`, `client`, `language`, `fs`, `settings`, `util`, …) now PASS
`frontier-check.sh`, and `zed_aros_app` links and boots (below). The original
findings, kept for the record:

**Findings (first ladder, 2026-07-22, toolchain `nightly-2026-06-27`).** The
break is exactly where predicted: the whole Apache platform layer compiles, and
every GPL editor crate fails not on editor logic but on a third-party dep
dragging in the **async reactor** or the **wasm toolchain**. Notably `rope` and
`text` — the GPL text-buffer core — both PASS; the buffer model is not the
problem.

| Crate | License | Result | Breaks on | Class |
|---|---|---|---|---|
| `sum_tree`, `collections`, `util`, `http_client`, `scheduler` | Apache | PASS | — | — |
| `gpui` | Apache | PASS | — | — |
| `rope` | GPL | **PASS** | — | — |
| `text` | GPL | **PASS** | — | — |
| `language` | GPL | FAIL | `target-lexicon` build.rs rejects `aarch64-unknown-aros` (pulled by `cranelift` → wasmtime) | load-bearing (wasm) |
| `lsp` | GPL | FAIL | `errno` crate: no AROS arm | trimmable |
| `multi_buffer` | GPL | FAIL | `errno` crate: no AROS arm | trimmable |
| `project` | GPL | FAIL | `polling`: unsupported target OS | load-bearing (async reactor) |
| `editor` | GPL | FAIL | `polling`: unsupported target OS | load-bearing (async reactor) |
| `workspace` | GPL | FAIL | `getrandom 0.2`: unsupported target | trimmable |
| `zed` | GPL | FAIL | `socket2`: `mod sys` no AROS arm (async networking) | load-bearing (async reactor) |
| `gpui-component` (Apache path) | Apache | **PASS** | — | — |

Reading it: the failures cluster into exactly two blockers, both already known.

- **Trimmable (a crate arm or cfg, no OS capability needed):** the third-party
  `errno` crate needs an AROS arm (we have real posixc errno to back it);
  `getrandom 0.2` needs the custom-backend wiring the `.cargo/config.toml`
  already does for the 0.3 line; `target-lexicon`'s build script rejects the
  target name. These are small patches.
- **Load-bearing (the async layer + wasm):** `polling`/`async-io` and
  `socket2` are the async reactor — the exact gap Nick's stance addresses
  (vendored `polling` AROS backend over `WaitSelect`/exec signals, plus real
  `FIONBIO`). `cranelift`/wasmtime behind `target-lexicon` is the WASM runtime
  (needs the Pulley interpreter, no JIT/mmap).

**Consequence for the decision.** The GPL Zed path is not blocked by the editor
crates themselves — `rope`/`text` compile — but by the async reactor and wasm
toolchain their upper crates depend on, i.e. the load-bearing OS work is
unavoidable to reach `editor`/`lsp`/`project`. The Apache `gpui-component`
editor compiles **today** with none of that. So the decision rule below holds
with evidence: default to the Apache component editor; the GPL path only becomes
worth its async-reactor cost if a Zed-only capability demands it.

## What std provides underneath (already verified)

The editor paths sit on the Rust std port, whose live-verified capabilities and
gaps are authoritative in
[hosted/rust/STD-PORT.md](../../../hosted/rust/STD-PORT.md). The load-bearing
summary for an editor:

- **Works:** blocking `std::net` (IPv4 TCP/UDP over the bsdsocket bridge),
  threads + full sync core, fs, env, time, sync `std::process`
  (`output()`/`status()` + per-command env/cwd). tree-sitter and sqlite already
  compile for the target in Feraille's build.
- **Absent (the async layer):** no live bidirectional pipes / async child (no
  `PIPE:` handler mounted on the hosted boot), `set_nonblocking`/socket timeouts
  are no-ops (so tokio/mio/smol cannot run). Genuinely absent and hard: PTY /
  terminal, wasmtime.

Consequences for the editor:

- **Boot-to-buffer does not need the async layer.** Zed's `BlockedHttpClient`
  (all-reject) satisfies `HttpClient` for a zero-network boot; `node_runtime`
  fetches lazily; telemetry/reconnect are skippable. Order is boot-to-buffer
  first, async second.
- **LSP has a license-independent shortcut: TCP, not pipes.** The bsdsocket
  bridge is host-passthrough, so AROS's `127.0.0.1` is the Mac's loopback. A
  host-side language server on a TCP port is reachable from AROS today with zero
  new OS work; the port work is a TCP arm in the transport (Zed's `lsp.rs` is
  `Stdio::piped` today). This is independent of, and cheaper than, mounting
  `PIPE:`.
- **Design decision: embed the language server in the Macaros host.** Rather
  than asking the user to launch `rust-analyzer` by hand, the Macaros host app
  (which already runs AROS as a macOS process) optionally spawns the language
  server as a native **host** child process and exposes it on a loopback TCP
  port; the AROS editor connects over the bsdsocket bridge. This puts all the
  subprocess/stdio complexity on macOS, where spawning is trivial, and leaves
  AROS needing only a blocking TCP client (works today) plus one `std::thread`
  for the read side. It means step-3 LSP does **not** wait on the AROS async /
  `PIPE:` OS work at all — that stays a step-4 concern for self-hosted tools.
  (rust-analyzer speaks LSP over stdio; the host wraps its stdio in a tiny
  stdio↔socket pump, or runs it in the LS's TCP mode where available.)

## OS capabilities requested (handoff to the AROS side)

The IDE features that need real OS primitives (networking/async, live child
pipes, a PTY) are speced as capability contracts for the OS team in
[os-requirements.md](os-requirements.md) — ordered by leverage, each with an
acceptance test, phrased AROS-idiom-neutral. Core editing needs none of them.

## Upstream stance (AROS core, 2026-07-22)

`fork`/`select`/`poll`/`mmap`/`dlopen` will not be implemented POSIX-style; use
AROS idioms. Mapping: spawn via `SystemTagList SYS_Asynch` / `CreateNewProc` +
the `PIPE:` handler that already exists in `aros-upstream`
(`workbench/fs/pipe`) but is unmounted on the hosted boot; readiness via exec
signals (`SetSocketSignals`/`WaitSelect` in the bsdsocket API) behind a vendored
`polling`-crate AROS backend; wasm via wasmtime's Pulley interpreter (no
JIT/mmap); `dlopen` → `OpenLibrary` shim. Upstream asks: mount `PIPE:` +
child-exit notification, `WaitSelect` + real `FIONBIO` in the hosted bsdsocket
bridge, per-task errno, host-synced RTC.
