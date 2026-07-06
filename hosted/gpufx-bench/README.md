# gpufx-bench — video conversion: software vs the 3D shim

A Rust program (`C:GpuFxBench`) that measures the YUV420 -> RGBA color
conversion a video player runs on every decoded frame, both ways:

- **software** — a pure-Rust BT.601 kernel, `-O3`/LTO (the honest baseline);
- **GPU (3D shim)** — the cocoametal display shim's `cm_gpu_convert_yuv420`
  compute path ([gpufx](../../docs/features/gpufx/README.md)), which runs on the
  same Metal device the AROS window already owns.

It builds a deterministic 1280x720 and 1920x1080 frame, times both paths, and
reports frames/s, megapixels/s, MB/s, the GPU speedup, and a correctness check
that the two paths agree.

## Results (Apple M-series, 2026-07-06, booted hosted AROS, gpufx ABI 2)

```
[1280x720]  (0.92 MP/frame)
  software (Rust -O3)       4.240 ms/frame    235.9 fps    217.4 MP/s     869 MB/s
  GPU (3D shim)             0.868 ms/frame   1152.3 fps   1062.0 MP/s    4248 MB/s
  => GPU speedup 4.89x vs software
     correctness: OK (matches software limited-range, diff 0)

[1920x1080]  (2.07 MP/frame)
  software (Rust -O3)      10.019 ms/frame     99.8 fps    207.0 MP/s     828 MB/s
  GPU (3D shim)             1.582 ms/frame    632.0 fps   1310.5 MP/s    5242 MB/s
  => GPU speedup 6.33x vs software
     correctness: OK (matches software limited-range, diff 0)
```

The GPU is **5-7x faster**, and the margin grows with resolution: the per-call
cost includes the CPU<->GPU buffer copies (shared-storage `MTLBuffer`s) and the
AROS<->host bridge, which is fixed overhead that more pixels amortize. A
zero-copy present path (gpufx GFX3, feeding the finished frame straight to the
window texture) would remove that copy and widen the gap further. Even so, at
1080p the software path (99 fps) is already near the limit for smooth 60 fps
video once decode + paint are added, while the GPU path (669 fps) has ample
headroom.

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
