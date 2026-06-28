# Native ffmpeg / libav* on AROS (aarch64)

Status: **scoping (not started)** — 2026-06-28. This is a bring-up plan, grounded
against the upstream tree; nothing here is built yet. Items not yet confirmed are
marked **UNVERIFIED**.

## Goal

Build the **libav\*** libraries — `libavutil`, `libavcodec`, `libavformat`,
`libswscale`, `libswresample` — **natively** for darwin-aarch64 AROS (compiled by
the AROS LLVM crosstools to ARM code, codec kernels and all), installed into the
AROS SDK so **other AROS programs can `-lavcodec`**. Optional surfaces on top: an
`ffmpeg`/`ffprobe` C: command, and a thin sound/picture **Datatype** so
MultiView/Wanderer open modern media.

Native, not a host bridge, on purpose: the value is a **portable library to build
on** (API availability), and that's about the C runtime, not the CPU. The
host-framework alternative — bridging to **VideoToolbox/AVFoundation** for
hardware-accelerated decode — is the *deferred* host-Datatypes idea (see the
features index). The two can coexist: native `libavcodec` for the API surface and
software codecs, a host bridge later for heavy 4K/H.265. Performance is *not* the
driver here — the M5 gives so much headroom that even `--disable-asm` pure-C
codecs are usable; the work is **runtime completeness and porting**.

## What it actually needs — POSIX/libc surface vs. what AROS has

Checked against `/Users/user/Source/aros-upstream`:

| ffmpeg needs | AROS status |
|---|---|
| C99 `stdio`/`stdlib`/`string` | **present** (`stdc`/`stdcio`) — but *correctness* gaps exist; `printf` floats were broken until the `__vcformat` fix (`UPSTREAM-NOTES.md` item 34). Expect more of this class. |
| `math`: `lrint`/`llrint`/`rint`/`round`/`trunc`/`cbrt`/`exp2`/`log2`/`hypot` | **present** (`compiler/crt/stdc/math`) |
| threads: `pthread_create`/`join`/`mutex`/`cond`/`once`/`key_create` (TLS) | **present** (`compiler/pthread` + `posixc.conf`) — *runtime robustness under heavy codec threading is* **UNVERIFIED** (the #1 risk) |
| clocks: `clock_gettime`/`nanosleep`/`usleep` | **present** (`posixc`) |
| `sysconf` (CPU count → thread autodetect) | **present** (`posixc`) — returns *correct* P+E counts only once the [processor.resource backend](../processor-resource/README.md) lands; until then thread count is **UNVERIFIED** |
| atomics: `stdatomic.h` / `__atomic_*` builtins | compiler-provided by LLVM aarch64 — **expected OK**, confirm no `libatomic` link dep |
| 64-bit `off_t`, `fseeko`/`ftello` (large media) | **UNVERIFIED** — emul-handler 64-bit inode is already fixed (item 31), but large-file seek on the AROS file path needs checking |
| `getenv` | **present** (`posixc`) |
| `mmap` | present in `posixc`, but hosted mmap is W^X-constrained — **avoid** mmap-based paths (ffmpeg runs without them) |
| `dlopen` (codec plugins) | **disable** (`--disable-…`); no native `LoadSeg` plugin path needed |
| tty/termios (the `ffmpeg` CLI only) | console exists; CLI is optional, not on the library path |

Takeaway: **the surface is overwhelmingly present.** This is a *completeness &
correctness* port (fix bugs as the build/run hits them), not a "write libc from
scratch" port. The printf fix is the template for what each gap looks like.

> **Shared substrate with [Rust on AROS](../rust-aros/README.md).** The `posixc`
> hardening this needs is the *same* work Rust's `std` port needs (the `printf`
> float bug is the prototype for both) — do it once, two payoffs. And once both
> exist, Rust can FFI straight into native `libavcodec`. Rust's `net` is already
> the cheaper of the two (it rides the live `bsdsocket`); the libc-completeness
> grind is what they hold in common.

## Build approach

ffmpeg uses its **own** configure (not autotools). Plan:

- Cross-build out-of-band with the AROS crosstools first (not an mmake module):
  `./configure --enable-cross-compile --arch=aarch64 --cc=<aros clang> --ar=… --ld=…
  --disable-everything --disable-programs --disable-doc --disable-asm` then enable
  codecs/asm incrementally. Produces static `libav*.a` + headers.
- ffmpeg has no `--target-os=aros`; start with a unix-like (`--target-os=none` or
  patch `configure` to add an `aros` os — sets lib naming/suffix/section flags).
  Likely a **small configure patch** (upstreamable to ffmpeg).
- Package the resulting `.a` + headers into the AROS SDK so programs link `-lavcodec`.
  Long-term option: wrap `libavcodec` as an AROS `.library` (shared); static first.
- `--disable-asm` is the always-correct fallback; enable aarch64 NEON later (FF3).

## Phased bring-up (greppable markers `[FFn]`, each unattended)

- **[FF0]** `libavutil` only, `--disable-everything`. Smoke: a C: program calls
  `av_version_info()` + `av_malloc`/`av_free` → prints version, asserts non-null.
  Proves the toolchain + base runtime.
- **[FF1]** `libavcodec` + `libswscale` with **one** decoder, `--disable-asm`.
  Decode a known compressed frame from a host-volume file → **checksum the output
  frame** against an expected value. The real "does the C runtime hold up" gate.
- **[FF2]** Enable codec **threading** (frame/slice threads). Decode multi-threaded
  → assert bit-identical to FF1 + no deadlock/leak. **Stresses pthread** — the
  highest-risk layer.
- **[FF3]** Enable **aarch64 NEON asm**. Re-run FF1/FF2: assert bit-exact, compare
  speed via [`bench-run`](../benchmarks/README.md). (Risk: ffmpeg's asm
  macros/directives vs. the LLVM integrated assembler.)
- **[FF4]** `libavformat`: demux a container (mp4/mkv) from `MacRW:` → decode A/V.
  Exercises file I/O + seeking (`off_t`).
- **[FF5]** Surface: an `ffmpeg`/`ffprobe` C: command **or** a thin sound/picture
  Datatype on `libavcodec` so MultiView/Wanderer open media (the native cousin of
  the deferred host-Datatypes bridge).

## Risks & decisions

1. **pthread robustness under load** (FF2) is the make-or-break — ffmpeg threading
   hammers cond vars + TLS far harder than anything booted so far.
2. **Runtime correctness gaps** in `stdc`/`posixc` will surface during build/run;
   each is a discrete fix (the printf bug is the prototype). Budget for several.
3. **stdatomic / libatomic** — confirm aarch64-aros needs no separate `libatomic`.
4. **Large-file seek** (`off_t`/`fseeko`) on the AROS file path — verify early (FF4).
5. **Build integration** — out-of-band crosstools build + SDK packaging first; an
   mmake/`.library` wrapper is a later nicety, not a blocker.
6. Keep `--disable-asm` as the correctness baseline; NEON is a perf layer, not a
   prerequisite.

## What "other programs build on it" means

Ship `libav*.a` + the public headers in the AROS SDK (`Developer/lib` +
`Developer/include`). Then any AROS program — a media player, a transcoder, a
thumbnailer, a future Datatype — links `-lavcodec -lavformat -lavutil` exactly as
on any other platform. That, not raw speed, is the deliverable.

---

*This is a scoping doc; no `[FF*]` milestone is built. It deliberately mirrors the
host-bridge feature docs in shape, but the work is a **native software port**, not
a `hostlib` bridge — see the features index note distinguishing the two.*
