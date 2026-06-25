/* d1_test.m — standalone proof of the Cocoa/Metal present pipeline ([D1]),
 * resolution-parametric scaling ([D2]), and the present-time shader stage ([D]).
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/spec.md
 * ("Verification — D1", the resolution-parametric cm_open, and the cm_set_effect
 * shader stage). No GPL emulator source (vAmiga/WinUAE/FS-UAE/Amiberry/E-UAE)
 * was read, searched, or consulted. Models the H7 readback + pixel-assert
 * discipline from this project's hosted/display.c [OURS] (ImageIO PNG encode,
 * "a file existing is not a PASS — the pixels are").
 *
 * What it proves, unattended, with no window required:
 *   [D1] At 320x200: device + queue, a BGRA8 shared framebuffer texture, a
 *        4-quadrant + marker scene uploaded via replaceRegion, rendered through
 *        the pass-through nearest pipeline into the offscreen oracle target,
 *        read back, PNG-encoded, and pixel-asserted (quadrants + marker exact,
 *        target == logical * scale).
 *   [D2] The SAME asserts at a DIFFERENT resolution (640x512) — proves cm_open
 *        is not hard-coded to 320x200 and the present path scales arbitrary WxH.
 *   [D]  The shader stage: render the framebuffer through the scanline effect
 *        (CM_FX_SCANLINE) into an offscreen target and assert it (a) differs
 *        from pass-through and (b) differs in the EXPECTED way — odd target rows
 *        darker than even ones — proving the selectable fragment-effect hook runs.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cocoametal.h"

/* Quadrant colours, stored as 0xAARRGGBB for human readability; we pack them
 * into the BGRA byte order the texture/format expect at upload time. */
#define C_TL 0xFFFF0000u   /* top-left:     red    */
#define C_TR 0xFF00FF00u   /* top-right:    green  */
#define C_BL 0xFF0000FFu   /* bottom-left:  blue   */
#define C_BR 0xFFFFFF00u   /* bottom-right: yellow */
#define C_MARK 0xFFFF00FFu /* marker pixel: magenta */

/* Write one BGRA pixel into a chunky framebuffer of width fbw from 0xAARRGGBB. */
static inline void put_bgra(uint8_t *fb, int fbw, int x, int y, uint32_t argb) {
    uint8_t *p = &fb[((size_t)y * fbw + x) * 4];
    p[0] = (uint8_t)(argb        );  /* B */
    p[1] = (uint8_t)(argb >>  8  );  /* G */
    p[2] = (uint8_t)(argb >> 16  );  /* R */
    p[3] = (uint8_t)(argb >> 24  );  /* A */
}

/* Read a BGRA pixel back as 0xAARRGGBB for comparison. */
static inline uint32_t get_argb(const uint8_t *buf, int stride, int x, int y) {
    const uint8_t *p = &buf[(size_t)y * stride + (size_t)x * 4];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] <<  8) |  (uint32_t)p[0];
}

/* Luma proxy (sum of R+G+B) of a BGRA pixel — enough to compare brightness. */
static inline int luma_sum(const uint8_t *buf, int stride, int x, int y) {
    const uint8_t *p = &buf[(size_t)y * stride + (size_t)x * 4];
    return (int)p[0] + (int)p[1] + (int)p[2];  /* B+G+R */
}

/* H7-style ImageIO PNG encode. The readback buffer is BGRA8; CoreGraphics emits
 * a correct PNG from it via kCGImageAlphaPremultipliedFirst + Little32 byteorder
 * (so byte 0=B,1=G,2=R,3=A reads as ARGB little-endian = what CG calls "First"). */
static int encode_png(const uint8_t *bgra, int w, int h, int stride, const char *path) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void *)bgra, w, h, 8, (size_t)stride, cs,
                          kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    int ok = 0;
    if (ctx) {
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        CFStringRef ps = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, ps, kCFURLPOSIXPathStyle, false);
        CGImageDestinationRef dst =
            CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (dst) {
            CGImageDestinationAddImage(dst, img, NULL);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CFRelease(url); CFRelease(ps); CGImageRelease(img); CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);
    return ok;
}

/* Fill a w*h BGRA framebuffer with the 4-quadrant scene + a marker pixel. */
static void build_scene(uint8_t *fb, int w, int h, int markX, int markY) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = (x < w/2) ? ((y < h/2) ? C_TL : C_BL)
                                   : ((y < h/2) ? C_TR : C_BR);
            put_bgra(fb, w, x, y, c);
        }
    put_bgra(fb, w, markX, markY, C_MARK);
}

/* One resolution case ([D1] at 320x200, [D2] at 640x512). Returns 1 on PASS.
 * tag is the marker printed ("D1"/"D2"); png is where to drop the PNG. */
static int run_res_case(const char *tag, int w, int h, const char *png) {
    int markX = w / 7, markY = h * 7 / 15;   /* arbitrary off-centre, in-bounds */

    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };

    char title[64];
    snprintf(title, sizeof title, "AROS [%s]", tag);
    CMContext *cx = cm_open(w, h, &fmt, title);
    if (!cx) { printf("[%s] FAIL cm_open returned NULL (device present)\n", tag); return 0; }

    uint8_t *fb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!fb) { printf("[%s] FAIL calloc framebuffer\n", tag); cm_close(cx); return 0; }
    build_scene(fb, w, h, markX, markY);
    cm_upload_rect(cx, fb, w * 4, 0, 0, w, h);

    cm_present(cx);

    int tw = 0, th = 0, scale = 0;
    cm_target_size(cx, &tw, &th, &scale);

    int stride = w * 4;
    uint8_t *rb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!rb) { printf("[%s] FAIL calloc readback\n", tag); free(fb); cm_close(cx); return 0; }
    if (cm_readback(cx, rb, stride, w, h) != 0) {
        printf("[%s] FAIL cm_readback\n", tag); free(fb); free(rb); cm_close(cx); return 0;
    }

    int png_ok = encode_png(rb, w, h, stride, png);

    int ok = 1;
    struct { const char *name; int x, y; uint32_t want; } checks[] = {
        { "TL.red",     w/4,   h/4,   C_TL },
        { "TR.green", 3*w/4,   h/4,   C_TR },
        { "BL.blue",    w/4, 3*h/4,   C_BL },
        { "BR.yellow",3*w/4, 3*h/4,   C_BR },
        { "marker",   markX,  markY,  C_MARK },
    };
    for (size_t i = 0; i < sizeof(checks)/sizeof(checks[0]); i++) {
        uint32_t got = get_argb(rb, stride, checks[i].x, checks[i].y);
        int pass = (got == checks[i].want);
        if (!pass) ok = 0;
        printf("[%s]   %-10s (%4d,%4d) want=%08X got=%08X  %s\n",
               tag, checks[i].name, checks[i].x, checks[i].y,
               checks[i].want, got, pass ? "ok" : "MISMATCH");
    }

    int dims_ok = (tw == w * scale && th == h * scale && scale >= 1);
    if (!dims_ok) ok = 0;
    printf("[%s]   target %dx%d (scale %d, expect %dx%d)  %s\n",
           tag, tw, th, scale, w * scale, h * scale, dims_ok ? "ok" : "MISMATCH");

    if (!png_ok) { ok = 0; printf("[%s]   PNG encode FAILED -> %s\n", tag, png); }
    else         { printf("[%s]   png -> %s\n", tag, png); }

    if (ok)
        printf("[%s] PASS quadrants exact + marker(%d,%d)=%08X + target %dx%d@%dx  png=%s\n",
               tag, markX, markY, C_MARK, tw, th, scale, png);
    else
        printf("[%s] FAIL see checks above\n", tag);

    free(fb); free(rb); cm_close(cx);
    return ok;
}

/* [D] shader stage. The scanline effect darkens odd TARGET rows, so we read the
 * effect output at the NATIVE target grid (tw x th) — where every target row is
 * distinct — so the odd/even row parity is directly observable at any host scale.
 * The logical framebuffer is small (so logical rows upscale to whole target-row
 * blocks); the effect is keyed off target-row parity in the fragment shader.
 *
 * Proves three things:
 *   (0) CM_FX_NEAREST through the effect pipeline == the oracle pass-through
 *       readback, byte-for-byte (so "effect 0" really is the verification path).
 *   (1) CM_FX_SCANLINE differs from pass-through somewhere.
 *   (2) That difference has the EXPECTED shape — inside a solid quadrant, odd
 *       target rows are darker than the even target rows above them. */
static int run_shader_case(void) {
    const int w = 256, h = 128;

    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };
    CMContext *cx = cm_open(w, h, &fmt, "AROS [D] shader");
    if (!cx) { printf("[D] FAIL cm_open returned NULL\n"); return 0; }

    int tw = 0, th = 0, scale = 0;
    cm_target_size(cx, &tw, &th, &scale);  /* read the effect output at tw x th */

    uint8_t *fb    = (uint8_t *)calloc((size_t)w  * h,  4);   /* logical upload */
    uint8_t *plain = (uint8_t *)calloc((size_t)tw * th, 4);   /* native readback */
    uint8_t *scan  = (uint8_t *)calloc((size_t)tw * th, 4);
    if (!fb || !plain || !scan) {
        printf("[D] FAIL calloc bufs\n");
        free(fb); free(plain); free(scan); cm_close(cx); return 0;
    }
    build_scene(fb, w, h, 5, 5);
    cm_upload_rect(cx, fb, w * 4, 0, 0, w, h);

    int nstride = tw * 4;
    int r1 = cm_render_effect_readback(cx, CM_FX_NEAREST,  plain, nstride, tw, th);
    int r2 = cm_render_effect_readback(cx, CM_FX_SCANLINE, scan,  nstride, tw, th);
    if (r1 || r2) {
        printf("[D] FAIL effect readback (nearest=%d scanline=%d)\n", r1, r2);
        free(fb); free(plain); free(scan); cm_close(cx); return 0;
    }

    /* (0) effect-0 (the pass-through fragment) must equal the OFFSCREEN ORACLE
     * byte-for-byte: read both at the LOGICAL grid and compare. The oracle uses
     * the fixed pass-through pipeline; effect-0 uses the effect pipeline with the
     * pass-through branch + same vertex/sampler — so they must be identical. This
     * is what guarantees the verification path is unaffected by the shader stage. */
    int lstride = w * 4;
    uint8_t *oracle_log = (uint8_t *)calloc((size_t)w * h, 4);
    uint8_t *eff0_log   = (uint8_t *)calloc((size_t)w * h, 4);
    int eff0_matches = 0;
    if (oracle_log && eff0_log) {
        cm_present(cx);                                       /* oracle target */
        int ro = cm_readback(cx, oracle_log, lstride, w, h);  /* oracle (logical) */
        int re = cm_render_effect_readback(cx, CM_FX_NEAREST, eff0_log, lstride, w, h);
        eff0_matches = (ro == 0 && re == 0 &&
                        memcmp(oracle_log, eff0_log, (size_t)lstride * h) == 0);
    }
    free(oracle_log); free(eff0_log);

    /* (1) scanline must differ from pass-through. */
    int differs = (memcmp(plain, scan, (size_t)nstride * th) != 0);

    /* (2) odd target rows darker than even, inside the solid TL quadrant. */
    int col = tw / 4, db = 0, bb = 0, n = 0;
    for (int y = 0; y + 1 < th/2; y += 2) {
        bb += luma_sum(scan, nstride, col, y);       /* even target row */
        db += luma_sum(scan, nstride, col, y + 1);   /* odd  target row */
        n++;
    }
    if (n > 0) { bb /= n; db /= n; }
    int parity = (n > 0) && (db < bb);

    int ok = eff0_matches && differs && parity;

    printf("[D]   effect0 == offscreen oracle (logical): %s\n", eff0_matches ? "ok" : "MISMATCH");
    printf("[D]   scanline differs from pass-through: %s\n", differs ? "ok" : "NO-DIFF");
    printf("[D]   odd target rows darker than even (scale=%d): dark=%d bright=%d over %d pairs  %s\n",
           scale, db, bb, n, parity ? "ok" : "FAIL");

    const char *png = "run/d-cocoametal-scanline.png";
    if (encode_png(scan, tw, th, nstride, png)) printf("[D]   png -> %s\n", png);

    if (ok) printf("[D] PASS scanline shader stage runs (differs + odd target rows darker)\n");
    else    printf("[D] FAIL shader stage\n");

    free(fb); free(plain); free(scan); cm_close(cx);
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[D1] cocoa/metal present pipeline + readback oracle (standalone)\n");

    /* Device availability gate -> SKIP cleanly if headless. */
    if (MTLCreateSystemDefaultDevice() == nil) {
        printf("[D1] SKIP no Metal device (MTLCreateSystemDefaultDevice == nil)\n");
        return 0;
    }

    int ok = 1;
    ok &= run_res_case("D1", 320, 200, "run/d1-cocoametal.png");
    printf("[D2] resolution-parametric check at a second resolution\n");
    ok &= run_res_case("D2", 640, 512, "run/d2-cocoametal.png");
    printf("[D] present-time shader stage (cm_set_effect / CM_FX_SCANLINE)\n");
    ok &= run_shader_case();

    if (ok) { printf("ALL PASS [D1] [D2] [D]\n"); return 0; }
    printf("FAIL see checks above\n");
    return 1;
}
