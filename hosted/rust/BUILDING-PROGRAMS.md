# Building Rust programs for AROS — 0 → hero

How to go from an empty Mac to **your own Rust program running as an AROS `C:`
command**, the same way `FFViewX` and the bundled `RustHello` do. This is the
*app-author* guide; for how the `std` port itself is built see
[STD-PORT.md](STD-PORT.md), and for the design/why see
[docs/features/rust-aros](../../docs/features/rust-aros/README.md).

## The one thing to understand first

Two different things get called "Rust on AROS":

- **The compiler** is a *Mac-side* cross-toolchain — stock `rustc`
  `nightly-2026-06-27` plus the AROS crosstools plus the `std` source in
  `../rust-aros`. It runs on your Mac and emits ARM code **for** AROS. It never
  runs on AROS.
- **Your program** is a native AROS binary. Once built, it is a `C:` command —
  you type its name at the AROS Shell and it runs, exactly like `FFViewX`.

So "build a Rust program for AROS" = *cross-compile on the Mac, run on AROS.*

## Prerequisites (once per machine)

1. **Rust toolchain + std source.** `rustup` installs the pinned nightly on first
   build. You also need the `std` source symlinked so `-Zbuild-std` compiles the
   `aros` platform layer from the local clone:
   ```sh
   rustup component add rust-src --toolchain nightly-2026-06-27-aarch64-apple-darwin
   SYSROOT=$(rustc +nightly-2026-06-27 --print sysroot); SRC="$SYSROOT/lib/rustlib/src/rust"
   cp -R "$SRC" ../rust-aros            # once; git-track it
   mv "$SRC" "$SRC.orig"; ln -s ../rust-aros "$SRC"
   ```
   (Full detail + why in [STD-PORT.md](STD-PORT.md#dev-environment-setup).)
2. **A built AROS tree + crosstools.** The link step needs `collect-aros`, the
   crosstools, and the AROS libs — i.e. a real AROS build at `~/aros-build`
   (crosstools at `~/aros-crosstools`). See
   [docs/features/build](../../docs/features/build/README.md). This is the same
   tree `graft/run-window.sh` boots.

The target spec (`aarch64-unknown-aros.json`) and all the `aros_*_glue.c` pal
glue live here in `hosted/rust/`; nothing else to fetch.

## The fastest path: copy the sample

`hosted/rust/sample/` is a complete, working `std` crate — use it as your
template rather than starting from scratch.

```sh
cp -R hosted/rust/sample hosted/rust/myprog
```

Its shape (keep this shape — it's what the link recipe expects):

- `Cargo.toml` — `crate-type = ["staticlib"]` (C owns AROS startup and calls into
  Rust; a Rust-owned `fn main()` is not the model here).
- `src/lib.rs` — exports one `#[no_mangle] pub extern "C" fn <name>_main() -> i32`.
  Everything inside is normal `std`: `println!`, `Vec`, `std::fs`, `std::net`,
  `std::thread`, … (see [what's supported](#whats-supported-and-whats-not)).

Rename the crate + the entry fn in your copy, then write ordinary Rust.

## Build + link + deploy

The whole recipe is already scripted for the sample:
[`sample-build.sh`](sample-build.sh). It does three things — read it as the
reference, and adapt it for your crate by changing the crate dir, the `.a` name,
and the C `main` that calls your entry fn:

1. **Cross-compile the staticlib** (std built from `../rust-aros`):
   ```sh
   cargo +nightly-2026-06-27 build --release \
       -Zjson-target-spec -Zbuild-std=std,panic_abort \
       --target hosted/rust/aarch64-unknown-aros.json
   ```
2. **Link into an AROS `C:` command** with `collect-aros`: a tiny C `main`
   (`hello_main.c` in the sample — C owns AROS startup and calls
   `rust_hello_main`), the `aros_*_glue.c` pal glues, your `.a`, and the AROS
   libs, into an **ET_REL** program. Startup object must be `startup.o` (not
   `elf-startup.o`); code model **large**; `-ffixed-x18` on the C glue.
3. **Deploy** to both `C:<Prog>` in the boot tree and the `MacRW:` share.

For the sample, that's just:
```sh
./hosted/rust/sample-build.sh        # -> C:RustHello and MacRW:RustHello
# AROS_BUILD=<tree>/bin/darwin-aarch64 ./hosted/rust/sample-build.sh   # other tree
```

## Run it (on booted AROS)

Boot AROS (`graft/run-window.sh`), then from the Shell:

```
RustHello                 the bundled C: command
MacRW:RustHello           the same binary off the read/write Mac share
SetEnv RUST_DEMO_WHO you  (optional) the sample personalises a line
```

Capture stdout to a host file via the shell redirect — `RustHello >MacRW:out.txt`
— because posixc `write(1)` honours the redirect while dos `PutStr` does not.

## What's supported (and what's not)

Full `std` runs: `println!`/`Vec`/`HashMap`/`format!`, `std::fs` (files,
metadata, `read_dir`, symlinks, perms, times), `std::net` (TCP/UDP over the
bsdsocket bridge, IPv4), `std::env` (read+write+`vars()`), `std::process`
(`output()`/`status()` with per-command env/cwd), `std::time`, and `std::thread`
+ the full sync core. The authoritative per-module status and the remaining
corners are in [STD-PORT.md](STD-PORT.md) and [FIX-PLAN.md](FIX-PLAN.md).

## Caveats that will bite you (each cost real time)

- **`-ffixed-x18` OS build.** The AROS world you run on must be built with it, or
  `time`/threads SIGBUS on Apple Silicon. `~/aros-build` already is.
- **Strip before deploy.** `llvm-strip --strip-debug` your binary — a debug-info
  ET_REL takes minutes to `LoadSeg`-relocate.
- **stdout capture** goes through the shell redirect, not a C harness `PutStr`.
- **`panic = "abort"`** — no unwinding; a panic aborts the command.
- IPv4 only; `set_nonblocking`/socket timeouts are no-ops (bsdsocket park model).

## Where the pieces live

| Piece | Path |
|---|---|
| Target spec | `hosted/rust/aarch64-unknown-aros.json` |
| Sample crate (your template) | `hosted/rust/sample/` |
| Build+link+deploy recipe | `hosted/rust/sample-build.sh` |
| pal glue (`net`/`fs`/`process`/`env`/`thread`/`sync`) | `hosted/rust/aros_*_glue.c` |
| The `std` pal itself | `../rust-aros/library/std/src/sys/*/aros.rs` |
| Std port dev loop, tools, resume map | [STD-PORT.md](STD-PORT.md) |
| Design / why | [docs/features/rust-aros](../../docs/features/rust-aros/README.md) |
