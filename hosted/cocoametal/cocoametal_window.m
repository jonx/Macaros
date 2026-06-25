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

/* Minimal window delegate: surfaces close-button / live-resize transitions (which
 * arrive as delegate callbacks / notifications, NOT plain dequeuable NSEvents) into
 * the context flags the pump drains as CM_EV_CLOSE / CM_EV_RESIZE. It holds the
 * CMContext* as an opaque pointer (no struct knowledge). windowShouldClose returns
 * NO so the window is NOT torn down under the shim — the AROS side decides what a
 * close means; we only report it. */
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

        NSView *view = win.contentView;
        view.wantsLayer = YES;

        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = (id<MTLDevice>)cm__device(cx);
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        /* framebufferOnly = YES is safe: cm_present draws the framebuffer texture
         * INTO the drawable via a render pass (color attachment), never a blit
         * copy, so the drawable is only ever a render target. */
        layer.framebufferOnly = YES;

        /* True (possibly fractional) backing scale drives the LIVE drawable; the
         * present render pass samples by normalized UV so any drawable size — and
         * thus any integer or fractional window scale — upscales correctly. */
        CGFloat scale = win.backingScaleFactor;
        if (scale < 1.0) scale = 1.0;
        layer.contentsScale = scale;
        /* Size the drawable to the view's actual backing-pixel size so a resized
         * or fractional-scale window still maps right (not a hard-coded w*scale). */
        NSRect backing = [view convertRectToBacking:view.bounds];
        if (backing.size.width < 1 || backing.size.height < 1)
            backing.size = NSMakeSize(w * scale, h * scale);
        layer.drawableSize = backing.size;

        view.layer = layer;

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
