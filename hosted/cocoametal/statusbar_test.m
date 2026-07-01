/* statusbar_test.m — [STATUS] verify the status-bar LEDs + theme against the REAL dylib.
 *
 * The footer LEDs and the Dark/Light/System theme are HOST CHROME outside the Metal
 * view, so the offscreen oracle (cm_readback) cannot see them — this test asserts the
 * AppKit objects directly (the same approach shell_test uses for the menu tree), so
 * the proof stays unattended (no screencapture / TCC):
 *   (1) cm_open built the status bar: an NSVisualEffectView holding a CMLEDView, plus
 *       the Macaros brand label, under the AROS window's content view.
 *   (2) cm_set_option(CM_OPT_THEME, …) drives NSApp.appearance: Dark→DarkAqua,
 *       Light→Aqua, System→nil; an out-of-range value is rejected; get roundtrips.
 *   (3) the Activity LED lights when AROS presents frames and decays when it stops
 *       (read via KVC on the CMLEDView's `activity`, ticked by its main-loop timer).
 *
 * dlopens build/cocoametal.dylib exactly as HostLib_Open does (like shell_test).
 * Independent work: AppKit docs [PUB] only.
 */
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "cocoametal.h"

typedef CMContext *(*open_fn)(int, int, const CMPixelDesc *, const char *);
typedef void       (*close_fn)(CMContext *);
typedef int        (*setopt_fn)(CMContext *, int, long);
typedef int        (*getopt_fn)(CMContext *, int, long *);
typedef void       (*upload_fn)(CMContext *, const void *, int, int, int, int, int);
typedef void       (*present_fn)(CMContext *);

static int g_fail = 0;
static int check(int c, const char *w) { if (!c) { g_fail++; printf("    FAIL: %s\n", w); } return c; }

static NSView *find_class(NSView *v, NSString *clsName) {
    if ([NSStringFromClass([v class]) isEqualToString:clsName]) return v;
    for (NSView *s in v.subviews) { NSView *r = find_class(s, clsName); if (r) return r; }
    return nil;
}
static int view_has_label(NSView *v, NSString *needle) {
    if ([v isKindOfClass:[NSTextField class]] &&
        [[(NSTextField *)v stringValue] containsString:needle]) return 1;
    for (NSView *s in v.subviews) if (view_has_label(s, needle)) return 1;
    return 0;
}
static void spin(double secs) {
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:secs]];
}

int main(int argc, const char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = (argc > 1) ? argv[1]
                     : getenv("COCOAMETAL_DYLIB") ? getenv("COCOAMETAL_DYLIB")
                     : "build/cocoametal.dylib";
    printf("[STATUS] status-bar LEDs + theme against the REAL dylib %s\n", path);

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("[STATUS] FAIL dlopen: %s\n", dlerror()); return 1; }
    open_fn    cm_open_    = (open_fn)   dlsym(h, "cm_open");
    close_fn   cm_close_   = (close_fn)  dlsym(h, "cm_close");
    setopt_fn  cm_set_     = (setopt_fn) dlsym(h, "cm_set_option");
    getopt_fn  cm_get_     = (getopt_fn) dlsym(h, "cm_get_option");
    upload_fn  cm_upload_  = (upload_fn) dlsym(h, "cm_upload_rect");
    present_fn cm_present_ = (present_fn)dlsym(h, "cm_present");
    if (!cm_open_ || !cm_close_ || !cm_set_ || !cm_get_ || !cm_upload_ || !cm_present_) {
        printf("[STATUS] FAIL dlsym\n"); return 1;
    }

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        CMPixelDesc fmt = { .bytesPerPixel = 4,
            .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
            .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
            .redMask = 0x00FF0000, .alphaMask = 0xFF000000 };
        CMContext *cx = cm_open_(320, 200, &fmt, "AROS [STATUS]");
        if (!cx) { printf("[STATUS] FAIL cm_open NULL\n"); return 1; }

        /* Find the AROS window by its unique Metal content view (CMContentView). */
        NSWindow *win = nil;
        for (NSWindow *w in [NSApp windows])
            if (find_class(w.contentView, @"CMContentView")) { win = w; break; }
        check(win != nil, "cm_open created the AROS window");

        /* (1) the status bar was built: a vibrancy bg + the LED view + the brand */
        NSView *bar = win ? find_class(win.contentView, @"NSVisualEffectView") : nil;
        NSView *leds = win ? find_class(win.contentView, @"CMLEDView") : nil;
        check(bar  != nil, "status bar uses a native NSVisualEffectView background");
        check(leds != nil, "status bar contains the CMLEDView (Power + Activity LEDs)");
        check(win && view_has_label(win.contentView, @"Macaros"), "brand label still present");

        /* (2) theme -> NSApp.appearance, and the window tracks it; bad value rejected */
        check(cm_set_(cx, CM_OPT_THEME, CM_THEME_DARK) == 0, "set theme Dark ok");
        check([NSApp.appearance.name isEqual:NSAppearanceNameDarkAqua], "Dark -> NSApp DarkAqua");
        check(win && [win.appearance.name isEqual:NSAppearanceNameDarkAqua], "Dark -> window DarkAqua");

        check(cm_set_(cx, CM_OPT_THEME, CM_THEME_LIGHT) == 0, "set theme Light ok");
        check([NSApp.appearance.name isEqual:NSAppearanceNameAqua], "Light -> NSApp Aqua");

        check(cm_set_(cx, CM_OPT_THEME, CM_THEME_SYSTEM) == 0, "set theme System ok");
        check(NSApp.appearance == nil, "System -> NSApp appearance nil (follow macOS)");

        check(cm_set_(cx, CM_OPT_THEME, 99) != 0, "out-of-range theme rejected");
        long tv = -1;
        check(cm_get_(cx, CM_OPT_THEME, &tv) == 0 && tv == CM_THEME_SYSTEM,
              "get theme roundtrips last good value (System)");

        /* (3) Activity LED: while AROS presents, the timer lights it; when presenting
         * stops, it decays. Present continuously and track the PEAK (the LED flickers
         * up to full whenever a tick sees a fresh frame, then fades between frames). */
        if (leds) {
            const int W = 320, HH = 200;
            uint8_t *fb = (uint8_t *)calloc((size_t)W * HH, 4);
            double lit = 0.0;
            for (int f = 0; f < 10; f++) {                  /* ~0.6s of ongoing presents */
                cm_upload_(cx, fb, W * 4, 0, 0, W, HH);
                cm_present_(cx);
                spin(0.06);
                double v = [[(id)leds valueForKey:@"activity"] doubleValue];
                if (v > lit) lit = v;                        /* peak over the busy window */
            }
            free(fb);
            check(lit > 0.3, "Activity LED lit while AROS presents");

            spin(1.2);                                      /* no presents -> decay */
            double dim = [[(id)leds valueForKey:@"activity"] doubleValue];
            check(dim < 0.1, "Activity LED decays when AROS stops presenting");
            printf("    activity: peak=%.2f -> decayed=%.2f\n", lit, dim);

            /* (4) the LEDs actually RENDER pixels (the host chrome the oracle can't
             * see): re-light, render the CMLEDView into a bitmap with cacheDisplayInRect
             * (no screen capture / TCC), assert saturated (non-grey) LED color drew, and
             * drop a PNG artifact a human can glance at. */
            fb = (uint8_t *)calloc((size_t)W * HH, 4);
            for (int f = 0; f < 4; f++) { cm_upload_(cx, fb, W * 4, 0, 0, W, HH); cm_present_(cx); }
            free(fb);
            spin(0.12);                                     /* one tick: activity -> lit */

            NSRect b = leds.bounds;
            NSBitmapImageRep *rep = [leds bitmapImageRepForCachingDisplayInRect:b];
            [leds cacheDisplayInRect:b toBitmapImageRep:rep];
            int sat = 0, greenish = 0, amberish = 0;
            for (NSInteger yy = 0; yy < rep.pixelsHigh; yy++)
                for (NSInteger xx = 0; xx < rep.pixelsWide; xx++) {
                    NSColor *c = [[rep colorAtX:xx y:yy]
                                     colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
                    CGFloat r = 0, g = 0, bl = 0, a = 0;
                    [c getRed:&r green:&g blue:&bl alpha:&a];
                    CGFloat mx = MAX(r, MAX(g, bl)), mn = MIN(r, MIN(g, bl));
                    if (mx - mn > 0.25) sat++;                       /* a colored (non-grey) pixel */
                    if (g > 0.5 && r < 0.55 && bl < 0.55) greenish++; /* power LED green */
                    if (r > 0.6 && g > 0.25 && g < 0.7 && bl < 0.4) amberish++; /* activity amber */
                }
            check(sat > 0, "LEDs render saturated (non-grey) pixels");
            check(greenish > 0, "Power LED renders green");
            check(amberish > 0, "Activity LED renders amber");

            const char *rd = getenv("AROS_RUN_DIR");
            NSString *dir = (rd && *rd) ? @(rd) : NSTemporaryDirectory();
            NSString *png = [dir stringByAppendingPathComponent:@"aros-statusbar-leds.png"];
            NSData *data = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            [data writeToFile:png atomically:YES];
            printf("    LED render: sat=%d green=%d amber=%d -> %s\n",
                   sat, greenish, amberish, png.UTF8String);
        }

        cm_close_(cx);
    }

    int ok = (g_fail == 0);
    printf("[STATUS] %s\n", ok
        ? "PASS — status bar built, theme drives NSApp.appearance, Activity LED tracks presents"
        : "FAIL see checks above");
    return ok ? 0 : 1;
}
