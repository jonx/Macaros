/* prefs_test.m — [PREFS] every setting reaches something, including the ones
 * changed from outside this process.
 *
 * Drives the REAL dylib the way the app does: open a context, put values in the
 * stores the settings window writes (NSUserDefaults for app-level choices, the
 * shared aros-host.conf for machine-level ones), and check what the machine
 * actually ends up doing — the live display options, the file the guest watches
 * for its keyboard layout, and the app-shell choices that are read where they
 * are used.
 *
 * The config file half is the interesting one: it is written here by a plain
 * file write, exactly as `aros-ctl media`, another Macaros, or a text editor
 * would, and the running process must notice and follow it.
 */
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <stdio.h>

#include "cocoametal.h"

static int failures = 0;

static void ck(int ok, const char *what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) failures++;
}

/* The hand-pumped run loop the app itself uses; dispatch sources fire here. */
static void pump(double seconds) {
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + seconds;
    while (CFAbsoluteTimeGetCurrent() < end)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
}

typedef CMContext *(*open_fn)(int, int, const CMPixelDesc *, const char *);
typedef void (*close_fn)(CMContext *);
typedef int  (*getopt_fn)(CMContext *, int, long *);
typedef int  (*setopt_fn)(CMContext *, int, long);
typedef void (*reload_fn)(CMContext *);
typedef void (*watch_fn)(CMContext *);

int main(void) {
    @autoreleasepool {
        NSString *work = [NSTemporaryDirectory() stringByAppendingPathComponent:@"cm-prefs-test"];
        NSString *share = [work stringByAppendingPathComponent:@"share"];
        NSString *conf = [work stringByAppendingPathComponent:@"aros-host.conf"];
        NSFileManager *fm = [NSFileManager defaultManager];
        [fm removeItemAtPath:work error:NULL];
        [fm createDirectoryAtPath:share withIntermediateDirectories:YES attributes:nil error:NULL];
        [@"memory 512\nkeymap pc105_f\n" writeToFile:conf atomically:YES
                                            encoding:NSUTF8StringEncoding error:NULL];
        setenv("AROS_HOST_CONF", conf.UTF8String, 1);
        setenv("AROS_HOST_VOLUME",
               [NSString stringWithFormat:@"MacRW:%@;WRITE", share].UTF8String, 1);

        void *h = dlopen("build/cocoametal.dylib", RTLD_NOW);
        if (!h) { printf("[PREFS] FAIL dlopen: %s\n", dlerror()); return 1; }
        open_fn   cm_open_   = (open_fn)  dlsym(h, "cm_open");
        close_fn  cm_close_  = (close_fn) dlsym(h, "cm_close");
        getopt_fn cm_get_    = (getopt_fn)dlsym(h, "cm_get_option");
        setopt_fn cm_set_    = (setopt_fn)dlsym(h, "cm_set_option");
        reload_fn cm_reload_ = (reload_fn)dlsym(h, "cm__prefs_reload");
        watch_fn  cm_watch_  = (watch_fn) dlsym(h, "cm__prefs_watch");
        ck(cm_open_ && cm_close_ && cm_get_ && cm_set_ && cm_reload_ && cm_watch_,
           "the dylib exports the settings entry points");
        if (failures) { printf("[PREFS] FAIL\n"); return 1; }

        NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
        for (NSString *k in @[@"cocoametal.filter", @"cocoametal.scaleMode",
                              @"cocoametal.effect", @"cocoametal.theme",
                              @"captures.movieFps", @"captures.movieCodec",
                              @"general.showDockIcon", @"general.confirmQuit",
                              @"input.releaseHotkey", @"input.autoCaptureFullscreen",
                              @"sharing.clipboard", @"sound.volume"])
            [d removeObjectForKey:k];

        /* The display settings are stored, then applied when the display opens:
         * a value chosen in an earlier session is in force in this one. */
        [d setInteger:CM_FILTER_LINEAR forKey:@"cocoametal.filter"];
        [d setInteger:CM_SCALE_PIXEL_PERFECT forKey:@"cocoametal.scaleMode"];
        [d setInteger:1 forKey:@"cocoametal.effect"];
        [d synchronize];

        CMPixelDesc fmt = { .bytesPerPixel = 4,
            .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
            .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
            .redMask = 0x00FF0000, .alphaMask = 0xFF000000 };
        CMContext *cx = cm_open_(320, 200, &fmt, "AROS [PREFS]");
        if (!cx) { printf("[PREFS] FAIL cm_open\n"); return 1; }
        pump(0.3);

        long v = 0;
        ck(cm_get_(cx, CM_OPT_FILTER, &v) == 0 && v == CM_FILTER_LINEAR,
           "a stored display filter is in force at start-up");
        ck(cm_get_(cx, CM_OPT_SCALE_MODE, &v) == 0 && v == CM_SCALE_PIXEL_PERFECT,
           "a stored scaling mode is in force at start-up");
        ck(cm_get_(cx, CM_OPT_EFFECT, &v) == 0 && v == CM_FX_SCANLINE,
           "a stored screen effect is in force at start-up");

        /* The keyboard layout is machine state the guest applies to itself, so
         * the choice is left where the guest's watcher reads it. */
        NSString *keymapFile = [share stringByAppendingPathComponent:@".macaros-keymap"];
        [fm removeItemAtPath:keymapFile error:NULL];
        cm_watch_(cx);
        cm_reload_(cx);
        pump(0.2);
        NSString *chosen = [[NSString stringWithContentsOfFile:keymapFile
                                                      encoding:NSUTF8StringEncoding error:NULL]
                               stringByTrimmingCharactersInSet:
                                   [NSCharacterSet whitespaceAndNewlineCharacterSet]];
        ck([chosen isEqualToString:@"pc105_f"],
           "the keyboard layout is handed to the guest");

        /* Now the point of the exercise: someone else edits the config file. */
        [@"memory 512\nkeymap pc105_d\n" writeToFile:conf atomically:YES
                                            encoding:NSUTF8StringEncoding error:NULL];
        for (int i = 0; i < 50; i++) {          /* the watcher fires on the run loop */
            pump(0.1);
            chosen = [[NSString stringWithContentsOfFile:keymapFile
                                                encoding:NSUTF8StringEncoding error:NULL]
                         stringByTrimmingCharactersInSet:
                             [NSCharacterSet whitespaceAndNewlineCharacterSet]];
            if ([chosen isEqualToString:@"pc105_d"]) break;
        }
        ck([chosen isEqualToString:@"pc105_d"],
           "a change made to the config file outside the app is noticed and applied");

        /* And a stored value is re-applied to the live display on the same pass,
         * so the window and the machine cannot drift apart. */
        cm_set_(cx, CM_OPT_FILTER, CM_FILTER_NEAREST);
        cm_reload_(cx);
        pump(0.1);
        ck(cm_get_(cx, CM_OPT_FILTER, &v) == 0 && v == CM_FILTER_LINEAR,
           "reloading puts the live display back in step with the stores");

        /* App-shell choices are read where they are used, so the check is that
         * the store answers, not that something was pushed anywhere. */
        [d setInteger:60 forKey:@"captures.movieFps"];
        [d setInteger:1 forKey:@"captures.movieCodec"];
        [d setBool:NO forKey:@"general.showDockIcon"];
        [d synchronize];
        cm_reload_(cx);
        pump(0.2);
        ck([NSApp activationPolicy] == NSApplicationActivationPolicyAccessory,
           "turning the Dock icon off takes effect immediately");
        [d setBool:YES forKey:@"general.showDockIcon"];
        [d synchronize];
        cm_reload_(cx);
        pump(0.2);
        ck([NSApp activationPolicy] == NSApplicationActivationPolicyRegular,
           "and turning it back on restores the Dock icon");

        cm_close_(cx);
        for (NSString *k in @[@"cocoametal.filter", @"cocoametal.scaleMode",
                              @"cocoametal.effect", @"captures.movieFps",
                              @"captures.movieCodec", @"general.showDockIcon"])
            [d removeObjectForKey:k];
        [d synchronize];
        [fm removeItemAtPath:work error:NULL];
    }
    printf("[PREFS] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
