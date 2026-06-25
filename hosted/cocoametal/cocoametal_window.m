/* cocoametal_window.m — optional AppKit live-window path for the Cocoa/Metal shim.
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/spec.md
 * ("Metal pipeline" step 2 + "HiDPI" + the render-pass present / framebufferOnly
 * rationale). No GPL emulator source (vAmiga/WinUAE/FS-UAE/Amiberry/E-UAE) was
 * read, searched, or consulted. Apple AppKit/QuartzCore docs only [PUB].
 *
 * This file is linked only into builds that have AppKit and a window server. It
 * provides the strong definitions of cm_try_window / cm_destroy_window that
 * override the weak stubs in cocoametal.m. Per the spec the window is a
 * NON-ESSENTIAL bonus: cm_try_window must never block and must fail silently
 * (leaving cx->scale==0) when there is no window server, so the offscreen
 * oracle path keeps working headless.
 */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include "cocoametal.h"

/* We only touch the two fields we need; mirror the layout prefix of CMContext.
 * To stay honest we instead reach them through accessor knowledge: CMContext is
 * defined in cocoametal.m. Rather than duplicate the struct, expose tiny setters
 * there. Simpler: re-declare the needed prefix is fragile — so we use helper
 * hooks. We forward-declare them as functions implemented in cocoametal.m. */

/* These live in cocoametal.m and give this file controlled access without
 * duplicating the struct definition. */
id   cm__device(CMContext *cx);
int  cm__logical_w(CMContext *cx);
int  cm__logical_h(CMContext *cx);
void cm__set_window(CMContext *cx, void *window, void *layer, int scale);
void *cm__get_window(CMContext *cx);
void cm__set_close_pending(CMContext *cx);
void cm__set_resize_pending(CMContext *cx);
int  cm__take_close_pending(CMContext *cx);
int  cm__take_resize_pending(CMContext *cx);

/* ---- the CAMetalLayer-fills-the-content-view contract (INTERFACE.md §2a/§9) ---
 * THE BUG THIS FIXES (measured): the live CAMetalLayer's frame + drawableSize were
 * set ONCE at cm_try_window and NEVER updated when the content view resized. On a
 * fullscreen enter the content view grows to fill the screen, but the layer stayed
 * the old (small) windowed size and the drawable stayed the old (small) pixel size,
 * so the rendered framebuffer occupied a tiny rect in the corner — the "small white
 * rect in a black fullscreen window" the user reported (white = the default view
 * background showing through the uncovered area; black = AppKit's fullscreen).
 *
 * THE FIX: a custom content view (CMContentView, backing-layer-hosting) that keeps
 * the CAMetalLayer glued to the content view on EVERY geometry change:
 *   - the layer IS the view's backing layer (-makeBackingLayer returns it), so
 *     AppKit autoresizes layer.frame to the view bounds for free, and
 *   - -layout / -setFrameSize: / -viewDidChangeBackingProperties recompute
 *     layer.contentsScale + layer.drawableSize from the view's CURRENT backing-pixel
 *     size, so a resize, a fractional/Retina scale change, and a fullscreen
 *     enter/exit all keep the drawable exactly the size of the view in pixels.
 * The present pass already fills the whole drawable (CM_SCALE_FIT) and aspect-
 * preserves with a black letterbox for the other scale modes, so once the drawable
 * matches the view the framebuffer fills the window/screen. Window + view
 * background are set BLACK so any letterbox / uncovered area is black, never white. */

/* Resync the layer's contentsScale + drawableSize to a content view's current
 * backing-pixel size. Shared by CMContentView's geometry hooks and the fullscreen
 * delegate callbacks. No-op if the view has no CAMetalLayer or zero size. */
static void cm__sync_layer_to_view(NSView *view) {
    if (!view) return;
    CAMetalLayer *layer = (CAMetalLayer *)view.layer;
    if (![layer isKindOfClass:[CAMetalLayer class]]) return;

    NSWindow *win = view.window;
    CGFloat scale = win ? win.backingScaleFactor : layer.contentsScale;
    if (scale < 1.0) scale = 1.0;
    layer.contentsScale = scale;

    /* Drawable size = the view's CURRENT size in backing pixels. convertRectToBacking
     * folds in the real (possibly fractional) screen scale; fall back to bounds*scale
     * if the view isn't in a window yet. */
    NSRect backing = [view convertRectToBacking:view.bounds];
    NSUInteger pw = (NSUInteger)(backing.size.width  + 0.5);
    NSUInteger ph = (NSUInteger)(backing.size.height + 0.5);
    if (pw < 1) pw = (NSUInteger)(view.bounds.size.width  * scale + 0.5);
    if (ph < 1) ph = (NSUInteger)(view.bounds.size.height * scale + 0.5);
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    CGSize want = CGSizeMake((CGFloat)pw, (CGFloat)ph);
    if (layer.drawableSize.width != want.width ||
        layer.drawableSize.height != want.height)
        layer.drawableSize = want;
}

/* Content view that hosts the CAMetalLayer as its backing layer and keeps the
 * drawable glued to its size across resize / scale-change / fullscreen. Background
 * is black (opaque, black backgroundColor on the layer) so nothing white ever shows
 * around the framebuffer. */
@interface CMContentView : NSView
@end
@implementation CMContentView
- (BOOL)isOpaque { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }   /* layer-backed: we never -drawRect: */
- (CALayer *)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);  /* no white */
    return layer;
}
- (void)layout {
    [super layout];
    cm__sync_layer_to_view(self);          /* fullscreen enter/exit lands here too */
}
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    cm__sync_layer_to_view(self);          /* live resize */
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    cm__sync_layer_to_view(self);          /* moved to a different-scale screen */
}
@end

/* Minimal window delegate: surfaces close-button / live-resize transitions (which
 * arrive as delegate callbacks / notifications, NOT plain dequeuable NSEvents) into
 * the context flags the pump drains as CM_EV_CLOSE / CM_EV_RESIZE. It holds the
 * CMContext* as an opaque pointer (no struct knowledge). windowShouldClose returns
 * NO so the window is NOT torn down under the shim — the AROS side decides what a
 * close means; we only report it.
 *
 * It ALSO force-resyncs the CAMetalLayer to the content view on the fullscreen
 * enter/exit transitions: -layout fires for these too, but the enter/exit callbacks
 * are the authoritative "geometry settled" moment, so resyncing here guarantees the
 * drawable matches the (now screen-sized or restored) content view even if a -layout
 * pass was missed under the hand-pumped run loop. */
@interface CMWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) CMContext *cx;
@end
@implementation CMWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    if (self.cx) cm__set_close_pending(self.cx);
    return NO;   /* report, don't destroy: AROS owns the lifecycle */
}
- (void)windowDidResize:(NSNotification *)note {
    (void)note;
    if (self.cx) cm__set_resize_pending(self.cx);
}
- (void)windowDidEnterFullScreen:(NSNotification *)note {
    NSWindow *win = note.object;
    cm__sync_layer_to_view(win.contentView);   /* drawable -> screen-sized now */
}
- (void)windowDidExitFullScreen:(NSNotification *)note {
    NSWindow *win = note.object;
    cm__sync_layer_to_view(win.contentView);   /* drawable -> restored window size */
}
@end

void cm_try_window(CMContext *cx, const char *title) {
    if (!cx) return;

    /* No window server (headless / launchd without a session) -> bail quietly. */
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        if (!app) return;
        /* Probing the main screen is a cheap window-server liveness check. */
        if ([NSScreen mainScreen] == nil) return;

        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        int w = cm__logical_w(cx), h = cm__logical_h(cx);
        NSRect frame = NSMakeRect(120, 120, w, h);
        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (!win) return;
        [win setTitle:[NSString stringWithUTF8String:(title ? title : "AROS")]];

        /* BLACK backgrounds so the fullscreen letterbox / any uncovered area is
         * never white (the reported bug showed white around a small rect). */
        win.backgroundColor = [NSColor blackColor];
        win.opaque = YES;

        /* Allow REAL native AppKit fullscreen (CM_OPT_FULLSCREEN → toggleFullScreen:,
         * INTERFACE.md §9). FullScreenPrimary makes this window the one that takes
         * over its own Space on a green-button / toggleFullScreen: request; without
         * it the window only "zooms" and styleMask never gains
         * NSWindowStyleMaskFullScreen. Set at creation so the toggle is available
         * the moment cm_set_option(CM_OPT_FULLSCREEN,1) arrives. */
        win.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;

        /* Custom content view that hosts the CAMetalLayer as its BACKING layer and
         * keeps the drawable glued to the view size across resize / scale change /
         * fullscreen (see CMContentView above — the fix for the small-rect bug). */
        CMContentView *view = [[CMContentView alloc] initWithFrame:frame];
        view.wantsLayer = YES;                 /* triggers -makeBackingLayer (CAMetalLayer) */
        win.contentView = view;

        CAMetalLayer *layer = (CAMetalLayer *)view.layer;
        layer.device = (id<MTLDevice>)cm__device(cx);
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        /* framebufferOnly = YES is safe: cm_present draws the framebuffer texture
         * INTO the drawable via a render pass (color attachment), never a blit
         * copy, so the drawable is only ever a render target. TEST builds flip this
         * to NO via cm__live_set_framebuffer_only (livedraw readback). */
        layer.framebufferOnly = YES;

        /* Glue the drawable to the view's current backing-pixel size (contentsScale +
         * drawableSize). CMContentView keeps it in sync from here on (-layout /
         * -setFrameSize: / -viewDidChangeBackingProperties); do it once now so the
         * first present already has a correctly-sized drawable. */
        cm__sync_layer_to_view(view);
        CGFloat scale = win.backingScaleFactor;
        if (scale < 1.0) scale = 1.0;

        /* Attach the close/resize-reporting delegate. NSWindow.delegate is weak, so
         * tie the delegate's lifetime to the window via an associated object. */
        CMWindowDelegate *del = [[CMWindowDelegate alloc] init];
        del.cx = cx;
        win.delegate = del;
        objc_setAssociatedObject(win, "cm_delegate", del,
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);

        [win makeKeyAndOrderFront:nil];

        /* Report an INTEGER oracle scale (rounded backing factor, >=1). Only the
         * offscreen oracle target + readback use it; the live drawable uses the
         * fractional drawableSize above. Integer keeps the readback block-exact
         * and deterministic. */
        int oscale = (int)(scale + 0.5);
        if (oscale < 1) oscale = 1;
        cm__set_window(cx, (__bridge_retained void *)win,
                       (__bridge void *)layer, oscale);
    }
}

void cm_destroy_window(CMContext *cx) {
    if (!cx) return;
    void *w = cm__get_window(cx);
    if (w) {
        NSWindow *win = (__bridge_transfer NSWindow *)w;   /* balance the retain */
        [win close];
        cm__set_window(cx, NULL, NULL, 0);
    }
}

/* ---- REAL native fullscreen (CM_OPT_FULLSCREEN, INTERFACE.md §9) -----------
 * Strong override of the weak cm__set_fullscreen_appkit / cm__window_is_fullscreen
 * stubs in cocoametal.m. Runs on the single AROS/main thread (the only cm_* caller,
 * §3). cm_set_option(CM_OPT_FULLSCREEN,v) calls cm__set_fullscreen_appkit AFTER it
 * records the flag.
 *
 * Why styleMask, not a stored bool: -toggleFullScreen: is an UNCONDITIONAL toggle
 * (enter if windowed, exit if fullscreen), so we must read the window's CURRENT
 * state and only toggle when it differs from the request — otherwise a redundant
 * cm_set_option(...,1) while already fullscreen would exit. NSWindowStyleMaskFull-
 * Screen on the live styleMask is the authoritative "are we fullscreen NOW" bit
 * (it flips when the transition completes), so it both gates the toggle and answers
 * cm__window_is_fullscreen.
 *
 * Async / non-blocking (§3): -toggleFullScreen: starts an ANIMATED transition and
 * returns immediately. This function REQUESTS the transition and returns at once;
 * it never waits for the animation. MEASURED ([FS] finding, see INTERFACE.md §9f):
 * the ENTER transition's styleMask bit flips essentially at the call and is honored
 * under the graft's hand-pumped CFRunLoopRunInMode(...,0,true) model; the EXIT
 * transition's state machine only advances under the APP run loop ([NSApp run]) —
 * bare hand-pumping does not clear it. Either way this call is non-blocking; the
 * caller decides how to service the run loop. The present path keeps composing to
 * whatever the live drawable size becomes (cm_present reads d.texture.width/height
 * per frame), so a now-fullscreen drawable upscales correctly with no extra
 * plumbing. */
void cm__set_fullscreen_appkit(CMContext *cx, int on) {
    if (!cx) return;
    void *w = cm__get_window(cx);
    if (!w) return;                         /* headless / no window: nothing to do */
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)w;
        BOOL isFS = (win.styleMask & NSWindowStyleMaskFullScreen) != 0;
        BOOL wantFS = on ? YES : NO;
        if (isFS == wantFS) return;         /* already in the requested state */
        /* Ensure the window can take over its own Space (set at creation too; keep
         * it here so a window made before this field existed still toggles). */
        win.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
        [win toggleFullScreen:nil];         /* async animated transition; returns now */
    }
}

/* Report the window's CURRENT fullscreen state from the live styleMask (the bit
 * AppKit flips when the transition completes). 1 = fullscreen, 0 = windowed /
 * no window. Non-blocking; just reads state. */
int cm__window_is_fullscreen(CMContext *cx) {
    if (!cx) return 0;
    void *w = cm__get_window(cx);
    if (!w) return 0;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)w;
        return (win.styleMask & NSWindowStyleMaskFullScreen) ? 1 : 0;
    }
}

/* Diagnostic for the [FS] threading-model probe (like cm__present_stats): report
 * whether the window's frame currently equals its screen's full frame — a second,
 * geometry-based confirmation that the fullscreen transition actually completed
 * (the styleMask bit and the frame both settle only when the animation finishes).
 * NOT part of the frozen cm_* ABI, NOT exported in the dylib. Returns 1 if the
 * window covers its whole screen, 0 otherwise / no window. */
int cm__window_covers_screen(CMContext *cx) {
    if (!cx) return 0;
    void *w = cm__get_window(cx);
    if (!w) return 0;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)w;
        NSScreen *scr = win.screen ? win.screen : [NSScreen mainScreen];
        if (!scr) return 0;
        NSRect wf = win.frame, sf = scr.frame;
        return NSEqualRects(wf, sf) ? 1 : 0;
    }
}

/* ---- live-present geometry diagnostics / test hooks -----------------------
 * NOT part of the frozen cm_* ABI; NOT exported in the dylib (absent from
 * cocoametal.exports). Used by the [LIVE] readback test and the [DIAG] probe to
 * verify, from measured numbers, that the CAMetalLayer fills the content view and
 * the drawable tracks it across the fullscreen transition. */

/* Force the layer's drawableSize/contentsScale back in sync with the content view
 * NOW. CMContentView's geometry hooks already do this on resize/scale/fullscreen,
 * but under the hand-pumped run loop a -layout pass can be deferred; callers that
 * just changed geometry (a test entering fullscreen) call this to settle it before
 * presenting/reading back. No-op headless. */
void cm__resync_layer(CMContext *cx) {
    if (!cx) return;
    void *w = cm__get_window(cx);
    if (!w) return;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)w;
        cm__sync_layer_to_view(win.contentView);
    }
}

/* Report the live layer's current drawableSize + the content view's backing-pixel
 * size, so a test can assert the drawable fills the view. Returns 1 if a layer
 * exists, 0 headless. */
int cm__live_drawable_size(CMContext *cx, int *dw, int *dh, int *viewW, int *viewH) {
    if (dw) *dw = 0; if (dh) *dh = 0; if (viewW) *viewW = 0; if (viewH) *viewH = 0;
    if (!cx) return 0;
    void *w = cm__get_window(cx);
    if (!w) return 0;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)w;
        NSView *v = win.contentView;
        CAMetalLayer *layer = (CAMetalLayer *)v.layer;
        if (![layer isKindOfClass:[CAMetalLayer class]]) return 0;
        if (dw) *dw = (int)(layer.drawableSize.width + 0.5);
        if (dh) *dh = (int)(layer.drawableSize.height + 0.5);
        NSRect b = [v convertRectToBacking:v.bounds];
        if (viewW) *viewW = (int)(b.size.width + 0.5);
        if (viewH) *viewH = (int)(b.size.height + 0.5);
        return 1;
    }
}

/* TEST-ONLY: flip the live layer's framebufferOnly flag. Production keeps it YES
 * (present via render pass). The [LIVE] readback test sets it NO so it can read the
 * presented drawable back and assert the scene fills it. No-op headless. */
void cm__live_set_framebuffer_only(CMContext *cx, int on) {
    if (!cx) return;
    void *w = cm__get_window(cx);
    if (!w) return;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)w;
        CAMetalLayer *layer = (CAMetalLayer *)win.contentView.layer;
        if ([layer isKindOfClass:[CAMetalLayer class]])
            layer.framebufferOnly = on ? YES : NO;
    }
}

/* ---- input: the real cm_pump_events drain (INTERFACE.md §5) ---------------
 * Strong override of the weak cm__pump_events_appkit stub in cocoametal.m.
 * Runs on the single AROS/main thread (the only cm_* caller, §3); drains pending
 * NSEvents NON-BLOCKING (untilDate:[NSDate distantPast], dequeue:YES), bounded by
 * maxEvents, translates each to a CMEvent, and writes it to out[]. Events we do
 * NOT translate (system / window-management: drag, close button, etc.) are
 * forwarded to [NSApp sendEvent:] so the window still behaves, then draining
 * continues — untranslated events never accumulate. Nothing here blocks or spins.
 *
 * Coordinate convention (INTERFACE.md §2/§5): CMEvent.x/y are LOGICAL pixels,
 * TOP-LEFT origin. NSEvent.locationInWindow is in POINTS (== logical here, since
 * the content size in points equals the logical W*H — the contentsScale lives
 * below the layer) with a BOTTOM-LEFT origin, so we flip Y:
 *     x = locationInWindow.x
 *     y = contentHeightPoints - locationInWindow.y
 * then clamp to [0,w) x [0,h). Modifier flags map NSEventModifierFlag{Shift,
 * Control,Option,Command} -> CM_MOD_{SHIFT,CONTROL,ALT,CMD}. */

static unsigned cm__map_mods(NSEventModifierFlags f) {
    unsigned m = 0;
    if (f & NSEventModifierFlagShift)   m |= CM_MOD_SHIFT;
    if (f & NSEventModifierFlagControl) m |= CM_MOD_CONTROL;
    if (f & NSEventModifierFlagOption)  m |= CM_MOD_ALT;
    if (f & NSEventModifierFlagCommand) m |= CM_MOD_CMD;
    return m;
}

/* Logical content height in points: the window's content view bounds height when
 * a window exists, else the logical H (== content points per §2). Used for the
 * bottom-left -> top-left Y flip. */
static int cm__content_h_points(CMContext *cx) {
    void *w = cm__get_window(cx);
    if (w) {
        NSWindow *win = (__bridge NSWindow *)w;
        NSView *v = win.contentView;
        if (v) {
            int hp = (int)(v.bounds.size.height + 0.5);
            if (hp > 0) return hp;
        }
    }
    return cm__logical_h(cx);
}

/* Fill a CMEvent's mouse position from an NSEvent (logical, top-left, clamped). */
static void cm__fill_mouse_xy(CMContext *cx, NSEvent *ev, CMEvent *out) {
    int w = cm__logical_w(cx), h = cm__logical_h(cx);
    int chp = cm__content_h_points(cx);
    NSPoint p = ev.locationInWindow;        /* points, bottom-left origin */
    int x = (int)p.x;
    int y = chp - (int)p.y;                  /* flip to top-left */
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    out->x = x; out->y = y;
}

int cm__pump_events_appkit(CMContext *cx, CMEvent *out, int maxEvents) {
    if (!cx || !out || maxEvents <= 0) return 0;
    NSApplication *app = NSApp;
    if (!app) return 0;                      /* no NSApp -> no event source */

    int n = 0;
    @autoreleasepool {
        /* Window-management transitions first (set by the delegate, not NSEvents):
         * a pending live-resize, then a pending close. Edge-triggered / one-shot. */
        if (n < maxEvents && cm__take_resize_pending(cx)) {
            CMEvent *e = &out[n++];
            e->type = CM_EV_RESIZE; e->x = e->y = e->code = e->pressed = 0; e->mods = 0;
        }
        if (n < maxEvents && cm__take_close_pending(cx)) {
            CMEvent *e = &out[n++];
            e->type = CM_EV_CLOSE; e->x = e->y = e->code = e->pressed = 0; e->mods = 0;
        }

        while (n < maxEvents) {
            /* Non-blocking: distantPast => return immediately if nothing queued. */
            NSEvent *ev = [app nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES];
            if (!ev) break;

            CMEvent *e = &out[n];
            e->type = CM_EV_NONE; e->x = e->y = e->code = e->pressed = 0; e->mods = 0;
            int emit = 0;             /* 1 => this event produced a CMEvent */
            int forward = 0;          /* 1 => forward to [NSApp sendEvent:] (untranslated) */

            switch (ev.type) {
            case NSEventTypeMouseMoved:
            case NSEventTypeLeftMouseDragged:
            case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDragged:
                e->type = CM_EV_MOUSEMOVE;
                cm__fill_mouse_xy(cx, ev, e);
                e->mods = cm__map_mods(ev.modifierFlags);
                emit = 1; forward = 1;   /* also forward so AppKit tracking works */
                break;

            case NSEventTypeLeftMouseDown:
            case NSEventTypeLeftMouseUp:
                e->type = CM_EV_MOUSEBTN; e->code = 0;
                e->pressed = (ev.type == NSEventTypeLeftMouseDown) ? 1 : 0;
                cm__fill_mouse_xy(cx, ev, e);
                e->mods = cm__map_mods(ev.modifierFlags);
                emit = 1; forward = 1;
                break;
            case NSEventTypeRightMouseDown:
            case NSEventTypeRightMouseUp:
                e->type = CM_EV_MOUSEBTN; e->code = 1;
                e->pressed = (ev.type == NSEventTypeRightMouseDown) ? 1 : 0;
                cm__fill_mouse_xy(cx, ev, e);
                e->mods = cm__map_mods(ev.modifierFlags);
                emit = 1; forward = 1;
                break;
            case NSEventTypeOtherMouseDown:
            case NSEventTypeOtherMouseUp:
                e->type = CM_EV_MOUSEBTN; e->code = 2;
                e->pressed = (ev.type == NSEventTypeOtherMouseDown) ? 1 : 0;
                cm__fill_mouse_xy(cx, ev, e);
                e->mods = cm__map_mods(ev.modifierFlags);
                emit = 1; forward = 1;
                break;

            case NSEventTypeKeyDown:
            case NSEventTypeKeyUp:
                e->type = CM_EV_KEY;
                e->code = (int)ev.keyCode;       /* macOS virtual keycode */
                e->pressed = (ev.type == NSEventTypeKeyDown) ? 1 : 0;
                e->mods = cm__map_mods(ev.modifierFlags);
                emit = 1;
                /* Do NOT forward: AppKit would beep on an unhandled keyDown with no
                 * responder. The AROS side owns the keyboard. */
                break;

            case NSEventTypeFlagsChanged: {
                /* A modifier transition. Emit CM_EV_KEY for the modifier key; derive
                 * pressed from whether THIS key's flag is now set. We test the device-
                 * independent flag that corresponds to the keyCode's modifier. */
                NSEventModifierFlags f = ev.modifierFlags;
                unsigned short kc = ev.keyCode;
                int pressed = 0;
                switch (kc) {
                case 56: case 60: pressed = (f & NSEventModifierFlagShift)   ? 1 : 0; break; /* L/R Shift */
                case 59: case 62: pressed = (f & NSEventModifierFlagControl) ? 1 : 0; break; /* L/R Control */
                case 58: case 61: pressed = (f & NSEventModifierFlagOption)  ? 1 : 0; break; /* L/R Option */
                case 55: case 54: pressed = (f & NSEventModifierFlagCommand) ? 1 : 0; break; /* L/R Command */
                case 57:          pressed = (f & NSEventModifierFlagCapsLock)? 1 : 0; break; /* Caps Lock */
                case 63:          pressed = (f & NSEventModifierFlagFunction)? 1 : 0; break; /* Fn */
                default:          pressed = 0; break;
                }
                e->type = CM_EV_KEY;
                e->code = (int)kc;
                e->pressed = pressed;
                e->mods = cm__map_mods(f);
                emit = 1;
                break;
            }

            default:
                /* System / window-management event we do not translate (window drag,
                 * close button, app-defined, etc.). Forward so the window behaves,
                 * then keep draining. */
                forward = 1;
                break;
            }

            if (forward) [app sendEvent:ev];
            if (emit) n++;
            /* If neither emit nor forward (shouldn't happen), the event is simply
             * dropped after dequeue — but every branch sets at least one. */
        }
    }
    return n;
}
