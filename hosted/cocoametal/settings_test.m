/* settings_test.m — the host settings panel + key/value option ABI ([SET]).
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/INTERFACE.md
 * (§9 settings & options, §3 threading, §5 CM_EV_SETTING, §6 readback oracle) +
 * spec.md + cocoametal.h. No GPL emulator source (vAmiga/WinUAE/FS-UAE/Amiberry/
 * E-UAE) was read, searched, or consulted. Apple AppKit/Metal/QuartzCore/
 * Foundation docs only [PUB].
 *
 * Runs under the SAME threading model as D2t (the graft's real model): main
 * pthread IS the display-server task, hand-pumped
 * CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true), NO NSApplicationMain / NO
 * [NSApp run]. Every cm_* call is on this one thread (§3).
 *
 * Asserts (INTERFACE.md §9):
 *   [SET-EFFECT] cm_set_option(CM_OPT_EFFECT, CM_FX_SCANLINE) -> cm_present: the
 *       PRESENTED path reflects the effect (odd target rows darker than even,
 *       via cm_render_effect_readback) while the OFFSCREEN ORACLE (cm_readback)
 *       stays pass-through, byte-for-byte unchanged; cm_get_option reflects it.
 *   [SET-AROS]   cm_set_option of an AROS-facing key (CM_OPT_REQUEST_MODE_W=640,
 *       _H=512) -> cm_pump_events returns a CM_EV_SETTING carrying the key/value
 *       (proving the host did NOT act on it — it is a pull surface, §3).
 *   [SET-PERSIST] NSUserDefaults round-trip: set host options, simulate a reopen
 *       (fresh cm_open re-reads the persisted defaults via
 *       cm__apply_persisted_options), assert they were restored.
 *
 * VERDICT: [SET] PASS only if all three groups pass. Bounded; the harness adds a
 * watchdog. Cleans up the test-written NSUserDefaults keys before exit.
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

/* The shim's NSUserDefaults keys (cocoametal_settings.m) — mirrored here so the
 * persistence round-trip exercises the REAL persisted state, and so we can clean
 * them up afterward (do not pollute the user's defaults domain). */
static NSString *const kDefEffect     = @"cocoametal.effect";
static NSString *const kDefScaleMode  = @"cocoametal.scaleMode";
static NSString *const kDefFullscreen = @"cocoametal.fullscreen";
static NSString *const kDefFilter     = @"cocoametal.filter";

#define C_TL 0xFFFF0000u
#define C_TR 0xFF00FF00u
#define C_BL 0xFF0000FFu
#define C_BR 0xFFFFFF00u
#define C_MARK 0xFFFF00FFu

static inline void put_bgra(uint8_t *fb, int fbw, int x, int y, uint32_t argb) {
    uint8_t *p = &fb[((size_t)y * fbw + x) * 4];
    p[0] = (uint8_t)(argb); p[1] = (uint8_t)(argb >> 8);
    p[2] = (uint8_t)(argb >> 16); p[3] = (uint8_t)(argb >> 24);
}
static inline int luma_sum(const uint8_t *buf, int stride, int x, int y) {
    const uint8_t *p = &buf[(size_t)y * stride + (size_t)x * 4];
    return (int)p[0] + (int)p[1] + (int)p[2];   /* B+G+R */
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

/* Hand-pump the CFRunLoop the way the display-server task does — bounded, non-
 * blocking, NEVER [NSApp run] (identical to d2t_test). */
static void hand_pump(int rounds) {
    for (int i = 0; i < rounds; i++)
        (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
}

/* ---- [SET-EFFECT]: a host-owned option (CM_OPT_EFFECT) takes effect on the
 * PRESENTED path while the offscreen ORACLE stays pass-through (§6/§9). ------ */
static int test_effect(void) {
    const int w = 256, h = 128;
    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };
    CMContext *cx = cm_open(w, h, &fmt, "AROS [SET] effect");
    if (!cx) { printf("[SET-EFFECT] FAIL cm_open NULL\n"); return 0; }
    hand_pump(8);

    int tw = 0, th = 0, scale = 0;
    cm_target_size(cx, &tw, &th, &scale);

    uint8_t *fb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!fb) { printf("[SET-EFFECT] FAIL calloc\n"); cm_close(cx); return 0; }
    build_scene(fb, w, h, 5, 5);
    cm_upload_rect(cx, fb, w * 4, 0, 0, w, h);

    /* Set the effect through the OPTION ABI (not cm_set_effect) and confirm
     * cm_get_option reflects it. */
    int set_rc = cm_set_option(cx, CM_OPT_EFFECT, CM_FX_SCANLINE);
    long got = -1;
    int get_rc = cm_get_option(cx, CM_OPT_EFFECT, &got);
    int opt_roundtrip = (set_rc == 0 && get_rc == 0 && got == CM_FX_SCANLINE);

    /* Present, then read the OFFSCREEN ORACLE (logical): it must stay pass-through
     * regardless of the selected effect (§6). Compare against the pass-through
     * effect readback (CM_FX_NEAREST) at the logical grid — they must be equal. */
    cm_present(cx);
    hand_pump(4);
    int lstride = w * 4;
    uint8_t *oracle = (uint8_t *)calloc((size_t)w * h, 4);
    uint8_t *passthru = (uint8_t *)calloc((size_t)w * h, 4);
    int oracle_unchanged = 0;
    if (oracle && passthru) {
        int ro = cm_readback(cx, oracle, lstride, w, h);
        int rp = cm_render_effect_readback(cx, CM_FX_NEAREST, passthru, lstride, w, h);
        oracle_unchanged = (ro == 0 && rp == 0 &&
                            memcmp(oracle, passthru, (size_t)lstride * h) == 0);
    }
    free(oracle); free(passthru);

    /* The PRESENTED path reflects the effect. We cannot read the live drawable, so
     * we use the documented [D]-shaped check: render the SAME effect the present
     * uses (CM_FX_SCANLINE) into a native-grid target and assert (a) it differs
     * from pass-through and (b) odd target rows are darker than even — the
     * presented look, distinct from the oracle. */
    int nstride = tw * 4;
    uint8_t *plain = (uint8_t *)calloc((size_t)tw * th, 4);
    uint8_t *scan  = (uint8_t *)calloc((size_t)tw * th, 4);
    int differs = 0, parity = 0, db = 0, bb = 0, n = 0;
    if (plain && scan) {
        int r1 = cm_render_effect_readback(cx, CM_FX_NEAREST,  plain, nstride, tw, th);
        int r2 = cm_render_effect_readback(cx, CM_FX_SCANLINE, scan,  nstride, tw, th);
        if (r1 == 0 && r2 == 0) {
            differs = (memcmp(plain, scan, (size_t)nstride * th) != 0);
            int col = tw / 4;
            for (int y = 0; y + 1 < th/2; y += 2) {
                bb += luma_sum(scan, nstride, col, y);
                db += luma_sum(scan, nstride, col, y + 1);
                n++;
            }
            if (n > 0) { bb /= n; db /= n; }
            parity = (n > 0) && (db < bb);
        }
    }
    free(plain); free(scan);

    int ok = opt_roundtrip && oracle_unchanged && differs && parity;
    printf("[SET-EFFECT]   cm_set_option(EFFECT,SCANLINE)/get roundtrip: %s (got=%ld)\n",
           opt_roundtrip ? "ok" : "MISMATCH", got);
    printf("[SET-EFFECT]   offscreen oracle stays pass-through (unchanged): %s\n",
           oracle_unchanged ? "ok" : "MISMATCH");
    printf("[SET-EFFECT]   presented path reflects effect (differs+odd rows darker, scale=%d): "
           "dark=%d bright=%d over %d  %s\n",
           scale, db, bb, n, (differs && parity) ? "ok" : "FAIL");
    printf("[SET-EFFECT] %s\n", ok ? "PASS" : "FAIL");

    free(fb); cm_close(cx);
    return ok;
}

/* ---- [SET-AROS]: an AROS-facing key is NOT host-acted — it surfaces as a
 * CM_EV_SETTING pulled via cm_pump_events (§3/§5/§9). -------------------------*/
static int test_aros_pull(void) {
    const int w = 320, h = 200;
    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };
    CMContext *cx = cm_open(w, h, &fmt, "AROS [SET] pull");
    if (!cx) { printf("[SET-AROS] FAIL cm_open NULL\n"); return 0; }
    hand_pump(8);

    /* Set an AROS-facing W×H request. The host must NOT change resolution — it
     * enqueues a CM_EV_SETTING per key. */
    int wrc = cm_set_option(cx, CM_OPT_REQUEST_MODE_W, 640);
    int hrc = cm_set_option(cx, CM_OPT_REQUEST_MODE_H, 512);

    /* Host did NOT act: cm_target_size still reflects the ORIGINAL 320x200 logical
     * size (scale-multiplied). */
    int tw = 0, th = 0, scale = 0;
    cm_target_size(cx, &tw, &th, &scale);
    int host_inert = (tw == w * scale && th == h * scale);

    /* Pull the events. We expect two CM_EV_SETTING events: _MODE_W (x=640) and
     * _MODE_H (x=512, y=640 — the paired partner). */
    CMEvent ev[16];
    int nev = cm_pump_events(cx, ev, 16);
    int sawW = 0, sawH = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].type != CM_EV_SETTING) continue;
        if (ev[i].code == CM_OPT_REQUEST_MODE_W && ev[i].x == 640) sawW = 1;
        if (ev[i].code == CM_OPT_REQUEST_MODE_H && ev[i].x == 512 && ev[i].y == 640) sawH = 1;
    }

    /* cm_get_option still reports the last-requested values (host tracks them for
     * the panel, but did not act). */
    long gw = -1, gh = -1;
    cm_get_option(cx, CM_OPT_REQUEST_MODE_W, &gw);
    cm_get_option(cx, CM_OPT_REQUEST_MODE_H, &gh);
    int tracked = (gw == 640 && gh == 512);

    int ok = (wrc == 0 && hrc == 0) && host_inert && sawW && sawH && tracked;
    printf("[SET-AROS]   set REQUEST_MODE_W=640,_H=512 accepted: %s\n",
           (wrc == 0 && hrc == 0) ? "ok" : "NO");
    printf("[SET-AROS]   host did NOT act (target still %dx%d@%dx == %dx%d logical): %s\n",
           tw, th, scale, w, h, host_inert ? "ok" : "MISMATCH");
    printf("[SET-AROS]   cm_pump_events surfaced CM_EV_SETTING W=%s H(+pair)=%s (nev=%d)\n",
           sawW ? "yes" : "NO", sawH ? "yes" : "NO", nev);
    printf("[SET-AROS]   cm_get_option tracks requested W=%ld H=%ld: %s\n",
           gw, gh, tracked ? "ok" : "MISMATCH");
    printf("[SET-AROS] %s\n", ok ? "PASS" : "FAIL");

    cm_close(cx);
    return ok;
}

/* ---- [SET-PERSIST]: NSUserDefaults round-trip. Write the host-owned defaults
 * (as the panel's save path does), then SIMULATE A REOPEN by a fresh cm_open —
 * which calls cm__apply_persisted_options to re-read the defaults — and assert
 * the options were restored. Cleans up the keys afterward. -------------------*/
static int test_persist(void) {
    /* 1) Seed persisted defaults to NON-DEFAULT values (the state the panel would
     * have saved on a previous run). */
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    [d setInteger:CM_FX_SCANLINE          forKey:kDefEffect];
    [d setInteger:CM_SCALE_PIXEL_PERFECT  forKey:kDefScaleMode];
    [d setInteger:1                       forKey:kDefFullscreen];
    [d setInteger:CM_FILTER_LINEAR        forKey:kDefFilter];
    [d synchronize];

    /* 2) Simulate a reopen: a fresh cm_open must load+apply them. */
    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };
    CMContext *cx = cm_open(320, 200, &fmt, "AROS [SET] persist");
    int ok = 0;
    if (!cx) {
        printf("[SET-PERSIST] FAIL cm_open NULL\n");
    } else {
        long e = -1, s = -1, f = -1, fs = -1;
        cm_get_option(cx, CM_OPT_EFFECT, &e);
        cm_get_option(cx, CM_OPT_SCALE_MODE, &s);
        cm_get_option(cx, CM_OPT_FULLSCREEN, &fs);
        cm_get_option(cx, CM_OPT_FILTER, &f);
        ok = (e == CM_FX_SCANLINE && s == CM_SCALE_PIXEL_PERFECT &&
              fs == 1 && f == CM_FILTER_LINEAR);
        printf("[SET-PERSIST]   reopened cm_open restored: effect=%ld scale=%ld fullscreen=%ld "
               "filter=%ld  %s\n", e, s, fs, f, ok ? "ok" : "MISMATCH");
        cm_close(cx);
    }
    printf("[SET-PERSIST] %s\n", ok ? "PASS" : "FAIL");

    /* 3) Clean up: do NOT pollute the user's defaults domain. */
    [d removeObjectForKey:kDefEffect];
    [d removeObjectForKey:kDefScaleMode];
    [d removeObjectForKey:kDefFullscreen];
    [d removeObjectForKey:kDefFilter];
    [d synchronize];
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[SET] host settings panel + key/value option ABI (INTERFACE.md §9, ABI v2)\n");

    if (MTLCreateSystemDefaultDevice() == nil) {
        printf("[SET] SKIP no Metal device\n");
        return 0;
    }

    /* The minimal AppKit init the display-server / boot task does ONCE (D2t). */
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        printf("[SET] AppKit init: sharedApplication + setActivationPolicy:Regular "
               "(isRunning=%d)  -- no run loop started\n", (int)[app isRunning]);
    }

    /* Open the settings panel best-effort, on this thread, hand-pumped. Either it
     * comes up (window server present) or degrades to a no-op (headless) — both
     * acceptable; we only assert it is callable and non-blocking. */
    {
        CMPixelDesc fmt = {
            .bytesPerPixel = 4,
            .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
            .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
            .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
        };
        CMContext *pcx = cm_open(320, 200, &fmt, "AROS [SET] panel");
        if (pcx) {
            int os = cm_open_settings(pcx);
            hand_pump(8);
            printf("[SET] cm_open_settings -> %d (%s); non-blocking under hand-pumped CFRunLoop\n",
                   os, os == 0 ? "panel up" : "no window server / no-op");
            cm_close(pcx);
        }
    }

    int e = test_effect();
    int a = test_aros_pull();
    int p = test_persist();

    int ok = e && a && p;
    printf("[SET] ---- summary ----  effect=%s  aros-pull=%s  persist=%s\n",
           e ? "PASS" : "FAIL", a ? "PASS" : "FAIL", p ? "PASS" : "FAIL");
    if (ok) {
        printf("[SET] PASS host-owned option acts on present (oracle unchanged), AROS-facing key "
               "pulls as CM_EV_SETTING, NSUserDefaults persistence round-trips\n");
        return 0;
    }
    printf("[SET] FAIL see checks above\n");
    return 1;
}
