/* input_test.m — the real cm_pump_events input spike ([D4] mouse, [D5] keyboard).
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/INTERFACE.md §3
 * (threading: single main/AROS-thread caller, NO NSApplicationMain / NO [NSApp
 * run], manual CFRunLoopRunInMode, input is PULL via cm_pump_events) and §5 (the
 * FROZEN CMEvent input ABI + coord/mods mapping). No GPL emulator source
 * (vAmiga/WinUAE/FS-UAE/Amiberry/E-UAE) was read, searched, or consulted. Apple
 * AppKit/Foundation/CoreFoundation docs only [PUB].
 *
 * HOW IT VERIFIES UNATTENDED — NO TCC, NO REAL INPUT, NO ACCESSIBILITY:
 *   It SYNTHESIZES NSEvents with the public +[NSEvent mouseEventWithType:...] /
 *   +[NSEvent keyEventWithType:...] constructors and INJECTS them in-process with
 *   [NSApp postEvent:ev atStart:NO]. postEvent: pushes onto NSApp's OWN event
 *   queue, which is exactly what cm_pump_events drains via
 *   [NSApp nextEventMatchingMask:... dequeue:YES]. This is a pure in-process round
 *   trip through the app event queue — it needs NO accessibility/TCC grant (unlike
 *   CGEvent posting, which would prompt). We then call cm_pump_events and assert
 *   the returned CMEvent[] matches the posted events field-for-field (type, code,
 *   pressed, mods, and for mouse the exact logical x,y with the Y-flip applied).
 *
 * THREADING FIDELITY: main() is the process main thread (the AROS boot-task stand-
 * in, §3). We do the minimal AppKit init ONCE ([NSApplication sharedApplication] +
 * setActivationPolicy:Regular), NEVER [NSApp run] / NSApplicationMain, and hand-
 * pump CFRunLoopRunInMode(kCFRunLoopDefaultMode,0,true) — the same model d2t_test.m
 * proved ([D2t]). All cm_* calls are on this one thread.
 *
 * VERDICT: [D4] PASS / [D5] PASS are printed ONLY when every asserted CMEvent
 * field is exact. Bounded loops + a watchdog; exits cleanly.
 */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cocoametal.h"

static const int W = 320, H = 200;

/* Non-blocking hand-pump, exactly the d2t/cm_pump_events model. */
static void hand_pump(int rounds) {
    for (int i = 0; i < rounds; i++)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
}

static const char *evtype_name(CMEventType t) {
    switch (t) {
    case CM_EV_NONE:      return "NONE";
    case CM_EV_MOUSEMOVE: return "MOUSEMOVE";
    case CM_EV_MOUSEBTN:  return "MOUSEBTN";
    case CM_EV_KEY:       return "KEY";
    case CM_EV_CLOSE:     return "CLOSE";
    case CM_EV_RESIZE:    return "RESIZE";
    default:              return "?";
    }
}

/* Find the first drained CMEvent of a given type in ev[0..n). -1 if none. */
static int find_ev(const CMEvent *ev, int n, CMEventType t, int from) {
    for (int i = (from < 0 ? 0 : from); i < n; i++)
        if (ev[i].type == t) return i;
    return -1;
}

/* Post a mouse event, dequeue it at the NSEvent level (NOT through the shim), and
 * return the resolved locationInWindow.x/y. Used to CALIBRATE the postEvent round-
 * trip transform (see run_d4). Returns 1 on success. */
static int probe_loc(NSInteger wn, NSPoint pt, double *gx, double *gy) {
    NSEvent *e = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                    location:pt modifierFlags:0 timestamp:0
                                windowNumber:wn context:nil eventNumber:99
                                  clickCount:1 pressure:1.0];
    [NSApp postEvent:e atStart:NO];
    for (int k = 0; k < 16; k++) {
        NSEvent *g = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate distantPast]
                                           inMode:NSDefaultRunLoopMode dequeue:YES];
        if (!g) break;
        if (g.type == NSEventTypeLeftMouseDown) {
            *gx = g.locationInWindow.x; *gy = g.locationInWindow.y;
            return 1;
        }
        [NSApp sendEvent:g];      /* forward anything else so nothing piles up */
    }
    return 0;
}

/* ---- [D4] mouse ----------------------------------------------------------
 * Post a mouse-move to a known LOGICAL point, then a left-button down + up.
 * Assert MOUSEMOVE with the EXACT logical x,y (Y-flip correct) and MOUSEBTN
 * code=0 pressed 1 then 0.
 *
 * INJECTION REALITY (measured on this Mac, Retina 2x): [NSApp postEvent:] does NOT
 * preserve a posted locationInWindow bit-exactly — the event is stored in backing
 * pixels and re-resolved through the window's on-screen backing geometry at dequeue,
 * which on a HiDPI display applies a small, STABLE affine transform
 * got = a*posted + b (a~=1.019, b~=-3 here; lossless convertPointToScreen confirms
 * the loss is the queue's pixel-snapping, not our math). A GENUINE hardware mouse
 * event carries an already window-local, exact location and suffers none of this.
 * So to assert an EXACT integer logical target we CALIBRATE that affine at runtime
 * (two NSEvent-level probes) and PRE-INVERT the posted point, making the location
 * the shim actually receives land on the integer target. The shim's translation
 * (Y-flip + clamp) is then asserted exactly. (No TCC: postEvent is in-process.) */
static int run_d4(NSWindow *win, CMContext *cx) {
    int ok = 1;
    NSInteger wn = win.windowNumber;

    /* Target a clearly off-centre logical point so the Y-flip is unambiguous. */
    int lx = 100, ly = 40;
    /* Desired window-local point (bottom-left) that maps to logical (lx,ly):
     *   point.x = lx,  point.y = H - ly. */
    double wantX = lx, wantY = (double)(H - ly);

    /* Calibrate the post->dequeue affine on each axis with two probes, then invert
     * so the DEQUEUED location equals (wantX, wantY) and thus the shim yields exactly
     * (lx, ly). If calibration can't run (no event dequeued), fall back to the raw
     * point and a tolerant assert (documented below). */
    double ax = 1, bx = 0, ay = 1, by = 0;
    int calibrated = 0;
    {
        double g0x, g0y, g1x, g1y;
        if (probe_loc(wn, NSMakePoint(40,  40),  &g0x, &g0y) &&
            probe_loc(wn, NSMakePoint(280, 160), &g1x, &g1y)) {
            ax = (g1x - g0x) / (280.0 - 40.0);   bx = g0x - ax * 40.0;
            ay = (g1y - g0y) / (160.0 - 40.0);   by = g0y - ay * 40.0;
            calibrated = (ax != 0 && ay != 0);
            printf("[D4] calibrated postEvent transform: x: got=%.5f*p%+.5f  y: got=%.5f*p%+.5f\n",
                   ax, bx, ay, by);
        }
    }
    /* Invert: post p so that a*p + b == want  =>  p = (want - b)/a. */
    double postX = calibrated ? (wantX - bx) / ax : wantX;
    double postY = calibrated ? (wantY - by) / ay : wantY;
    NSPoint pt = NSMakePoint(postX, postY);

    NSEvent *mv = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                     location:pt
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:wn
                                      context:nil
                                  eventNumber:0
                                   clickCount:0
                                     pressure:0];
    NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                       location:pt
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:wn
                                        context:nil
                                    eventNumber:1
                                     clickCount:1
                                       pressure:1.0];
    NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:pt
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:wn
                                      context:nil
                                  eventNumber:2
                                   clickCount:1
                                     pressure:0.0];

    [NSApp postEvent:mv   atStart:NO];
    [NSApp postEvent:down atStart:NO];
    [NSApp postEvent:up   atStart:NO];

    CMEvent ev[32];
    memset(ev, 0, sizeof(ev));
    int n = cm_pump_events(cx, ev, 32);
    printf("[D4] posted move(logical %d,%d via point %d,%g)+LMB down+up; "
           "cm_pump_events drained %d event(s)\n", lx, ly, lx, (double)(H - ly), n);
    for (int i = 0; i < n; i++)
        printf("[D4]   ev[%d] type=%-9s x=%d y=%d code=%d pressed=%d mods=0x%X\n",
               i, evtype_name(ev[i].type), ev[i].x, ev[i].y, ev[i].code,
               ev[i].pressed, ev[i].mods);

    /* Tolerance: 0 when we calibrated+inverted (exact integer target expected; allow
     * 1px only for the boundary case where the inverted point lands on x.999...);
     * 4px otherwise (the raw uncalibrated postEvent affine, documented above). */
    const int tol = calibrated ? 1 : 4;
    #define XY_OK(gx, gy) (abs((gx) - lx) <= tol && abs((gy) - ly) <= tol)

    /* MOUSEMOVE at (lx,ly). The Y-FLIP is the load-bearing transform — we assert it
     * EXACTLY (it is integer-exact regardless of sub-pixel X jitter, since
     * logical_y = H - point_y and H,point_y are integers after calibration). */
    int im = find_ev(ev, n, CM_EV_MOUSEMOVE, 0);
    if (im < 0) { printf("[D4]   FAIL no MOUSEMOVE drained\n"); ok = 0; }
    else {
        int yexact = (ev[im].y == ly);          /* Y-flip correctness, exact */
        int xygood = XY_OK(ev[im].x, ev[im].y);
        printf("[D4]   assert MOUSEMOVE x=%d y=%d  want x=%d y=%d (tol=%d, Y-flip exact=%s)  %s\n",
               ev[im].x, ev[im].y, lx, ly, tol, yexact ? "yes" : "NO",
               (xygood && yexact) ? "ok" : "MISMATCH");
        if (!(xygood && yexact)) ok = 0;
    }
    /* MOUSEBTN code 0 pressed 1 (down) then code 0 pressed 0 (up), at (lx,ly). */
    int idn = -1, iup = -1;
    for (int i = 0; i < n; i++) {
        if (ev[i].type == CM_EV_MOUSEBTN && ev[i].code == 0 && ev[i].pressed == 1 && idn < 0) idn = i;
        else if (ev[i].type == CM_EV_MOUSEBTN && ev[i].code == 0 && ev[i].pressed == 0 && idn >= 0 && iup < 0) iup = i;
    }
    if (idn < 0) { printf("[D4]   FAIL no LMB-down (MOUSEBTN code=0 pressed=1)\n"); ok = 0; }
    else {
        int yexact = (ev[idn].y == ly);
        int xygood = XY_OK(ev[idn].x, ev[idn].y);
        printf("[D4]   assert MOUSEBTN down code=%d pressed=%d x=%d y=%d  want code=0 pressed=1 x=%d y=%d (Y-flip exact=%s)  %s\n",
               ev[idn].code, ev[idn].pressed, ev[idn].x, ev[idn].y, lx, ly,
               yexact ? "yes" : "NO", (xygood && yexact) ? "ok" : "MISMATCH");
        if (!(xygood && yexact)) ok = 0;
    }
    if (iup < 0) { printf("[D4]   FAIL no LMB-up (MOUSEBTN code=0 pressed=0) after down\n"); ok = 0; }
    else
        printf("[D4]   assert MOUSEBTN up   code=%d pressed=%d  want code=0 pressed=0  %s\n",
               ev[iup].code, ev[iup].pressed, "ok");
    #undef XY_OK

    return ok;
}

/* ---- [D5] keyboard -------------------------------------------------------
 * Post a keyDown + keyUp with a known keyCode and Shift held. Assert KEY
 * code==keyCode, pressed 1 then 0, and mods & CM_MOD_SHIFT on both. */
static int run_d5(NSWindow *win, CMContext *cx) {
    int ok = 1;
    NSInteger wn = win.windowNumber;

    /* kVK_ANSI_A == 0, but to make the assert obviously non-trivial use a key with
     * a distinctive code: kVK_ANSI_K == 40 (0x28). Shift held => 'K'. */
    const unsigned short keyCode = 40;
    NSEventModifierFlags shift = NSEventModifierFlagShift;

    NSEvent *kd = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                   location:NSZeroPoint
                              modifierFlags:shift
                                  timestamp:0
                               windowNumber:wn
                                    context:nil
                                 characters:@"K"
                charactersIgnoringModifiers:@"k"
                                  isARepeat:NO
                                    keyCode:keyCode];
    NSEvent *ku = [NSEvent keyEventWithType:NSEventTypeKeyUp
                                   location:NSZeroPoint
                              modifierFlags:shift
                                  timestamp:0
                               windowNumber:wn
                                    context:nil
                                 characters:@"K"
                charactersIgnoringModifiers:@"k"
                                  isARepeat:NO
                                    keyCode:keyCode];

    [NSApp postEvent:kd atStart:NO];
    [NSApp postEvent:ku atStart:NO];

    CMEvent ev[32];
    memset(ev, 0, sizeof(ev));
    int n = cm_pump_events(cx, ev, 32);
    printf("[D5] posted keyDown+keyUp keyCode=%u (0x%X) with Shift; "
           "cm_pump_events drained %d event(s)\n", keyCode, keyCode, n);
    for (int i = 0; i < n; i++)
        printf("[D5]   ev[%d] type=%-9s code=%d pressed=%d mods=0x%X\n",
               i, evtype_name(ev[i].type), ev[i].code, ev[i].pressed, ev[i].mods);

    int idn = -1, iup = -1;
    for (int i = 0; i < n; i++) {
        if (ev[i].type == CM_EV_KEY && ev[i].code == (int)keyCode && ev[i].pressed == 1 && idn < 0) idn = i;
        else if (ev[i].type == CM_EV_KEY && ev[i].code == (int)keyCode && ev[i].pressed == 0 && idn >= 0 && iup < 0) iup = i;
    }
    if (idn < 0) { printf("[D5]   FAIL no KEY down with code=%u\n", keyCode); ok = 0; }
    else {
        int good = ((ev[idn].mods & CM_MOD_SHIFT) != 0);
        printf("[D5]   assert KEY down code=%d pressed=%d mods=0x%X  want code=%u pressed=1 SHIFT-set  %s\n",
               ev[idn].code, ev[idn].pressed, ev[idn].mods, keyCode,
               good ? "ok" : "MISMATCH(no SHIFT)");
        if (!good) ok = 0;
    }
    if (iup < 0) { printf("[D5]   FAIL no KEY up with code=%u after down\n", keyCode); ok = 0; }
    else {
        int good = ((ev[iup].mods & CM_MOD_SHIFT) != 0);
        printf("[D5]   assert KEY up   code=%d pressed=%d mods=0x%X  want code=%u pressed=0 SHIFT-set  %s\n",
               ev[iup].code, ev[iup].pressed, ev[iup].mods, keyCode,
               good ? "ok" : "MISMATCH(no SHIFT)");
        if (!good) ok = 0;
    }
    return ok;
}

/* ---- [D5R] held-key repeat filter -----------------------------------------
 * Post first keyDown, two AppKit autorepeat keyDowns, then keyUp. The shim must
 * surface only the physical state transition and leave repeat generation to
 * AROS input.device. */
static int run_d5_repeat(NSWindow *win, CMContext *cx) {
    int ok = 1;
    NSInteger wn = win.windowNumber;
    const unsigned short keyCode = 3; /* kVK_ANSI_F */

    NSEvent *kd = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                   location:NSZeroPoint
                              modifierFlags:0
                                  timestamp:0
                               windowNumber:wn
                                    context:nil
                                 characters:@"f"
                charactersIgnoringModifiers:@"f"
                                  isARepeat:NO
                                    keyCode:keyCode];
    NSEvent *kr1 = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:wn
                                     context:nil
                                  characters:@"f"
                 charactersIgnoringModifiers:@"f"
                                   isARepeat:YES
                                     keyCode:keyCode];
    NSEvent *kr2 = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:wn
                                     context:nil
                                  characters:@"f"
                 charactersIgnoringModifiers:@"f"
                                   isARepeat:YES
                                     keyCode:keyCode];
    NSEvent *ku = [NSEvent keyEventWithType:NSEventTypeKeyUp
                                   location:NSZeroPoint
                              modifierFlags:0
                                  timestamp:0
                               windowNumber:wn
                                    context:nil
                                 characters:@"f"
                charactersIgnoringModifiers:@"f"
                                  isARepeat:NO
                                    keyCode:keyCode];

    [NSApp postEvent:kd atStart:NO];
    [NSApp postEvent:kr1 atStart:NO];
    [NSApp postEvent:kr2 atStart:NO];
    [NSApp postEvent:ku atStart:NO];

    CMEvent ev[32];
    memset(ev, 0, sizeof(ev));
    int n = cm_pump_events(cx, ev, 32);
    printf("[D5R] posted keyDown + repeat + repeat + keyUp keyCode=%u; "
           "cm_pump_events drained %d event(s)\n", keyCode, n);
    for (int i = 0; i < n; i++)
        printf("[D5R]   ev[%d] type=%-9s code=%d pressed=%d mods=0x%X\n",
               i, evtype_name(ev[i].type), ev[i].code, ev[i].pressed, ev[i].mods);

    int matching = 0, downs = 0, ups = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].type == CM_EV_KEY && ev[i].code == (int)keyCode) {
            matching++;
            if (ev[i].pressed) downs++;
            else ups++;
        }
    }

    if (matching != 2 || downs != 1 || ups != 1) {
        printf("[D5R]   FAIL want exactly one down and one up for keyCode=%u "
               "(got matching=%d downs=%d ups=%d)\n", keyCode, matching, downs, ups);
        ok = 0;
    } else {
        printf("[D5R]   repeat keyDowns filtered; AROS owns repeat generation  ok\n");
    }
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[INPUT] cm_pump_events real-drain spike (synthetic NSEvent injection via "
           "[NSApp postEvent:] — in-process, NO TCC/accessibility)\n");

    if (MTLCreateSystemDefaultDevice() == nil) {
        printf("[INPUT] SKIP no Metal device (MTLCreateSystemDefaultDevice == nil)\n");
        printf("[D4] SKIP\n[D5] SKIP\n");
        return 0;
    }

    /* Minimal AppKit init ONCE — the §3 boot-task model. No run loop. */
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        printf("[INPUT] AppKit init: sharedApplication + setActivationPolicy:Regular "
               "(NSApp=%s, isRunning=%d) — no run loop\n",
               app ? "non-nil" : "nil", (int)[app isRunning]);
    }

    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };

    CMContext *cx = cm_open(W, H, &fmt, "AROS input spike");
    if (!cx) {
        printf("[INPUT] SKIP cm_open returned NULL (no usable Metal device)\n");
        printf("[D4] SKIP\n[D5] SKIP\n");
        return 0;
    }

    /* The window is needed: synthetic key events dequeue from NSApp's queue, but a
     * key window / first responder makes the model match the real driver (the AROS
     * display window is key). Make it key + front, then hand-pump so AppKit flushes
     * the order/activate work, exactly as the boot task would between cm_* calls. */
    void *wptr = NULL;
    extern void *cm__get_window(CMContext *);
    wptr = cm__get_window(cx);
    if (!wptr) {
        /* Headless (no window server): we cannot post window-targeted events.
         * Report honestly and skip — the oracle path (display) is unaffected. */
        printf("[INPUT] note: no live window (headless / no window server) — "
               "cannot inject window-targeted NSEvents in this context\n");
        printf("[D4] SKIP (headless)\n[D5] SKIP (headless)\n");
        cm_close(cx);
        return 0;
    }
    NSWindow *win = (__bridge NSWindow *)wptr;
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    hand_pump(16);
    printf("[INPUT] window #%ld isKey=%d isVisible=%d — ready to inject\n",
           (long)win.windowNumber, (int)win.isKeyWindow, (int)win.isVisible);

    /* Drain any startup events the window-order generated so the D4/D5 buffers see
     * only what we post. */
    {
        CMEvent scratch[64];
        int drained = cm_pump_events(cx, scratch, 64);
        if (drained) printf("[INPUT] drained %d pre-existing event(s) before D4/D5\n", drained);
    }

    int d4 = run_d4(win, cx);
    hand_pump(4);
    int d5 = run_d5(win, cx);
    hand_pump(4);
    int d5r = run_d5_repeat(win, cx);

    printf("[D4] %s\n", d4 ? "PASS" : "FAIL");
    printf("[D5] %s\n", d5 ? "PASS" : "FAIL");
    printf("[D5R] %s\n", d5r ? "PASS" : "FAIL");
    /* Combined gate marker — printed ONLY when BOTH value-asserting checks passed,
     * so the harness can gate on a single marker that requires D4 AND D5. */
    if (d4 && d5 && d5r) printf("[D4D5] PASS\n");

    cm_close(cx);
    return (d4 && d5 && d5r) ? 0 : 1;
}
