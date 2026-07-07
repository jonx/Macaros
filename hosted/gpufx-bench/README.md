# gpufx-bench — software vs the 3D shim (video convert + gpui scale)

A Rust program (`C:GpuFxBench`) that measures the two pixel hot paths gpufx
accelerates, each software vs GPU:

- **video** — YUV420 -> RGBA color conversion (what a player runs per decoded
  frame; `libswscale`'s job), via `cm_gpu_convert_yuv420`;
- **gpui present-scale** — RGBA bilinear upscale (render at 1x, GPU-upscale to a
  2x HiDPI window — "dynamic resolution"), via `cm_gpu_scale`.

Software is a pure-Rust `-O3`/LTO kernel (the honest baseline); GPU is the
cocoametal shim's compute path ([gpufx](../../docs/features/gpufx/README.md)),
on the same Metal device the AROS window already owns. Each path is timed with
the host ns clock and byte-verified against the software reference.

## Results (Apple M-series, 2026-07-07, booted hosted AROS, gpufx ABI 2)

Video conversion (YUV420 -> RGBA, BT.601 limited):

```
[1280x720]   software  4.39 ms  228 fps  |  GPU  0.88 ms  1131 fps  => 4.96x  (diff 0)
[1920x1080]  software 10.89 ms   92 fps  |  GPU  1.58 ms   634 fps  => 6.90x  (diff 0)
```

gpui present-scale (RGBA bilinear upscale, render 1x -> HiDPI 2x):

```
[640x400  -> 1280x800]   software  9.85 ms  102 fps  |  GPU 1.02 ms  986 fps  =>  9.71x  (diff 0)
[1280x800 -> 2560x1600]  software 38.98 ms   26 fps  |  GPU 3.00 ms  334 fps  => 13.00x  (diff 0)
```

Video: GPU **5-7x**, margin growing with resolution (the per-call CPU<->GPU copy
+ AROS<->host bridge is fixed overhead more pixels amortize). At 1080p software
(92 fps) is already near the smooth-video limit once decode + paint are added;
GPU (634 fps) has ample headroom.

gpui scale: GPU **10-13x**, larger because bilinear scaling is heavier per output
pixel and the CPU cost grows with output size — at 2560x1600 the software upscale
is 39 ms/frame (26 fps, unusable for a smooth UI) while the GPU does it in 3 ms.

**Honest scope for gpui.** This measures the *scale* step, which is the only
part of gpui's present that gpufx accelerates. gpui's real per-frame cost is
CPU *rasterization* (tiny-skia turning the scene into pixels), which gpufx does
NOT touch, and gpui's default 1:1 present already ends in Metal via the shim.
So the 10-13x is the win *if* gpui adopts a dynamic-resolution present (render
low, GPU-upscale — a quality/speed tradeoff), not a blanket "gpui is 13x
faster." A full gpui GPU speedup would need a Metal scene renderer (gpufx GFX4,
out of scope).

## Two things this benchmark surfaced

1. **The AROS hosted `CLOCK_MONOTONIC` is too coarse to time a frame** (per-call
   `std::time::Instant` reads zero). The benchmark borrows the **host** ns clock
   (`clock_gettime_nsec_np` via `HostBind_LibcSym`) — the same wall clock the GPU
   work runs against — so a short, safe frame count gives accurate numbers.
   (A long pure-compute Rust loop that never yields to AROS also faults under
   SIGALRM preemption, so short bounded loops are required regardless.)

2. **A trailing argument was misread across the AROS->host call (found, fixed).**
   With gpufx ABI 1, `cm_gpu_convert_yuv420`'s 11th argument (`fullRange`,
   stack-passed on AArch64) reached the GPU as garbage-nonzero even when 0 was
   passed, so the GPU always computed full-range. Root cause: an AROS->host call
   passes arguments 9+ on the stack, and the AArch64-ELF (AROS caller) and
   Apple-arm64 (host dylib) stack-argument layouts differ; host-direct calls
   (`make cocoametal-gpu`) were correct, so it was purely a marshalling bug. Fixed
   in gpufx ABI 2 by passing a request **struct by pointer** (one register arg,
   no stack args) — the benchmark now reports `correctness: OK ... diff 0`. The
   general rule for this port: **a host-facing shim call with more than 8
   register-class arguments is unsafe; use a struct pointer.**

## Build & run

```sh
# build + deploy C:GpuFxBench into the boot image
hosted/gpufx-bench/build.sh

# on booted AROS (redirect to a host-visible file; posixc write honors it):
GpuFxBench >MacRW:bench.out
```

The benchmark host-binds `cm_gpu_*` from `cocoametal.dylib` via
`<aros/hostbind.h>` (the hosted/hostbind "Shape (b)" pattern). It runs inside
the Macaros host process, so the shim's `cm_open` has already adopted the
display's Metal device+queue as the shared GPU context — the benchmark measures
that same context the window uses, not a second device. Without the GPU path
(native build, or no shim) it reports software-only.
