# gpufx.library — GPU-accelerated 2D for hosted AROS (design)

**Status: design only, not built.** This is the plan for giving hosted AROS a
GPU fast-path for pixel work (scale, blit, colour convert, and eventually
scene rasterisation), reusing the Metal device the display already owns. Today
everything is CPU: the [Feraille/gpui_aros](../feraille-gpui/README.md)
renderer is pure tiny-skia, and ffmpeg's colour conversion is a scalar kernel.

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
- **[GFX2]** ffmpeg `libswscale` YUV→RGB routed through `gpufx` (or direct
  `cm_gpu_*`); FFView video verified, output byte-compared to the CPU kernel.
- **[GFX3]** `gpui_aros` present/scale through `gpufx` — hand the finished RGBA
  frame to `cm_gpu_scale` for the upload+present instead of the CPU
  `WritePixelArray` blit. **This is what yields a gpui software-vs-GPU number**
  (today gpui is CPU-only, so there is none). Pairs with a gpui-scene benchmark
  alongside the video one. Software fallback verified identical.
- **[GFX4]** (optional) transparent `graphics.library` scale/`CopyBox` hook.

## Current state (2026-07-07)

Built and verified on booted AROS: **GFX0 + GFX1** — the shim compute section
(ABI 2), the Rust video benchmark (5-6× vs software, diff 0), and
**`gpufx.library`** as the AROS-native front door (`C:GpuFxTest` = `GPUFX: PASS`,
GPU path AVAILABLE). GFX2/GFX3 unstarted. Recommended next: **GFX3** (route
gpui's present/scale through gpufx) — it unlocks the gpui software-vs-GPU number
the video benchmark can't give — then **GFX2** (the ffmpeg consumer).

## Risks

- Keeping the shared queue's ordering correct between window present and compute
  submissions (fences/ordering on one queue).
- Texture lifetime across the host-bridge boundary (who owns/frees).
- Determinism for the unattended test loop — GPU output must be verifiable
  (byte-compare against the CPU path with a fixed input).
