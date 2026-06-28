/* cocoametal_statusbar.m — the window status bar: Amiga-style Power + Activity
 * LEDs and the host app theme (Dark / Light / System).
 *
 * The footer strip under the Metal view (cocoametal_window.m) becomes a proper
 * status bar: a NATIVE vibrancy background (NSVisualEffectView, titlebar material)
 * that tracks the active appearance for free, a brand label on the left, and an LED
 * cluster on the right. Two LEDs, driven by what the HOST can honestly observe:
 *   - POWER   — green while running; amber/red on a CM_OPT_POWER lifecycle request.
 *   - ACTIVITY— lit by cm_present cadence (AROS pushed a frame => the machine is
 *               busy). Honestly an activity light, NOT a real DF0 disk LED (the host
 *               cannot see AROS disk I/O without an OS-tree hook + an ABI bump).
 *
 * Theme is the host-acted CM_OPT_THEME option: cm__apply_theme_appkit (the strong
 * override of the weak stub in cocoametal.m) sets NSApp.appearance, so every chrome
 * surface — title bar, menus, Settings, this status bar — themes coherently. The
 * black Metal area (the guest framebuffer) is deliberately never recolored.
 *
 * Strong override split, same as cocoametal_shell.m / cocoametal_window.m: the
 * AppKit-free cocoametal.m carries weak no-op stubs; this file (linked only into the
 * AppKit dylib) supplies the real behavior. It reaches the few CMContext fields it
 * needs through the controlled accessors in cocoametal.m (cm__present_count /
 * cm__power_state), never by duplicating the struct.
 *
 * Independent work: no third-party implementation source — emulator, agent, driver,
 * or otherwise — was read, searched, or consulted in producing it, and any
 * resemblance to existing implementations is coincidental. Apple AppKit/CoreGraphics
 * docs [PUB] + Apple HIG [PUB] only.
 */
#import <AppKit/AppKit.h>
#include "cocoametal.h"

/* Controlled accessors implemented in cocoametal.m (the CMContext struct lives
 * there). cm__present_count is monotonic; cm__power_state is the last CM_POWER_*
 * request or -1 = none (running). */
int cm__present_count(CMContext *cx);
int cm__power_state(CMContext *cx);

/* ----------------------------------------------------------------- LED view --
 * Draws the two LEDs (each a soft glow + a solid dot + a small specular highlight)
 * with a short label, flowing left to right. Colors are deliberate and constant;
 * the label text uses the semantic secondaryLabelColor so it tracks the theme. */
@interface CMLEDView : NSView
@property (nonatomic) CGFloat activity;   /* 0..1, decays toward 0 between frames */
@property (nonatomic) int     powerReq;   /* a CM_POWER_*, or -1 = running */
@end

@implementation CMLEDView

- (BOOL)isOpaque { return NO; }

/* Power LED color from the lifecycle state: green = running, amber = soft/reset
 * request in flight, red = forced down/quit. */
- (NSColor *)powerColor {
    switch (self.powerReq) {
    case CM_POWER_REQUEST_DOWN:
    case CM_POWER_RESET:        return [NSColor colorWithRed:0.96 green:0.66 blue:0.16 alpha:1.0]; /* amber */
    case CM_POWER_FORCE_DOWN:
    case CM_POWER_FORCE_QUIT:   return [NSColor colorWithRed:0.91 green:0.27 blue:0.22 alpha:1.0]; /* red   */
    default:                    return [NSColor colorWithRed:0.30 green:0.85 blue:0.42 alpha:1.0]; /* green */
    }
}

/* Activity LED color: a warm Amiga drive-amber, brightened by the activity level.
 * Idle is a dim ember (the LED is present but unlit), not fully black, so the
 * cluster always reads as "two LEDs". */
- (NSColor *)activityColor {
    CGFloat lvl = 0.18 + 0.82 * self.activity;   /* dim ember .. full */
    return [NSColor colorWithRed:1.00 * lvl green:0.58 * lvl blue:0.12 * lvl alpha:1.0];
}

/* Draw one glowing LED centered at (cx,cy) with the given radius + color. */
static void draw_led(CGContextRef g, CGFloat cx, CGFloat cy, CGFloat r, NSColor *color) {
    CGFloat rc[4]; NSColor *rgb = [color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    [rgb getRed:&rc[0] green:&rc[1] blue:&rc[2] alpha:&rc[3]];

    /* soft outer glow */
    CGContextSetRGBFillColor(g, rc[0], rc[1], rc[2], 0.28);
    CGContextFillEllipseInRect(g, CGRectMake(cx - r - 2.0, cy - r - 2.0, (r + 2.0) * 2.0, (r + 2.0) * 2.0));
    /* solid body */
    CGContextSetRGBFillColor(g, rc[0], rc[1], rc[2], 1.0);
    CGContextFillEllipseInRect(g, CGRectMake(cx - r, cy - r, r * 2.0, r * 2.0));
    /* specular highlight (top-left) */
    CGContextSetRGBFillColor(g, 1.0, 1.0, 1.0, 0.55);
    CGFloat hr = r * 0.38;
    CGContextFillEllipseInRect(g, CGRectMake(cx - r * 0.45 - hr, cy + r * 0.30 - hr, hr * 2.0, hr * 2.0));
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    CGContextRef g = [[NSGraphicsContext currentContext] CGContext];
    CGFloat cy = NSMidY(self.bounds);
    CGFloat r  = 4.0;

    NSDictionary *attrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:9 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor secondaryLabelColor],
    };
    struct { NSString *label; NSColor *color; } leds[2] = {
        { @"PWR", [self powerColor]    },   /* power: real (running + lifecycle) */
        { @"ACT", [self activityColor] },   /* activity: present cadence (honest — not DF0 disk I/O) */
    };

    CGFloat x = 6.0;
    for (int i = 0; i < 2; i++) {
        NSString *t = leds[i].label;
        NSSize ts = [t sizeWithAttributes:attrs];
        [t drawAtPoint:NSMakePoint(x, cy - ts.height / 2.0) withAttributes:attrs];
        x += ts.width + 6.0;
        draw_led(g, x + r, cy, r, leds[i].color);
        x += r * 2.0 + 14.0;             /* dot + gap to the next group */
    }
}
@end

/* ------------------------------------------------------------- the controller --
 * One per process (static-strong, like gShell): owns the LED view + the sampling
 * timer + the CMContext the timer reads. The timer fires on the main run loop in
 * the common modes so it ticks under the hand-pumped CFRunLoop (and during menu
 * tracking). Every UI touch here is on the main thread. */
@interface CMStatusBar : NSObject
@property (nonatomic, assign) CMContext *cx;
@property (nonatomic, strong) CMLEDView *leds;
@property (nonatomic, strong) NSTimer   *timer;
@property (nonatomic, assign) int        lastPresent;
@end

@implementation CMStatusBar
- (void)tick:(NSTimer *)t {
    (void)t;
    if (!self.cx || !self.leds) return;
    int now = cm__present_count(self.cx);
    CGFloat act = self.leds.activity;
    if (now != self.lastPresent) { act = 1.0; self.lastPresent = now; }   /* a frame landed */
    else                         { act *= 0.55; if (act < 0.02) act = 0.0; }  /* decay */

    int pwr = cm__power_state(self.cx);
    BOOL changed = (act != self.leds.activity) || (pwr != self.leds.powerReq);
    self.leds.activity = act;
    self.leds.powerReq = pwr;
    if (changed) [self.leds setNeedsDisplay:YES];
}
@end

static CMStatusBar *gStatus = nil;   /* static-strong: one status bar per process */

/* Build the status-bar footer view (called by cocoametal_window.m in place of the
 * old flat strip). NSVisualEffectView background tracks the appearance natively;
 * the brand label fills the left, the LED cluster is pinned to the right. Starts the
 * sampling timer. Returns a view already framed at (0,0,width,height) for the root. */
NSView *cm__build_status_bar(CMContext *cx, int width, int height) {
    NSRect frame = NSMakeRect(0, 0, width, height);
    NSVisualEffectView *bar = [[NSVisualEffectView alloc] initWithFrame:frame];
    bar.material = NSVisualEffectMaterialTitlebar;       /* match the title bar */
    bar.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    bar.state = NSVisualEffectStateActive;
    bar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;

    NSTextField *brand = [NSTextField labelWithString:@"Daedalos — AROS on Apple Silicon"];
    brand.font = [NSFont systemFontOfSize:10];
    brand.textColor = [NSColor secondaryLabelColor];     /* semantic: themes itself */
    brand.frame = NSMakeRect(10, (height - 14) / 2.0, width - 150, 14);
    brand.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin;
    [bar addSubview:brand];

    const CGFloat LED_W = 120;
    CMLEDView *leds = [[CMLEDView alloc] initWithFrame:NSMakeRect(width - LED_W, 0, LED_W, height)];
    leds.autoresizingMask = NSViewMinXMargin | NSViewHeightSizable;
    leds.powerReq = cm__power_state(cx);
    leds.activity = 0.0;
    [bar addSubview:leds];

    if (!gStatus) gStatus = [CMStatusBar new];
    gStatus.cx = cx;
    gStatus.leds = leds;
    gStatus.lastPresent = cm__present_count(cx);
    [gStatus.timer invalidate];
    gStatus.timer = [NSTimer timerWithTimeInterval:0.1 target:gStatus
                                          selector:@selector(tick:) userInfo:nil repeats:YES];
    /* Common modes so the timer ticks under the hand-pumped run loop AND while a
     * menu is tracking (NSEventTrackingRunLoopMode). */
    [[NSRunLoop mainRunLoop] addTimer:gStatus.timer forMode:NSRunLoopCommonModes];

    return bar;
}

/* ----------------------------------------------------- cm__apply_theme_appkit --
 * Strong override of the weak stub in cocoametal.m. Sets NSApp.appearance so every
 * chrome surface themes coherently. System = nil (follow macOS), Light = Aqua,
 * Dark = DarkAqua. cm_set_option(CM_OPT_THEME, …) calls this on the main thread
 * (the only cm_* caller, §3); we also push the appearance onto each existing window
 * and force a redraw, so the switch lands immediately under the hand-pumped run
 * loop (which does not spin [NSApp run]). */
void cm__apply_theme_appkit(CMContext *cx, int theme) {
    (void)cx;
    void (^apply)(void) = ^{
        NSAppearance *ap = nil;
        if (theme == CM_THEME_LIGHT) ap = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
        else if (theme == CM_THEME_DARK) ap = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        /* CM_THEME_SYSTEM: ap stays nil => follow the macOS setting. */
        if (NSApp) {
            NSApp.appearance = ap;
            for (NSWindow *w in [NSApp windows]) { w.appearance = ap; [w displayIfNeeded]; }
        }
    };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}
