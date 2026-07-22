/* cocoametal_shell.m — host app shell: menu bar + About + app icon + screenshot.
 *
 * The "first-class Mac citizen" layer (docs/features/host-app-shell/). Merged from
 * the verified POC in hosted/hostshell/ (cmshell.m): same menu tree + About + the
 * CoreGraphics app icon, but production-wired — the controller holds the CMContext
 * and calls the cm_* ABI directly (same dylib), instead of the POC's test sink.
 *
 * Strong override of the weak cm__install_shell stub in cocoametal.m (the same
 * weak/strong split as cm_try_window / cm__open_settings_appkit), so the AppKit-free
 * TU stays AppKit-free. cm_open calls cm__install_shell once the window is up.
 *
 * The menu/About/icon are HOST-SIDE: they reach AROS only through the existing
 * cm_set_option -> CM_EV_SETTING channel (no callback into AROS). Menu mouse-clicks
 * work under the existing hand-pumped run loop (cm__pump_events_appkit forwards
 * untranslated NSEvents to [NSApp sendEvent:], which drives menu tracking) — no
 * [NSApp run] is introduced.
 *
 * Sources: Apple AppKit/CoreGraphics/ImageIO docs [PUB] + Apple HIG [PUB]. The
 * menu/Power-submenu shape was inspired by UTM's publicly-visible design
 * (Apache-2.0 — observed from its public docs/app, never its source) [PUB-UTM].
 * Independent work: no third-party implementation source — emulator, agent,
 * driver, or otherwise — was read, searched, or consulted in producing it, and any
 * resemblance to existing implementations is coincidental.
 */
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#include "cocoametal.h"
#include "macaron_icon.h"

/* Host-internal: inject a synthetic key transition into the event ring (defined in
 * cocoametal_control.m), so the Edit menu can hand the Amiga clipboard keys to AROS. */
extern void cm__inject_key(int vk, int pressed, unsigned mods);
extern int  cm__logical_w(CMContext *cx);   /* current framebuffer size, for the */
extern int  cm__logical_h(CMContext *cx);   /* Resolution menu checkmark          */

/* View ▸ Resolution: the standard sizes the AROS mode ladder offers. A menu tag
 * packs (w << 16) | h; the AROS side snaps a request to its nearest database
 * mode and reopens the screen, which resizes this window. */
static const struct { int w, h; } kCMResolutions[] = {
    {  640,  480 }, {  800,  600 }, { 1024,  768 }, { 1152,  864 },
    { 1280,  720 }, { 1280,  800 }, { 1280, 1024 }, { 1366,  768 },
    { 1440,  900 }, { 1600,  900 }, { 1600, 1200 }, { 1680, 1050 },
    { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
};

/* ------------------------------------------------------------------ icon ----
 * App icon: the purple macaron, decoded from an embedded base64 PNG
 * (macaron_icon.h) so it sets even on a headless/bare run with no external asset. */
static NSImage *cmsh_make_icon(void) {
    NSString *b64 = [NSString stringWithUTF8String:macaron_icon_b64];
    NSData *png = [[NSData alloc] initWithBase64EncodedString:b64
                       options:NSDataBase64DecodingIgnoreUnknownCharacters];
    return png ? [[NSImage alloc] initWithData:png] : nil;
}

/* Custom About panel. The stock NSAboutPanel ignores our runtime icon on a
 * bare/unbundled run (it shows the generic app icon), and a custom window lets us
 * brand it. Built like the settings window so it behaves under the hand-pumped run
 * loop; a static-strong ref keeps it alive across open/close. */
@interface CMAboutBG : NSView @end
@implementation CMAboutBG
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSGradient *g = [[NSGradient alloc] initWithColorsAndLocations:
        [NSColor colorWithSRGBRed:0.96 green:0.93 blue:0.99 alpha:1.0], 0.0,
        [NSColor whiteColor], 0.62, nil];
    [g drawInRect:self.bounds angle:270];
}
@end

static NSTextField *cmsh_about_label(NSString *s, CGFloat sz, NSColor *col, NSFontWeight w) {
    NSTextField *t = [NSTextField labelWithString:s];
    t.font = [NSFont systemFontOfSize:sz weight:w];
    t.textColor = col;
    t.alignment = NSTextAlignmentCenter;
    return t;
}

static NSWindow *gAboutWin = nil;

static void cmsh_show_about(void) {
    if (gAboutWin) { [gAboutWin makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; return; }

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 380, 440)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskFullSizeContentView)
                    backing:NSBackingStoreBuffered defer:NO];
    win.title = @"About Macaros";
    win.titleVisibility = NSWindowTitleHidden;
    win.titlebarAppearsTransparent = YES;
    win.movableByWindowBackground = YES;
    win.releasedWhenClosed = NO;
    win.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];  /* keep the light card in dark mode */

    CMAboutBG *bg = [[CMAboutBG alloc] initWithFrame:NSMakeRect(0, 0, 380, 440)];
    win.contentView = bg;

    NSColor *violet = [NSColor colorWithSRGBRed:0.52 green:0.28 blue:0.70 alpha:1.0];

    NSImage *macaron = cmsh_make_icon();
    NSImageView *iv = [NSImageView imageViewWithImage:(macaron ?: [NSImage imageNamed:NSImageNameApplicationIcon])];
    iv.imageScaling = NSImageScaleProportionallyUpOrDown;
    [iv.widthAnchor  constraintEqualToConstant:124].active = YES;
    [iv.heightAnchor constraintEqualToConstant:124].active = YES;

    NSTextField *name   = cmsh_about_label(@"Macaros", 28, [NSColor labelColor], NSFontWeightBold);
    NSTextField *tag    = cmsh_about_label(@"a macaron: AROS on a Mac", 13, violet, NSFontWeightMedium);
    NSTextField *ver    = cmsh_about_label(@"Version 0.1  ·  hosted darwin-aarch64", 11, [NSColor secondaryLabelColor], NSFontWeightRegular);

    NSBox *sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;
    [sep.widthAnchor constraintEqualToConstant:300].active = YES;

    NSTextField *blurb = cmsh_about_label(
        @"Macaros runs AROS, the open-source AmigaOS reimplementation, natively in a "
        @"Cocoa/Metal window. It bridges keyboard, mouse, display, clipboard and host folders.",
        11, [NSColor secondaryLabelColor], NSFontWeightRegular);
    blurb.preferredMaxLayoutWidth = 300;
    blurb.lineBreakMode = NSLineBreakByWordWrapping;
    blurb.maximumNumberOfLines = 0;

    NSTextField *lic    = cmsh_about_label(@"AROS is distributed under the AROS Public License.", 10, [NSColor tertiaryLabelColor], NSFontWeightRegular);
    NSTextField *author = cmsh_about_label(@"Created by John Knipper", 12, [NSColor labelColor], NSFontWeightSemibold);
    NSTextField *copyr  = cmsh_about_label(@"© 2026", 10, [NSColor tertiaryLabelColor], NSFontWeightRegular);

    NSStackView *stack = [NSStackView stackViewWithViews:@[iv, name, tag, ver, sep, blurb, lic, author, copyr]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 7;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [stack setCustomSpacing:16 afterView:iv];
    [stack setCustomSpacing:3  afterView:name];
    [stack setCustomSpacing:12 afterView:ver];
    [stack setCustomSpacing:14 afterView:sep];
    [stack setCustomSpacing:16 afterView:lic];
    [stack setCustomSpacing:5  afterView:author];

    [bg addSubview:stack];
    [stack.centerXAnchor constraintEqualToAnchor:bg.centerXAnchor].active = YES;
    [stack.topAnchor constraintEqualToAnchor:bg.topAnchor constant:38].active = YES;
    [stack.widthAnchor constraintLessThanOrEqualToConstant:320].active = YES;

    [win center];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    gAboutWin = win;
}

/* --------------------------------------------------------------- controller -- */
@interface CMShellController : NSObject <NSApplicationDelegate>
@property (nonatomic, assign) CMContext *cx;
@property (nonatomic) BOOL fullscreenOn, scanlinesOn, retinaOn, clipboardOn, captureInputOn, recordingOn;
@end

/* Where screenshots / recordings go: $AROS_RUN_DIR (the project's run/darwin-aarch64,
 * set by aros-ctl / run-window) if present, else ~/Desktop for a standalone app.
 * Timestamped so scripted demos don't clobber each other. */
static NSString *cmsh_capture_path(NSString *prefix, NSString *ext) {
    const char *rd = getenv("AROS_RUN_DIR");
    NSString *dir = (rd && *rd) ? @(rd)
                  : [NSHomeDirectory() stringByAppendingPathComponent:@"Desktop"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                withIntermediateDirectories:YES attributes:nil error:NULL];
    NSDateFormatter *f = [NSDateFormatter new];
    f.dateFormat = @"yyyyMMdd-HHmmss";
    return [dir stringByAppendingPathComponent:
            [NSString stringWithFormat:@"AROS-%@-%@.%@", prefix, [f stringFromDate:[NSDate date]], ext]];
}

@implementation CMShellController

/* App menu */
- (void)aboutAction:(id)s    { cmsh_show_about(); }
- (void)settingsAction:(id)s { cm_open_settings(_cx); }

/* File */
- (void)shotAction:(id)s {
    NSString *p = cmsh_capture_path(@"screenshot", @"png");
    int rc = cm_capture_png(_cx, p.UTF8String);
    if (rc == 0) NSLog(@"[shell] screenshot -> %@", p);
    else         NSLog(@"[shell] screenshot failed (%d) -> %@", rc, p);
}
- (void)recordAction:(id)s {
    NSMenuItem *item = (NSMenuItem *)s;
    if (_recordingOn) {
        int rc = cm_record_stop(_cx);
        _recordingOn = NO;
        item.title = @"Record Movie…";
        NSLog(@"[shell] recording stopped (rc=%d)", rc);
    } else {
        NSString *p = cmsh_capture_path(@"movie", @"mov");
        int rc = cm_record_start(_cx, p.UTF8String, 30, 0);
        if (rc == 0) {
            _recordingOn = YES;
            item.title = @"Stop Recording";
            NSLog(@"[shell] recording -> %@", p);
        } else {
            NSLog(@"[shell] record start failed (rc=%d) -> %@", rc, p);
        }
    }
}
- (void)openVolumeAction:(id)s {
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.canChooseDirectories = YES; op.canChooseFiles = NO; op.prompt = @"Mount";
    if ([op runModal] == NSModalResponseOK && op.URL) {
        NSString *spec = [NSString stringWithFormat:@"Mac:%@;WRITE", op.URL.path];
        cm_set_option_str(_cx, CM_OPT_VOLUME_ADD, spec.UTF8String);
    }
}

/* View (host-acted presentation) */
- (void)fullscreenAction:(id)s {
    _fullscreenOn = !_fullscreenOn;
    [(NSMenuItem *)s setState:_fullscreenOn ? NSControlStateValueOn : NSControlStateValueOff];
    cm_set_option(_cx, CM_OPT_FULLSCREEN, _fullscreenOn);
}
- (void)resolutionAction:(id)s {
    NSInteger tag = [(NSMenuItem *)s tag];
    cm_set_option(_cx, CM_OPT_REQUEST_MODE_W, (tag >> 16) & 0xFFFF);
    cm_set_option(_cx, CM_OPT_REQUEST_MODE_H, tag & 0xFFFF);
}
- (void)scalingAction:(id)s { cm_set_option(_cx, CM_OPT_SCALE_MODE, [(NSMenuItem *)s tag]); }
- (void)filterAction:(id)s  { cm_set_option(_cx, CM_OPT_FILTER, [(NSMenuItem *)s tag]); }
- (void)scanlinesAction:(id)s {
    _scanlinesOn = !_scanlinesOn;
    [(NSMenuItem *)s setState:_scanlinesOn ? NSControlStateValueOn : NSControlStateValueOff];
    cm_set_option(_cx, CM_OPT_EFFECT, _scanlinesOn ? CM_FX_SCANLINE : CM_FX_NEAREST);
}
- (void)retinaAction:(id)s {
    _retinaOn = !_retinaOn;
    [(NSMenuItem *)s setState:_retinaOn ? NSControlStateValueOn : NSControlStateValueOff];
    cm_set_option(_cx, CM_OPT_RETINA, _retinaOn);
}
/* View ▸ Theme: System / Light / Dark. Host-acted (cm_set_option sets NSApp.appearance
 * so the whole app + the status bar theme together). Also written to the defaults store
 * so the choice persists and the Settings popup agrees (theme is a sticky app pref, so
 * unlike the live-only toggles above it persists from the menu too). Radio checkmark. */
- (void)themeAction:(id)s {
    NSMenuItem *item = (NSMenuItem *)s;
    cm_set_option(_cx, CM_OPT_THEME, (long)item.tag);
    [[NSUserDefaults standardUserDefaults] setInteger:item.tag forKey:@"cocoametal.theme"];
    for (NSMenuItem *it in item.menu.itemArray)
        it.state = (it.tag == item.tag) ? NSControlStateValueOn : NSControlStateValueOff;
}

/* Machine (lifecycle + host integration) */
- (void)resetAction:(id)s     { cm_set_option(_cx, CM_OPT_POWER, CM_POWER_RESET); }
- (void)powerDownAction:(id)s { cm_set_option(_cx, CM_OPT_POWER, CM_POWER_REQUEST_DOWN); }
- (void)forceDownAction:(id)s { cm_set_option(_cx, CM_OPT_POWER, CM_POWER_FORCE_DOWN); }
- (void)forceQuitAction:(id)s { cm_set_option(_cx, CM_OPT_POWER, CM_POWER_FORCE_QUIT); }
- (void)captureInputAction:(id)s {
    _captureInputOn = !_captureInputOn;
    [(NSMenuItem *)s setState:_captureInputOn ? NSControlStateValueOn : NSControlStateValueOff];
    /* TODO: a real exclusive input grab + release hotkey (host-side). Recorded for now. */
}
- (void)clipboardShareAction:(id)s {
    _clipboardOn = !_clipboardOn;
    [(NSMenuItem *)s setState:_clipboardOn ? NSControlStateValueOn : NSControlStateValueOff];
    cm_set_option(_cx, CM_OPT_CLIPBOARD_SHARE, _clipboardOn);
}

/* Edit — the AROS shell's clipboard keys are Right-Amiga+C / Right-Amiga+V (handled
 * by console.device + ConClip), so the Mac Copy/Paste items synthesize that chord
 * into AROS. macOS virtual keycodes: 54 = Right Command -> RAWKEY_RAMIGA, 8='c', 9='v'.
 * The Right-Amiga key carries the RCOMMAND qualifier (set on its down, cleared on its
 * up); the C/V ride inside that hold. The bridge then syncs PRIMARY_CLIP <-> Mac. */
- (void)editAmigaChord:(int)key {
    enum { RAMIGA = 54 };
    cm__inject_key(RAMIGA, 1, CM_MOD_CMD);   /* Right-Amiga down */
    cm__inject_key(key,    1, CM_MOD_CMD);   /* C/V down (RCOMMAND active) */
    cm__inject_key(key,    0, CM_MOD_CMD);   /* C/V up */
    cm__inject_key(RAMIGA, 0, 0);            /* Right-Amiga up (mods=0 clears RCOMMAND) */
}
- (void)editCopyAction:(id)s  { [self editAmigaChord:8]; }   /* Right-Amiga+C */
- (void)editPasteAction:(id)s { [self editAmigaChord:9]; }   /* Right-Amiga+V */

/* Enable Paste only when the Mac clipboard actually holds text (Copy stays on: it
 * copies the console selection, a no-op if nothing is marked). */
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(editPasteAction:))
        return [[NSPasteboard generalPasteboard]
                   canReadObjectForClasses:@[[NSString class]] options:nil];
    if (item.action == @selector(resolutionAction:)) {
        NSInteger cur = ((NSInteger)cm__logical_w(_cx) << 16) | cm__logical_h(_cx);
        item.state = (item.tag == cur) ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    return YES;
}

/* Help */
- (void)websiteAction:(id)s {
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://aros.org"]];
}
- (void)reportAction:(id)s {
    [[NSWorkspace sharedWorkspace]
        openURL:[NSURL URLWithString:@"https://github.com/aros-development-team/AROS/issues"]];
}
@end

/* ----------------------------------------------------------- menu builders -- */
static NSMenuItem *cmsh_add(NSMenu *m, NSString *t, SEL a, id tgt, NSString *k,
                            NSEventModifierFlags mods) {
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:t action:a keyEquivalent:k ?: @""];
    if (mods) it.keyEquivalentModifierMask = mods;
    if (tgt) it.target = tgt;
    [m addItem:it];
    return it;
}
static NSMenu *cmsh_submenu(NSMenu *bar, NSString *title) {
    NSMenuItem *host = [[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""];
    NSMenu *sub = [[NSMenu alloc] initWithTitle:title];
    host.submenu = sub; [bar addItem:host];
    return sub;
}

static void cmsh_build_menu(CMShellController *c) {
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@"MainMenu"];

    NSMenu *app = cmsh_submenu(bar, @"Macaros");
    cmsh_add(app, @"About Macaros", @selector(aboutAction:), c, @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Settings…", @selector(settingsAction:), c, @",", NSEventModifierFlagCommand);
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem *services = [[NSMenuItem alloc] initWithTitle:@"Services" action:NULL keyEquivalent:@""];
    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    services.submenu = servicesMenu; [app addItem:services]; [NSApp setServicesMenu:servicesMenu];
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Hide Macaros", @selector(hide:), nil, @"h", NSEventModifierFlagCommand);
    cmsh_add(app, @"Hide Others", @selector(hideOtherApplications:), nil, @"h",
             NSEventModifierFlagCommand | NSEventModifierFlagOption);
    cmsh_add(app, @"Show All", @selector(unhideAllApplications:), nil, @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Quit Macaros", @selector(terminate:), nil, @"q", NSEventModifierFlagCommand);

    NSMenu *file = cmsh_submenu(bar, @"File");
    cmsh_add(file, @"Take Screenshot", @selector(shotAction:), c, @"3",
             NSEventModifierFlagShift | NSEventModifierFlagCommand);
    cmsh_add(file, @"Record Movie…", @selector(recordAction:), c, @"", 0);
    [file addItem:[NSMenuItem separatorItem]];
    cmsh_add(file, @"Open Folder as Volume…", @selector(openVolumeAction:), c, @"o",
             NSEventModifierFlagCommand);
    [file addItem:[NSMenuItem separatorItem]];
    cmsh_add(file, @"Close", @selector(performClose:), nil, @"w", NSEventModifierFlagCommand);

    NSMenu *edit = cmsh_submenu(bar, @"Edit");
    cmsh_add(edit, @"Undo", @selector(undo:), nil, @"z", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Redo", @selector(redo:), nil, @"z",
             NSEventModifierFlagShift | NSEventModifierFlagCommand);
    [edit addItem:[NSMenuItem separatorItem]];
    cmsh_add(edit, @"Cut", @selector(cut:), nil, @"x", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Copy", @selector(editCopyAction:), c, @"c", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Paste", @selector(editPasteAction:), c, @"v", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Select All", @selector(selectAll:), nil, @"a", NSEventModifierFlagCommand);

    NSMenu *view = cmsh_submenu(bar, @"View");
    cmsh_add(view, @"Enter Full Screen", @selector(fullscreenAction:), c, @"f",
             NSEventModifierFlagControl | NSEventModifierFlagCommand);
    NSMenu *resolution = [[NSMenu alloc] initWithTitle:@"Resolution"];
    NSMenuItem *rh = [[NSMenuItem alloc] initWithTitle:@"Resolution" action:NULL keyEquivalent:@""];
    rh.submenu = resolution; [view addItem:rh];
    for (size_t i = 0; i < sizeof(kCMResolutions)/sizeof(kCMResolutions[0]); i++) {
        int w = kCMResolutions[i].w, h = kCMResolutions[i].h;
        NSString *t = [NSString stringWithFormat:@"%d × %d", w, h];
        cmsh_add(resolution, t, @selector(resolutionAction:), c, @"", 0).tag = (w << 16) | h;
    }
    NSMenu *scaling = [[NSMenu alloc] initWithTitle:@"Scaling"];
    NSMenuItem *sh = [[NSMenuItem alloc] initWithTitle:@"Scaling" action:NULL keyEquivalent:@""];
    sh.submenu = scaling; [view addItem:sh];
    cmsh_add(scaling, @"Aspect Fit", @selector(scalingAction:), c, @"", 0).tag = CM_SCALE_ASPECT_FIT;
    cmsh_add(scaling, @"Integer", @selector(scalingAction:), c, @"", 0).tag = CM_SCALE_INTEGER_NEAREST;
    cmsh_add(scaling, @"Pixel-Perfect", @selector(scalingAction:), c, @"", 0).tag = CM_SCALE_PIXEL_PERFECT;
    cmsh_add(scaling, @"Stretch", @selector(scalingAction:), c, @"", 0).tag = CM_SCALE_FIT;
    NSMenu *filter = [[NSMenu alloc] initWithTitle:@"Filter"];
    NSMenuItem *fh = [[NSMenuItem alloc] initWithTitle:@"Filter" action:NULL keyEquivalent:@""];
    fh.submenu = filter; [view addItem:fh];
    cmsh_add(filter, @"Nearest", @selector(filterAction:), c, @"", 0).tag = CM_FILTER_NEAREST;
    cmsh_add(filter, @"Linear", @selector(filterAction:), c, @"", 0).tag = CM_FILTER_LINEAR;
    cmsh_add(view, @"Scanlines", @selector(scanlinesAction:), c, @"", 0);
    cmsh_add(view, @"Retina / HiDPI", @selector(retinaAction:), c, @"", 0);
    [view addItem:[NSMenuItem separatorItem]];
    NSMenu *theme = [[NSMenu alloc] initWithTitle:@"Theme"];
    NSMenuItem *th = [[NSMenuItem alloc] initWithTitle:@"Theme" action:NULL keyEquivalent:@""];
    th.submenu = theme; [view addItem:th];
    cmsh_add(theme, @"System", @selector(themeAction:), c, @"", 0).tag = CM_THEME_SYSTEM;
    cmsh_add(theme, @"Light",  @selector(themeAction:), c, @"", 0).tag = CM_THEME_LIGHT;
    cmsh_add(theme, @"Dark",   @selector(themeAction:), c, @"", 0).tag = CM_THEME_DARK;
    /* Reflect the persisted choice as the initial checkmark (default System). */
    NSInteger curTheme = [[NSUserDefaults standardUserDefaults] integerForKey:@"cocoametal.theme"];
    for (NSMenuItem *it in theme.itemArray)
        it.state = (it.tag == curTheme) ? NSControlStateValueOn : NSControlStateValueOff;

    NSMenu *machine = cmsh_submenu(bar, @"Machine");
    cmsh_add(machine, @"Reset", @selector(resetAction:), c, @"r",
             NSEventModifierFlagControl | NSEventModifierFlagCommand);
    NSMenu *power = [[NSMenu alloc] initWithTitle:@"Power"];
    NSMenuItem *ph = [[NSMenuItem alloc] initWithTitle:@"Power" action:NULL keyEquivalent:@""];
    ph.submenu = power; [machine addItem:ph];
    cmsh_add(power, @"Request Power Down", @selector(powerDownAction:), c, @"", 0);
    cmsh_add(power, @"Force Shut Down", @selector(forceDownAction:), c, @"", 0);
    cmsh_add(power, @"Force Quit", @selector(forceQuitAction:), c, @"", 0);
    [machine addItem:[NSMenuItem separatorItem]];
    cmsh_add(machine, @"Capture Input", @selector(captureInputAction:), c, @"i",
             NSEventModifierFlagControl | NSEventModifierFlagCommand);
    NSMenuItem *share = cmsh_add(machine, @"Share Clipboard", @selector(clipboardShareAction:), c, @"", 0);
    id sharePref = [[NSUserDefaults standardUserDefaults] objectForKey:@"sharing.clipboard"];
    c.clipboardOn = sharePref ? [[NSUserDefaults standardUserDefaults] boolForKey:@"sharing.clipboard"] : YES;
    share.state = c.clipboardOn ? NSControlStateValueOn : NSControlStateValueOff;

    NSMenu *window = cmsh_submenu(bar, @"Window");
    cmsh_add(window, @"Minimize", @selector(performMiniaturize:), nil, @"m", NSEventModifierFlagCommand);
    cmsh_add(window, @"Zoom", @selector(performZoom:), nil, @"", 0);
    [NSApp setWindowsMenu:window];

    NSMenu *help = cmsh_submenu(bar, @"Help");
    cmsh_add(help, @"AROS Website", @selector(websiteAction:), c, @"", 0);
    cmsh_add(help, @"Report an Issue", @selector(reportAction:), c, @"", 0);
    [NSApp setHelpMenu:help];

    [NSApp setMainMenu:bar];
}

/* ---------------------------------------------------- cm__install_shell ----- */
static CMShellController *gShell = nil;   /* static-strong: one shell per process */

void cm__install_shell(CMContext *cx) {
    @autoreleasepool {
        [NSApplication sharedApplication];            /* idempotent (cm_try_window did it) */
        if (!gShell) gShell = [CMShellController new];
        gShell.cx = cx;
        if ([NSApp delegate] == nil) [NSApp setDelegate:gShell];
        NSImage *icon = cmsh_make_icon();
        if (icon) [NSApp setApplicationIconImage:icon];
        cmsh_build_menu(gShell);
    }
}

/* ---------------------------------------------- cm_capture_png (screenshot) -- */
int cm_capture_png(CMContext *cx, const char *path) {
    if (!cx || !path) return 1;
    int tw = 0, th = 0, scale = 0;
    if (cm_target_size(cx, &tw, &th, &scale) != 0 || scale <= 0) return 2;
    int w = tw / scale, h = th / scale;               /* logical dims (cm_readback wants logical) */
    if (w <= 0 || h <= 0) return 2;
    size_t stride = (size_t)w * 4;
    uint8_t *buf = (uint8_t *)malloc(stride * (size_t)h);
    if (!buf) return 3;
    if (cm_readback(cx, buf, (int)stride, w, h) != 0) { free(buf); return 4; }

    /* BGRA8 bytes -> CGImage. The 32-bit little-endian word is 0xAARRGGBB, so the
     * low 3 memory bytes are B,G,R. AROS renders an OPAQUE framebuffer but leaves
     * the alpha byte at 0; the live window looks right only because the Metal layer
     * is opaque. So capture as opaque (skip alpha) -- with premultiplied alpha the
     * whole desktop goes transparent and a PNG viewer renders it white (the bug:
     * only the icon, which carried alpha, survived). */
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, stride, cs,
        (CGBitmapInfo)(kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little));
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    int rc = 5;
    if (img) {
        NSURL *url = [NSURL fileURLWithPath:@(path)];
        CGImageDestinationRef d =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
        if (d) {
            CGImageDestinationAddImage(d, img, NULL);
            rc = CGImageDestinationFinalize(d) ? 0 : 6;
            CFRelease(d);
        }
        CGImageRelease(img);
    }
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    free(buf);
    return rc;
}

/* ------------------------------------------ cm_record_* (movie via AVFoundation)
 * Frame source = the per-present hook cm__record_frame (called from cm_present),
 * so capture is driven by AROS's own present cadence — deterministic and
 * unattended-verifiable (N presents -> N frames), no run-loop timer. The frames
 * are our own cm_readback pixels, so there is NO Screen-Recording / TCC. */
@interface CMRecorder : NSObject
@property (nonatomic, strong) AVAssetWriter *writer;
@property (nonatomic, strong) AVAssetWriterInput *input;
@property (nonatomic, strong) AVAssetWriterInputPixelBufferAdaptor *adaptor;
@property (nonatomic, assign) int w, h, fps;
@property (nonatomic, assign) long frameIdx;
@property (nonatomic, assign) BOOL active;
@property (nonatomic, assign) CFAbsoluteTime startTime;  /* wall-clock anchor for real-time PTS */
@property (nonatomic, assign) CMTime lastPTS;            /* keep PTS strictly increasing */
@end
@implementation CMRecorder @end

static CMRecorder *gRec = nil;

int cm_record_start(CMContext *cx, const char *path, int fps, int codec) {
    if (!cx || !path) return 1;
    if (gRec && gRec.active) return 2;                 /* already recording */
    int tw = 0, th = 0, scale = 0;
    if (cm_target_size(cx, &tw, &th, &scale) != 0 || scale <= 0) return 3;
    int w = tw / scale, h = th / scale;                 /* logical dims (cm_readback) */
    if (w <= 0 || h <= 0) return 3;
    if (fps <= 0) fps = 30;

    @autoreleasepool {
        NSString *p = @(path);
        [[NSFileManager defaultManager] removeItemAtPath:p error:NULL];
        NSError *err = nil;
        AVAssetWriter *writer = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:p]
                                                         fileType:AVFileTypeQuickTimeMovie error:&err];
        if (!writer) { NSLog(@"[shell] record: writer failed: %@", err); return 4; }
        NSString *codecType = (codec == 1) ? AVVideoCodecTypeHEVC : AVVideoCodecTypeH264;
        AVAssetWriterInput *input = [AVAssetWriterInput
            assetWriterInputWithMediaType:AVMediaTypeVideo
                           outputSettings:@{ AVVideoCodecKey: codecType,
                                             AVVideoWidthKey: @(w), AVVideoHeightKey: @(h) }];
        input.expectsMediaDataInRealTime = NO;
        AVAssetWriterInputPixelBufferAdaptor *adaptor =
            [AVAssetWriterInputPixelBufferAdaptor
                assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                           sourcePixelBufferAttributes:@{
                    (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                    (NSString *)kCVPixelBufferWidthKey: @(w),
                    (NSString *)kCVPixelBufferHeightKey: @(h) }];
        if (![writer canAddInput:input]) { NSLog(@"[shell] record: cannot add input"); return 5; }
        [writer addInput:input];

        /* AUDIO SEAM (future, when the CoreAudio capture lands): add a second input
         *   AVAssetWriterInput *audio = [AVAssetWriterInput
         *       assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:<AAC>];
         *   [writer addInput:audio];  // store on the CMRecorder
         * and append CMSampleBuffers from a cm__record_audio() hook (mirroring
         * cm__record_frame) fed by the audio device's playback ring — same writer +
         * session, so A/V stay muxed and time-aligned. Video-only until then. */

        if (![writer startWriting]) { NSLog(@"[shell] record: startWriting failed: %@", writer.error); return 6; }
        [writer startSessionAtSourceTime:kCMTimeZero];

        CMRecorder *r = [CMRecorder new];
        r.writer = writer; r.input = input; r.adaptor = adaptor;
        r.w = w; r.h = h; r.fps = fps; r.frameIdx = 0; r.active = YES;
        r.startTime = CFAbsoluteTimeGetCurrent(); r.lastPTS = kCMTimeInvalid;
        gRec = r;
    }
    return 0;
}

void cm__record_frame(CMContext *cx) {
    CMRecorder *r = gRec;
    if (!cx || !r || !r.active || !r.input.isReadyForMoreMediaData) return;
    @autoreleasepool {
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolRef pool = r.adaptor.pixelBufferPool;
        if (pool) (void)CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb);
        if (!pb && CVPixelBufferCreate(NULL, r.w, r.h, kCVPixelFormatType_32BGRA,
                                       NULL, &pb) != kCVReturnSuccess) return;
        CVPixelBufferLockBaseAddress(pb, 0);
        void  *dst    = CVPixelBufferGetBaseAddress(pb);
        size_t stride = CVPixelBufferGetBytesPerRow(pb);
        cm_readback(cx, dst, (int)stride, r.w, r.h);    /* BGRA8 logical frame */
        CVPixelBufferUnlockBaseAddress(pb, 0);
        /* Real-time PTS: frames are appended per AROS present (sparse on static
         * screens), so timestamp by wall-clock — a demo that ran N seconds becomes
         * an N-second movie. The guard keeps PTS strictly increasing for bursts. */
        CMTime pts = CMTimeMakeWithSeconds(CFAbsoluteTimeGetCurrent() - r.startTime, 600);
        if (CMTIME_IS_VALID(r.lastPTS) && CMTimeCompare(pts, r.lastPTS) <= 0)
            pts = CMTimeAdd(r.lastPTS, CMTimeMake(1, 600));
        if ([r.adaptor appendPixelBuffer:pb withPresentationTime:pts]) {
            r.lastPTS = pts;
            r.frameIdx++;
        }
        CVPixelBufferRelease(pb);
    }
}

int cm_record_stop(CMContext *cx) {
    (void)cx;
    CMRecorder *r = gRec;
    if (!r || !r.active) return 1;
    r.active = NO;
    @autoreleasepool {
        [r.input markAsFinished];
        __block BOOL done = NO;
        [r.writer finishWritingWithCompletionHandler:^{ done = YES; }];
        /* bounded wait so the .mov is finalized when we return (pump the run loop). */
        CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 5.0;
        while (!done && CFAbsoluteTimeGetCurrent() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
        NSLog(@"[shell] recording stopped: %ld frames -> %@", r.frameIdx, r.writer.outputURL.path);
    }
    gRec = nil;
    return 0;
}

/* Auto-stop the CURRENT recording after `seconds` — the harness "record N seconds"
 * form (aros-ctl record 25). The timer lives in the app process (not the harness),
 * so the caller returns immediately while the recording runs and stops itself.
 * Generation-safe: captures this session and only fires if gRec is still it and
 * active, so a manual Stop or a later recording is never clobbered. Main queue
 * (where cm_record_stop pumps the writer's finalize). */
void cm__record_autostop(CMContext *cx, double seconds) {
    if (seconds <= 0) return;
    CMRecorder *target = gRec;
    if (!target || !target.active) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        if (gRec == target && target.active) {
            NSLog(@"[shell] auto-stop after %.1fs", seconds);
            cm_record_stop(cx);
        }
    });
}
