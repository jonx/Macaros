/* gpu_test.c -- host test for the GPU compute section (docs/features/gpufx).
 *
 * Plain C, links none of the .m files: dlopens build/cocoametal.dylib the
 * way HostLib does and dlsym's only the cm_gpu_* contract, then verifies:
 *
 *   [GPU0] section opens, cm_gpu_abi matches CM_GPU_ABI
 *   [GPU1] nearest scale == CPU reference, byte-exact (same arithmetic)
 *   [GPU2] bilinear scale within +/-1 of the CPU reference (float order)
 *   [GPU3] YUV420->RGBA (limited + full range) within +/-2 of CPU reference
 *
 * The CPU references mirror cs_scale_rgba / cs_yuv420_rgba op for op —
 * they are the same formulas gpufx consumers use as software fallback.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cocoametal.h"

typedef int (*fn_open)(void);
typedef int (*fn_abi)(void);
typedef int (*fn_scale)(const CmGpuScaleReq *);
typedef int (*fn_yuv)(const CmGpuYuvReq *);

static unsigned char clamp255(float v)
{
    return (unsigned char)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
}

/* CPU reference of cs_scale_rgba (keep in sync). */
static void ref_scale(const unsigned char *src, int srcStride, int sw,
                      int sh, unsigned char *dst, int dstStride, int dw,
                      int dh, int filter)
{
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            float u = (x + 0.5f) * (float)sw / (float)dw;
            float v = (y + 0.5f) * (float)sh / (float)dh;
            unsigned char *d = dst + y * dstStride + x * 4;
            if (!filter) {
                int ix = (int)u;
                int iy = (int)v;
                if (ix > sw - 1) ix = sw - 1;
                if (iy > sh - 1) iy = sh - 1;
                memcpy(d, src + iy * srcStride + ix * 4, 4);
                continue;
            }
            float fu = u - 0.5f, fv = v - 0.5f;
            int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x0 > sw - 1) x0 = sw - 1;
            if (y0 > sh - 1) y0 = sh - 1;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
            float ax = fu - (float)x0, ay = fv - (float)y0;
            if (ax < 0.0f) ax = 0.0f;
            if (ax > 1.0f) ax = 1.0f;
            if (ay < 0.0f) ay = 0.0f;
            if (ay > 1.0f) ay = 1.0f;
            for (int c = 0; c < 4; c++) {
                float s00 = src[y0 * srcStride + x0 * 4 + c];
                float s10 = src[y0 * srcStride + x1 * 4 + c];
                float s01 = src[y1 * srcStride + x0 * 4 + c];
                float s11 = src[y1 * srcStride + x1 * 4 + c];
                float top = s00 + (s10 - s00) * ax;
                float bot = s01 + (s11 - s01) * ax;
                d[c] = clamp255(top + (bot - top) * ay + 0.5f);
            }
        }
    }
}

/* CPU reference of cs_yuv420_rgba (keep in sync). */
static void ref_yuv(const unsigned char *yp, int yStride,
                    const unsigned char *up, int uStride,
                    const unsigned char *vp, int vStride, int w, int h,
                    unsigned char *dst, int dstStride, int fullRange)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float Y = yp[y * yStride + x];
            float U = up[(y >> 1) * uStride + (x >> 1)] - 128.0f;
            float V = vp[(y >> 1) * vStride + (x >> 1)] - 128.0f;
            float r, g, b;
            if (fullRange) {
                r = Y + 1.402000f * V;
                g = Y - 0.344136f * U - 0.714136f * V;
                b = Y + 1.772000f * U;
            } else {
                float C = 1.164383f * (Y - 16.0f);
                r = C + 1.596027f * V;
                g = C - 0.391762f * U - 0.812968f * V;
                b = C + 2.017232f * U;
            }
            unsigned char *d = dst + y * dstStride + x * 4;
            d[0] = clamp255(r + 0.5f);
            d[1] = clamp255(g + 0.5f);
            d[2] = clamp255(b + 0.5f);
            d[3] = 255;
        }
    }
}

static int max_diff(const unsigned char *a, const unsigned char *b, int n)
{
    int worst = 0;
    for (int i = 0; i < n; i++) {
        int d = abs((int)a[i] - (int)b[i]);
        if (d > worst)
            worst = d;
    }
    return worst;
}

int main(void)
{
    void *dl = dlopen("build/cocoametal.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        fprintf(stderr, "[GPU] FAIL: dlopen: %s\n", dlerror());
        return 1;
    }
    fn_open gpu_open = (fn_open)dlsym(dl, "cm_gpu_open");
    fn_abi gpu_abi = (fn_abi)dlsym(dl, "cm_gpu_abi");
    fn_scale gpu_scale = (fn_scale)dlsym(dl, "cm_gpu_scale");
    fn_yuv gpu_yuv = (fn_yuv)dlsym(dl, "cm_gpu_convert_yuv420");
    if (!gpu_open || !gpu_abi || !gpu_scale || !gpu_yuv) {
        fprintf(stderr, "[GPU] FAIL: missing cm_gpu_* symbols\n");
        return 1;
    }
    if (gpu_open() != 0) {
        fprintf(stderr, "[GPU] FAIL: cm_gpu_open\n");
        return 1;
    }
    if (gpu_abi() != CM_GPU_ABI) {
        fprintf(stderr, "[GPU] FAIL: abi %d != %d\n", gpu_abi(), CM_GPU_ABI);
        return 1;
    }
    printf("[GPU0] PASS open + abi %d\n", CM_GPU_ABI);

    /* Deterministic source image: gradients + hash noise. */
    enum { SW = 317, SH = 201, DW = 640, DH = 480, DW2 = 123, DH2 = 77 };
    static unsigned char src[SH * SW * 4];
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            unsigned char *p = src + (y * SW + x) * 4;
            p[0] = (unsigned char)x;
            p[1] = (unsigned char)y;
            p[2] = (unsigned char)((x * 7 + y * 13) ^ (x >> 2));
            p[3] = 255;
        }

    static unsigned char gout[DH * DW * 4], rout[DH * DW * 4];

    /* [GPU1] nearest, upscale + downscale: byte-exact vs the reference. */
    CmGpuScaleReq sc = { src, gout, SW * 4, SW, SH, DW * 4, DW, DH, 0 };
    if (gpu_scale(&sc) != 0) {
        fprintf(stderr, "[GPU1] FAIL: cm_gpu_scale nearest\n");
        return 1;
    }
    ref_scale(src, SW * 4, SW, SH, rout, DW * 4, DW, DH, 0);
    if (memcmp(gout, rout, DH * DW * 4) != 0) {
        fprintf(stderr, "[GPU1] FAIL: nearest upscale mismatch\n");
        return 1;
    }
    CmGpuScaleReq sc2 = { src, gout, SW * 4, SW, SH, DW2 * 4, DW2, DH2, 0 };
    if (gpu_scale(&sc2) != 0
        || (ref_scale(src, SW * 4, SW, SH, rout, DW2 * 4, DW2, DH2, 0),
            memcmp(gout, rout, DH2 * DW2 * 4) != 0)) {
        fprintf(stderr, "[GPU1] FAIL: nearest downscale mismatch\n");
        return 1;
    }
    printf("[GPU1] PASS nearest scale byte-exact (%dx%d->%dx%d, ->%dx%d)\n",
           SW, SH, DW, DH, DW2, DH2);

    /* [GPU2] bilinear: within +/-1 (GPU may fuse multiply-adds). */
    sc.filter = 1;
    if (gpu_scale(&sc) != 0) {
        fprintf(stderr, "[GPU2] FAIL: cm_gpu_scale bilinear\n");
        return 1;
    }
    ref_scale(src, SW * 4, SW, SH, rout, DW * 4, DW, DH, 1);
    int diff = max_diff(gout, rout, DH * DW * 4);
    if (diff > 1) {
        fprintf(stderr, "[GPU2] FAIL: bilinear max diff %d\n", diff);
        return 1;
    }
    printf("[GPU2] PASS bilinear scale (max channel diff %d)\n", diff);

    /* [GPU3] YUV420 -> RGBA, limited + full range, odd size. */
    enum { YW = 321, YH = 203, CW = (YW + 1) / 2, CH = (YH + 1) / 2 };
    static unsigned char yb[YH * YW], ub[CH * CW], vb[CH * CW];
    for (int i = 0; i < YH * YW; i++)
        yb[i] = (unsigned char)(i * 3);
    for (int i = 0; i < CH * CW; i++) {
        ub[i] = (unsigned char)(i * 5 + 7);
        vb[i] = (unsigned char)(255 - i * 11);
    }
    static unsigned char gy[YH * YW * 4], ry[YH * YW * 4];
    for (int full = 0; full <= 1; full++) {
        CmGpuYuvReq yr = { yb, ub, vb, gy, YW, CW, CW, YW, YH, YW * 4, full };
        if (gpu_yuv(&yr) != 0) {
            fprintf(stderr, "[GPU3] FAIL: cm_gpu_convert_yuv420 full=%d\n",
                    full);
            return 1;
        }
        ref_yuv(yb, YW, ub, CW, vb, CW, YW, YH, ry, YW * 4, full);
        diff = max_diff(gy, ry, YH * YW * 4);
        if (diff > 2) {
            fprintf(stderr, "[GPU3] FAIL: yuv full=%d max diff %d\n", full,
                    diff);
            return 1;
        }
        printf("[GPU3] PASS yuv420->rgba full=%d (max channel diff %d)\n",
               full, diff);
    }

    printf("[GPU] PASS all\n");
    return 0;
}
