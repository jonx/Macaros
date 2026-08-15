# Contributing

This is a solo-maintained hobby project. Issues and pull requests are welcome,
with the caveats below so nobody wastes time.

## Ground rules

- **This repo is the host/graft layer only.** Changes to the AROS operating
  system itself (kernel, libraries, C: commands) belong in the AROS fork
  ([jonx/AROS](https://github.com/jonx/AROS), branch `aarch64-darwin-graft`),
  not here. `graft/upstream-patches/` is a backup snapshot, never the place to
  edit.
- **License:** contributions are accepted under the repo license
  ([APL 1.1](LICENSE), the AROS license). Do not add third-party code without
  an entry in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
- **Do not modify `hosted/jit68k/emu68/`.** Those files are vendored verbatim
  from Emu68 (MPL-2.0) and must stay byte-identical; see
  [hosted/jit68k/emu68/NOTICE](hosted/jit68k/emu68/NOTICE). Fixes go in the
  surrounding JIT glue or as build-time transforms.
- Feature `spec.md` files follow the
  [independent-work process](docs/features/CLEANROOM.md); read it before
  touching one.

## Building

- Newcomer path: [GETTING-STARTED.md](GETTING-STARTED.md).
- Quick, self-contained target: `make run68k` (the 68k JIT; needs only Xcode
  clang), then `build/run68k hosted/jit68k/apps68k/bin/mandel.exe`.
- The full hosted OS build needs the AROS source as a sibling checkout and a
  configured build tree; read
  [docs/features/build/README.md](docs/features/build/README.md) first. The
  short version: never a bare `make` at the AROS level, build metatargets, in
  a stable build dir.

## Testing

- `make test` boots the bare-metal AArch64 backend on QEMU and asserts the
  milestone markers.
- `make hosted-test` builds and runs the hosted host-layer spikes.
- Feature work has per-feature smoke scripts under `graft/` (`resize-smoke`,
  `clipboard-smoke`, `crash-smoke`, ...) that drive a booted AROS through
  `graft/aros-ctl`. Run the one covering what you changed; its PASS line is
  the acceptance gate.

## Pull requests

- Small and focused beats large and mixed. One concern per PR.
- Say what you ran to verify (which smoke, which target, on what machine).
- Match the surrounding code style; this codebase favors heavily commented C
  with the reasoning inline.
- Docs live next to the feature (`docs/features/<name>/`); if your change
  makes a doc stale, fix the doc in the same PR.
