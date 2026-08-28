/* shell_test.m — [GSHELL] verify the host app shell against the REAL dylib.
 *
 * dlopens build/cocoametal.dylib exactly as HostLib_Open does (like abi_test.c),
 * calls cm_open (which now installs the menu bar + About + icon via the strong
 * cm__install_shell in cocoametal_shell.m), then asserts — against the PRODUCTION
 * dylib — that (1) the menu tree is installed, and (2) invoking a menu item drives
 * the real cm_* ABI: a host-acted item reaches cm_set_option (read back via
 * cm_get_option) and an AROS-facing item also surfaces a CM_EV_SETTING.
 *
 * This is the de-risk the merge needed: the same checks the isolated POC proved
 * ([G-MENU]/[G-ACTION]), but now through the real dylib + real cm_* wiring (not a
 * mock sink). Headless-safe (no screenshot, no TCC). Clean-room: AppKit docs [PUB].
 */
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "cocoametal.h"

typedef CMContext *(*open_fn)(int, int, const CMPixelDesc *, const char *);
typedef void       (*close_fn)(CMContext *);
typedef int        (*getopt_fn)(CMContext *, int, long *);
typedef int        (*pump_fn)(CMContext *, CMEvent *, int);
typedef int        (*setoptstr_fn)(CMContext *, int, const char *);
typedef int        (*getoptstr_fn)(CMContext *, int, char *, int);
typedef void       (*upload_fn)(CMContext *, const void *, int, int, int, int, int);
typedef void       (*present_fn)(CMContext *);
typedef int        (*recstart_fn)(CMContext *, const char *, int, int);
typedef int        (*recstop_fn)(CMContext *);

static int g_fail = 0;
static int check(int c, const char *w) { if (!c) { g_fail++; printf("    FAIL: %s\n", w); } return c; }
static NSMenu *sub(NSMenu *bar, NSString *t) {
    for (NSMenuItem *it in bar.itemArray)
        if ([it.title isEqual:t] || [it.submenu.title isEqual:t]) return it.submenu;
    return nil;
}
static NSMenuItem *item(NSMenu *m, NSString *t) {
    for (NSMenuItem *it in m.itemArray) if ([it.title isEqual:t]) return it;
    return nil;
}
/* First view of a class anywhere under `v` (the tab bodies are nested). */
static NSView *view_of_class(NSView *v, Class wanted) {
    if ([v isKindOfClass:wanted]) return v;
    for (NSView *s in v.subviews) {
        NSView *found = view_of_class(s, wanted);
        if (found) return found;
    }
    return nil;
}

static int view_has_label(NSView *v, NSString *needle) {
    if ([v isKindOfClass:[NSTextField class]] &&
        [[(NSTextField *)v stringValue] containsString:needle]) return 1;
    for (NSView *s in v.subviews) if (view_has_label(s, needle)) return 1;
    return 0;
}
static int saw_setting(CMEvent *ev, int n, int code, int x) {
    for (int i = 0; i < n; i++)
        if (ev[i].type == CM_EV_SETTING && ev[i].code == code &&
            (x < 0 || ev[i].x == x)) return 1;
    return 0;
}
static int saw_amiga_chord(CMEvent *ev, int n, int key) {
    for (int i = 0; i + 3 < n; i++) {
        if (ev[i].type     == CM_EV_KEY && ev[i].code     == 54 && ev[i].pressed     == 1 && ev[i].mods     == CM_MOD_CMD &&
            ev[i + 1].type == CM_EV_KEY && ev[i + 1].code == key && ev[i + 1].pressed == 1 && ev[i + 1].mods == CM_MOD_CMD &&
            ev[i + 2].type == CM_EV_KEY && ev[i + 2].code == key && ev[i + 2].pressed == 0 && ev[i + 2].mods == CM_MOD_CMD &&
            ev[i + 3].type == CM_EV_KEY && ev[i + 3].code == 54 && ev[i + 3].pressed == 0 && ev[i + 3].mods == 0)
            return 1;
    }
    return 0;
}
static int dir_has_png(NSString *dir) {
    NSArray<NSString *> *files = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:NULL];
    for (NSString *f in files)
        if ([f hasPrefix:@"Macaros-screenshot-"] && [f hasSuffix:@".png"]) return 1;
    return 0;
}

int main(int argc, const char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = (argc > 1) ? argv[1]
                     : getenv("COCOAMETAL_DYLIB") ? getenv("COCOAMETAL_DYLIB")
                     : "build/cocoametal.dylib";
    printf("[GSHELL] menu bar + action wiring against the REAL dylib %s\n", path);

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("[GSHELL] FAIL dlopen: %s\n", dlerror()); return 1; }
    open_fn   cm_open_  = (open_fn)  dlsym(h, "cm_open");
    close_fn  cm_close_ = (close_fn) dlsym(h, "cm_close");
    getopt_fn cm_get_   = (getopt_fn)dlsym(h, "cm_get_option");
    pump_fn   cm_pump_  = (pump_fn)  dlsym(h, "cm_pump_events");
    setoptstr_fn cm_set_str_ = (setoptstr_fn)dlsym(h, "cm_set_option_str");
    getoptstr_fn cm_get_str_ = (getoptstr_fn)dlsym(h, "cm_get_option_str");
    if (!cm_open_ || !cm_close_ || !cm_get_ || !cm_pump_) {
        printf("[GSHELL] FAIL dlsym\n"); return 1;
    }

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        setenv("MACAROS_VERSION", "9.8.7", 1);
        setenv("MACAROS_BUILD_NUMBER", "321", 1);

        CMPixelDesc fmt = { .bytesPerPixel = 4,
            .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
            .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
            .redMask = 0x00FF0000, .alphaMask = 0xFF000000 };
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"sharing.clipboard"];
        CMContext *cx = cm_open_(320, 200, &fmt, "AROS [GSHELL]");
        if (!cx) { printf("[GSHELL] FAIL cm_open NULL\n"); return 1; }

        /* (1) cm_open installed the menu bar */
        NSMenu *bar = [NSApp mainMenu];
        check(bar && bar.itemArray.count >= 7, "cm_open installed a menu bar (>=7 menus)");

        /* (1a) The window carries the HOST app name, not the guest's cm_open title
         * argument (deliberately "AROS [GSHELL]" above). */
        NSWindow *dispWin = [NSApp windows].firstObject;
        check(dispWin && [dispWin.title isEqual:@"Macaros"],
              "display window is titled by the host app, not the cm_open argument");
        NSMenu *file = sub(bar, @"File"), *edit = sub(bar, @"Edit"),
               *view = sub(bar, @"View"), *machine = sub(bar, @"Machine");
        NSMenu *help = sub(bar, @"Help");
        NSMenuItem *shot = item(file, @"Take Screenshot");
        check(shot && [shot.keyEquivalent isEqual:@"3"], "File ▸ Take Screenshot = ⇧⌘3");
        check(sub(view, @"Scaling") && item(view, @"Scanlines"), "View ▸ Scaling + Scanlines present");
        check(item(machine, @"Reset") &&
              item(item(machine, @"Power").submenu, @"Force Quit"),
              "Machine ▸ Reset + Power ▸ Force Quit present");
        check(item(help, @"Report a Compatibility Problem…") &&
              item(help, @"Show Reports") &&
              item(help, @"Open Macaros Issue Tracker"),
              "Help exposes local reports and the Macaros issue tracker separately");
        NSMenuItem *about = item(bar.itemArray.firstObject.submenu, @"About Macaros");
        [NSApp sendAction:about.action to:about.target from:about];
        NSWindow *aboutWin = nil;
        for (NSWindow *w in [NSApp windows])
            if ([w.title isEqual:@"About Macaros"]) { aboutWin = w; break; }
        check(aboutWin && view_has_label(aboutWin.contentView, @"Version 9.8.7 (build 321)"),
              "About reads release version and build number at runtime");
        CMEvent ev[16]; int n = cm_pump_(cx, ev, 16);
        long cs = -1; cm_get_(cx, CM_OPT_CLIPBOARD_SHARE, &cs);
        check(cs == 1 && !saw_setting(ev, n, CM_OPT_CLIPBOARD_SHARE, -1),
              "Share Clipboard default is ON without an early AROS-facing event");

        /* Edit ▸ Paste must inject the guest's complete Right-Amiga+V chord.
         * A menu flash without these four transitions is a silent no-op. */
        NSMenuItem *paste = item(edit, @"Paste");
        check(paste != nil, "Edit ▸ Paste present");
        [NSApp sendAction:paste.action to:paste.target from:paste];
        n = cm_pump_(cx, ev, 16);
        check(saw_amiga_chord(ev, n, 9),
              "Edit ▸ Paste injects Right-Amiga+V down/up into AROS");

        /* (1b) File ▸ Take Screenshot uses the real menu action and AROS_RUN_DIR. */
        NSString *shotDir = [NSTemporaryDirectory() stringByAppendingPathComponent:
                             [NSString stringWithFormat:@"aros-gshell-shot-%@", NSUUID.UUID.UUIDString]];
        [[NSFileManager defaultManager] createDirectoryAtPath:shotDir
                                  withIntermediateDirectories:YES attributes:nil error:NULL];
        setenv("AROS_RUN_DIR", shotDir.UTF8String, 1);
        [NSApp sendAction:shot.action to:shot.target from:shot];
        check(dir_has_png(shotDir), "Take Screenshot wrote Macaros-screenshot-*.png to AROS_RUN_DIR");
        [[NSFileManager defaultManager] removeItemAtPath:shotDir error:NULL];

        /* (2a) host-acted: Scanlines -> cm_set_option(EFFECT) inside the dylib */
        NSMenuItem *scan = item(view, @"Scanlines");
        [NSApp sendAction:scan.action to:scan.target from:scan];
        long e = -1; cm_get_(cx, CM_OPT_EFFECT, &e);
        check(e == CM_FX_SCANLINE, "Scanlines -> real cm_set_option(EFFECT=SCANLINE)");

        NSMenuItem *pp = item(sub(view, @"Scaling"), @"Pixel-Perfect");
        [NSApp sendAction:pp.action to:pp.target from:pp];
        long sm = -1; cm_get_(cx, CM_OPT_SCALE_MODE, &sm);
        check(sm == CM_SCALE_PIXEL_PERFECT, "Scaling ▸ Pixel-Perfect -> real SCALE_MODE");

        NSMenuItem *linear = item(sub(view, @"Filter"), @"Linear");
        [NSApp sendAction:linear.action to:linear.target from:linear];
        long flt = -1; cm_get_(cx, CM_OPT_FILTER, &flt);
        check(flt == CM_FILTER_LINEAR, "Filter ▸ Linear -> real FILTER=LINEAR");

        NSMenuItem *fs = item(view, @"Enter Full Screen");
        [NSApp sendAction:fs.action to:fs.target from:fs];
        long fullscreen = -1; cm_get_(cx, CM_OPT_FULLSCREEN, &fullscreen);
        check(fullscreen == 1, "Enter Full Screen -> real FULLSCREEN=1");

        NSMenuItem *retina = item(view, @"Retina / HiDPI");
        [NSApp sendAction:retina.action to:retina.target from:retina];
        long hi = -1; cm_get_(cx, CM_OPT_RETINA, &hi);
        check(hi == 1, "Retina / HiDPI -> real RETINA=1");

        NSMenuItem *dark = item(sub(view, @"Theme"), @"Dark");
        [NSApp sendAction:dark.action to:dark.target from:dark];
        long theme = -1; cm_get_(cx, CM_OPT_THEME, &theme);
        check(theme == CM_THEME_DARK, "Theme ▸ Dark -> real THEME=DARK");

        /* (2b) AROS-facing: Share Clipboard -> cm_set_option(CLIPBOARD_SHARE) ->
         * recorded + surfaced as a CM_EV_SETTING the AROS side pulls */
        NSMenuItem *clip = item(machine, @"Share Clipboard");
        check(clip.state == NSControlStateValueOn, "Share Clipboard menu starts checked");
        [NSApp sendAction:clip.action to:clip.target from:clip];
        cs = -1; cm_get_(cx, CM_OPT_CLIPBOARD_SHARE, &cs);
        check(cs == 0, "Share Clipboard first click -> CLIPBOARD_SHARE=0 recorded");
        n = cm_pump_(cx, ev, 16);
        check(saw_setting(ev, n, CM_OPT_CLIPBOARD_SHARE, 0),
              "Share Clipboard OFF surfaced CM_EV_SETTING (AROS-facing relay)");
        [NSApp sendAction:clip.action to:clip.target from:clip];
        cs = -1; cm_get_(cx, CM_OPT_CLIPBOARD_SHARE, &cs);
        check(cs == 1, "Share Clipboard second click -> CLIPBOARD_SHARE=1 recorded");
        n = cm_pump_(cx, ev, 16);
        check(saw_setting(ev, n, CM_OPT_CLIPBOARD_SHARE, 1),
              "Share Clipboard ON surfaced CM_EV_SETTING (AROS-facing relay)");

        NSMenuItem *reset = item(machine, @"Reset");
        [NSApp sendAction:reset.action to:reset.target from:reset];
        long power = -1; cm_get_(cx, CM_OPT_POWER, &power);
        n = cm_pump_(cx, ev, 16);
        check(power == CM_POWER_RESET && saw_setting(ev, n, CM_OPT_POWER, CM_POWER_RESET),
              "Machine ▸ Reset records and relays CM_OPT_POWER=RESET");

        NSMenuItem *forceQuit = item(item(machine, @"Power").submenu, @"Force Quit");
        [NSApp sendAction:forceQuit.action to:forceQuit.target from:forceQuit];
        power = -1; cm_get_(cx, CM_OPT_POWER, &power);
        n = cm_pump_(cx, ev, 16);
        check(power == CM_POWER_FORCE_QUIT && saw_setting(ev, n, CM_OPT_POWER, CM_POWER_FORCE_QUIT),
              "Power ▸ Force Quit records and relays CM_OPT_POWER=FORCE_QUIT");

        if (cm_set_str_ && cm_get_str_) {
            const char *spec = "Mac:/tmp;WRITE";
            char got[128];
            check(cm_set_str_(cx, CM_OPT_VOLUME_ADD, spec) == 0,
                  "cm_set_option_str(VOLUME_ADD) accepted host volume spec");
            check(cm_get_str_(cx, CM_OPT_VOLUME_ADD, got, sizeof got) == 0 &&
                  strcmp(got, spec) == 0,
                  "cm_get_option_str(VOLUME_ADD) returns the recorded spec");
            n = cm_pump_(cx, ev, 16);
            check(saw_setting(ev, n, CM_OPT_VOLUME_ADD, -1),
                  "VOLUME_ADD surfaced CM_EV_SETTING for AROS-facing relay");
        } else {
            check(0, "cm_set/get_option_str symbols exported");
        }

        /* (3) Settings… opens the schema-driven window generated from settings.json */
        NSMenuItem *settings = item(bar.itemArray.firstObject.submenu, @"Settings…");
        [NSApp sendAction:settings.action to:settings.target from:settings];
        NSWindow *sw = nil;
        for (NSWindow *w in [NSApp windows])
            if ([w.title hasPrefix:@"Macaros Settings"]) { sw = w; break; }
        check(sw != nil, "Settings… opened the schema-driven settings window");
        check(sw && sw.toolbar.items.count >= 4, "settings window generated tab toolbar from schema");
        check(sw && view_has_label(sw.contentView, @"Schema:"),
              "settings window shows where the schema was loaded from (footer)");

        /* (3b) The Media tab: a live device list generated from the schema's
         * "media" control, so the user chooses which physical disk AROS sees. */
        NSToolbarItem *mediaTab = nil;
        for (NSToolbarItem *ti in sw.toolbar.items)
            if ([ti.itemIdentifier isEqualToString:@"Media"]) mediaTab = ti;
        check(mediaTab != nil, "settings window has the Media tab");
        if (mediaTab) {
            [NSApp sendAction:mediaTab.action to:mediaTab.target from:mediaTab];
            check(view_of_class(sw.contentView, [NSTableView class]) != nil,
                  "the Media tab presents the device list");
        }

        /* (3c) Capture Input is a real grab now: turning it on installs the key
         * monitor that sends Command combinations to the guest, and turning it
         * off takes it away again. */
        NSMenuItem *capture = nil;
        for (NSMenuItem *top in bar.itemArray)
            if ([top.submenu itemWithTitle:@"Capture Input"])
                capture = [top.submenu itemWithTitle:@"Capture Input"];
        check(capture != nil, "Machine menu offers Capture Input");
        if (capture) {
            [NSApp sendAction:capture.action to:capture.target from:capture];
            check(capture.state == NSControlStateValueOn, "Capture Input turns on");
            [NSApp sendAction:capture.action to:capture.target from:capture];
            check(capture.state == NSControlStateValueOff, "Capture Input turns off again");
        }

        /* (4) Movie recording: present N frames -> cm__record_frame appends -> probe .mov */
        upload_fn   cm_upload_    = (upload_fn)  dlsym(h, "cm_upload_rect");
        present_fn  cm_present_   = (present_fn) dlsym(h, "cm_present");
        recstart_fn cm_rec_start_ = (recstart_fn)dlsym(h, "cm_record_start");
        recstop_fn  cm_rec_stop_  = (recstop_fn) dlsym(h, "cm_record_stop");
        if (cm_upload_ && cm_present_ && cm_rec_start_ && cm_rec_stop_) {
            const int W = 320, HH = 200, N = 8;
            NSString *mov = [NSTemporaryDirectory() stringByAppendingPathComponent:@"aros-gshell.mov"];
            int rs = cm_rec_start_(cx, mov.UTF8String, 10, 0);
            check(rs == 0, "cm_record_start ok");
            if (rs == 0) {
                uint8_t *fb = (uint8_t *)calloc((size_t)W * HH, 4);
                for (int f = 0; f < N; f++) {
                    uint8_t v = (uint8_t)(f * 30);
                    for (int i = 0; i < W * HH; i++) {
                        fb[i*4] = v; fb[i*4+1] = 0; fb[i*4+2] = (uint8_t)(255 - v); fb[i*4+3] = 255;
                    }
                    cm_upload_(cx, fb, W * 4, 0, 0, W, HH);
                    cm_present_(cx);                 /* each present appends one frame */
                }
                free(fb);
                check(cm_rec_stop_(cx) == 0, "cm_record_stop ok");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:mov] options:nil];
                NSArray<AVAssetTrack *> *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
                double dur = CMTimeGetSeconds(asset.duration);
                CGSize sz = tracks.count ? tracks.firstObject.naturalSize : CGSizeZero;
#pragma clang diagnostic pop
                check(tracks.count >= 1, "movie has a video track");
                check(dur > 0, "movie duration > 0");
                check((int)sz.width == W && (int)sz.height == HH, "movie frame size matches logical W×H");
                printf("    movie: %dx%d, %.2fs, %lu track(s)\n",
                       (int)sz.width, (int)sz.height, dur, (unsigned long)tracks.count);
                [[NSFileManager defaultManager] removeItemAtPath:mov error:NULL];
            }
        }

        cm_close_(cx);
    }

    int ok = (g_fail == 0);
    printf("[GSHELL] %s\n", ok
        ? "PASS — menu installed by the real dylib; actions drive the real cm_* ABI"
        : "FAIL see checks above");
    return ok ? 0 : 1;
}
