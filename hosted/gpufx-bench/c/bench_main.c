/* bench_main.c -- C harness + host-bridge glue for the gpufx video benchmark.
 *
 * C owns AROS startup (hands argc/argv to the rust-aros std) and binds the
 * cocoametal 3D shim's GPU compute entry points through <aros/hostbind.h>,
 * exactly the "Shape (b)" pattern in hosted/hostbind. The benchmark runs
 * inside the Macaros host process, so cocoametal.dylib is already loaded and
 * cm_open has already adopted the display's Metal device+queue as the shared
 * GPU context (cm__gpu_adopt) -- the benchmark measures that same context the
 * window uses, no second device.
 */
#include <proto/exec.h>
#include <proto/dos.h> /* PutStr */
#include <aros/hostbind.h>

#include <stddef.h>

#include "cocoametal.h" /* CmGpuYuvReq / CmGpuScaleReq (gpufx ABI 2) */

#define GFXB_MAGIC 0x47465842u /* "GFXB" */

extern unsigned int gpufx_bench_main(void);

/* Read by std sys/args/aros.rs so std::env::args() works (C owns main). */
int aros_argc = 0;
char **aros_argv = 0;

/* The cm_gpu_* subset of the shim ABI (docs/features/gpufx). Field order MUST
 * match gpufx_syms below (HostBind_Interface binds positionally). ABI 2 takes
 * a request struct by pointer (see cocoametal.h: a scalar arg past the 8th
 * register slot is misread across the AROS->host stack-arg boundary). */
struct GpuFxIFace {
    int (*cm_gpu_open)(void);
    int (*cm_gpu_abi)(void);
    int (*cm_gpu_scale)(const CmGpuScaleReq *req);
    int (*cm_gpu_convert_yuv420)(const CmGpuYuvReq *req);
};

static const char *gpufx_libs[] = { "cocoametal.dylib", (const char *)0 };
static const char *gpufx_syms[] = { "cm_gpu_open", "cm_gpu_abi", "cm_gpu_scale",
                                    "cm_gpu_convert_yuv420", (const char *)0 };

static struct GpuFxIFace *gpufx_bind(void)
{
    static struct GpuFxIFace *iface;
    static int tried;
    if (!tried) {
        ULONG unresolved = 0;
        iface = (struct GpuFxIFace *)HostBind_Interface(gpufx_libs, gpufx_syms,
                                                        &unresolved);
        if (unresolved)
            iface = NULL; /* partial bind: treat as unavailable */
        tried = 1;
    }
    return iface;
}

/* Host high-resolution monotonic clock, in nanoseconds. The AROS hosted
 * CLOCK_MONOTONIC is too coarse to time a single-frame conversion (reads 0),
 * so for the benchmark we borrow the host's clock via HostBind_LibcSym -- the
 * same wall clock the GPU work actually runs against. macOS
 * clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW=4) returns ns directly; -1 if the
 * host libc doesn't provide it (a native build), and Rust falls back to std. */
long long gfxbench_host_nanos(void)
{
    typedef unsigned long long (*nsec_fn)(int);
    static nsec_fn host = 0;
    static int tried = 0;
    if (!tried) {
        host = (nsec_fn)HostBind_LibcSym("clock_gettime_nsec_np");
        tried = 1;
    }
    if (host)
        return (long long)host(4 /* CLOCK_MONOTONIC_RAW */);
    return -1;
}

/* Rust FFI surface. */
int gfxbench_gpu_available(void)
{
    struct GpuFxIFace *g = gpufx_bind();
    if (!g || !g->cm_gpu_open)
        return -1;
    return g->cm_gpu_open(); /* 0 = compute section ready */
}

int gfxbench_gpu_abi(void)
{
    struct GpuFxIFace *g = gpufx_bind();
    return (g && g->cm_gpu_abi) ? g->cm_gpu_abi() : -1;
}

int gfxbench_gpu_convert_yuv420(const void *y, int yStride, const void *u,
                                int uStride, const void *v, int vStride,
                                int w, int h, void *rgba, int dstStride,
                                int fullRange)
{
    struct GpuFxIFace *g = gpufx_bind();
    if (!g || !g->cm_gpu_convert_yuv420)
        return -1;
    CmGpuYuvReq req = { y, u, v, rgba, yStride, uStride, vStride,
                        w, h, dstStride, fullRange };
    return g->cm_gpu_convert_yuv420(&req);
}

int gfxbench_gpu_scale(const void *src, int srcStride, int sw, int sh,
                       void *dst, int dstStride, int dw, int dh, int filter)
{
    struct GpuFxIFace *g = gpufx_bind();
    if (!g || !g->cm_gpu_scale)
        return -1;
    CmGpuScaleReq req = { src, dst, srcStride, sw, sh, dstStride, dw, dh, filter };
    return g->cm_gpu_scale(&req);
}

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    PutStr("[GpuFxBench] running (redirect stdout to a MacRW: file to capture)\n");
    if (gpufx_bench_main() == GFXB_MAGIC) {
        PutStr("[GpuFxBench] done\n");
        return 0;
    }
    PutStr("[GpuFxBench] FAIL\n");
    return 20;
}
