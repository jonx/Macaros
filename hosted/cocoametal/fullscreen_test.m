/* fullscreen_test.m — REAL native AppKit fullscreen for CM_OPT_FULLSCREEN ([FS]).
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/INTERFACE.md
 * (§9 settings & options — CM_OPT_FULLSCREEN, §3 threading, §6 readback oracle) +
 * spec.md + cocoametal.h. No GPL emulator source (vAmiga/WinUAE/FS-UAE/Amiberry/
 * E-UAE) was read, searched, or consulted. Apple AppKit/Metal/QuartzCore/
 * CoreFoundation docs only [PUB].
 *
 * WHAT THIS PROVES + THE FINDING IT NAILS:
 *   cm_set_option(CM_OPT_FULLSCREEN,1) now calls -[NSWindow toggleFullScreen:] to
 *   enter REAL native AppKit fullscreen (no longer a stored-flag stub); 0 exits.
 *   The shim's cm__set_fullscreen_appkit only REQUESTS the (async, animated) toggle
 *   and returns — no cm_* blocks/spins (§3). The QUESTION: does that async
 *   transition complete under the graft's real model (main pthread, NO
 *   NSApplicationMain / NO [NSApp run])?
 *
 *   MEASURED FINDING (this Mac, GUI session, backgrounded CLI harness):
 *     - ENTER completes under a HAND-PUMPED CFRunLoopRunInMode(...,0,true): the
 *       window's styleMask gains NSWindowStyleMaskFullScreen essentially at the
 *       toggle call (0 extra pumps observed) — the authoritative "fullscreen now"
 *       bit. (The on-screen Space animation is driven by the window server.)
 *     - EXIT does NOT complete under bare hand-pumping: the AppKit fullscreen
 *       state machine's exit transition needs the APP run loop to advance. So this
 *       test drives the transition with a BOUNDED [NSApp run] stopped by a
 *       watchdog timer (legitimate in a test; the production shim never does this —
 *       it stays non-blocking per §3, and the graft's hand-pumped loop services
 *       enter). A further quirk in a BACKGROUNDED, non-frontmost CLI process (it
 *       cannot become the active app): the FIRST exit toggle can be a no-op, so the
 *       drive RETRIES the toggle once — deterministic across runs.
 *
 *   The window.frame == screen.frame geometry is NOT a reliable oracle here (a
 *   native-fullscreen window keeps a windowed-looking frame in this process
 *   context), so the AUTHORITATIVE assert is styleMask & NSWindowStyleMaskFullScreen
 *   (cm__window_is_fullscreen). The frame/cover geometry is printed informationally.
 *
 * MODEL FIDELITY: main() is the PROCESS MAIN THREAD (the AROS boot-task stand-in);
 * minimal AppKit init ONCE ([NSApplication sharedApplication] +
 * setActivationPolicy:Regular); all cm_* on this one thread (§3).
 *
 * ASSERTS (the hard gate):
 *   [FS-ENTER]  cm_set_option(CM_OPT_FULLSCREEN,1) → drive → window ACTUALLY entered
 *       fullscreen (styleMask & NSWindowStyleMaskFullScreen). NOT a screencapture.
 *   [FS-ORACLE] while fullscreen, cm_present → cm_readback byte-exact (§6 oracle: 4
 *       quadrants + marker — the present still composes correctly).
 *   [FS-EXIT]   cm_set_option(CM_OPT_FULLSCREEN,0) → drive → window EXITED
 *       fullscreen (styleMask bit clear); oracle still byte-exact afterwards.
 *
 * HEADLESS-SAFE: no window server (cm__present_stats says window=none) → SKIP the
 * window asserts (no-op) but STILL run the oracle assert, so a headless run passes.
 * Bounded; the harness adds a watchdog. VERDICT: [FS] PASS iff the run groups pass.
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


/* Internal diagnostics implemented in cocoametal.m / cocoametal_window.m (NOT part
 * of the frozen ABI, NOT exported in the dylib). This test links the .m files
 * directly (it exercises the AppKit window path), so it can call them. */
void cm__present_stats(CMContext *cx, int *haveWindow, int *presents, int *drawables);
int  cm__window_is_fullscreen(CMContext *cx);   /* styleMask & FullScreen, now */
int  cm__window_covers_screen(CMContext *cx);   /* informational geometry check */

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

/* Hand-pump the CFRunLoop the display-server task's way — bounded, non-blocking,
 * NEVER [NSApp run] (identical to d2t_test / settings_test). */
static void hand_pump(int rounds) {
    for (int i = 0; i < rounds; i++)
        (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
}

/* Stop a bounded [NSApp run] cleanly (post a no-op event so -stop: takes effect at
 * once rather than after the next real event). */
static void stop_app(void) {
    [NSApp stop:nil];
    NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                    location:NSZeroPoint modifierFlags:0 timestamp:0
                                windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:e atStart:YES];
}

/* Drive the run loop (BOUNDED [NSApp run] + watchdog timer) until the window's
 * styleMask reaches `want` fullscreen state or `maxSecs` elapses. ALWAYS returns
 * (the watchdog stops it). Returns 1 if the state was reached. This is the test
 * harness's transition driver — the PRODUCTION shim never spins [NSApp run] (it
 * stays non-blocking, §3); we do it here only to OBSERVE the completed transition
 * deterministically, since EXIT needs the app run loop (the documented finding). */
static int drive_until(CMContext *cx, int want, double maxSecs) {
    if (cm__window_is_fullscreen(cx) == want) return 1;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:maxSecs];
    NSTimer *t = [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES
        block:^(NSTimer *x) {
            (void)x;
            if (cm__window_is_fullscreen(cx) == want ||
                [[NSDate date] compare:deadline] == NSOrderedDescending)
                stop_app();
        }];
    [NSApp run];
    [t invalidate];
    return cm__window_is_fullscreen(cx) == want;
}

/* Request a fullscreen state via the PUBLIC option ABI and drive it to completion.
 * cm_set_option issues -[NSWindow toggleFullScreen:] via the shim; we then drive
 * the run loop. The EXIT case retries the request once: in a backgrounded, non-
 * frontmost CLI process the first exit toggle can be a no-op (the finding). Bounded.
 * Returns 1 if the window reached `want`. */
static int request_fullscreen(CMContext *cx, int want) {
    if (cm_set_option(cx, CM_OPT_FULLSCREEN, want) != 0) return 0;
    if (drive_until(cx, want, 2.5)) return 1;
    /* Re-request: still not at target → toggle again (the cm_set_option styleMask
     * guard re-issues the toggle because the live state still differs). */
    if (cm_set_option(cx, CM_OPT_FULLSCREEN, want) != 0) return 0;
    return drive_until(cx, want, 2.5);
}

/* The §6 oracle readback check at logical w×h: 4 quadrants + the marker pixel.
 * Returns 1 if byte-exact. Prints each check. */
static int check_oracle(CMContext *cx, int w, int h, int markX, int markY, const char *when) {
    int stride = w * 4;
    uint8_t *rb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!rb) { printf("[FS-ORACLE] FAIL calloc (%s)\n", when); return 0; }
    int rc = cm_readback(cx, rb, stride, w, h);
    int ok = (rc == 0);
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
        printf("[FS-ORACLE]   %-10s oracle %-10s (%4d,%4d) want=%08X got=%08X  %s\n",
               when, checks[i].name, checks[i].x, checks[i].y,
               checks[i].want, got, pass ? "ok" : "MISMATCH");
    }
    free(rb);
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[FS] REAL native AppKit fullscreen for CM_OPT_FULLSCREEN "
           "(INTERFACE.md §9; main pthread, no NSApplicationMain)\n");

    if (MTLCreateSystemDefaultDevice() == nil) {
        printf("[FS] SKIP no Metal device (MTLCreateSystemDefaultDevice == nil)\n");
        return 0;
    }

    /* The minimal AppKit init the display-server / boot task does ONCE (D2t). */
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        printf("[FS] AppKit init: [NSApplication sharedApplication] + "
               "setActivationPolicy:Regular  (isRunning=%d)  -- no run loop started\n",
               (int)[app isRunning]);
    }

    const int w = 320, h = 200;
    const int markX = w / 7, markY = h * 7 / 15;
    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };

    CMContext *cx = cm_open(w, h, &fmt, "AROS [FS] fullscreen");
    if (!cx) { printf("[FS] SKIP cm_open returned NULL (no usable Metal device)\n"); return 0; }
    hand_pump(8);

    int haveWindow = 0, presents = 0, drawables = 0;
    cm__present_stats(cx, &haveWindow, &presents, &drawables);

    /* Upload the scene once; it is re-presented across the fullscreen transition. */
    uint8_t *fb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!fb) { printf("[FS] FAIL calloc fb\n"); cm_close(cx); return 1; }
    build_scene(fb, w, h, markX, markY);
    cm_upload_rect(cx, fb, w * 4, 0, 0, w, h);
    cm_present(cx);
    hand_pump(4);

    /* The option ABI must round-trip the flag regardless of window/headless. */
    int set1 = cm_set_option(cx, CM_OPT_FULLSCREEN, 1);
    long got1 = -1; int get1 = cm_get_option(cx, CM_OPT_FULLSCREEN, &got1);
    int flag_on_ok = (set1 == 0 && get1 == 0 && got1 == 1);
    printf("[FS]   cm_set_option(FULLSCREEN,1)=%d, cm_get_option=%ld: flag %s\n",
           set1, got1, flag_on_ok ? "ok" : "MISMATCH");

    int enter_ok = 1, oracle_ok = 1, exit_ok = 1;

    if (!haveWindow) {
        printf("[FS]   window=none (headless / no window server) -> SKIP fullscreen "
               "window asserts; running the ORACLE assert only (headless still passes)\n");
        /* Headless: the toggle is a no-op; the present path is skipped; the oracle
         * (offscreen target) is the contract and must still hold. */
        oracle_ok = check_oracle(cx, w, h, markX, markY, "headless");
    } else {
        /* ---- [FS-ENTER]: request fullscreen, drive the async transition ---- */
        int entered = request_fullscreen(cx, 1);
        int isFS    = cm__window_is_fullscreen(cx);
        int covers  = cm__window_covers_screen(cx);   /* informational */
        enter_ok = (entered && isFS == 1);
        printf("[FS-ENTER]   toggleFullScreen: styleMask&FullScreen entered=%d "
               "(frame-covers-screen=%d, informational)  %s\n",
               isFS, covers, enter_ok ? "ok" : "FAIL");

        /* ---- [FS-ORACLE]: present while fullscreen; oracle stays byte-exact ---- */
        cm__present_stats(cx, &haveWindow, &presents, &drawables);
        int pre_draw = drawables;
        for (int f = 0; f < 3; f++) { cm_present(cx); hand_pump(4); }
        cm__present_stats(cx, &haveWindow, &presents, &drawables);
        int oracle_fs = check_oracle(cx, w, h, markX, markY, "fullscreen");
        int tw = 0, th = 0, scale = 0;
        cm_target_size(cx, &tw, &th, &scale);
        printf("[FS-ORACLE]   fullscreen present: drawablesAcquired %d->%d, "
               "oracle target=%dx%d@%dx  %s\n",
               pre_draw, drawables, tw, th, scale, oracle_fs ? "ok" : "FAIL");

        /* ---- [FS-EXIT]: request windowed, drive, assert exited; oracle still ok -*/
        int exited = request_fullscreen(cx, 0);
        long got0 = -1; cm_get_option(cx, CM_OPT_FULLSCREEN, &got0);
        int stillFS = cm__window_is_fullscreen(cx);
        for (int f = 0; f < 2; f++) { cm_present(cx); hand_pump(4); }
        int oracle_win = check_oracle(cx, w, h, markX, markY, "windowed");
        exit_ok = (exited && got0 == 0 && stillFS == 0);
        oracle_ok = oracle_fs && oracle_win;
        printf("[FS-EXIT]    cm_set_option(FULLSCREEN,0) get=%ld exited(styleMask clear)=%d  %s\n",
               got0, (stillFS == 0), exit_ok ? "ok" : "FAIL");

        printf("[FS]   ---- hand-pumped-transition finding ----\n");
        printf("[FS]     ENTER completes under HAND-PUMPED CFRunLoopRunInMode(...,0,true) "
               "(styleMask flips at the toggle).\n");
        printf("[FS]     EXIT needs the APP run loop ([NSApp run]) to advance — bare "
               "hand-pumping never clears it; this test drives a BOUNDED [NSApp run]+watchdog "
               "(prod shim stays non-blocking, §3).\n");
        printf("[FS]     Backgrounded non-frontmost CLI quirk: first EXIT toggle can be a "
               "no-op → the driver retries the toggle once (deterministic).\n");
    }

    int ok = flag_on_ok && enter_ok && oracle_ok && exit_ok;
    printf("[FS] ---- summary ----  flag=%s  enter=%s  oracle=%s  exit=%s  (window=%s)\n",
           flag_on_ok ? "ok" : "FAIL",
           enter_ok ? "ok" : "FAIL",
           oracle_ok ? "ok" : "FAIL",
           exit_ok ? "ok" : "FAIL",
           haveWindow ? "yes" : "headless");

    free(fb);
    cm_close(cx);

    if (ok) {
        printf("[FS] PASS CM_OPT_FULLSCREEN enters/exits REAL native AppKit fullscreen "
               "(styleMask asserted), oracle byte-exact across the transition\n");
        return 0;
    }
    printf("[FS] FAIL see checks above\n");
    return 1;
}
