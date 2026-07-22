# hosted/zed — the Zed-editor-on-AROS build rig

The rig for the [Zed-shaped editor](../../docs/features/zed-editor/README.md)
investigation. Same split as [hosted/rust/](../rust/): the rig lives here, the
Rust source lives in sibling repos (`../../../zed-aros`,
`../../../gpui-component-aros`, `../../../rust-aros`). Read the feature doc first
— **especially the licensing boundary**, which decides which editor path we
build before any port code is written.

## Prerequisites (the pinned toolchain + rust-src symlink)

Identical to the std port; see
[hosted/rust/STD-PORT.md](../rust/STD-PORT.md) "Dev environment setup". The
short version:

- Toolchain `nightly-2026-06-27` with `rust-src`.
- `$(rustc +nightly-2026-06-27 --print sysroot)/lib/rustlib/src/rust`
  symlinked to `../../../rust-aros` (so `-Zbuild-std` compiles our AROS pal).
  Verify: `ls -l` that path resolves to `~/Source/rust-aros`.
- Target spec: [../rust/aarch64-unknown-aros.json](../rust/aarch64-unknown-aros.json)
  (aarch64 ELF, `code-model: large`, `+reserve-x18`, static, panic=abort).
- All sibling repos on their AROS branches: `zed-aros` `aros-platform`,
  `gpui-component-aros` `aros-port`.

## The compile-frontier probe

`frontier-check.sh` cargo-checks each named crate for `aarch64-unknown-aros`
and records pass / fail (with the first error line) per crate, without stopping
at the first failure — the point is the whole map, not the first wall.

```sh
# Zed's own stack (GPL editor crates on top of the Apache gpui family):
FRONTIER_LOG_DIR=/tmp/frontier-zed \
  hosted/zed/frontier-check.sh ../zed-aros \
    sum_tree collections util http_client scheduler gpui \
    rope text language lsp project multi_buffer editor workspace zed

# The Apache component-editor path (checked from the Feraille workspace,
# which already patches gpui → zed-aros):
FRONTIER_LOG_DIR=/tmp/frontier-comp \
  hosted/zed/frontier-check.sh ../Feraille gpui-component
```

Results: `$FRONTIER_LOG_DIR/frontier-results.txt` (one `PASS`/`FAIL` line per
crate), plus a full `<crate>.log` for each. Read a `FAIL` log to classify the
break:

- **trimmable** — a cfg-gate or dep swap fixes it (the `util` process/command
  modules gated `#[cfg(not(target_os = "aros"))]` are the model), or
- **load-bearing** — needs an OS capability that does not exist yet (async
  reactor, PTY, wasmtime). These are the real port work; record which
  capability against [STD-PORT.md](../rust/STD-PORT.md)'s gap list.

Gotcha: after editing the rust-aros clone's `build.rs`, `cargo clean` first or
the frontier readings are stale (cargo caches the std build). Capture std
stdout via the posixc redirect, never a C harness `PutStr` — same rules as the
std port.

## What builds today (first ladder, 2026-07-22)

Full per-crate table + classification is in the
[feature doc](../../docs/features/zed-editor/README.md#compile-frontier-what-actually-builds-for-aros).
Summary:

- **PASS:** all Apache leaves (`sum_tree`, `collections`, `util`,
  `http_client`, `scheduler`), `gpui`, and — notably — the GPL text-buffer core
  `rope` and `text`. The Apache **`gpui-component`** editor path also PASSes.
- **FAIL:** `language`/`lsp`/`multi_buffer`/`project`/`editor`/`workspace`/`zed`
  — none on editor logic, all on a dragged-in dep: `errno`/`getrandom`/
  `target-lexicon` (trimmable) or `polling`/`socket2`/wasmtime (load-bearing:
  the async reactor + wasm runtime).

## Next steps (once the frontier is mapped)

1. Classify every `FAIL` as trimmable vs load-bearing; fill the frontier table
   in the feature doc.
2. Pick the editor path per the licensing decision rule (default: the Apache
   gpui-component editor).
3. Stand up the entry point — either a `zed_aros_app` staticlib in `zed-aros`
   (GPL, separate artifact) or a component-editor `bin` — mirroring
   `feraille-aros-app`. Boot-to-buffer with `BlockedHttpClient` and everything
   network stubbed; async layer and LSP-over-TCP come second.
