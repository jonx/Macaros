# `hosted/ffmpeg/` — native libavutil on AROS (`[FF0]`)

The first milestone of the [native ffmpeg port](../../docs/features/ffmpeg-native/README.md):
cross-build **libavutil** for darwin-aarch64 AROS with the AROS crosstools and prove
it with one `C:` smoke. A **native software port** — ARM code built *for* AROS, not
a macOS dylib bridged in — the C sibling of the [Rust port](../rust/README.md).

## Status — build → link → deploy **GREEN**; on-AROS run via `graft/ffmpeg-smoke`

`graft/ffmpeg-smoke` runs the whole pipeline: cross-build libavutil → link the smoke
into an AROS `C:` command → deploy → boot AROS → assert. The smoke prints, on AROS:

```
[FF0] libavutil 60.x.x  version_info="8.1.2"
[FF0] av_malloc(4096) ok, writable
[FF0] av_mallocz(4096) ok, zeroed
FFMPEG-AROS: [FF0] ALL PASS
```

What is proven and reproducible:

- **libavutil cross-compiles for AROS.** Stock ffmpeg `configure` + `make`, driven by
  the AROS crosstools, produce `libavutil.a` whose every member is
  `elf64-littleaarch64`. (`build.sh`)
- **The smoke links into a real ET_REL AROS command** with no undefined symbols and
  deploys into the AROS `C:` directory. (`graft/ffmpeg-smoke`)
- The on-AROS assertion runs through the same `bench-run` harness as everything else;
  it depends only on a healthy AROS windowed boot.

## How it works — the toolchain recipe (verified live, see NOTES.md "[FF0]")

ffmpeg's `configure` is pointed at **`aros-cc.sh`**, which is the AROS-patched clang
driver (`--target=aarch64-unknown-aros`) plus three AROS-specific flags. The driver
supplies the correct `aros/posixc`+`aros/stdc` includes itself, so libc headers
(`<stdio.h>`/`<stdlib.h>`/`<math.h>`) resolve cleanly — none of the host-SDK
`cc_include` pollution the Rust glue had to dodge. The flags, each grounded:

| Flag | Why | Source |
|------|-----|--------|
| `-mcmodel=large` | GOT-free `MOVW_UABS_*` relocs; AROS LoadSeg has no GOT | `make.cfg`; same as the Rust port |
| `-Wl,--allow-multiple-definition` (link) | the `AROS_LIBREQ` duplicate marker | UPSTREAM-NOTES item 18 |
| `COMPILER_PATH=tools:crosstools/bin` | driver finds `collect-aros`; it finds `ld` | the `aarch64-darwin-aros-ld` wrapper |

### posixc gaps found (the point of [FF0])
libavutil compiled against AROS once four POSIX functions — present as symbols in
`libposixc.a` but **under-declared in the headers** — were given correct prototypes in
**`aros-compat.h`** (force-included via `--extra-cflags=-include`): `fdopen`,
`mkstemp`, `tempnam`, `posix_memalign`. Correct prototypes, not blanket
`-Wno-implicit-function-declaration` (which would truncate the pointer-returning ones
on this LP64 target). The proper fix is to expose these in the AROS posixc headers
upstream; the shim unblocks the port without touching the OS tree. One link dep:
libavutil takes a global mutex, so the smoke links `-lpthread` (AROS `libpthread.a`).

## Files

| File | Role |
|------|------|
| `aros-cc.sh` | the `--cc`/linker for ffmpeg: AROS clang driver + the three flags. Reusable for any configure-style port. |
| `aros-compat.h` | correct prototypes for posixc functions AROS has but under-declares; force-included into every ffmpeg compile. |
| `build.sh` | fetch pinned ffmpeg → cross-`configure` (libavutil only, `--disable-asm`) → `make` → install to `build/ffmpeg/sysroot`. |
| `ff0_main.c` | the AROS `C:` smoke (version string + `av_malloc`/`av_mallocz`/`av_free`). |
| `../../graft/ffmpeg-smoke` | end-to-end: build → link → deploy → boot → assert. |

## Build & run

```sh
../../graft/ffmpeg-smoke            # build libavutil + link + deploy + boot + assert
hosted/ffmpeg/build.sh              # just cross-build libavutil into the sysroot
../../graft/ffmpeg-smoke --no-build # relink the smoke + run against a built sysroot
```

`build.sh` needs an AROS build tree (crosstools + SDK + `collect-aros`); it discovers
it the way `graft/deploy-check` finds the boot dir, or set `$AROS_BUILD`. ffmpeg
source is fetched into the gitignored `build/` (pinned `8.1.2`, sha256-verifiable via
`FFMPEG_SHA256`), never vendored — it's LGPL/GPL; we build it, we don't modify it.

## Scope

`[FF0]` is **libavutil only** + one smoke. Deliberately out of scope: codecs (FF1),
codec threading (FF2, the real risk later), NEON asm (FF3). The shared substrate with
the [Rust std port](../rust/README.md) is exactly this `posixc`-hardening grind.

---

*Native software port: no macOS bridge. ffmpeg is built by the AROS crosstools into
ARM code for AROS. The `aros-compat.h` shim documents real, specific posixc
header-exposure gaps rather than papering over them.*
