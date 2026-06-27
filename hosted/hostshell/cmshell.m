/* cmshell.m — host app-shell controller: menu bar + About + app icon.
 *
 * See cmshell.h for the provenance and the engine/shell seam. This
 * file is pure AppKit [PUB]; it pulls NO AROS headers and does NOT touch the
 * cocoametal Metal shim. Every menu action routes to the CMShellSink.
 *
 * Built with the HOST clang (-fobjc-arc), like the rest of hosted/cocoametal/.
 */
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include "cmshell.h"

/* ------------------------------------------------------------------ icon ----
 * A placeholder app icon drawn with CoreGraphics (NO window server needed, so a
 * headless/unattended run still sets a Dock icon). Motif = the project's
 * signature 4-quadrant test scene inside a rounded rect. At merge, swap for a
 * real .icns referenced by CFBundleIconFile (spec.md R-IDENTITY). */
static NSImage *cmsh_make_icon(void) {
    const int S = 256;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(NULL, S, S, 8, 0, cs,
                                           (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    if (!c) { CGColorSpaceRelease(cs); return nil; }

    /* transparent ground, then a rounded-rect mask */
    CGContextClearRect(c, CGRectMake(0, 0, S, S));
    CGFloat inset = S * 0.08, r = S * 0.22;
    CGRect rr = CGRectMake(inset, inset, S - 2 * inset, S - 2 * inset);
    CGPathRef path = CGPathCreateWithRoundedRect(rr, r, r, NULL);
    CGContextAddPath(c, path);
    CGContextClip(c);

    /* four quadrants: TL red, TR green, BL blue, BR yellow (origin bottom-left) */
    CGFloat h = S / 2.0;
    const CGFloat quad[4][3] = {
        {0.16, 0.20, 0.85},  /* BL blue   */
        {0.92, 0.85, 0.15},  /* BR yellow */
        {0.88, 0.18, 0.18},  /* TL red    */
        {0.20, 0.78, 0.28},  /* TR green  */
    };
    CGContextSetRGBFillColor(c, quad[0][0], quad[0][1], quad[0][2], 1);
    CGContextFillRect(c, CGRectMake(0, 0, h, h));
    CGContextSetRGBFillColor(c, quad[1][0], quad[1][1], quad[1][2], 1);
    CGContextFillRect(c, CGRectMake(h, 0, h, h));
    CGContextSetRGBFillColor(c, quad[2][0], quad[2][1], quad[2][2], 1);
    CGContextFillRect(c, CGRectMake(0, h, h, h));
    CGContextSetRGBFillColor(c, quad[3][0], quad[3][1], quad[3][2], 1);
    CGContextFillRect(c, CGRectMake(h, h, h, h));

    /* a thin dark seam between the quadrants for a touch of polish */
    CGContextSetRGBStrokeColor(c, 0, 0, 0, 0.35);
    CGContextSetLineWidth(c, S * 0.012);
    CGContextMoveToPoint(c, h, inset);   CGContextAddLineToPoint(c, h, S - inset);
    CGContextMoveToPoint(c, inset, h);   CGContextAddLineToPoint(c, S - inset, h);
    CGContextStrokePath(c);

    CGImageRef img = CGBitmapContextCreateImage(c);
    NSImage *ns = img ? [[NSImage alloc] initWithCGImage:img
                                                    size:NSMakeSize(S, S)] : nil;
    if (img) CGImageRelease(img);
    CGPathRelease(path);
    CGContextRelease(c);
    CGColorSpaceRelease(cs);
    return ns;
}

/* --------------------------------------------------------------- controller -- */
@interface CMShellController : NSObject <NSApplicationDelegate>
@property (nonatomic) CMShellSink sink;
@property (nonatomic) BOOL fullscreenOn;
@property (nonatomic) BOOL scanlinesOn;
@property (nonatomic) BOOL retinaOn;
@property (nonatomic) BOOL clipboardShareOn;
@property (nonatomic) BOOL captureInputOn;
@end

@implementation CMShellController

/* ---- intent helpers (the only path to the engine) ---- */
- (void)opt:(int)key value:(long)value {
    if (_sink.set_option) _sink.set_option(_sink.ctx, key, value);
}
- (void)optStr:(int)key str:(const char *)s {
    if (_sink.set_option_str) _sink.set_option_str(_sink.ctx, key, s);
}

/* ---- App menu ---- */
- (void)aboutAction:(id)sender    { cmshell_show_about((__bridge void *)self); }
- (void)settingsAction:(id)sender {
    if (_sink.open_settings) _sink.open_settings(_sink.ctx);   /* → cm_open_settings */
}

/* ---- File menu ---- */
- (void)shotAction:(id)sender {
    if (_sink.capture_png) _sink.capture_png(_sink.ctx, "screenshot.png");
}
- (void)recordAction:(id)sender {
    if (_sink.record_start) _sink.record_start(_sink.ctx, "movie.mov", 30, 0);
}
- (void)openVolumeAction:(id)sender {
    /* the panel is wired at merge; the POC routes a fixed spec to prove the seam */
    if (_sink.volume_add) _sink.volume_add(_sink.ctx, "Mac:~/AROS/Shared");
}

/* ---- View menu (host-acted presentation; maps onto the existing cm_* keys) -- */
- (void)fullscreenAction:(id)sender {
    _fullscreenOn = !_fullscreenOn;
    [(NSMenuItem *)sender setState:_fullscreenOn ? NSControlStateValueOn
                                                 : NSControlStateValueOff];
    [self opt:CM_OPT_FULLSCREEN value:_fullscreenOn ? 1 : 0];
}
- (void)scalingAction:(id)sender {
    [self opt:CM_OPT_SCALE_MODE value:[(NSMenuItem *)sender tag]];
}
- (void)filterAction:(id)sender {
    [self opt:CM_OPT_FILTER value:[(NSMenuItem *)sender tag]];
}
- (void)scanlinesAction:(id)sender {
    _scanlinesOn = !_scanlinesOn;
    [(NSMenuItem *)sender setState:_scanlinesOn ? NSControlStateValueOn
                                                : NSControlStateValueOff];
    [self opt:CM_OPT_EFFECT value:_scanlinesOn ? CM_FX_SCANLINE : CM_FX_NEAREST];
}
- (void)retinaAction:(id)sender {
    _retinaOn = !_retinaOn;
    [(NSMenuItem *)sender setState:_retinaOn ? NSControlStateValueOn
                                             : NSControlStateValueOff];
    [self opt:CM_OPT_RETINA value:_retinaOn ? 1 : 0];
}

/* ---- Machine menu (lifecycle + host integration) ---- */
- (void)resetAction:(id)sender {
    if (_sink.power) _sink.power(_sink.ctx, CM_POWER_RESET);
}
- (void)powerDownAction:(id)sender {
    if (_sink.power) _sink.power(_sink.ctx, CM_POWER_REQUEST_DOWN);
}
- (void)forceDownAction:(id)sender {
    if (_sink.power) _sink.power(_sink.ctx, CM_POWER_FORCE_DOWN);
}
- (void)forceQuitAction:(id)sender {
    if (_sink.power) _sink.power(_sink.ctx, CM_POWER_FORCE_QUIT);
}
- (void)captureInputAction:(id)sender {
    _captureInputOn = !_captureInputOn;
    [(NSMenuItem *)sender setState:_captureInputOn ? NSControlStateValueOn
                                                   : NSControlStateValueOff];
    if (_sink.set_capture_input) _sink.set_capture_input(_sink.ctx, _captureInputOn ? 1 : 0);
}
- (void)clipboardShareAction:(id)sender {
    _clipboardShareOn = !_clipboardShareOn;
    [(NSMenuItem *)sender setState:_clipboardShareOn ? NSControlStateValueOn
                                                     : NSControlStateValueOff];
    [self opt:CM_OPT_CLIPBOARD_SHARE value:_clipboardShareOn ? 1 : 0];
}

/* ---- Help menu ---- */
- (void)websiteAction:(id)sender {
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://aros.org"]];
}
- (void)reportAction:(id)sender {
    [[NSWorkspace sharedWorkspace]
        openURL:[NSURL URLWithString:@"https://github.com/aros-development-team/AROS/issues"]];
}

/* a dropped folder → mount as a volume (R-VOLUME drag-and-drop seam) */
- (void)dropFolder:(NSString *)path writable:(BOOL)rw {
    if (!_sink.volume_add || !path) return;
    NSString *spec = [NSString stringWithFormat:@"Mac:%@%@", path, rw ? @";WRITE" : @""];
    _sink.volume_add(_sink.ctx, spec.UTF8String);
}

/* clean shutdown request instead of a unilateral exit() (embeddable-lib rule) */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    if (_sink.power) _sink.power(_sink.ctx, CM_POWER_REQUEST_DOWN);
    return NSTerminateNow;   /* POC: allow the standard quit; merge gates on engine */
}
@end

/* ----------------------------------------------------------- menu builders -- */
static NSMenuItem *cmsh_add(NSMenu *menu, NSString *title, SEL action, id target,
                            NSString *keyEquiv, NSEventModifierFlags mods) {
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title
                                                action:action
                                         keyEquivalent:keyEquiv ?: @""];
    if (mods) it.keyEquivalentModifierMask = mods;
    if (target) it.target = target;     /* custom items target the controller    */
    [menu addItem:it];                  /* nil target → responder chain (std cmds) */
    return it;
}

static NSMenu *cmsh_submenu(NSMenu *bar, NSString *title) {
    NSMenuItem *host = [[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""];
    NSMenu *sub = [[NSMenu alloc] initWithTitle:title];
    host.submenu = sub;
    [bar addItem:host];
    return sub;
}

static void cmsh_build_menu(CMShellController *c) {
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@"MainMenu"];

    /* --- App menu (system shows the app name; HIG order) --- */
    NSMenu *app = cmsh_submenu(bar, @"AROS");
    cmsh_add(app, @"About AROS", @selector(aboutAction:), c, @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Settings…", @selector(settingsAction:), c, @",", NSEventModifierFlagCommand);
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem *services = [[NSMenuItem alloc] initWithTitle:@"Services" action:NULL keyEquivalent:@""];
    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    services.submenu = servicesMenu;
    [app addItem:services];
    [NSApp setServicesMenu:servicesMenu];
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Hide AROS", @selector(hide:), nil, @"h", NSEventModifierFlagCommand);
    cmsh_add(app, @"Hide Others", @selector(hideOtherApplications:), nil, @"h",
             NSEventModifierFlagCommand | NSEventModifierFlagOption);
    cmsh_add(app, @"Show All", @selector(unhideAllApplications:), nil, @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Quit AROS", @selector(terminate:), nil, @"q", NSEventModifierFlagCommand);

    /* --- File --- */
    NSMenu *file = cmsh_submenu(bar, @"File");
    cmsh_add(file, @"Take Screenshot", @selector(shotAction:), c, @"3",
             NSEventModifierFlagShift | NSEventModifierFlagCommand);
    cmsh_add(file, @"Record Movie…", @selector(recordAction:), c, @"", 0);
    [file addItem:[NSMenuItem separatorItem]];
    cmsh_add(file, @"Open Folder as Volume…", @selector(openVolumeAction:), c, @"o",
             NSEventModifierFlagCommand);
    NSMenuItem *recent = [[NSMenuItem alloc] initWithTitle:@"Open Recent" action:NULL keyEquivalent:@""];
    recent.submenu = [[NSMenu alloc] initWithTitle:@"Open Recent"];
    [file addItem:recent];
    [file addItem:[NSMenuItem separatorItem]];
    NSMenuItem *open68k = cmsh_add(file, @"Open 68k Program…", NULL, nil, @"", 0); /* future */
    open68k.enabled = NO;
    [file addItem:[NSMenuItem separatorItem]];
    cmsh_add(file, @"Close", @selector(performClose:), nil, @"w", NSEventModifierFlagCommand);

    /* --- Edit (host-side affordance for the clipboard bridge) --- */
    NSMenu *edit = cmsh_submenu(bar, @"Edit");
    cmsh_add(edit, @"Undo", @selector(undo:), nil, @"z", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Redo", @selector(redo:), nil, @"z",
             NSEventModifierFlagShift | NSEventModifierFlagCommand);
    [edit addItem:[NSMenuItem separatorItem]];
    cmsh_add(edit, @"Cut", @selector(cut:), nil, @"x", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Copy", @selector(copy:), nil, @"c", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Paste", @selector(paste:), nil, @"v", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Select All", @selector(selectAll:), nil, @"a", NSEventModifierFlagCommand);

    /* --- View (host-acted presentation → existing cm_* keys) --- */
    NSMenu *view = cmsh_submenu(bar, @"View");
    cmsh_add(view, @"Enter Full Screen", @selector(fullscreenAction:), c, @"f",
             NSEventModifierFlagControl | NSEventModifierFlagCommand);
    NSMenu *scaling = [[NSMenu alloc] initWithTitle:@"Scaling"];
    NSMenuItem *scalingHost = [[NSMenuItem alloc] initWithTitle:@"Scaling" action:NULL keyEquivalent:@""];
    scalingHost.submenu = scaling; [view addItem:scalingHost];
    cmsh_add(scaling, @"Aspect Fit", @selector(scalingAction:), c, @"", 0).tag = CM_SCALE_ASPECT_FIT;
    cmsh_add(scaling, @"Integer", @selector(scalingAction:), c, @"", 0).tag = CM_SCALE_INTEGER_NEAREST;
    cmsh_add(scaling, @"Pixel-Perfect", @selector(scalingAction:), c, @"", 0).tag = CM_SCALE_PIXEL_PERFECT;
    cmsh_add(scaling, @"Stretch", @selector(scalingAction:), c, @"", 0).tag = CM_SCALE_FIT;
    NSMenu *filter = [[NSMenu alloc] initWithTitle:@"Filter"];
    NSMenuItem *filterHost = [[NSMenuItem alloc] initWithTitle:@"Filter" action:NULL keyEquivalent:@""];
    filterHost.submenu = filter; [view addItem:filterHost];
    cmsh_add(filter, @"Nearest", @selector(filterAction:), c, @"", 0).tag = CM_FILTER_NEAREST;
    cmsh_add(filter, @"Linear", @selector(filterAction:), c, @"", 0).tag = CM_FILTER_LINEAR;
    cmsh_add(view, @"Scanlines", @selector(scanlinesAction:), c, @"", 0);
    cmsh_add(view, @"Retina / HiDPI", @selector(retinaAction:), c, @"", 0);

    /* --- Machine (lifecycle + host integration) --- */
    NSMenu *machine = cmsh_submenu(bar, @"Machine");
    cmsh_add(machine, @"Reset", @selector(resetAction:), c, @"r",
             NSEventModifierFlagControl | NSEventModifierFlagCommand);
    NSMenu *power = [[NSMenu alloc] initWithTitle:@"Power"];
    NSMenuItem *powerHost = [[NSMenuItem alloc] initWithTitle:@"Power" action:NULL keyEquivalent:@""];
    powerHost.submenu = power; [machine addItem:powerHost];
    cmsh_add(power, @"Request Power Down", @selector(powerDownAction:), c, @"", 0);
    cmsh_add(power, @"Force Shut Down", @selector(forceDownAction:), c, @"", 0);
    cmsh_add(power, @"Force Quit", @selector(forceQuitAction:), c, @"", 0);
    [machine addItem:[NSMenuItem separatorItem]];
    cmsh_add(machine, @"Capture Input", @selector(captureInputAction:), c, @"i",
             NSEventModifierFlagControl | NSEventModifierFlagCommand);
    NSMenuItem *volumes = [[NSMenuItem alloc] initWithTitle:@"Volumes" action:NULL keyEquivalent:@""];
    volumes.submenu = [[NSMenu alloc] initWithTitle:@"Volumes"];   /* filled at runtime */
    [machine addItem:volumes];
    cmsh_add(machine, @"Share Clipboard", @selector(clipboardShareAction:), c, @"", 0);

    /* --- Window --- */
    NSMenu *window = cmsh_submenu(bar, @"Window");
    cmsh_add(window, @"Minimize", @selector(performMiniaturize:), nil, @"m", NSEventModifierFlagCommand);
    cmsh_add(window, @"Zoom", @selector(performZoom:), nil, @"", 0);
    [window addItem:[NSMenuItem separatorItem]];
    cmsh_add(window, @"Bring All to Front", @selector(arrangeInFront:), nil, @"", 0);
    [NSApp setWindowsMenu:window];

    /* --- Help --- */
    NSMenu *help = cmsh_submenu(bar, @"Help");
    cmsh_add(help, @"AROS Website", @selector(websiteAction:), c, @"", 0);
    cmsh_add(help, @"Report an Issue", @selector(reportAction:), c, @"", 0);
    [NSApp setHelpMenu:help];

    [NSApp setMainMenu:bar];
}

/* -------------------------------------------------------------- C entries --- */
static CMShellController *gShell = nil;   /* static-strong keeps it alive (one per app) */

void *cmshell_install(const CMShellSink *sink) {
    if (!gShell) gShell = [CMShellController new];
    if (sink) gShell.sink = *sink;
    if ([NSApp delegate] == nil) [NSApp setDelegate:gShell];

    NSImage *icon = cmsh_make_icon();
    if (icon) [NSApp setApplicationIconImage:icon];

    cmsh_build_menu(gShell);
    return (__bridge void *)gShell;
}

void cmshell_show_about(void *handle) {
    (void)handle;
    NSDictionary *opts = @{
        NSAboutPanelOptionApplicationName: @"AROS",
        NSAboutPanelOptionApplicationVersion: @"hosted-darwin-aarch64",
        NSAboutPanelOptionCredits:
            [[NSAttributedString alloc]
                initWithString:@"AROS — the open-source AmigaOS reimplementation, "
                               @"hosted natively on Apple Silicon.\n"
                               @"Distributed under the AROS Public License."],
    };
    [NSApp orderFrontStandardAboutPanel:opts];
}
