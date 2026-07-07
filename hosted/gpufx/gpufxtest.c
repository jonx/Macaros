/* gpufxtest.c -- C:GpuFxTest, the gpufx.library driver + verifier.
 *
 * Opens gpufx.library, reports whether the GPU path is up, then runs a scale
 * (nearest + bilinear) and a YUV420->RGBA convert through the library and
 * byte-compares the result against an in-test CPU reference (the same formulas
 * the shim's Metal kernels and gpufx's own fallback use). Whether the library
 * ran the op on the GPU or on its CPU fallback, the output must match the
 * reference within rounding. Prints GPUFX: PASS on success.
 *
 * Redirect stdout to a host-visible file to capture it under automation:
 *   GpuFxTest >MacRW:gpufxtest.out
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/gpufx.h>
#include <proto/gpufx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct Library *GfxFxBase;

static UBYTE clamp255(float v)
{
    v += 0.5f;
    if (v < 0.0f)   return 0;
    if (v > 255.0f) return 255;
    return (UBYTE)v;
}

static void ref_scale(const UBYTE *src, int ss, int sw, int sh,
                      UBYTE *dst, int ds, int dw, int dh, int filter)
{
    int x, y, c;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++)
        {
            float u = (x + 0.5f) * (float)sw / (float)dw;
            float v = (y + 0.5f) * (float)sh / (float)dh;
            UBYTE *d = dst + y * ds + x * 4;
            if (!filter)
            {
                int ix = (int)u, iy = (int)v;
                if (ix > sw - 1) ix = sw - 1;
                if (iy > sh - 1) iy = sh - 1;
                memcpy(d, src + iy * ss + ix * 4, 4);
                continue;
            }
            {
                float fu = u - 0.5f, fv = v - 0.5f;
                int x0 = (int)(fu < 0 ? 0 : fu), y0 = (int)(fv < 0 ? 0 : fv);
                int x1, y1; float ax, ay;
                if (x0 > sw - 1) x0 = sw - 1;
                if (y0 > sh - 1) y0 = sh - 1;
                x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
                y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
                ax = fu - x0; if (ax < 0) ax = 0; if (ax > 1) ax = 1;
                ay = fv - y0; if (ay < 0) ay = 0; if (ay > 1) ay = 1;
                for (c = 0; c < 4; c++)
                {
                    float s00 = src[y0*ss+x0*4+c], s10 = src[y0*ss+x1*4+c];
                    float s01 = src[y1*ss+x0*4+c], s11 = src[y1*ss+x1*4+c];
                    float top = s00 + (s10 - s00) * ax;
                    float bot = s01 + (s11 - s01) * ax;
                    d[c] = clamp255(top + (bot - top) * ay);
                }
            }
        }
}

static void ref_yuv(const UBYTE *yp, int ys, const UBYTE *up, int us,
                    const UBYTE *vp, int vs, int w, int h,
                    UBYTE *dst, int ds, int full)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            float Y = yp[y*ys + x];
            float U = up[(y>>1)*us + (x>>1)] - 128.0f;
            float V = vp[(y>>1)*vs + (x>>1)] - 128.0f;
            float r, g, b;
            UBYTE *d = dst + y*ds + x*4;
            if (full) { r = Y + 1.402000f*V; g = Y - 0.344136f*U - 0.714136f*V; b = Y + 1.772000f*U; }
            else { float C = 1.164383f*(Y-16.0f); r = C + 1.596027f*V; g = C - 0.391762f*U - 0.812968f*V; b = C + 2.017232f*U; }
            d[0]=clamp255(r); d[1]=clamp255(g); d[2]=clamp255(b); d[3]=255;
        }
}

static int max_diff(const UBYTE *a, const UBYTE *b, int n)
{
    int i, w = 0;
    for (i = 0; i < n; i++) { int dd = abs((int)a[i] - (int)b[i]); if (dd > w) w = dd; }
    return w;
}

int main(void)
{
    GfxFxBase = OpenLibrary("gpufx.library", 0);
    if (!GfxFxBase)
    {
        printf("GPUFX: FAIL -- OpenLibrary(gpufx.library) failed\n");
        return 20;
    }

    LONG avail = GfxFx_Available();
    printf("gpufx.library open; GPU path %s\n", avail ? "AVAILABLE" : "unavailable (CPU fallback)");

    int fails = 0;

    /* ---- scale (nearest byte-exact, bilinear +/-1) ---- */
    enum { SW = 317, SH = 201, DW = 640, DH = 480 };
    static UBYTE src[SH*SW*4], gout[DH*DW*4], rout[DH*DW*4];
    int i;
    for (i = 0; i < SH*SW; i++) { src[i*4]=(UBYTE)i; src[i*4+1]=(UBYTE)(i>>3); src[i*4+2]=(UBYTE)(i*5+7); src[i*4+3]=255; }

    {
        struct GfxFxScaleReq sc;
        sc.src = src; sc.dst = gout; sc.srcStride = SW*4; sc.sw = SW; sc.sh = SH;
        sc.dstStride = DW*4; sc.dw = DW; sc.dh = DH; sc.filter = 0;
        if (GfxFx_Scale(&sc) != 0) { printf("GPUFX: FAIL -- scale nearest returned nonzero\n"); fails++; }
        else {
            ref_scale(src, SW*4, SW, SH, rout, DW*4, DW, DH, 0);
            int d = max_diff(gout, rout, DH*DW*4);
            if (d != 0) { printf("GPUFX: FAIL -- scale nearest diff %d\n", d); fails++; }
            else printf("[scale nearest] OK (byte-exact)\n");
        }
        sc.filter = 1;
        if (GfxFx_Scale(&sc) != 0) { printf("GPUFX: FAIL -- scale bilinear returned nonzero\n"); fails++; }
        else {
            ref_scale(src, SW*4, SW, SH, rout, DW*4, DW, DH, 1);
            int d = max_diff(gout, rout, DH*DW*4);
            if (d > 1) { printf("GPUFX: FAIL -- scale bilinear diff %d\n", d); fails++; }
            else printf("[scale bilinear] OK (max diff %d)\n", d);
        }
    }

    /* ---- YUV420 -> RGBA (limited + full, byte-verify) ---- */
    {
        enum { YW = 321, YH = 203, CW = (YW+1)/2, CH = (YH+1)/2 };
        static UBYTE yb[YH*YW], ub[CH*CW], vb[CH*CW], gy[YH*YW*4], ry[YH*YW*4];
        int full;
        for (i = 0; i < YH*YW; i++) yb[i] = (UBYTE)(i*3);
        for (i = 0; i < CH*CW; i++) { ub[i] = (UBYTE)(i*5+7); vb[i] = (UBYTE)(255 - i*11); }
        for (full = 0; full <= 1; full++)
        {
            struct GfxFxYuvReq yr;
            yr.y = yb; yr.u = ub; yr.v = vb; yr.rgba = gy;
            yr.yStride = YW; yr.uStride = CW; yr.vStride = CW;
            yr.w = YW; yr.h = YH; yr.dstStride = YW*4; yr.fullRange = full;
            if (GfxFx_ConvertYUV420(&yr) != 0) { printf("GPUFX: FAIL -- yuv full=%d returned nonzero\n", full); fails++; continue; }
            ref_yuv(yb, YW, ub, CW, vb, CW, YW, YH, ry, YW*4, full);
            int d = max_diff(gy, ry, YH*YW*4);
            if (d > 2) { printf("GPUFX: FAIL -- yuv full=%d diff %d\n", full, d); fails++; }
            else printf("[yuv420 full=%d] OK (max diff %d)\n", full, d);
        }
    }

    CloseLibrary(GfxFxBase);

    if (fails == 0) { printf("GPUFX: PASS\n"); return 0; }
    printf("GPUFX: %d check(s) failed\n", fails);
    return 20;
}
