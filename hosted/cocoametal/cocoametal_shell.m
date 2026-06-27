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
 * Clean-room: Apple AppKit/CoreGraphics/ImageIO docs [PUB] + Apple HIG [PUB]. No GPL
 * emulator source read. UTM (Apache-2.0) informed only the public menu layout.
 */
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#include "cocoametal.h"

/* ------------------------------------------------------------------ icon ---- */
static NSImage *cmsh_make_icon(void) {
    const int S = 256;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(NULL, S, S, 8, 0, cs,
                                           (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    if (!c) { CGColorSpaceRelease(cs); return nil; }
    CGContextClearRect(c, CGRectMake(0, 0, S, S));
    CGFloat inset = S * 0.08, r = S * 0.22;
    CGPathRef path = CGPathCreateWithRoundedRect(
        CGRectMake(inset, inset, S - 2 * inset, S - 2 * inset), r, r, NULL);
    CGContextAddPath(c, path); CGContextClip(c);
    CGFloat h = S / 2.0;
    CGContextSetRGBFillColor(c, 0.16, 0.20, 0.85, 1); CGContextFillRect(c, CGRectMake(0, 0, h, h));
    CGContextSetRGBFillColor(c, 0.92, 0.85, 0.15, 1); CGContextFillRect(c, CGRectMake(h, 0, h, h));
    CGContextSetRGBFillColor(c, 0.88, 0.18, 0.18, 1); CGContextFillRect(c, CGRectMake(0, h, h, h));
    CGContextSetRGBFillColor(c, 0.20, 0.78, 0.28, 1); CGContextFillRect(c, CGRectMake(h, h, h, h));
    CGContextSetRGBStrokeColor(c, 0, 0, 0, 0.35); CGContextSetLineWidth(c, S * 0.012);
    CGContextMoveToPoint(c, h, inset); CGContextAddLineToPoint(c, h, S - inset);
    CGContextMoveToPoint(c, inset, h); CGContextAddLineToPoint(c, S - inset, h);
    CGContextStrokePath(c);
    CGImageRef img = CGBitmapContextCreateImage(c);
    NSImage *ns = img ? [[NSImage alloc] initWithCGImage:img size:NSMakeSize(S, S)] : nil;
    if (img) CGImageRelease(img);
    CGPathRelease(path); CGContextRelease(c); CGColorSpaceRelease(cs);
    return ns;
}

static void cmsh_show_about(void) {
    [NSApp orderFrontStandardAboutPanel:@{
        NSAboutPanelOptionApplicationName: @"Daedalus",
        NSAboutPanelOptionApplicationVersion: @"hosted-darwin-aarch64",
        NSAboutPanelOptionCredits:
            [[NSAttributedString alloc]
                initWithString:@"Daedalus — the macOS host that gives AROS its wings on "
                               @"Apple Silicon.\nRuns AROS, the open-source AmigaOS "
                               @"reimplementation, in a native Cocoa/Metal window.\n"
                               @"AROS is distributed under the AROS Public License."],
    }];
}

/* --------------------------------------------------------------- controller -- */
@interface CMShellController : NSObject <NSApplicationDelegate>
@property (nonatomic, assign) CMContext *cx;
@property (nonatomic) BOOL fullscreenOn, scanlinesOn, retinaOn, clipboardOn, captureInputOn;
@end

@implementation CMShellController

/* App menu */
- (void)aboutAction:(id)s    { cmsh_show_about(); }
- (void)settingsAction:(id)s { cm_open_settings(_cx); }

/* File */
- (void)shotAction:(id)s {
    NSString *p = [NSHomeDirectory() stringByAppendingPathComponent:@"Desktop/AROS-screenshot.png"];
    int rc = cm_capture_png(_cx, p.UTF8String);
    if (rc == 0) NSLog(@"[shell] screenshot -> %@", p);
    else         NSLog(@"[shell] screenshot failed (%d) -> %@", rc, p);
}
- (void)recordAction:(id)s {
    NSString *p = [NSHomeDirectory() stringByAppendingPathComponent:@"Desktop/AROS-movie.mov"];
    int rc = cm_record_start(_cx, p.UTF8String, 30, 0);
    if (rc != 0) NSLog(@"[shell] movie recording not wired yet (AVFoundation spike pending)");
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

    NSMenu *app = cmsh_submenu(bar, @"Daedalus");
    cmsh_add(app, @"About Daedalus", @selector(aboutAction:), c, @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Settings…", @selector(settingsAction:), c, @",", NSEventModifierFlagCommand);
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem *services = [[NSMenuItem alloc] initWithTitle:@"Services" action:NULL keyEquivalent:@""];
    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    services.submenu = servicesMenu; [app addItem:services]; [NSApp setServicesMenu:servicesMenu];
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Hide Daedalus", @selector(hide:), nil, @"h", NSEventModifierFlagCommand);
    cmsh_add(app, @"Hide Others", @selector(hideOtherApplications:), nil, @"h",
             NSEventModifierFlagCommand | NSEventModifierFlagOption);
    cmsh_add(app, @"Show All", @selector(unhideAllApplications:), nil, @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    cmsh_add(app, @"Quit Daedalus", @selector(terminate:), nil, @"q", NSEventModifierFlagCommand);

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
    cmsh_add(edit, @"Copy", @selector(copy:), nil, @"c", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Paste", @selector(paste:), nil, @"v", NSEventModifierFlagCommand);
    cmsh_add(edit, @"Select All", @selector(selectAll:), nil, @"a", NSEventModifierFlagCommand);

    NSMenu *view = cmsh_submenu(bar, @"View");
    cmsh_add(view, @"Enter Full Screen", @selector(fullscreenAction:), c, @"f",
             NSEventModifierFlagControl | NSEventModifierFlagCommand);
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
    cmsh_add(machine, @"Share Clipboard", @selector(clipboardShareAction:), c, @"", 0);

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

    /* BGRA8 bytes -> CGImage (little-endian + alpha-first reads memory as B,G,R,A). */
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, stride, cs,
        (CGBitmapInfo)(kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little));
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
        if (![writer startWriting]) { NSLog(@"[shell] record: startWriting failed: %@", writer.error); return 6; }
        [writer startSessionAtSourceTime:kCMTimeZero];

        CMRecorder *r = [CMRecorder new];
        r.writer = writer; r.input = input; r.adaptor = adaptor;
        r.w = w; r.h = h; r.fps = fps; r.frameIdx = 0; r.active = YES;
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
        if ([r.adaptor appendPixelBuffer:pb withPresentationTime:CMTimeMake(r.frameIdx, r.fps)])
            r.frameIdx++;
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
