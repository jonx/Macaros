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
| `build-video.sh` | `sysroot-video` | ~23 MB | **FFView** — common image+video codecs |
| `build-full.sh` | `sysroot-full` | ~88 MB | the *complete* native decoder/demuxer set |

`build-full.sh` proves ffmpeg ports **in full** (every native decoder, demuxer,
parser, bsf, plus swscale/swresample, decode-only). But a binary that links the
whole decoder registry is ~90 MB, and **relocating that at `LoadSeg` is too slow
to be usable** (it never finishes booting). So FFView links `sysroot-video`, a
curated common-format subset, giving a ~29 MB binary that loads. See
[memory & binary size](#memory--binary-size).

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

## Memory & binary size

Hosted AROS RAM is set by `memory <MB>` in `AROSBootstrap.conf` (and
`AROS_HOST_MEMORY`); the bootstrap default is 256 MB. A real Amiga had 0.5–128 MB;
hosted AROS draws from the Mac, so RAM is cheap to raise. The constraints that
bit us:

- **Binary size / relocation, not RAM, is the limit.** The 90 MB full-registry
  binary fits in 256 MB but never finishes `LoadSeg` relocation. Keep the linked
  decoder set curated (`build-video.sh`).
- **Decode working set** is frame buffers: a 320×240 RGB24 frame is ~230 KB, a
  1080p one ~6 MB. Small clips are comfortable; HD needs a RAM bump and prompt
  frame freeing (FFView holds one decode frame + one RGB frame).

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
