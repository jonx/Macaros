# Macaros release: the RustHello sample

RustHello is a small Rust program built with the real Rust standard library,
running natively on AROS on Apple Silicon. It is proof that "Rust std runs on
Macaros" and a nice thing to show off in the release.

## What to bundle

- Ship this one file: `bin/darwin-aarch64/AROS/C/RustHello` (about 1.6 MB, a
  self-contained AROS ELF). It is already deployed in `~/aros-build`. No extra
  runtime, no shared libs to include; it links everything it needs statically.
- It is already installed as a `C:` command in the boot tree, so a bundled boot
  image picks it up automatically.

## How a user runs it (from an AROS Shell)

- `RustHello` (the bundled C: command), or
- `MacRW:RustHello` (same binary, off the read/write Mac share).
- Optional: `SetEnv RUST_DEMO_WHO yourname` before running to personalize one line.

It prints a banner and exercises live std features (args, iterators, an env var, a
monotonic timer, a directory listing and a file write on the Mac share, and 3 real
threads), then exits 0. Running it also writes `MacRW:rust-was-here.txt` on the
share, which is harmless.

## Rebuilding it (if needed)

- `./hosted/rust/sample-build.sh` from the repo root. It builds the crate, links
  with the AROS toolchain, and deploys to both `C:RustHello` and the `MacRW:` share.
- By default it targets `~/aros-build`, which is the tree `graft/run-window.sh`
  boots. To target another tree:
  `AROS_BUILD=<tree>/bin/darwin-aarch64 ./hosted/rust/sample-build.sh`.
- It needs the pinned Rust nightly (`nightly-2026-06-27`, auto-installed by rustup on
  first build) and the prebuilt AROS crosstools. It does not rebuild the OS or the
  LLVM toolchain.

## One caveat worth knowing

- The AROS world it runs on must be built with `-ffixed-x18` (the Apple Silicon
  platform-register reservation). The `~/aros-build` tree already is. If you rebuild
  the OS for the release from a different tree, keep that flag or the Rust
  threads/timers can fault.

Source lives in `hosted/rust/sample/` (the crate) and `hosted/rust/sample-build.sh`
(the build recipe). Full background on the Rust-on-AROS port is in
`hosted/rust/STD-PORT.md`.
