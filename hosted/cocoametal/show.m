/* show.m — PERSISTENT human-facing "look at it" build for the Cocoa/Metal shim.
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/INTERFACE.md
 * (§2a live-present-fills-drawable contract, §9 fullscreen) + spec.md + cocoametal.h.
 * No GPL emulator source (vAmiga/WinUAE/FS-UAE/Amiberry/E-UAE) was read, searched, or
 * consulted. Apple AppKit/Metal/QuartzCore/Foundation docs only [PUB].
 *
 * NOT part of the regression matrix — this is the build the USER runs to VISUALLY
 * confirm the live present fills the window/screen (on-screen look is human-judged).
 * It opens the window, draws an OBVIOUS scene (4 quadrant colours + a 1px BRIGHT WHITE
 * border at the framebuffer edges so edge-fill is unmistakable + a magenta marker),
 * presents CONTINUOUSLY, stays WINDOWED a few seconds, then ENTERS FULLSCREEN and
 * stays so you can SEE it fill the screen. Aspect-fit (default) keeps 320x200's shape;
 * any bars are BLACK (never white). Bounded: auto-exits after ~20s or on a key press
 * so it can never hang.
 *
 * Unlike the regression tests (which stay non-blocking per §3), this human-facing tool
 * legitimately drives [NSApp run] so the window stays live and animating while you
 * watch; a repeating timer presents frames and steps the windowed -> fullscreen ->
 * exit sequence; a watchdog timer stops the run loop at the deadline.
 */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "cocoametal.h"

#define C_TL   0xFFFF0000u   /* red    */
#define C_TR   0xFF00FF00u   /* green  */
#define C_BL   0xFF0000FFu   /* blue   */
#define C_BR   0xFFFFFF00u   /* yellow */
#define C_MARK 0xFFFF00FFu   /* magenta marker */
#define C_EDGE 0xFFFFFFFFu   /* bright white 1px border (edge-fill tell) */

static inline void put_bgra(uint8_t *fb, int fbw, int x, int y, uint32_t argb) {
    uint8_t *p = &fb[((size_t)y * fbw + x) * 4];
    p[0] = (uint8_t)(argb); p[1] = (uint8_t)(argb >> 8);
    p[2] = (uint8_t)(argb >> 16); p[3] = (uint8_t)(argb >> 24);
}

/* The obvious scene: 4 quadrants, a 1px bright-white border at the framebuffer edges
 * (so you can SEE the content reaches the very edge of the filled area), and a marker.
 * `phase` slides the marker so you can tell frames are actually being presented. */
static void build_scene(uint8_t *fb, int w, int h, int phase) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = (x < w/2) ? ((y < h/2) ? C_TL : C_BL)
                                   : ((y < h/2) ? C_TR : C_BR);
            if (x == 0 || y == 0 || x == w-1 || y == h-1) c = C_EDGE;  /* edge border */
            put_bgra(fb, w, x, y, c);
        }
    int mx = (phase % (w - 4)) + 2, my = h/2;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            put_bgra(fb, w, mx + dx, my + dy, C_MARK);   /* a moving 5x5 marker */
}

@interface CMShowDriver : NSObject
@property (nonatomic, assign) CMContext *cx;
@property (nonatomic, assign) int w;
@property (nonatomic, assign) int h;
@property (nonatomic, assign) int frame;
@property (nonatomic, assign) BOOL wentFullscreen;
@property (nonatomic, strong) NSDate *deadline;
@end

@implementation CMShowDriver
- (void)tick:(NSTimer *)t {
    (void)t;
    uint8_t *fb = (uint8_t *)malloc((size_t)self.w * self.h * 4);
    if (fb) {
        build_scene(fb, self.w, self.h, self.frame);
        cm_upload_rect(self.cx, fb, self.w * 4, 0, 0, self.w, self.h);
        free(fb);
    }
    cm_present(self.cx);

    /* After ~4s of windowed display, enter fullscreen ONCE so the user sees it fill
     * the whole screen. */
    if (!self.wentFullscreen && self.frame == 240) {   /* ~4s at 60fps */
        printf("[SHOW] entering fullscreen — it should FILL the screen "
               "(black bars only if the screen aspect differs; never white)\n");
        cm_set_option(self.cx, CM_OPT_FULLSCREEN, 1);
        self.wentFullscreen = YES;
    }

    /* Drain events so a key press can end it early (and the window stays responsive). */
    CMEvent ev[16];
    int n = cm_pump_events(self.cx, ev, 16);
    for (int i = 0; i < n; i++) {
        if (ev[i].type == CM_EV_KEY && ev[i].pressed) {
            printf("[SHOW] key pressed — exiting\n");
            [NSApp stop:nil];
        }
        if (ev[i].type == CM_EV_CLOSE) { printf("[SHOW] close — exiting\n"); [NSApp stop:nil]; }
    }

    if ([[NSDate date] compare:self.deadline] == NSOrderedDescending) {
        printf("[SHOW] auto-exit deadline reached — exiting\n");
        [NSApp stop:nil];
    }
    self.frame++;
}
@end

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (MTLCreateSystemDefaultDevice() == nil) {
        printf("[SHOW] no Metal device — nothing to show\n"); return 0;
    }
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    [app activateIgnoringOtherApps:YES];

    const int w = 320, h = 200;
    CMPixelDesc fmt = {
        .bytesPerPixel = 4, .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00, .redMask = 0x00FF0000, .alphaMask = 0xFF000000,
    };
    CMContext *cx = cm_open(w, h, &fmt, "AROS Cocoa/Metal — live present (look: it should FILL)");
    if (!cx) { printf("[SHOW] cm_open NULL — no usable Metal device\n"); return 0; }

    printf("[SHOW] window is up. Watch ~4s WINDOWED (scene fills the window, white edge "
           "border visible), then it goes FULLSCREEN and should FILL the screen. "
           "Auto-exits ~20s; press any key or Esc, or close the window, to exit early.\n");

    CMShowDriver *drv = [[CMShowDriver alloc] init];
    drv.cx = cx; drv.w = w; drv.h = h; drv.frame = 0; drv.wentFullscreen = NO;
    drv.deadline = [NSDate dateWithTimeIntervalSinceNow:20.0];

    NSTimer *timer = [NSTimer scheduledTimerWithTimeInterval:(1.0/60.0)
                                                     repeats:YES
                                                       block:^(NSTimer *t){ [drv tick:t]; }];
    [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];

    [app run];     /* human-facing: a real run loop so the window stays live + animating */

    cm_set_option(cx, CM_OPT_FULLSCREEN, 0);   /* leave no fullscreen Space behind */
    [timer invalidate];
    cm_close(cx);
    printf("[SHOW] done.\n");
    return 0;
}
