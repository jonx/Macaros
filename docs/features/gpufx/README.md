# gpufx.library — GPU-accelerated 2D for hosted AROS

**Status: GFX0-GFX3 done on booted AROS** (2026-07-07) — the shim compute
section, `gpufx.library` (the AROS-native front door), the gpui dynamic-
resolution present (opt-in), and the ffmpeg consumer (FFViewX, verified live). Measured: **5-7× video-conversion** and **10-13×
gpui present-scale** over software, all output diff 0. Remaining: GFX4 (a full GPU scene rasteriser — multi-week, deferred); see
[Milestones](#milestones-greppable-gfx).
This gives hosted AROS a GPU fast-path for pixel work (scale, colour convert, and
eventually scene rasterisation), reusing the Metal device the display already
owns. The baseline stays CPU — the [Feraille/gpui_aros](../feraille-gpui/README.md)
renderer is pure tiny-skia and ffmpeg's colour conversion is a scalar kernel —
and `gpufx` accelerates it without becoming a hard dependency.

## The core idea: share one GPU context, don't make a second

The [cocoametal display shim](../cocoa-metal-display/design.md)
(`hosted/cocoametal/cocoametal.m`) already holds a live `MTLDevice` (~L179) and
`MTLCommandQueue` (~L363) driving the AROS window. **Add a compute/blit section
to that existing shim** rather than standing up a separate device/dylib — so
the library and the window share one GPU context and can hand textures back and
forth without copies. A second `MTLCreateSystemDefaultDevice` would compete for
the GPU and duplicate memory.

So the shape is:

- **Host side**: new `cm_*` compute/blit entry points in `cocoametal.m`/`.h`
  that encode `MTLComputePipelineState` / blit passes onto the *shared* queue
  (`cx->device`, `cx->queue`). E.g. `cm_gpu_scale`, `cm_gpu_blit`,
  `cm_gpu_convert` (YUV→RGB), `cm_gpu_submit`.
- **AROS side**: a new **`gpufx.library`** — the front door AROS programs call.
  It forwards to the new `cm_*` entry points through the existing
  [host-bridge](../host-bridge/README.md) / hostlib mechanism, the same way the
  Cocoa display HIDD already reaches the shim. Build it as a
  [native module](../native-modules/README.md).

## Integration: explicit first, transparent only once proven

Two integration styles, in order:

1. **Explicit (build this first).** Apps call `gpufx.library` directly. Simple,
   opt-in, deterministically testable. **First consumer: ffmpeg** — its
   `libswscale` YUV→RGB path (currently a faulting/scalar kernel, see
   [ffmpeg-native](../ffmpeg-native/README.md)) is a clean, isolated
   GPU-compute candidate and proves the shared-context blit/scale end to end.
   Feraille/`gpui_aros` then routes its final present/scale (and later, sprite
   compositing) through the same API.
2. **Transparent (later, only after explicit is proven).** Hook the accel
   behind `graphics.library`'s existing scale / `CopyBox` paths so *unmodified*
   apps benefit for free. More elegant but invasive and much harder to verify
   deterministically — do not start here.

**Software stays the baseline.** `gpufx` is an optimisation over an
always-correct CPU path (tiny-skia in `gpui_aros`; the scalar kernel in
ffmpeg). When the library/compute path is unavailable (or on any other
platform), the software path runs unchanged. GPU is never a hard dependency.

## How it plugs into gpui_aros

`gpui_aros` today: tiny-skia rasterises the whole `Scene` into an RGBA buffer,
then `gpa_blit` → cybergraphics `WritePixelArray` → the window RastPort → the
cocoametal shim presents it. gpufx can accelerate this in stages, cheapest
first:

1. **Present/scale** — hand the finished RGBA buffer to `gpufx` for the
   upload+scale+present instead of the CPU blit. Smallest change, real win at
   large window sizes.
2. **Sprite/quad compositing** — offload the per-primitive fills/blits.
3. **Full GPU rasteriser** — the long game (a Metal port of the scene renderer,
   closer to what gpui does on macOS). Only worth it if 1–2 aren't enough.

Start at (1); it needs no change to the renderer's logic, only its output path.

## Milestones (greppable `[GFX*]`)

- **[GFX0] DONE** — `cm_gpu_*` compute section in the shim
  (`hosted/cocoametal/cocoametal_gpu.m`: `cm_gpu_open/abi/scale/convert_yuv420`),
  sharing the display's `MTLDevice`+queue via `cm__gpu_adopt`. Host test
  `make cocoametal-gpu` PASS (nearest byte-exact, bilinear ±1, YUV ±2). A
  separate dlsym contract (`CM_GPU_ABI`); the frozen display CMIFace and
  `CM_ABI_VERSION` are untouched.
  - **On-device measured** ([`hosted/gpufx-bench`](../../../hosted/gpufx-bench/README.md),
    `C:GpuFxBench`, a Rust software-vs-shim video benchmark): YUV420→RGBA is
    **~5× faster on the GPU at 720p, ~6× at 1080p**, output verified byte-equal
    (diff 0) to the software reference.
  - **ABI 2 — struct-by-pointer, and why** (fixed here): ABI 1 passed the ops as
    long argument lists (11 for convert, 9 for scale). Through the AROS→host
    bridge the trailing scalar arg was misread — the benchmark caught the GPU
    computing full-range when limited was requested — because an AROS→host call
    passes arguments 9+ on the stack and the **AArch64-ELF (AROS) and Apple-arm64
    (host dylib) stack-argument layouts differ**. Host-direct calls
    (`make cocoametal-gpu`) honored it, so it was purely a marshalling bug. The
    fix: every op takes a single request **struct by pointer** (`CmGpuScaleReq`,
    `CmGpuYuvReq`), so all fields ride in one register. **Port-wide lesson: a
    host-facing shim call with >8 register-class args is unsafe on this bridge —
    prefer a struct pointer.** Verified: `fullRange` now honored (benchmark diff
    0), `[GPU] PASS all` host-side.
- **[GFX1] DONE** — `gpufx.library`, the AROS-native front door
  (`aros-upstream/arch/all-darwin/libs/gpufx/`, public header
  `compiler/include/libraries/gpufx.h`). Opens `cocoametal.dylib` via
  `hostlib.resource` at library init (under `Disable()` so Metal/dispatch
  threads inherit a blocked SIGALRM mask, same as the cocoa HIDD), resolves
  `cm_gpu_open/abi/scale/convert_yuv420`, and refuses the GPU path unless
  `cm_gpu_abi() == 2`. Public API (request **structs by pointer** — dodges the
  >8-arg hazard on the AROS call side too): `GfxFx_Available()`,
  `GfxFx_Scale(const struct GfxFxScaleReq *)`,
  `GfxFx_ConvertYUV420(const struct GfxFxYuvReq *)`. Each op runs the GPU when
  available, else a **CPU fallback** (`gpufx_fallback.c`, the same formulas as
  the shim kernels / `gpu_test.c`), so a call always succeeds — software is the
  baseline, GPU the fast path. Builds via the `hostlibs-gpufx` metatarget into
  `AROS/Libs/gpufx.library`. **Verified on booted AROS** (`C:GpuFxTest`,
  `hosted/gpufx/`): *GPU path AVAILABLE*, scale byte-exact / bilinear ±1, YUV
  both ranges diff 0, `GPUFX: PASS`.
  - *Note:* consumers can also reach `cm_gpu_*` directly via `HostBind_Interface`
    (the benchmark does) — the library's value is DRY (one bridge + one fallback)
    and being a normal AROS call per the [host-bridge](../host-bridge/README.md)
    convention.
- **[GFX2] DONE (verified live)** — `FFView`/`FFViewX` (`hosted/ffmpeg/ffview.c`)
  opens `gpufx.library` and, for planar 8-bit 4:2:0 frames, runs the per-frame
  colour convert on the GPU (`GfxFx_ConvertYUV420`) plus a GPU bilinear downscale
  (`GfxFx_Scale`) when the video doesn't fit the window, then blits RGBA. Any
  other format, or an absent library, falls straight back to the scalar/sws path
  — no behaviour change. The gpufx usage mirrors the verified `C:GpuFxTest`
  exactly. Verified live on booted AROS (2026-07-08): FFViewX plays a YUV420 clip
  through the GPU convert path. Build: `build.sh` -> `build-video.sh` +
  `build-videox.sh` -> `deploy.sh` (which requires gpufx.library built first, for
  the gpufx headers).
- **[GFX3] DONE (capability + integration)** — the gpui present-scale path.
  - *Capability, measured:* `gpufx-bench`'s gpui-scale section times RGBA
    bilinear upscale (render 1x -> HiDPI 2x) software vs GPU on booted AROS:
    **9.7x at 1280x800, 13.0x at 2560x1600**, output diff 0.
  - *Integration, live:* `gpui_aros` reads `GPUI_AROS_RENDER_SCALE` (< 1.0,
    default 1.0). When set (and `gpufx.library` is present), gpui renders into a
    smaller drawable and GPU-upscales it to the window via `GfxFx_Scale` instead
    of the CPU `WritePixelArray`. Verified on booted AROS at 0.5: a full-window,
    softer render, no crash — `gpui_aros` is now a real `gpufx.library` consumer.
  - *Honest scope:* gpufx accelerates the *scale* step only. gpui's per-frame
    cost is CPU *rasterization* (tiny-skia), which gpufx does not touch, and the
    default 1:1 present already ends in Metal via the shim. So dynamic-resolution
    is a quality/speed tradeoff (softer image, fewer pixels rasterized), off by
    default. A blanket gpui speedup needs GFX4.
- **[GFX4] NOT STARTED (large)** — a full GPU scene rasteriser: a Metal port of
  gpui's quad/sprite/path/shadow renderer, so the *rasterization* (gpui's real
  per-frame cost) runs on the GPU, not just the present-scale. This is what gpui
  does natively on macOS. It is a multi-week effort (reimplementing the renderer
  against the shared Metal device), not a drop-in — deliberately deferred. A
  transparent `graphics.library` scale/`CopyBox` hook (accel for unmodified
  apps) is the other optional GFX4-tier item.

## Current state (2026-07-07)

Done + verified on booted AROS: **GFX0** (shim compute section, ABI 2),
**GFX1** (`gpufx.library`, `C:GpuFxTest` PASS), **GFX3** (gpui dynamic-resolution
present, `GPUI_AROS_RENDER_SCALE`, verified at 0.5). `C:GpuFxBench` measures both
hot paths: video YUV->RGBA (5-7x) and gpui present-scale (10-13x), both diff 0.
**GFX2** (ffmpeg/FFView) is code-complete; on-device verify waits on an ffmpeg
sysroot rebuild. **GFX4** (full GPU scene rasteriser) is deferred (multi-week).
All GPU paths keep a CPU fallback, so software is always the baseline.

## Risks

- Keeping the shared queue's ordering correct between window present and compute
  submissions (fences/ordering on one queue).
- Texture lifetime across the host-bridge boundary (who owns/frees).
- Determinism for the unattended test loop — GPU output must be verifiable
  (byte-compare against the CPU path with a fixed input).
