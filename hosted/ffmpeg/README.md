# `hosted/ffmpeg/` — native libavutil on AROS (`[FF0]`)

The first milestone of the [native ffmpeg port](../../docs/features/ffmpeg-native/README.md):
cross-build **libavutil** for darwin-aarch64 AROS with the AROS crosstools and prove
it with one `C:` smoke. A **native software port** — ARM code built *for* AROS, not
a macOS dylib bridged in — the C sibling of the [Rust port](../rust/README.md).

## Status — **PROVEN LIVE on AROS** (2026-06-30)

Native libavutil (ffmpeg 8.1.2) runs on booted darwin-aarch64 AROS. Proof:
`run/darwin-aarch64/proofs/ff0-on-aros-20260630.png`. On AROS:

```
[FF0] libavutil 60.26.102  version_info=8.1.2
[FF0] av_malloc(4096) ok, writable
[FF0] av_mallocz(4096) ok, zeroed
FFMPEG-AROS: [FF0] ALL PASS
```

What is proven and reproducible:

- **libavutil cross-compiles for AROS.** Stock ffmpeg `configure` + `make`, driven by
  the AROS crosstools, produce `libavutil.a` whose every member is
  `elf64-littleaarch64`. (`build.sh`)
- **The smoke links into a real ET_REL AROS command** with no undefined symbols and
  runs on booted AROS (`av_version_info` + `av_malloc`/`av_mallocz`/`av_free`).

### Run / deploy reality (read before re-running)

- **Toolchain trees are split.** SDK at `/tmp/arosbuild`, AROS-patched crosstools at
  `/tmp/aros-crosstools`. Discovery requires a COMPLETE SDK (stale `/private/tmp`
  scratchpad copies get OS-GC'd half-empty). See NOTES.md `[[aros-build-tree-layout]]`.
- **`/tmp/arosbuild` is missing `Libs/stdcio.library`** (has the static lib + headers,
  and posixc/stdc/dos/intuition runtime). FF0Smoke needs `StdCIOBase` via
  libavutil/posixc. Deploy `stdcio.library` into `Libs/` (the tree owner should build
  it; a stopgap copy from another tree works).
- **Run with `graft/aros-ctl run`** (known-good console boot) + `type FF0Smoke` + `shot`.
  `graft/ffmpeg-smoke`'s `bench-run` path cold-start-halts on `/tmp/arosbuild`; that
  harness needs updating to the `aros-ctl` method.

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

### posixc feature flags (the point of [FF0])
libavutil needed four POSIX functions that AROS provides in `libposixc.a` but does
not declare in the default include mode: `fdopen`, `mkstemp`, `tempnam`,
`posix_memalign`. They are not missing, just gated behind feature-test macros
(`fdopen`/`tempnam`/`mkstemp` behind `_GNU_SOURCE` or `_XOPEN_SOURCE`,
`posix_memalign` behind `_GNU_SOURCE` or `_POSIX_C_SOURCE >= 200112`). `--target-os=none`
left those macros off, so nothing was visible. The fix is one flag:
`--extra-cflags=-D_GNU_SOURCE` (it turns the others on, per `aros/features.h`), and the
build re-runs `configure` with it so its `HAVE_*` matches. Credit to Nick Andrews
(kalamatee) for the feature-flag diagnosis. One link dep remains: libavutil takes a
global mutex, so the smoke links `-lpthread` (AROS `libpthread.a`).

## Files

| File | Role |
|------|------|
| `aros-cc.sh` | the `--cc`/linker for ffmpeg: AROS clang driver + the three flags. Reusable for any configure-style port. |
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
ARM code for AROS. The only AROS-side adjustment is one feature-flag
(`-D_GNU_SOURCE`); the functions were always in `libposixc.a`.*
