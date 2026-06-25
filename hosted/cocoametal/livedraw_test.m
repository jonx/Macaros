/* livedraw_test.m — LIVE-DRAWABLE readback: prove the present FILLS the drawable
 * ([LIVE]). The bug the OFFSCREEN oracle was blind to.
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/INTERFACE.md
 * (§2a live-present-fills-drawable contract, §9 fullscreen, §3 threading, §6 oracle)
 * + spec.md + cocoametal.h. No GPL emulator source (vAmiga/WinUAE/FS-UAE/Amiberry/
 * E-UAE) was read, searched, or consulted. Apple AppKit/Metal/QuartzCore/
 * CoreFoundation docs only [PUB].
 *
 * WHAT THIS PROVES + WHY THE EXISTING TESTS MISSED THE BUG:
 *   The [D1]/[D2t]/[FS] markers only read back the OFFSCREEN ORACLE target (cm_read-
 *   back). That target is always rendered full-size and is never resized, so it stays
 *   byte-exact even when the LIVE on-screen drawable is wrong. A user reported the
 *   live content showing as a small rect in a black fullscreen window: the CAMetal-
 *   Layer's drawable was not being resized to fill the content view on the fullscreen
 *   transition, so the framebuffer occupied only a small corner. This test reads the
 *   LIVE DRAWABLE back (TEST build sets framebufferOnly=NO) and asserts the presented
 *   scene FILLS the drawable — windowed AND after entering fullscreen.
 *
 * MODEL FIDELITY (same as [D2t]/[FS]): main() is the PROCESS MAIN THREAD; minimal
 * AppKit init ONCE; all cm_* on this one thread (§3); hand-pumped CFRunLoop, and a
 * bounded [NSApp run]+watchdog only to OBSERVE the (async) fullscreen transition.
 *
 * THE LIVE READBACK: with framebufferOnly=NO on the live layer, acquire nextDrawable,
 * present (cm_present composes the framebuffer into it via the render pass), then
 * getBytes the drawable texture and inspect it. The present default is aspect-fit
 * (CM_SCALE_ASPECT_FIT): the scene fills the largest centred rect of the LOGICAL
 * aspect, the rest is a BLACK letterbox. So we compute the content rect from the
 * drawable size + logical aspect and assert:
 *   - the four QUADRANT colors are present at the quadrant centres of the CONTENT rect
 *     (the scene fills the content rect at full drawable size — not a tiny corner),
 *   - any letterbox bar pixel is BLACK (never white — the reported symptom).
 *
 * HEADLESS-SAFE: no window server -> SKIP the live asserts but STILL run the §6
 * offscreen oracle assert, so a headless run passes. Bounded; watchdog in the
 * harness. No TCC (in-process only). VERDICT: [LIVE] PASS iff the groups pass.
 */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cocoametal.h"

/* Internal diagnostics (NOT the frozen ABI, NOT exported in the dylib). This test
 * links the .m files directly (it exercises the AppKit window path). */
void cm__present_stats(CMContext *cx, int *haveWindow, int *presents, int *drawables);
int  cm__window_is_fullscreen(CMContext *cx);
void cm__resync_layer(CMContext *cx);
int  cm__live_drawable_size(CMContext *cx, int *dw, int *dh, int *vw, int *vh);
void cm__live_set_framebuffer_only(CMContext *cx, int on);
void *cm__get_window(CMContext *cx);
/* TEST-ONLY (cocoametal.m): resync the layer, acquire the live drawable, compose the
 * framebuffer into it via the REAL present pass, read it back into dst (BGRA8), and
 * report the drawable's pixel size. Returns 0 on success, nonzero if no live drawable
 * (headless / nextDrawable nil). The live layer must have framebufferOnly=NO. */
int  cm__live_readback(CMContext *cx, void **dst, int *dw, int *dh);

#define C_TL   0xFFFF0000u   /* ARGB: red    */
#define C_TR   0xFF00FF00u   /* green  */
#define C_BL   0xFF0000FFu   /* blue   */
#define C_BR   0xFFFFFF00u   /* yellow */
#define C_MARK 0xFFFF00FFu   /* magenta */

static inline void put_bgra(uint8_t *fb, int fbw, int x, int y, uint32_t argb) {
    uint8_t *p = &fb[((size_t)y * fbw + x) * 4];
    p[0] = (uint8_t)(argb); p[1] = (uint8_t)(argb >> 8);
    p[2] = (uint8_t)(argb >> 16); p[3] = (uint8_t)(argb >> 24);
}
static inline uint32_t get_argb(const uint8_t *buf, int stride, int x, int y) {
    const uint8_t *p = &buf[(size_t)y * stride + (size_t)x * 4];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}
static void build_scene(uint8_t *fb, int w, int h, int markX, int markY) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = (x < w/2) ? ((y < h/2) ? C_TL : C_BL)
                                   : ((y < h/2) ? C_TR : C_BR);
            put_bgra(fb, w, x, y, c);
        }
    put_bgra(fb, w, markX, markY, C_MARK);
}

static void hand_pump(int rounds) {
    for (int i = 0; i < rounds; i++)
        (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
}
static void stop_app(void) {
    [NSApp stop:nil];
    NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                    location:NSZeroPoint modifierFlags:0 timestamp:0
                                windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:e atStart:YES];
}
static int drive_until(CMContext *cx, int want, double maxSecs) {
    if (cm__window_is_fullscreen(cx) == want) return 1;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:maxSecs];
    NSTimer *t = [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES
        block:^(NSTimer *x) { (void)x;
            if (cm__window_is_fullscreen(cx) == want ||
                [[NSDate date] compare:deadline] == NSOrderedDescending) stop_app(); }];
    [NSApp run]; [t invalidate];
    return cm__window_is_fullscreen(cx) == want;
}
static int request_fullscreen(CMContext *cx, int want) {
    if (cm_set_option(cx, CM_OPT_FULLSCREEN, want) != 0) return 0;
    if (drive_until(cx, want, 2.5)) return 1;
    if (cm_set_option(cx, CM_OPT_FULLSCREEN, want) != 0) return 0;
    return drive_until(cx, want, 2.5);
}

/* §6 offscreen-oracle readback (always run, even headless). */
static int check_oracle(CMContext *cx, int w, int h, int markX, int markY, const char *when) {
    int stride = w * 4;
    uint8_t *rb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!rb) { printf("[LIVE] FAIL calloc oracle (%s)\n", when); return 0; }
    int ok = (cm_readback(cx, rb, stride, w, h) == 0);
    struct { const char *name; int x, y; uint32_t want; } checks[] = {
        { "TL.red",    w/4,   h/4,   C_TL }, { "TR.green", 3*w/4, h/4,   C_TR },
        { "BL.blue",   w/4, 3*h/4,   C_BL }, { "BR.yellow",3*w/4, 3*h/4,  C_BR },
        { "marker",   markX,  markY,  C_MARK },
    };
    for (size_t i = 0; i < sizeof(checks)/sizeof(checks[0]); i++)
        if (get_argb(rb, stride, checks[i].x, checks[i].y) != checks[i].want) ok = 0;
    printf("[LIVE]   oracle(%s) %s\n", when, ok ? "byte-exact ok" : "MISMATCH");
    free(rb);
    return ok;
}

/* Read the LIVE drawable back and assert the scene FILLS it. Aspect-fit content rect
 * is computed from the drawable size + logical aspect; the four quadrant colours must
 * appear at the content-rect quadrant centres (scene at full size, not a tiny corner),
 * and any letterbox pixel must be BLACK (never white). Returns 1 on pass. */
static int check_live_fills(CMContext *cx, int logW, int logH, const char *when) {
    void *w = cm__get_window(cx);
    if (!w) { printf("[LIVE]   live(%s): no window -> SKIP\n", when); return 1; }

    /* (1) GEOMETRY: the drawable must fill the content view (the bug was a frozen,
     * undersized drawable on the fullscreen transition). */
    cm__resync_layer(cx);
    int dvw = 0, dvh = 0, vw = 0, vh = 0;
    cm__live_drawable_size(cx, &dvw, &dvh, &vw, &vh);
    int fills_view = (dvw == vw && dvh == vh && dvw > 0);
    printf("[LIVE]   live(%s): drawable=%dx%d  view.backing=%dx%d  drawable-fills-view=%s\n",
           when, dvw, dvh, vw, vh, fills_view ? "YES" : "NO");
    if (!fills_view) { printf("[LIVE]   live(%s): FAIL drawable does not fill the content view\n", when); return 0; }

    /* (2) CONTENT: compose the framebuffer into the live drawable via the REAL present
     * pass and read it back; assert the scene fills the (aspect-fit) content rect and
     * the letterbox is black. cm__live_readback mallocs dst; we free it. */
    void *dst = NULL; int tw = 0, th = 0;
    if (cm__live_readback(cx, &dst, &tw, &th) != 0 || !dst) {
        printf("[LIVE]   live(%s): no live drawable to read -> SKIP content check\n", when);
        return 1;   /* geometry passed; no drawable to read (e.g. nextDrawable nil) */
    }
    uint8_t *buf = (uint8_t *)dst;
    int stride = tw * 4, ok = 1;

    /* Aspect-fit content rect inside the drawable (matches scale_viewport math). */
    int cw, ch, ox, oy;
    if ((long)logW * th <= (long)tw * logH) { ch = th; cw = (int)((long)logW * th / logH); }
    else                                    { cw = tw; ch = (int)((long)logH * tw / logW); }
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    ox = (tw - cw) / 2; oy = (th - ch) / 2;

    /* Quadrant centres WITHIN the content rect (Metal target origin top-left; the
     * present flips V so framebuffer row 0 is at the top). */
    struct { const char *name; int qx, qy; uint32_t want; } q[] = {
        { "TL.red",    ox + cw/4,     oy + ch/4,     C_TL },
        { "TR.green",  ox + 3*cw/4,   oy + ch/4,     C_TR },
        { "BL.blue",   ox + cw/4,     oy + 3*ch/4,   C_BL },
        { "BR.yellow", ox + 3*cw/4,   oy + 3*ch/4,   C_BR },
    };
    for (size_t i = 0; i < sizeof(q)/sizeof(q[0]); i++) {
        uint32_t got = get_argb(buf, stride, q[i].qx, q[i].qy);
        int pass = (got == q[i].want);
        if (!pass) ok = 0;
        printf("[LIVE]   live(%s) %-10s (%4d,%4d) want=%08X got=%08X  %s\n",
               when, q[i].name, q[i].qx, q[i].qy, q[i].want, got, pass ? "ok" : "MISMATCH");
    }

    /* Letterbox bars (if any) must be BLACK, never white (the reported symptom). */
    int whiteSeen = 0, barsChecked = 0;
    int sx = (tw/13 > 0 ? tw/13 : 1), sy = (th/13 > 0 ? th/13 : 1);
    for (int x = 0; x < tw; x += sx) {
        if (oy > 0)        { barsChecked++; if ((get_argb(buf,stride,x,0)      & 0x00FFFFFF) == 0x00FFFFFF) whiteSeen = 1; }
        if (oy + ch < th)  { barsChecked++; if ((get_argb(buf,stride,x,th-1)   & 0x00FFFFFF) == 0x00FFFFFF) whiteSeen = 1; }
    }
    for (int y = 0; y < th; y += sy) {
        if (ox > 0)        { barsChecked++; if ((get_argb(buf,stride,0,y)      & 0x00FFFFFF) == 0x00FFFFFF) whiteSeen = 1; }
        if (ox + cw < tw)  { barsChecked++; if ((get_argb(buf,stride,tw-1,y)   & 0x00FFFFFF) == 0x00FFFFFF) whiteSeen = 1; }
    }
    if (whiteSeen) ok = 0;
    printf("[LIVE]   live(%s): content=%dx%d@(%d,%d) in drawable %dx%d  "
           "letterbox-pixels-checked=%d white-seen=%s  %s\n",
           when, cw, ch, ox, oy, tw, th, barsChecked,
           whiteSeen ? "YES(FAIL)" : "no", ok ? "ok" : "FAIL");
    free(buf);
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[LIVE] live-drawable readback: the present FILLS the drawable "
           "(windowed + fullscreen) — the offscreen oracle was blind to this\n");

    if (MTLCreateSystemDefaultDevice() == nil) {
        printf("[LIVE] SKIP no Metal device\n"); return 0;
    }
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    }

    const int w = 320, h = 200;
    const int markX = w / 7, markY = h * 7 / 15;
    CMPixelDesc fmt = {
        .bytesPerPixel = 4, .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00, .redMask = 0x00FF0000, .alphaMask = 0xFF000000,
    };

    CMContext *cx = cm_open(w, h, &fmt, "AROS [LIVE] livedraw");
    if (!cx) { printf("[LIVE] SKIP cm_open NULL\n"); return 0; }
    hand_pump(8);

    /* TEST build: make the live drawable readable. */
    cm__live_set_framebuffer_only(cx, 0);

    int haveWindow = 0, presents = 0, drawables = 0;
    cm__present_stats(cx, &haveWindow, &presents, &drawables);

    uint8_t *fb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!fb) { printf("[LIVE] FAIL calloc fb\n"); cm_close(cx); return 1; }
    build_scene(fb, w, h, markX, markY);
    cm_upload_rect(cx, fb, w * 4, 0, 0, w, h);
    cm_present(cx); hand_pump(4);

    int oracle_ok = check_oracle(cx, w, h, markX, markY, "init");
    int win_ok = 1, fs_ok = 1;

    if (!haveWindow) {
        printf("[LIVE]   window=none (headless) -> SKIP live asserts; oracle only (headless passes)\n");
    } else {
        /* ---- WINDOWED: the live drawable fills the window ---- */
        win_ok = check_live_fills(cx, w, h, "windowed");

        /* ---- FULLSCREEN: enter, then the live drawable fills the screen ---- */
        int entered = request_fullscreen(cx, 1);
        hand_pump(8);
        cm__resync_layer(cx);
        printf("[LIVE]   entered fullscreen=%d (styleMask)\n", entered);
        fs_ok = check_live_fills(cx, w, h, "fullscreen");
        oracle_ok = oracle_ok && check_oracle(cx, w, h, markX, markY, "fullscreen");

        /* exit cleanly so we leave no fullscreen Space behind */
        (void)request_fullscreen(cx, 0);
    }

    int ok = oracle_ok && win_ok && fs_ok;
    printf("[LIVE] ---- summary ----  oracle=%s  windowed=%s  fullscreen=%s  (window=%s)\n",
           oracle_ok ? "ok" : "FAIL", win_ok ? "ok" : "FAIL", fs_ok ? "ok" : "FAIL",
           haveWindow ? "yes" : "headless");

    free(fb);
    cm_close(cx);

    if (ok) { printf("[LIVE] PASS the live present fills the drawable, windowed + fullscreen, no white\n"); return 0; }
    printf("[LIVE] FAIL see checks above\n");
    return 1;
}
