# Native ffmpeg / libav* on AROS (aarch64)

Status: **working** — 2026-06-30. ffmpeg (8.1.2) is cross-built natively for
darwin-aarch64 AROS (ARM code, codec kernels and all), and a real image/video
viewer, **FFView**, runs on it: decode + display + play/pause, proven live.

## What runs today

| Marker | What | Proof |
|---|---|---|
| **[FF0]** | `libavutil` links + runs (version, av_malloc) | `ff0_main.c` |
| **[FF1]** | decode a JPEG with libavcodec + libswscale | `ff1_decode.c`, commit 841f88e |
| **[FF2]** | show a decoded frame in a window | `ff2_show.c`, commit b29091e |
| **[FF3]** | decode **through libavformat** over the dos-backed AVIO | `ff3_avio.c` (`packets=1`, was 0) |
| **FFView** | image/video viewer: open → decode → display, video play/pause | proofs `run/darwin-aarch64/proofs/ffview-*.png` |

Proven media: a still JPEG, MJPEG-in-AVI (full-range YUVJ444P) playing+looping
with SPACE toggling PLAYING/PAUSED, and MPEG-4/DivX (limited-range YUV420P,
inter-frame). All decode through the real demuxers and display correctly.

## The right-way file I/O — `aros_avio.c`

libavformat's built-in `file:` protocol (posixc `open`/`read`/`lseek`) returns
**empty reads** on this target: `avformat_open_input` succeeds and the probe even
identifies the codec, but `av_read_frame` returns **0 packets**, so nothing
decodes. The fix is the idiomatic ffmpeg one — a **custom `AVIOContext`** whose
`read`/`seek` callbacks are AROS dos `Read` / `Seek` (`aros_avio_open()`), with
`AVFMT_FLAG_CUSTOM_IO`. Every demuxer (avi/mov/mp4/mkv/image2/...) then reads
through dos and works. FF3 is the regression proof: `av_read_frame` returns
packets and decodes through the container, where the posixc path returned zero.

> ffmpeg also parses a leading `Volume:` as a `protocol:` URL, so even the file
> protocol mis-handles AROS paths like `MacRW:clip.avi`. The custom AVIO opens the
> dos path directly and sidesteps that too.

## Builds — three configures, one source

All three use `aros-cc.sh` (the `--target=aarch64-unknown-aros`, `-mcmodel=large`
clang wrapper) with `--extra-cflags=-D_GNU_SOURCE` and link `-lpthread`
(libavutil's global mutex). `build.sh` fetches the ffmpeg source first.

| Script | Sysroot | libavcodec.a | For |
|---|---|---|---|
| `build.sh` | `sysroot-aros` | — | [FF0] libavutil only |
| `build-ff1.sh` | `sysroot-ff1` | ~7 MB | [FF1] minimal codec (mjpeg/png/bmp) |
| `build-video.sh` | `sysroot-video` | ~6.5 MB | **FFView** — mjpeg/mpeg1/2/4 + png/bmp/gif |
| `build-full.sh` | `sysroot-full` | ~88 MB | the *complete* native decoder/demuxer set |

`build-full.sh` proves ffmpeg ports **in full** (every native decoder, demuxer,
parser, bsf, plus swscale/swresample, decode-only). But linking the whole decoder
registry is unusable on AROS — see [load time](#load-time--relocations-the-real-cap).
FFView links `sysroot-video`, a small set (mjpeg, mpeg1/2/4, png, bmp, gif) chosen
to keep the relocation count low; it gives a ~3.8 MB binary that loads in ~2 s.
h264/hevc/vp8/vp9 live in the complete build for when a longer load is acceptable.

## FFView — the viewer

```
FFView <file> [tracefile]
```

Opens any supported image or video through `aros_avio`, decodes with libavcodec,
and shows it in an intuition window on the Workbench screen. Single-frame files
are shown as a still image; video plays back paced by **timer.device**.

Controls (window active): **SPACE** play/pause · **R** restart · **Q / ESC /
close** quit. The title bar shows `PLAYING` / `PAUSED` / `image`. Video loops at
EOF. (Audio is not wired — no AHI output yet; video only.)

Pixel conversion is a **hand-written planar-YUV → RGB24** (`yuv_to_rgb24()`):
chroma subsampling is read from the format descriptor (handles 4:2:0 / 4:2:2 /
4:4:4), full- and limited-range BT.601. This exists because libswscale's
`yuv2rgb24_full` C writer **faults on this target** (a bug in the cross-built
output path) — see below. Non-YUV sources (RGB/paletted images) fall back to sws.
`WritePixelArray(RECTFMT_RGB)` blits the result.

### Build + deploy FFView

```sh
# one-time: fetch source + build the video codec set
hosted/ffmpeg/build.sh && hosted/ffmpeg/build-video.sh

# build + deploy the viewer (and the FF0..FF3 demos) into the SDK C:
hosted/ffmpeg/deploy.sh
```

`deploy.sh` compiles `ffview.c` + `aros_avio.c` against `sysroot-video` and
copies `FFView` into `$BUILD/bin/darwin-aarch64/AROS/C/`. FFView is **not** baked
into the boot image (29 MB); deploy it on demand. Put media on `MacRW:` (=
`~/AROS/Shared`) and run `FFView MacRW:clip.avi` from a shell.

## libswscale crash — why the manual converter

`sws_scale` to RGB24 faults deterministically inside `yuv2rgb24_full_1_c` after a
handful of frames (SIGSEGV, same pc). It is *not* a destination-buffer overrun
(padding the dst with `av_frame_get_buffer` slack rows didn't help) — it is a bug
in the cross-built C output kernel on aarch64-AROS. FF2 survived because it
converted a *single* frame; a playback loop hits it. The viewer therefore does
its own YUV→RGB and only falls back to sws for non-YUV sources. Re-enabling sws
for YUV is gated on fixing that kernel (candidate FF-followup).

## Load time is not the problem (a corrected note)

An earlier version of this doc claimed a static ffmpeg binary took ~25 s to load
("relocations", then "symbol-table read"). **Both were wrong — never reproduced
under measurement.** Measured in console mode (`aros-ctl run` returns immediately,
so the timer includes the boot):

| build | file | relocations | boot → main() | → window |
|---|---|---|---|---|
| FFView lite, stripped | 3.8 MB | 51 K | ~1 s | ~2 s |
| FFViewX broad, stripped | 8.2 MB | 436 K | ~1 s | ~2 s |
| FFView broad, **un**stripped | 31 MB | 436 K | ~1 s | ~2 s |

The 31 MB unstripped binary loads as fast as the 3.8 MB one. Reading the file is
~ms (the Mac caches it) and the relocation pass is O(n) in memory and fast. So
**neither file size, symbol table, nor relocation count gates load** at these
sizes. `--strip-unneeded` (in `deploy.sh`) is kept only because smaller binaries
are tidier, not as a load fix. `-mcmodel=large` is required (AROS has no GOT).

What actually looks like a "freeze":

- **Intermittent boot stall.** Some boots stop right after `display registered`
  and never reach `cm_open` (no screen) — a pre-existing hosted-boot flakiness,
  more often in desktop mode. The window never appears; it reads as a hang. Retry.
- **h264/hevc** — see below: those decoders crash, so opening an h264 file (e.g.
  most `.mp4`/`.mov`) traps almost immediately.

RAM is not the limit either: hosted AROS RAM is `memory <MB>` in
`AROSBootstrap.conf` (default 256 MB). Decode working set is just frame buffers (a
320×240 RGB24 frame ~230 KB, 1080p ~6 MB; FFView holds one decode + one RGB frame).

## Codec sets for the viewer

`build-video.sh` (FFView, the default) is a small safe set — mjpeg, mpeg1/2/4,
png/bmp/gif, `--disable-asm` — that has no known decoder crashes. `build-videox.sh`
(FFViewX) adds the broader containers (mkv/webm, flv, mov/mp4, asf, ogg) and codecs,
and builds with **NEON asm enabled** (ffmpeg's aarch64 asm assembles cleanly with
the AROS clang — the toolchain risk noted earlier did not materialise; it is faster
and the proper, as-developed path). It loads just as fast (stripped). **Caveats:**

- **h264/hevc — FIXED. Root cause: the `x18` register.** h264 used to crash in
  every config (pure-C `-O2`/`-O1`/`-O0`, NEON), decoding a few frames then
  faulting at a data-dependent point in `ff_h264_decode_mb_cavlc` /
  `ff_h264_queue_decode_slice`. The [fault-address trap dump](../debug-tools/README.md)
  pinned it: `fault addr=0x268`, instruction `ldr w2, [x18, x2]`, **`x18 = 0`** —
  the GetBitContext bitstream buffer pointer lived in `x18`. `x18` is the AAPCS64
  platform register, **reserved on Darwin**; AROS-hosted runs on Darwin and
  preempts via signals, which don't preserve `x18`, so the aros clang target
  (which doesn't know to reserve it) put a long-lived pointer there and a
  mid-decode preemption wiped it to NULL. **Fix:** `-ffixed-x18` in `aros-cc.sh`
  (the correct Darwin ABI). h264/hevc now decode and play. This is *not* an ffmpeg
  bug — it's a target-ABI flag, the as-developed way to build for a platform that
  reserves x18.
- **Port-wide implication:** nothing in the AROS aarch64 toolchain reserves `x18`,
  so the OS itself has the same latent bug — it just rarely keeps a live value in
  x18 across a preemption. A prime suspect for the intermittent hosted-boot stall.
  The proper follow-up is `-ffixed-x18` (or x18 preservation) across the whole
  aarch64-darwin toolchain. See [NOTES.md](../../../NOTES.md).
- Raw-ES demuxers (`h264`/`hevc`) are deliberately **off**: their fuzzy probe
  mis-claims other raw streams (a `.m4v` mpeg4 ES) and then scans the whole file
  via the custom AVIO and stalls. Container h264 still comes through mov/matroska.

## libc surface (reference)

The POSIX/libc surface ffmpeg needs is overwhelmingly present in `stdc`/`posixc`
(C99 stdio/stdlib/string/math, pthread, clocks, `sysconf`, atomics). This was a
*completeness & correctness* port, not a from-scratch one. Confirmed gaps hit so
far: the libavformat posixc file path (worked around via custom AVIO) and the
libswscale yuv2rgb kernel (worked around via the manual converter). Shares the
`posixc` substrate with [Rust on AROS](../rust-aros/README.md).

## Remaining work

- **libswscale yuv2rgb kernel** fix (then drop the manual converter / use sws for
  scaling too).
- **Codec threading** (frame/slice threads) — currently single-threaded
  (`--disable-pthreads`); the highest-risk runtime layer, untested under load.
- **aarch64 NEON asm** (currently `--disable-asm`) — a perf layer, not required.
- **Audio output** via AHI, then A/V sync in FFView.
- **Datatype / MultiView** surface on `libavcodec` so the desktop opens media.
- A scaled display (fit-to-window) and an ASL file requester in FFView.

---

*A native software port (ARM code built for AROS), not a `hostlib` bridge. The
deferred VideoToolbox/AVFoundation host-decode idea is separate (see the features
index).*
