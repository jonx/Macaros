# Feraille on AROS (GPUI port)

Status: **stage 1 PASS on booted AROS** (2026-07-03). Feraille's four UI-free
crates (`feraille-core`, `feraille-disk-usage`, `feraille-fs-native`,
`feraille-meta` incl. bundled SQLite) cross-compile for `aarch64-unknown-aros`
and run live: `C:FerailleProbe` prints `FERAILLE-AROS: CORE PASS` (treemap
layout + HTML export, homoglyph name hazards, SQLite preference/ant-trail
roundtrip). Probe + link recipe: [`hosted/feraille/`](../../../hosted/feraille/).

## Goal

Run [Feraille](https://github.com/jonx/Feraille) (the GPUI file manager,
local checkout `~/Source/Feraille`, branch `aros-port`) natively on
darwin-aarch64 AROS, with the native AROS look. **Nothing is pushed to any
upstream** (zed, Feraille, rust) until the port is presentable; all work is
local.

## Architecture

GPUI draws every pixel itself on all platforms; "native" is the shell around
the canvas. The port follows the exact structure the mac/win/linux backends
use at Feraille's pinned zed rev (`1d217ee`), where platform backends are
separate crates:

- **`gpui_aros`** — new crate in the local zed fork
  (`~/Source/zed-aros`, branch `aros-platform`), plus one
  `cfg(target_os = "aros")` arm in `gpui_platform::current_platform()`.
  Feraille's `aros-port` branch redirects the zed git deps to this fork via
  `[patch]`.
- **Renderer**: software rasterization (tiny-skia + the pure-Rust text stack
  cosmic-text/swash/rustybuzz, reused unchanged) into a BGRA buffer, blitted
  into the Intuition window's RastPort via cybergraphics `WritePixelArray`;
  the [cocoametal display driver](../cocoa-metal-display/design.md) presents
  the screen as usual (the app never talks to the shim directly). Dirty-rect
  repaint is a day-one design requirement, not an optimization. This software
  path is also the permanent fallback on every platform.
- **Native shell**: Intuition window + system gadgets, `SetMenuStrip`
  pulldowns, ASL requesters, [clipboard bridge](../clipboard-bridge/README.md),
  `SetPointer`, input from the window's IDCMP port; the AROS-API surface
  lives in small C glue files (the `aros_*_glue.c` pattern the std pal uses),
  Rust calls them `extern "C"`. Background executor = std threads (proven),
  foreground executor pumped from the IDCMP event loop.
- **GPU accel (later, explicit-first)**: a compute/blit section added inside
  the existing cocoametal shim sharing its one `MTLDevice`+queue, fronted by a
  new `gpufx.library` (native-modules + host-bridge pattern); first consumer
  is the [ffmpeg](../ffmpeg-native/README.md) swscale path, then Feraille's
  present/scale. Transparent `graphics.library` hooks only once that is
  proven. MUI/Zune is not used for the app interior; Zune/theme is only a
  late theming source (system font/pens into `feraille-design` tokens).

## gpui-core dependency port (the real Track-B gate)

Getting `cargo check -p gpui` green for `aarch64-unknown-aros` is the gate before
the backend crate can compile. Key finding (2026-07-04): **gpui core itself is
clean** — its only async dependency is `async-task` (pure Rust, no reactor), and
it builds on our `PlatformDispatcher`, not on `async-io`/`smol`. So **no
rustix/polling/libc port is needed.** The POSIX reactor/syscall stack enters only
through two sibling workspace crates:

- `util` → `smol` → `async-io` → `polling`/`rustix`/`errno` (reactor); also
  `which` (exe discovery) and `dirs`/`dirs_sys` (home dir).
- `http_client` → `github_download` → `async-tar` → `filetime`/`libc`.

Strategy = **trim, don't port**: gate these off for `target_os = "aros"` (a file
manager needs no network reactor / tar / exe-PATH search), plus tiny leaf shims
for the crates gpui pulls directly:

- `stacker` ← `stacksafe` ← gpui (direct; mmap stack growth) — needs a small
  aros arm/patch.
- `getrandom` — custom backend cfg (per-version).
- `dirs_sys` — env-based paths for aros.

Gated so far (fork-local, commented, `cfg(not(target_os="aros"))`): `which`
(util), `async-tar`+`github_download` (http_client). Remaining: the `smol` gate
in util + the leaf shims. This is mechanical crate-trimming, ~1-2 days, not the
multi-week ecosystem port the raw error list first suggested.

## Cross-build facts (hard-won, reuse them)

All encoded in `hosted/feraille/core-probe/.cargo/config.toml` and
`hosted/feraille/build.sh`; they will apply verbatim to the full app build:

- `getrandom` 0.4: custom backend (`__getrandom_v03_custom`) over posixc
  `arc4random_buf`; selected by `--cfg getrandom_backend="custom"`.
- C built by cc-rs (blake3 NEON, SQLite) needs: `--target=aarch64-unknown-none-elf
  -fno-pic -mcmodel=large -ffixed-x18`, AROS SDK includes with **posixc before
  stdc** (that's where `localtime_r` lives), and `-DNO_AMIGA_LINKAGE_TYPES`
  (exec/types.h `GLOBAL` collides with SQLite's `GLOBAL(t,v)`).
- SQLite: `-DSQLITE_OMIT_WAL -DSQLITE_MAX_MMAP_SIZE=0
  -DSQLITE_OMIT_LOAD_EXTENSION` (no shm/mmap/dlopen on AROS; feraille-meta
  falls back to the DELETE journal by design).
- Apple `ar` writes **empty archives** from ELF objects; use crosstools
  `llvm-ar` (`AR_aarch64_unknown_aros`).
- Build-script-produced `.a`s (libsqlite3, blake3_neon) are not folded into
  the Rust staticlib; add them to the collect-aros link line explicitly.
- Harness must define `aros_argc`/`aros_argv` globals (std args pal contract).

## Layout

| Piece | Where |
|---|---|
| Stage-1 probe (crates + harness + build) | `hosted/feraille/` |
| GPUI fork (gpui_aros) | `~/Source/zed-aros` @ `aros-platform` |
| Feraille app branch | `~/Source/Feraille` @ `aros-port` |
| Rust std pal | `../rust-aros` ([rust-aros](../rust-aros/README.md)) |

## Milestones

1. ~~UI-free crates run on booted AROS~~ — **DONE** (`C:FerailleProbe`).
2. Static window: gpui_aros opens an Intuition-hosted window via the shim,
   software-rasterizes a quad + one line of shaped text, `cm_present`s it.
3. Interactive shell: input/clipboard/menus/dialogs/dispatchers; Feraille's
   real views; dirty-rect repaint model.
4. GPU accel explicit path (`gpufx.library` + shim compute section).
5. Native-the-Amiga-way feature pass (e.g. quarantine "where from" ->
   filenote provenance), Zune theming.
