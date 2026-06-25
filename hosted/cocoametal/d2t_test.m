/* d2t_test.m — D2 under the REAL graft threading model ([D2t]).
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/INTERFACE.md §3
 * (threading & call contract) + spec.md ("Threading model"). No GPL emulator
 * source (vAmiga/WinUAE/FS-UAE/Amiberry/E-UAE) was read, searched, or consulted.
 * Apple AppKit/Metal/QuartzCore/CoreFoundation docs only [PUB].
 *
 * THE QUESTION THIS ANSWERS (INTERFACE.md §8 item 3, the de-risk):
 *   Under the graft's real model — the AROS boot task IS the host main pthread
 *   (H4), driving CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) BY HAND,
 *   with NO NSApplicationMain and NO [NSApp run] (§3) — do cm_open (NSWindow
 *   creation) and cm_present (CAMetalLayer nextDrawable) actually work? And what
 *   is the MINIMAL AppKit initialization the boot task must do once?
 *
 * MODEL FIDELITY:
 *   - This binary's main() runs on the PROCESS MAIN THREAD — the stand-in for the
 *     AROS boot/anchor task pinned to the host main pthread (H4).
 *   - It performs the candidate minimal AppKit init ONCE, up front:
 *         [NSApplication sharedApplication];                 // create NSApp
 *         [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
 *     This is INITIALIZATION ONLY — it does NOT start a run loop. We then NEVER
 *     call [NSApp run] or NSApplicationMain.
 *   - All cm_* calls happen on this one thread (the §3 single-caller rule).
 *   - Between/around the cm_* calls we hand-pump the run loop with
 *         CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true)
 *     in a bounded, non-blocking way (0 timeout, returnAfterSourceHandled=true) —
 *     exactly what cm_pump_events will do for event delivery. Nothing blocks.
 *
 * VERDICT RULE: PASS requires the §6 readback ORACLE to be exact at both
 * resolutions (320x200 and 640x512) under this threading model — that is the
 * non-negotiable contract. Whether the live window + nextDrawable additionally
 * succeed is REPORTED (it is the answer we owe the AROS side), but a "needs the
 * app run loop / no drawable" outcome is a finding, not a failure — the offscreen
 * oracle path is what the driver depends on.
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

/* Internal diagnostic accessor implemented in cocoametal.m (not part of the
 * frozen ABI, not exported in the dylib). Lets us observe, from evidence, whether
 * cm_present acquired a live CAMetalDrawable under the hand-pumped model. */
void cm__present_stats(CMContext *cx, int *haveWindow, int *presents, int *drawables);

#define C_TL   0xFFFF0000u
#define C_TR   0xFF00FF00u
#define C_BL   0xFF0000FFu
#define C_BR   0xFFFFFF00u
#define C_MARK 0xFFFF00FFu

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

/* Hand-pump the CFRunLoop the way cm_pump_events will: bounded, non-blocking,
 * NEVER [NSApp run]. Drains whatever sources/AppKit have queued and returns.
 * Returns the number of pump iterations that handled a source (informational). */
static int hand_pump(int rounds) {
    int handled = 0;
    for (int i = 0; i < rounds; i++) {
        /* 0 timeout + returnAfterSourceHandled=true => process at most one ready
         * source then return immediately; if nothing is ready it returns at once.
         * This is the non-blocking drain §3 mandates. */
        SInt32 r = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        if (r == kCFRunLoopRunHandledSource) handled++;
        else if (r == kCFRunLoopRunFinished || r == kCFRunLoopRunTimedOut) {
            /* Finished = no sources; TimedOut = nothing ready in the window.
             * Either way there is nothing more to drain right now. */
        }
    }
    return handled;
}

/* One resolution case under the threading model. Returns 1 if the ORACLE passed
 * (the contract). Fills *outDrawable with whether nextDrawable succeeded. */
static int run_case(const char *tag, int w, int h, int *outHaveWindow, int *outDrawable) {
    int markX = w / 7, markY = h * 7 / 15;

    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };

    /* cm_open on the main pthread. The shim's cm_try_window also performs the
     * sharedApplication + setActivationPolicy init internally (idempotent), so by
     * doing it up front in main() we prove the boot-task ordering works. */
    CMContext *cx = cm_open(w, h, &fmt, tag);
    if (!cx) { printf("[D2t][%s] note cm_open returned NULL (no Metal device) -> SKIP case\n", tag); return -1; }

    /* Pump once right after window creation: lets AppKit flush the window-order /
     * layer-attach work it queued, the way the boot task would between cm_* calls. */
    hand_pump(8);

    uint8_t *fb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!fb) { printf("[D2t][%s] FAIL calloc fb\n", tag); cm_close(cx); return 0; }
    build_scene(fb, w, h, markX, markY);
    cm_upload_rect(cx, fb, w * 4, 0, 0, w, h);

    /* Present several frames, pumping the run loop between them — the realistic
     * cadence (present, service the run loop, present...). cm_present must stay
     * non-blocking; if nextDrawable needs the app run loop, the present pass is
     * skipped but the oracle pass still completes. */
    for (int f = 0; f < 3; f++) {
        cm_present(cx);
        hand_pump(4);
    }

    /* cm_pump_events: the non-blocking drain. Must return promptly. */
    CMEvent evbuf[16];
    int nev = cm_pump_events(cx, evbuf, 16);

    /* §6 oracle readback — the contract. */
    int stride = w * 4;
    uint8_t *rb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!rb) { printf("[D2t][%s] FAIL calloc rb\n", tag); free(fb); cm_close(cx); return 0; }
    int rbrc = cm_readback(cx, rb, stride, w, h);

    int ok = (rbrc == 0);
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
        printf("[D2t][%s]   oracle %-10s (%4d,%4d) want=%08X got=%08X  %s\n",
               tag, checks[i].name, checks[i].x, checks[i].y,
               checks[i].want, got, pass ? "ok" : "MISMATCH");
    }

    int haveWindow = 0, presents = 0, drawables = 0;
    cm__present_stats(cx, &haveWindow, &presents, &drawables);
    int tw = 0, th = 0, scale = 0;
    cm_target_size(cx, &tw, &th, &scale);

    printf("[D2t][%s]   threading-model report: window=%s  presents=%d  drawablesAcquired=%d  "
           "pumpEvents=%d  oracle=%dx%d@%dx\n",
           tag, haveWindow ? "created" : "none (headless)",
           presents, drawables, nev, tw, th, scale);

    if (outHaveWindow) *outHaveWindow = haveWindow;
    if (outDrawable)   *outDrawable   = drawables;

    free(fb); free(rb); cm_close(cx);
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[D2t] D2 under the REAL graft threading model "
           "(main pthread, manual CFRunLoop, NO NSApplicationMain / NO [NSApp run])\n");

    if (MTLCreateSystemDefaultDevice() == nil) {
        printf("[D2t] SKIP no Metal device (MTLCreateSystemDefaultDevice == nil)\n");
        return 0;
    }

    /* ---- THE MINIMAL APPKIT INITIALIZATION (candidate the boot task does ONCE).
     * This is INITIALIZATION, not a run loop: it creates the shared NSApplication
     * and sets its activation policy so the process can own windows. We do NOT
     * call [NSApp finishLaunching], [NSApp run], or NSApplicationMain. */
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        printf("[D2t] AppKit init done: [NSApplication sharedApplication] + "
               "setActivationPolicy:Regular  (NSApp=%s, isRunning=%d)  -- no run loop started\n",
               app ? "non-nil" : "nil", (int)[app isRunning]);
    }

    /* Drive the full sequence at two resolutions on THIS (main) thread, hand-
     * pumping the run loop. D2 = the 640x512 case; we also run 320x200. */
    int w1 = 0, d1 = 0, w2 = 0, d2 = 0;
    int r1 = run_case("320x200", 320, 200, &w1, &d1);
    int r2 = run_case("640x512", 640, 512, &w2, &d2);

    if (r1 < 0 || r2 < 0) {
        /* cm_open returned NULL somewhere (no device) — clean skip. */
        printf("[D2t] SKIP no usable Metal device for a case\n");
        return 0;
    }

    int oracle_ok = (r1 == 1 && r2 == 1);
    int window_ok = (w1 && w2);
    int drawable_ok = (d1 > 0 && d2 > 0);

    /* The de-risk answer, stated plainly for the AROS side. */
    printf("[D2t] ---- threading-model findings ----\n");
    printf("[D2t]   minimal AppKit init (boot task, once): "
           "[NSApplication sharedApplication] + setActivationPolicy:Regular; NO run loop.\n");
    printf("[D2t]   run-loop servicing: manual CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true); "
           "NO NSApplicationMain / NO [NSApp run].\n");
    printf("[D2t]   cm_open window creation under this model: %s\n",
           window_ok ? "WORKS (NSWindow created)" : "DEGRADED (headless: no window server)");
    printf("[D2t]   cm_present nextDrawable under this model: %s\n",
           drawable_ok ? "WORKS (live CAMetalDrawable acquired, hand-pumped run loop sufficient)"
                       : (window_ok ? "NO DRAWABLE (window exists but nextDrawable did not yield one)"
                                     : "N/A (headless, present pass skipped)"));
    printf("[D2t]   offscreen oracle (cm_readback) under this model: %s\n",
           oracle_ok ? "PASS (the contract holds)" : "FAIL");

    if (oracle_ok) {
        printf("[D2t] PASS oracle exact at 320x200 + 640x512 under main-pthread/manual-CFRunLoop "
               "(window=%s, nextDrawable=%s)\n",
               window_ok ? "yes" : "headless",
               drawable_ok ? "yes" : (window_ok ? "no" : "n/a"));
        return 0;
    }
    printf("[D2t] FAIL oracle did not hold under the threading model — see checks above\n");
    return 1;
}
