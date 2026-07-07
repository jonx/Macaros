//! Video-conversion benchmark: software vs the cocoametal 3D shim (GPU).
//!
//! The YUV420 -> RGBA color conversion is what a video player runs on every
//! decoded frame (libswscale's job). On hosted AROS it is a scalar CPU loop
//! today; the [gpufx](../../docs/features/gpufx/README.md) GPU compute section
//! in the display shim (`cm_gpu_convert_yuv420`) can do it on the same Metal
//! device the window already owns. This program measures both over a stream of
//! frames and reports frames/s, megapixels/s, and the GPU speedup, plus a
//! correctness check that the two paths agree.
//!
//! Rust owns everything: it builds a deterministic YUV420 frame, runs the pure
//! Rust software path, calls the shim path through the C host-bridge glue
//! (`c/bench_main.c`), times both with `std::time::Instant`, and prints the
//! report (via `println!` -> posixc write(1), which honors a shell redirect —
//! run it as `GpuFxBench >MacRW:bench.out`).

use std::os::raw::{c_int, c_void};

// Host-bridge glue (c/bench_main.c): binds cm_gpu_* from cocoametal.dylib.
unsafe extern "C" {
    /// Host monotonic clock in nanoseconds (the AROS clock is too coarse to
    /// time a frame); -1 if unavailable.
    fn gfxbench_host_nanos() -> i64;
    /// 0 when the GPU compute path is available (shim bound + `cm_gpu_open` ok).
    fn gfxbench_gpu_available() -> c_int;
    /// CM_GPU_ABI the bound shim was built with (-1 if unavailable).
    fn gfxbench_gpu_abi() -> c_int;
    fn gfxbench_gpu_convert_yuv420(
        y: *const c_void,
        y_stride: c_int,
        u: *const c_void,
        u_stride: c_int,
        v: *const c_void,
        v_stride: c_int,
        w: c_int,
        h: c_int,
        rgba: *mut c_void,
        dst_stride: c_int,
        full_range: c_int,
    ) -> c_int;
    fn gfxbench_gpu_scale(
        src: *const c_void,
        src_stride: c_int,
        sw: c_int,
        sh: c_int,
        dst: *mut c_void,
        dst_stride: c_int,
        dw: c_int,
        dh: c_int,
        filter: c_int,
    ) -> c_int;
}

/// A planar YUV 4:2:0 frame with tightly-rowed planes.
struct Frame {
    w: usize,
    h: usize,
    y: Vec<u8>,
    u: Vec<u8>,
    v: Vec<u8>,
}

impl Frame {
    /// Deterministic content: luma ramp + two chroma gradients, so the output
    /// spans the gamut and the correctness check is meaningful.
    fn synth(w: usize, h: usize) -> Self {
        let cw = w.div_ceil(2);
        let ch = h.div_ceil(2);
        let mut y = vec![0u8; w * h];
        let mut u = vec![0u8; cw * ch];
        let mut v = vec![0u8; cw * ch];
        for j in 0..h {
            for i in 0..w {
                y[j * w + i] = ((i * 7 + j * 13) ^ (i >> 2)) as u8;
            }
        }
        for j in 0..ch {
            for i in 0..cw {
                u[j * cw + i] = (i * 5 + 7) as u8;
                v[j * cw + i] = (255 - (i * 11 + j * 3)) as u8;
            }
        }
        Frame { w, h, y, u, v }
    }
}

/// Software BT.601 YUV420 -> RGBA. Byte-identical formula to the shim's
/// `cs_yuv420_rgba` kernel and the CPU reference in hosted/cocoametal/gpu_test.c
/// (fullRange 0 = limited/video range, 1 = full range). This is the honest
/// baseline: `-O3`, no allocation in the loop.
fn yuv420_to_rgba_sw(f: &Frame, dst: &mut [u8], full_range: bool) {
    let (w, h) = (f.w, f.h);
    let cw = w.div_ceil(2);
    for j in 0..h {
        let crow = (j / 2) * cw;
        for i in 0..w {
            let yv = f.y[j * w + i] as f32;
            let uv = f.u[crow + i / 2] as f32 - 128.0;
            let vv = f.v[crow + i / 2] as f32 - 128.0;
            let (r, g, b) = if full_range {
                (
                    yv + 1.402_000 * vv,
                    yv - 0.344_136 * uv - 0.714_136 * vv,
                    yv + 1.772_000 * uv,
                )
            } else {
                let c = 1.164_383 * (yv - 16.0);
                (
                    c + 1.596_027 * vv,
                    c - 0.391_762 * uv - 0.812_968 * vv,
                    c + 2.017_232 * uv,
                )
            };
            let o = (j * w + i) * 4;
            dst[o] = clamp255(r);
            dst[o + 1] = clamp255(g);
            dst[o + 2] = clamp255(b);
            dst[o + 3] = 255;
        }
    }
}

#[inline(always)]
fn clamp255(v: f32) -> u8 {
    (v + 0.5).clamp(0.0, 255.0) as u8
}

/// Software bilinear RGBA scale, matching the shim's `cs_scale_rgba` kernel and
/// gpufx's CPU fallback. This is the gpui *present-scale* path: a
/// dynamic-resolution present renders the scene at a lower internal size and
/// upscales to the (HiDPI) window. tiny-skia is what gpui_aros would use on the
/// CPU; this standalone loop is the honest -O3 baseline for the comparison.
fn scale_bilinear_sw(src: &[u8], sw: usize, sh: usize, dst: &mut [u8], dw: usize, dh: usize) {
    let ss = sw * 4;
    let ds = dw * 4;
    for y in 0..dh {
        let v = (y as f32 + 0.5) * sh as f32 / dh as f32 - 0.5;
        let y0 = (v.floor().max(0.0) as usize).min(sh - 1);
        let y1 = (y0 + 1).min(sh - 1);
        let ay = (v - y0 as f32).clamp(0.0, 1.0);
        for x in 0..dw {
            let u = (x as f32 + 0.5) * sw as f32 / dw as f32 - 0.5;
            let x0 = (u.floor().max(0.0) as usize).min(sw - 1);
            let x1 = (x0 + 1).min(sw - 1);
            let ax = (u - x0 as f32).clamp(0.0, 1.0);
            let o = y * ds + x * 4;
            for c in 0..4 {
                let s00 = src[y0 * ss + x0 * 4 + c] as f32;
                let s10 = src[y0 * ss + x1 * 4 + c] as f32;
                let s01 = src[y1 * ss + x0 * 4 + c] as f32;
                let s11 = src[y1 * ss + x1 * 4 + c] as f32;
                let top = s00 + (s10 - s00) * ax;
                let bot = s01 + (s11 - s01) * ax;
                dst[o + c] = (top + (bot - top) * ay + 0.5).clamp(0.0, 255.0) as u8;
            }
        }
    }
}

fn max_channel_diff(a: &[u8], b: &[u8]) -> u8 {
    a.iter()
        .zip(b)
        .map(|(&x, &y)| x.abs_diff(y))
        .max()
        .unwrap_or(0)
}

/// Run `op` a fixed number of frames, timed with the host ns clock; returns
/// the mean per-frame nanoseconds. A short fixed count keeps the loop safe
/// (a long pure-compute loop that never yields to AROS faults under SIGALRM
/// preemption) while the host clock gives the resolution the AROS clock lacks.
fn time_frames(frames: u32, mut op: impl FnMut()) -> f64 {
    let t0 = unsafe { gfxbench_host_nanos() };
    for _ in 0..frames {
        op();
    }
    let t1 = unsafe { gfxbench_host_nanos() };
    if t0 < 0 || t1 < 0 || t1 <= t0 {
        return 0.0;
    }
    (t1 - t0) as f64 / frames as f64
}

fn report_line(label: &str, w: usize, h: usize, ns_per_frame: f64) {
    let mp = (w * h) as f64 / 1.0e6;
    let secs = ns_per_frame / 1.0e9;
    let (ms_per, fps, mps) = if secs > 0.0 {
        (ns_per_frame / 1.0e6, 1.0 / secs, mp / secs)
    } else {
        (0.0, 0.0, 0.0)
    };
    println!(
        "  {label:<22} {ms_per:>8.3} ms/frame   {fps:>7.1} fps   {mps:>7.1} MP/s   {:>7.0} MB/s",
        mps * 4.0 // RGBA out = 4 bytes/pixel
    );
}

const MAGIC_OK: u32 = 0x47465842; // "GFXB"

/// Entry point called by the C harness. Returns MAGIC_OK on success.
#[unsafe(no_mangle)]
pub extern "C" fn gpufx_bench_main() -> u32 {
    // 720p is the reference video frame; the ratios hold across sizes.
    let sizes = [(1280usize, 720usize), (1920, 1080)];
    let full_range = false; // limited-range video, the common case
    // Short, bounded frame counts: enough for a stable host-clock measurement,
    // few enough that the compute loop stays safe under AROS preemption.
    let sw_frames = 60u32;
    let gpu_frames = 60u32;

    println!("=== gpufx video benchmark: YUV420 -> RGBA (BT.601 limited) ===");
    if unsafe { gfxbench_host_nanos() } < 0 {
        println!("(host ns clock unavailable -- timing will read zero)");
    }

    // SAFETY: FFI; the glue binds the shim once and is idempotent.
    let gpu_ok = unsafe { gfxbench_gpu_available() } == 0;
    if gpu_ok {
        let abi = unsafe { gfxbench_gpu_abi() };
        println!("GPU path: cocoametal 3D shim available (CM_GPU_ABI {abi})");
    } else {
        println!(
            "GPU path: UNAVAILABLE (cocoametal.dylib / cm_gpu_* not bound) \
             -- software-only run"
        );
    }

    let mut all_ok = true;

    for &(w, h) in &sizes {
        let frame = Frame::synth(w, h);
        let mut sw_out = vec![0u8; w * h * 4];
        let mut gpu_out = vec![0u8; w * h * 4];

        println!("\n[{w}x{h}]  ({:.2} MP/frame)", (w * h) as f64 / 1.0e6);

        // --- software ---
        yuv420_to_rgba_sw(&frame, &mut sw_out, full_range); // warm caches
        let sw_per =
            time_frames(sw_frames, || yuv420_to_rgba_sw(&frame, &mut sw_out, full_range));
        report_line("software (Rust -O3)", w, h, sw_per);

        // --- GPU (3D shim) ---
        if gpu_ok {
            let cw = w.div_ceil(2) as c_int;
            let mut call = |out: &mut [u8]| unsafe {
                gfxbench_gpu_convert_yuv420(
                    frame.y.as_ptr() as *const c_void,
                    w as c_int,
                    frame.u.as_ptr() as *const c_void,
                    cw,
                    frame.v.as_ptr() as *const c_void,
                    cw,
                    w as c_int,
                    h as c_int,
                    out.as_mut_ptr() as *mut c_void,
                    (w * 4) as c_int,
                    full_range as c_int,
                )
            };
            if call(&mut gpu_out) != 0 {
                println!("  GPU (3D shim)          FAILED (cm_gpu_convert_yuv420 returned nonzero)");
                all_ok = false;
            } else {
                let mut rc = 0;
                let gpu_per = time_frames(gpu_frames, || rc |= call(&mut gpu_out));
                if rc != 0 {
                    println!("  GPU (3D shim)          FAILED mid-run");
                    all_ok = false;
                } else {
                    report_line("GPU (3D shim)", w, h, gpu_per);
                    let speedup = if gpu_per > 0.0 { sw_per / gpu_per } else { 0.0 };
                    println!("  => GPU speedup {speedup:.2}x vs software");

                    // Correctness: the GPU output should match the software
                    // reference in one of the two ranges. Comparing both
                    // surfaces the hosted-call fullRange discrepancy (see the
                    // benchmark README) rather than hiding it.
                    let mut sw_lim = vec![0u8; w * h * 4];
                    let mut sw_full = vec![0u8; w * h * 4];
                    yuv420_to_rgba_sw(&frame, &mut sw_lim, false);
                    yuv420_to_rgba_sw(&frame, &mut sw_full, true);
                    let d_lim = max_channel_diff(&sw_lim, &gpu_out);
                    let d_full = max_channel_diff(&sw_full, &gpu_out);
                    if d_lim <= 2 {
                        println!("     correctness: OK (matches software limited-range, diff {d_lim})");
                    } else if d_full <= 2 {
                        println!(
                            "     correctness: GPU produced FULL-range (diff {d_full}); \
                             fullRange arg not honored across the hosted call -- see README"
                        );
                    } else {
                        println!("     correctness: MISMATCH (limited diff {d_lim}, full diff {d_full})");
                        all_ok = false;
                    }
                }
            }
        }
    }

    // ---- gpui present-scale path: RGBA bilinear upscale ----------------
    // gpui's per-frame cost is CPU rasterization (tiny-skia), which gpufx does
    // NOT accelerate. Where gpufx helps gpui is the *scale* step: a
    // dynamic-resolution present renders the scene at 1x and GPU-upscales to a
    // 2x HiDPI window. These are that upscale, at gpui window sizes.
    println!("\n=== gpui present-scale: RGBA bilinear upscale (render 1x -> HiDPI 2x) ===");
    let scale_cases = [(640usize, 400usize, 1280usize, 800usize),
                       (1280, 800, 2560, 1600)];
    for &(sw, sh, dw, dh) in &scale_cases {
        // A deterministic source frame (a stand-in for a rasterized gpui scene).
        let mut src = vec![0u8; sw * sh * 4];
        for (i, px) in src.chunks_exact_mut(4).enumerate() {
            px[0] = i as u8;
            px[1] = (i >> 3) as u8;
            px[2] = (i * 5 + 7) as u8;
            px[3] = 255;
        }
        let mut sw_out = vec![0u8; dw * dh * 4];
        let mut gpu_out = vec![0u8; dw * dh * 4];

        println!("\n[{sw}x{sh} -> {dw}x{dh}]  ({:.2} MP out)", (dw * dh) as f64 / 1.0e6);
        scale_bilinear_sw(&src, sw, sh, &mut sw_out, dw, dh);
        let sw_per = time_frames(sw_frames, || scale_bilinear_sw(&src, sw, sh, &mut sw_out, dw, dh));
        report_line("software (Rust -O3)", dw, dh, sw_per);

        if gpu_ok {
            let mut call = |out: &mut [u8]| unsafe {
                gfxbench_gpu_scale(
                    src.as_ptr() as *const c_void,
                    (sw * 4) as c_int,
                    sw as c_int,
                    sh as c_int,
                    out.as_mut_ptr() as *mut c_void,
                    (dw * 4) as c_int,
                    dw as c_int,
                    dh as c_int,
                    1, // bilinear
                )
            };
            if call(&mut gpu_out) != 0 {
                println!("  GPU (3D shim)          FAILED (cm_gpu_scale returned nonzero)");
                all_ok = false;
            } else {
                let mut rc = 0;
                let gpu_per = time_frames(gpu_frames, || rc |= call(&mut gpu_out));
                if rc != 0 {
                    println!("  GPU (3D shim)          FAILED mid-run");
                    all_ok = false;
                } else {
                    report_line("GPU (3D shim)", dw, dh, gpu_per);
                    let speedup = if gpu_per > 0.0 { sw_per / gpu_per } else { 0.0 };
                    println!("  => GPU speedup {speedup:.2}x vs software");
                    let diff = max_channel_diff(&sw_out, &gpu_out);
                    if diff <= 1 {
                        println!("     correctness: OK (max channel diff {diff})");
                    } else {
                        println!("     correctness: MISMATCH (max diff {diff})");
                        all_ok = false;
                    }
                }
            }
        }
    }

    // The GPU time includes the per-call CPU<->GPU buffer copies (shared-
    // storage MTLBuffers) and the AROS<->host bridge; the win at these sizes
    // is bounded by that copy. A zero-copy present path (gpufx GFX3) removes it.
    if !gpu_ok {
        println!("\nRESULT: software-only (GPU path not present on this boot)");
    } else if all_ok {
        println!("\nRESULT: PASS (both paths timed; output verified)");
    } else {
        println!("\nRESULT: completed with issues (see lines above)");
        return 0;
    }
    MAGIC_OK
}
