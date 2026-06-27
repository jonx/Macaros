/* cocoametal_settings.m — native AppKit settings panel + NSUserDefaults
 * persistence for the Cocoa/Metal display shim (INTERFACE.md §9).
 *
 * Implemented from docs/features/cocoa-metal-display/INTERFACE.md (§9 settings &
 * options, §3 threading, §5 CM_EV_SETTING) + spec.md + cocoametal.h. Independent
 * work: no third-party implementation source — emulator, agent, driver, or
 * otherwise — was read, searched, or consulted in producing it, and any
 * resemblance to existing implementations is coincidental. Apple AppKit/Foundation
 * docs only [PUB].
 *
 * This file is linked only into builds that have AppKit. It provides the strong
 * definitions of cm__open_settings_appkit and cm__apply_persisted_options that
 * override the weak stubs in cocoametal.m — the same weak/strong split as
 * cm_try_window, so the AppKit-free translation unit pulls no AppKit headers.
 *
 * OWNERSHIP SPLIT (§9):
 *   - HOST-OWNED controls (effect on/off, scale mode, fullscreen, filter) are
 *     wired straight to cm_set_option, which applies them to the live present.
 *     Their state is persisted in NSUserDefaults (load at cm_open via
 *     cm__apply_persisted_options, save on every change).
 *   - The AROS-OWNED control (a resolution request) ALSO goes through
 *     cm_set_option, but for an AROS-FACING key (CM_OPT_REQUEST_MODE_W/H): the
 *     host does NOT act on it — cm_set_option enqueues a CM_EV_SETTING for the
 *     AROS side to pull. The panel never touches AROS state directly.
 *
 * THREADING (§3): the panel lives on the single display-server / main pthread
 * (the only cm_* caller). cm_open_settings creates the NSWindow under the same
 * hand-pumped CFRunLoop model proven at D2t; control actions run on that thread
 * when the run loop is serviced. Nothing here blocks or starts [NSApp run].
 */
#import <AppKit/AppKit.h>

#include "cocoametal.h"

/* Per-context controller registry. We cannot use objc_setAssociatedObject on the
 * CMContext* (it is a plain malloc'd struct, not an Obj-C object — that is UB and
 * crashes), so we key a strong-valued NSMapTable by the opaque context pointer.
 * Touched only on the single display-server / main thread (§3), so no locking. */
static NSMapTable *cm__ctlRegistry(void) {
    static NSMapTable *t = nil;
    if (!t)
        /* OpaquePersonality (pointer hash/equality, NO -hash/-isEqual: messaging)
         * + OpaqueMemory (no retain/release): the key is a raw CMContext* — it is
         * NOT an Obj-C object, so the table must never message it. Values are
         * strong (retain the controller). */
        t = [[NSMapTable alloc] initWithKeyOptions:(NSPointerFunctionsOpaqueMemory |
                                                    NSPointerFunctionsOpaquePersonality)
                                      valueOptions:NSPointerFunctionsStrongMemory
                                          capacity:4];
    return t;
}

/* NSUserDefaults keys — namespaced so they cannot collide with anything else in
 * the host process's domain. Only the HOST-OWNED options are persisted (the
 * AROS-facing requests are transient pull events, not host state). */
static NSString *const kDefEffect     = @"cocoametal.effect";
static NSString *const kDefScaleMode  = @"cocoametal.scaleMode";
static NSString *const kDefFullscreen = @"cocoametal.fullscreen";
static NSString *const kDefFilter     = @"cocoametal.filter";

/* ---- persistence ----------------------------------------------------------
 * Save the four host-owned options to NSUserDefaults from the live shim state. */
static void cm__save_defaults(CMContext *cx) {
    if (!cx) return;
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    long v = 0;
    if (cm_get_option(cx, CM_OPT_EFFECT, &v)     == 0) [d setInteger:(NSInteger)v forKey:kDefEffect];
    if (cm_get_option(cx, CM_OPT_SCALE_MODE, &v) == 0) [d setInteger:(NSInteger)v forKey:kDefScaleMode];
    if (cm_get_option(cx, CM_OPT_FULLSCREEN, &v) == 0) [d setInteger:(NSInteger)v forKey:kDefFullscreen];
    if (cm_get_option(cx, CM_OPT_FILTER, &v)     == 0) [d setInteger:(NSInteger)v forKey:kDefFilter];
    [d synchronize];
}

/* Load persisted host-owned options and apply them via cm_set_option. Strong
 * override of the weak stub in cocoametal.m; called once at the end of cm_open.
 * If a key is absent from the domain we leave the cm_open default in place. */
void cm__apply_persisted_options(CMContext *cx) {
    if (!cx) return;
    @autoreleasepool {
        NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
        if ([d objectForKey:kDefEffect])     cm_set_option(cx, CM_OPT_EFFECT,     (long)[d integerForKey:kDefEffect]);
        if ([d objectForKey:kDefScaleMode])  cm_set_option(cx, CM_OPT_SCALE_MODE,  (long)[d integerForKey:kDefScaleMode]);
        if ([d objectForKey:kDefFullscreen]) cm_set_option(cx, CM_OPT_FULLSCREEN,  (long)[d integerForKey:kDefFullscreen]);
        if ([d objectForKey:kDefFilter])     cm_set_option(cx, CM_OPT_FILTER,      (long)[d integerForKey:kDefFilter]);
    }
}

/* ---- the panel controller -------------------------------------------------
 * Holds the CMContext* as an opaque pointer (no struct knowledge — it only ever
 * calls the public cm_set_option / cm_get_option ABI). Each control's target/
 * action both APPLIES the change (cm_set_option) and PERSISTS host-owned ones. */
@interface CMSettingsController : NSObject
@property (nonatomic, assign) CMContext *cx;
@property (nonatomic, strong) NSWindow *window;
/* host-owned controls */
@property (nonatomic, strong) NSButton    *effectCheck;     /* CM_FX_SCANLINE on/off */
@property (nonatomic, strong) NSPopUpButton *scalePopup;    /* CMScaleMode */
@property (nonatomic, strong) NSButton    *fullscreenCheck; /* 0/1 */
@property (nonatomic, strong) NSPopUpButton *filterPopup;   /* CMFilter */
/* AROS-owned control (routes via CM_EV_SETTING, never acts directly) */
@property (nonatomic, strong) NSPopUpButton *resPopup;      /* CM_OPT_REQUEST_MODE_W/H */
@end

@implementation CMSettingsController

/* Host-owned: scanline effect on/off. */
- (void)effectChanged:(id)sender {
    (void)sender;
    long v = (self.effectCheck.state == NSControlStateValueOn) ? CM_FX_SCANLINE : CM_FX_NEAREST;
    cm_set_option(self.cx, CM_OPT_EFFECT, v);
    cm__save_defaults(self.cx);
}
/* Host-owned: scale mode. tag of the selected item == the CMScaleMode value. */
- (void)scaleChanged:(id)sender {
    (void)sender;
    cm_set_option(self.cx, CM_OPT_SCALE_MODE, (long)self.scalePopup.selectedTag);
    cm__save_defaults(self.cx);
}
/* Host-owned: fullscreen 0/1. */
- (void)fullscreenChanged:(id)sender {
    (void)sender;
    cm_set_option(self.cx, CM_OPT_FULLSCREEN,
                  (self.fullscreenCheck.state == NSControlStateValueOn) ? 1 : 0);
    cm__save_defaults(self.cx);
}
/* Host-owned: filter nearest/linear. tag == the CMFilter value. */
- (void)filterChanged:(id)sender {
    (void)sender;
    cm_set_option(self.cx, CM_OPT_FILTER, (long)self.filterPopup.selectedTag);
    cm__save_defaults(self.cx);
}
/* AROS-owned: a resolution request. The selected item encodes W in tag>>16 and
 * H in tag&0xFFFF. We set _MODE_W then _MODE_H via cm_set_option — each enqueues
 * a CM_EV_SETTING for the AROS side. The host does NOT change resolution itself,
 * and nothing here is persisted (it is a transient request, not host state). */
- (void)resChanged:(id)sender {
    (void)sender;
    NSInteger tag = self.resPopup.selectedTag;
    long w = (long)((tag >> 16) & 0xFFFF);
    long h = (long)(tag & 0xFFFF);
    cm_set_option(self.cx, CM_OPT_REQUEST_MODE_W, w);
    cm_set_option(self.cx, CM_OPT_REQUEST_MODE_H, h);
    /* deliberately NOT cm__save_defaults: AROS-owned, not host state. */
}
@end

/* Build a left-aligned label. */
static NSTextField *cm__label(NSString *s, CGFloat y) {
    NSTextField *t = [[NSTextField alloc] initWithFrame:NSMakeRect(16, y, 110, 20)];
    t.stringValue = s;
    t.bezeled = NO; t.drawsBackground = NO; t.editable = NO; t.selectable = NO;
    t.alignment = NSTextAlignmentRight;
    return t;
}

/* Sync the panel controls to the current shim option values (so a reopened panel
 * reflects persisted/live state). */
static void cm__sync_controls(CMSettingsController *c) {
    CMContext *cx = c.cx;
    long v = 0;
    if (cm_get_option(cx, CM_OPT_EFFECT, &v) == 0)
        c.effectCheck.state = (v == CM_FX_SCANLINE) ? NSControlStateValueOn : NSControlStateValueOff;
    if (cm_get_option(cx, CM_OPT_SCALE_MODE, &v) == 0)
        [c.scalePopup selectItemWithTag:(NSInteger)v];
    if (cm_get_option(cx, CM_OPT_FULLSCREEN, &v) == 0)
        c.fullscreenCheck.state = v ? NSControlStateValueOn : NSControlStateValueOff;
    if (cm_get_option(cx, CM_OPT_FILTER, &v) == 0)
        [c.filterPopup selectItemWithTag:(NSInteger)v];
}

/* Strong override of the weak cm__open_settings_appkit stub in cocoametal.m.
 * Best-effort like cm_try_window: bail (return nonzero) with no window server.
 * Idempotent — a second call just re-fronts the existing panel. Returns 0 if the
 * panel is up. */
int cm__open_settings_appkit(CMContext *cx) {
    if (!cx) return 1;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        if (!app) return 1;
        if ([NSScreen mainScreen] == nil) return 1;     /* no window server */
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        /* Already open? Re-front and resync (idempotent). The controller is kept
         * alive in the per-context registry. */
        CMSettingsController *existing = [cm__ctlRegistry() objectForKey:(__bridge id)cx];
        if (existing && existing.window) {
            cm__sync_controls(existing);
            [existing.window makeKeyAndOrderFront:nil];
            return 0;
        }

        CMSettingsController *c = [[CMSettingsController alloc] init];
        c.cx = cx;

        NSRect frame = NSMakeRect(160, 160, 320, 210);
        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (!win) return 1;
        [win setTitle:@"AROS Display Settings"];
        [win setReleasedWhenClosed:NO];
        NSView *cv = win.contentView;

        /* Row Y positions (top-down). */
        CGFloat yEffect = 160, yScale = 128, yFull = 96, yFilter = 64, yRes = 24;

        /* --- HOST-OWNED: scanline effect on/off --- */
        [cv addSubview:cm__label(@"CRT effect:", yEffect)];
        c.effectCheck = [[NSButton alloc] initWithFrame:NSMakeRect(134, yEffect, 170, 20)];
        [c.effectCheck setButtonType:NSButtonTypeSwitch];
        c.effectCheck.title = @"Scanlines";
        c.effectCheck.target = c; c.effectCheck.action = @selector(effectChanged:);
        [cv addSubview:c.effectCheck];

        /* --- HOST-OWNED: scale mode --- */
        [cv addSubview:cm__label(@"Scale:", yScale)];
        c.scalePopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(132, yScale, 174, 24)];
        [c.scalePopup addItemWithTitle:@"Aspect fit (letterbox)"];
        [[c.scalePopup lastItem] setTag:CM_SCALE_ASPECT_FIT];
        [c.scalePopup addItemWithTitle:@"Fit (stretch)"];
        [[c.scalePopup lastItem] setTag:CM_SCALE_FIT];
        [c.scalePopup addItemWithTitle:@"Integer nearest"];
        [[c.scalePopup lastItem] setTag:CM_SCALE_INTEGER_NEAREST];
        [c.scalePopup addItemWithTitle:@"Pixel perfect (1:1)"];
        [[c.scalePopup lastItem] setTag:CM_SCALE_PIXEL_PERFECT];
        c.scalePopup.target = c; c.scalePopup.action = @selector(scaleChanged:);
        [cv addSubview:c.scalePopup];

        /* --- HOST-OWNED: fullscreen --- */
        [cv addSubview:cm__label(@"Display:", yFull)];
        c.fullscreenCheck = [[NSButton alloc] initWithFrame:NSMakeRect(134, yFull, 170, 20)];
        [c.fullscreenCheck setButtonType:NSButtonTypeSwitch];
        c.fullscreenCheck.title = @"Fullscreen";
        c.fullscreenCheck.target = c; c.fullscreenCheck.action = @selector(fullscreenChanged:);
        [cv addSubview:c.fullscreenCheck];

        /* --- HOST-OWNED: filter --- */
        [cv addSubview:cm__label(@"Filter:", yFilter)];
        c.filterPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(132, yFilter, 174, 24)];
        [c.filterPopup addItemWithTitle:@"Nearest (crisp)"];
        [[c.filterPopup lastItem] setTag:CM_FILTER_NEAREST];
        [c.filterPopup addItemWithTitle:@"Linear (smooth)"];
        [[c.filterPopup lastItem] setTag:CM_FILTER_LINEAR];
        c.filterPopup.target = c; c.filterPopup.action = @selector(filterChanged:);
        [cv addSubview:c.filterPopup];

        /* --- AROS-OWNED: resolution request (routes via CM_EV_SETTING) --- */
        [cv addSubview:cm__label(@"Resolution:", yRes)];
        c.resPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(132, yRes, 174, 24)];
        /* tag = (W<<16)|H */
        [c.resPopup addItemWithTitle:@"320 x 200"];  [[c.resPopup lastItem] setTag:(320<<16)|200];
        [c.resPopup addItemWithTitle:@"640 x 256"];  [[c.resPopup lastItem] setTag:(640<<16)|256];
        [c.resPopup addItemWithTitle:@"640 x 512"];  [[c.resPopup lastItem] setTag:(640<<16)|512];
        c.resPopup.target = c; c.resPopup.action = @selector(resChanged:);
        [cv addSubview:c.resPopup];

        c.window = win;
        cm__sync_controls(c);                 /* reflect persisted/live state */

        /* Keep the controller alive, keyed by the shim context. */
        [cm__ctlRegistry() setObject:c forKey:(__bridge id)cx];

        [win makeKeyAndOrderFront:nil];
        return 0;
    }
}
