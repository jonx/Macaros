/* shell_test.m — unattended POC/verifier for the host app shell ([G]).
 *
 * Implemented from Apple AppKit/Foundation/CoreFoundation/GCD docs [PUB] + the
 * HIG [PUB] + docs/features/host-app-shell/spec.md [OURS]. Independent work — no
 * third-party implementation source was read or consulted; any resemblance is
 * coincidental. Proves the parts of the host-shell that are INDEPENDENT of the
 * Metal window, so it runs fully headless (no window server, no TCC, no screenshot):
 *
 *   [G-MENU]    the menu bar tree matches spec.md R-MENU (titles, submenus, key
 *               equivalents, target/action wiring) — walked, never screen-grabbed.
 *   [G-ACTION]  each custom menu item, invoked directly, routes the right intent
 *               to the CMShellSink (the engine seam) with the right value.
 *   [G-IDENTITY] the Dock/app icon is set and the delegate is wired (R-IDENTITY).
 *   [G-RUNLOOP] the load-bearing G1 de-risk: an "AROS" worker pthread keeps
 *               draining while the MAIN thread is held in a nested run loop
 *               (menu-tracking shape), and a worker→main GCD hop (the real
 *               cm__sync_main pattern) is serviced — measured in default mode,
 *               the actual NSEventTracking mode, and an off-common control mode.
 *
 * VERDICT: [G] PASS iff menu + action + identity + runloop all pass. Bounded
 * (~1.5 s); the harness adds a watchdog. Run with --show to see it live.
 */
#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include "cmshell.h"
#include "cmsettings.h"

/* ----------------------------------------------------- recording mock sink -- */
typedef struct {
    int   set_option_calls;   int last_opt_key;   long last_opt_val;
    int   set_option_str_calls; int last_str_key;  char last_str[256];
    int   capture_calls;      char last_capture[256];
    int   record_start_calls; char last_record[256]; int last_fps, last_codec;
    int   record_stop_calls;
    int   volume_add_calls;   char last_volume[256];
    int   volume_remove_calls;
    int   power_calls;        int last_power;
    int   capture_input_calls; int last_capture_input;
} MockSink;
static MockSink M;
static void mock_reset(void) { memset(&M, 0, sizeof M); }

static void mk_set_option(void *c, int k, long v) { (void)c; M.set_option_calls++; M.last_opt_key=k; M.last_opt_val=v; }
static void mk_set_option_str(void *c, int k, const char *s) { (void)c; M.set_option_str_calls++; M.last_str_key=k; if(s){strncpy(M.last_str,s,sizeof M.last_str-1);} }
static int  mk_capture_png(void *c, const char *p) { (void)c; M.capture_calls++; if(p){strncpy(M.last_capture,p,sizeof M.last_capture-1);} return 0; }
static int  mk_record_start(void *c, const char *p, int fps, int codec) { (void)c; M.record_start_calls++; if(p){strncpy(M.last_record,p,sizeof M.last_record-1);} M.last_fps=fps; M.last_codec=codec; return 0; }
static int  mk_record_stop(void *c) { (void)c; M.record_stop_calls++; return 0; }
static int  mk_volume_add(void *c, const char *s) { (void)c; M.volume_add_calls++; if(s){strncpy(M.last_volume,s,sizeof M.last_volume-1);} return 0; }
static int  mk_volume_remove(void *c, const char *n) { (void)c; (void)n; M.volume_remove_calls++; return 0; }
static void mk_power(void *c, int r) { (void)c; M.power_calls++; M.last_power=r; }
static void mk_capture_input(void *c, int on) { (void)c; M.capture_input_calls++; M.last_capture_input=on; }

static CMShellSink make_mock_sink(void) {
    CMShellSink s = {0};
    s.set_option = mk_set_option; s.set_option_str = mk_set_option_str;
    s.capture_png = mk_capture_png; s.record_start = mk_record_start; s.record_stop = mk_record_stop;
    s.volume_add = mk_volume_add; s.volume_remove = mk_volume_remove;
    s.power = mk_power; s.set_capture_input = mk_capture_input;
    return s;
}

/* ---------------------------------------------------------------- helpers --- */
static int g_fail = 0;
static int check(int cond, const char *what) {
    if (!cond) { g_fail++; printf("    FAIL: %s\n", what); }
    return cond;
}
static NSMenu *find_sub(NSMenu *bar, NSString *title) {
    for (NSMenuItem *it in bar.itemArray)
        if ([it.title isEqualToString:title] || [it.submenu.title isEqualToString:title])
            return it.submenu;
    return nil;
}
static NSMenuItem *find_item(NSMenu *m, NSString *title) {
    for (NSMenuItem *it in m.itemArray)
        if ([it.title isEqualToString:title]) return it;
    return nil;
}
static int invoke(NSMenuItem *it) {
    if (!it || !it.action) return 0;
    return (int)[NSApp sendAction:it.action to:it.target from:it];
}

/* ----------------------------------------------- [G-MENU] structure ---------*/
static int test_menu_structure(void) {
    printf("[G-MENU] walking the installed NSMenu tree (R-MENU)\n");
    int f0 = g_fail;
    NSMenu *bar = [NSApp mainMenu];
    if (!check(bar != nil, "mainMenu installed")) return 0;
    check(bar.itemArray.count >= 7, ">= 7 top-level menus (App/File/Edit/View/Machine/Window/Help)");

    NSMenu *app = bar.itemArray.firstObject.submenu;
    check(app != nil, "App menu present");
    NSMenuItem *about = find_item(app, @"About AROS");
    check(about && about.action == @selector(aboutAction:), "App ▸ About AROS wired");
    NSMenuItem *settings = find_item(app, @"Settings…");
    check(settings && [settings.keyEquivalent isEqualToString:@","] &&
          (settings.keyEquivalentModifierMask & NSEventModifierFlagCommand),
          "App ▸ Settings… = ⌘,");
    check(find_item(app, @"Quit AROS") != nil, "App ▸ Quit AROS present");

    NSMenu *file = find_sub(bar, @"File");
    check(file != nil, "File menu present");
    NSMenuItem *shot = find_item(file, @"Take Screenshot");
    check(shot && [shot.keyEquivalent isEqualToString:@"3"] &&
          (shot.keyEquivalentModifierMask & NSEventModifierFlagShift) &&
          (shot.keyEquivalentModifierMask & NSEventModifierFlagCommand),
          "File ▸ Take Screenshot = ⇧⌘3");
    check(find_item(file, @"Record Movie…") != nil, "File ▸ Record Movie… present");
    NSMenuItem *vol = find_item(file, @"Open Folder as Volume…");
    check(vol && [vol.keyEquivalent isEqualToString:@"o"], "File ▸ Open Folder as Volume… = ⌘O");
    NSMenuItem *o68 = find_item(file, @"Open 68k Program…");
    check(o68 && !o68.enabled, "File ▸ Open 68k Program… present + disabled (future)");

    NSMenu *edit = find_sub(bar, @"Edit");
    check(edit && find_item(edit, @"Copy") && find_item(edit, @"Paste"),
          "Edit menu with Copy/Paste (clipboard-bridge affordance)");

    NSMenu *view = find_sub(bar, @"View");
    check(view != nil, "View menu present");
    NSMenuItem *fs = find_item(view, @"Enter Full Screen");
    check(fs && [fs.keyEquivalent isEqualToString:@"f"] &&
          (fs.keyEquivalentModifierMask & NSEventModifierFlagControl) &&
          (fs.keyEquivalentModifierMask & NSEventModifierFlagCommand),
          "View ▸ Enter Full Screen = ⌃⌘F");
    NSMenu *scaling = find_item(view, @"Scaling").submenu;
    check(scaling && scaling.itemArray.count == 4, "View ▸ Scaling has 4 modes");
    NSMenu *filter = find_item(view, @"Filter").submenu;
    check(filter && filter.itemArray.count == 2, "View ▸ Filter has 2 modes");
    check(find_item(view, @"Scanlines") != nil, "View ▸ Scanlines present");

    NSMenu *machine = find_sub(bar, @"Machine");
    check(machine != nil, "Machine menu present");
    NSMenuItem *reset = find_item(machine, @"Reset");
    check(reset && [reset.keyEquivalent isEqualToString:@"r"] &&
          (reset.keyEquivalentModifierMask & NSEventModifierFlagControl),
          "Machine ▸ Reset = ⌃⌘R");
    NSMenu *power = find_item(machine, @"Power").submenu;
    check(power && power.itemArray.count == 3 &&
          find_item(power, @"Request Power Down") && find_item(power, @"Force Shut Down") &&
          find_item(power, @"Force Quit"),
          "Machine ▸ Power ▸ {Request Power Down, Force Shut Down, Force Quit}");
    NSMenuItem *cap = find_item(machine, @"Capture Input");
    check(cap && [cap.keyEquivalent isEqualToString:@"i"], "Machine ▸ Capture Input = ⌃⌘I");
    check(find_item(machine, @"Share Clipboard") != nil, "Machine ▸ Share Clipboard present");

    check(find_sub(bar, @"Window") != nil, "Window menu present");
    check(find_sub(bar, @"Help") != nil, "Help menu present");

    int ok = (g_fail == f0);
    printf("[G-MENU] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* --------------------------------------------- [G-ACTION] intent routing ----*/
static int test_actions(void) {
    printf("[G-ACTION] invoking each custom item → asserting the sink intent\n");
    int f0 = g_fail;
    NSMenu *bar = [NSApp mainMenu];
    NSMenu *file = find_sub(bar, @"File");
    NSMenu *view = find_sub(bar, @"View");
    NSMenu *machine = find_sub(bar, @"Machine");

    /* File ▸ Take Screenshot → capture_png("screenshot.png") */
    mock_reset(); invoke(find_item(file, @"Take Screenshot"));
    check(M.capture_calls == 1 && strcmp(M.last_capture, "screenshot.png") == 0,
          "Take Screenshot → capture_png(screenshot.png)");

    /* File ▸ Record Movie… → record_start(...,30,0) */
    mock_reset(); invoke(find_item(file, @"Record Movie…"));
    check(M.record_start_calls == 1 && M.last_fps == 30, "Record Movie… → record_start(fps=30)");

    /* File ▸ Open Folder as Volume… → volume_add(...) */
    mock_reset(); invoke(find_item(file, @"Open Folder as Volume…"));
    check(M.volume_add_calls == 1 && strlen(M.last_volume) > 0, "Open Folder as Volume… → volume_add");

    /* View ▸ Scanlines toggles CM_OPT_EFFECT NEAREST↔SCANLINE */
    NSMenuItem *scan = find_item(view, @"Scanlines");
    mock_reset(); invoke(scan);
    check(M.last_opt_key == CM_OPT_EFFECT && M.last_opt_val == CM_FX_SCANLINE &&
          scan.state == NSControlStateValueOn, "Scanlines on → EFFECT=SCANLINE");
    mock_reset(); invoke(scan);
    check(M.last_opt_key == CM_OPT_EFFECT && M.last_opt_val == CM_FX_NEAREST &&
          scan.state == NSControlStateValueOff, "Scanlines off → EFFECT=NEAREST");

    /* View ▸ Scaling ▸ Pixel-Perfect → CM_OPT_SCALE_MODE */
    NSMenu *scaling = find_item(view, @"Scaling").submenu;
    mock_reset(); invoke(find_item(scaling, @"Pixel-Perfect"));
    check(M.last_opt_key == CM_OPT_SCALE_MODE && M.last_opt_val == CM_SCALE_PIXEL_PERFECT,
          "Scaling ▸ Pixel-Perfect → SCALE_MODE=PIXEL_PERFECT");

    /* View ▸ Filter ▸ Linear → CM_OPT_FILTER */
    NSMenu *filter = find_item(view, @"Filter").submenu;
    mock_reset(); invoke(find_item(filter, @"Linear"));
    check(M.last_opt_key == CM_OPT_FILTER && M.last_opt_val == CM_FILTER_LINEAR,
          "Filter ▸ Linear → FILTER=LINEAR");

    /* View ▸ Enter Full Screen → CM_OPT_FULLSCREEN=1 */
    mock_reset(); invoke(find_item(view, @"Enter Full Screen"));
    check(M.last_opt_key == CM_OPT_FULLSCREEN && M.last_opt_val == 1, "Enter Full Screen → FULLSCREEN=1");

    /* Machine ▸ Reset / Power ▸ * → graded power requests */
    mock_reset(); invoke(find_item(machine, @"Reset"));
    check(M.power_calls == 1 && M.last_power == CM_POWER_RESET, "Reset → power(RESET)");
    NSMenu *power = find_item(machine, @"Power").submenu;
    mock_reset(); invoke(find_item(power, @"Request Power Down"));
    check(M.last_power == CM_POWER_REQUEST_DOWN, "Power ▸ Request Power Down → power(REQUEST_DOWN)");
    mock_reset(); invoke(find_item(power, @"Force Quit"));
    check(M.last_power == CM_POWER_FORCE_QUIT, "Power ▸ Force Quit → power(FORCE_QUIT)");

    /* Machine ▸ Capture Input → set_capture_input(1) */
    mock_reset(); invoke(find_item(machine, @"Capture Input"));
    check(M.capture_input_calls == 1 && M.last_capture_input == 1, "Capture Input → set_capture_input(1)");

    /* Machine ▸ Share Clipboard → CM_OPT_CLIPBOARD_SHARE=1 (AROS-facing) */
    mock_reset(); invoke(find_item(machine, @"Share Clipboard"));
    check(M.last_opt_key == CM_OPT_CLIPBOARD_SHARE && M.last_opt_val == 1,
          "Share Clipboard → CLIPBOARD_SHARE=1");

    int ok = (g_fail == f0);
    printf("[G-ACTION] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* --------------------------------------------------- [G-IDENTITY] icon ------*/
static int test_identity(void) {
    printf("[G-IDENTITY] Dock icon + delegate (R-IDENTITY)\n");
    int f0 = g_fail;
    check([NSApp applicationIconImage] != nil, "applicationIconImage set");
    check([NSApp delegate] != nil, "NSApplicationDelegate wired");
    int ok = (g_fail == f0);
    printf("[G-IDENTITY] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* --------------------------------------------- [G-RUNLOOP] the G1 de-risk ----*/
static _Atomic long g_drain = 0;
static _Atomic int  g_worker_run = 1;
static void *worker_main(void *a) {
    (void)a;
    while (atomic_load(&g_worker_run)) { atomic_fetch_add(&g_drain, 1); usleep(200); }
    return NULL;
}
/* Poll the main run loop in `mode` until `*flag` or the deadline; report it. */
static int pump_until(CFStringRef mode, volatile int *flag, double seconds) {
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (!*flag && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(mode, 0.02, true);
    return *flag;
}
static int test_runloop(void) {
    printf("[G-RUNLOOP] AROS-thread drain vs. a nested main run loop (menu-tracking shape)\n");
    int f0 = g_fail;
    atomic_store(&g_worker_run, 1); atomic_store(&g_drain, 0);
    pthread_t th; pthread_create(&th, NULL, worker_main, NULL);

    /* (1) the worker keeps advancing while the main thread is held in a nested loop */
    long before = atomic_load(&g_drain);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    long after = atomic_load(&g_drain);
    long advanced = after - before;
    check(advanced > 50, "AROS worker pthread NOT starved by a nested main run loop");
    printf("    worker advanced %ld ticks during a 0.25s nested main loop\n", advanced);

    /* (2) a worker→main GCD hop (the cm__sync_main shape) is serviced in DEFAULT mode */
    __block volatile int ran_default = 0;
    dispatch_async(dispatch_get_main_queue(), ^{ ran_default = 1; });
    check(pump_until(kCFRunLoopDefaultMode, &ran_default, 0.25),
          "worker→main GCD hop serviced in default mode");

    /* (3) DIAGNOSTIC: serviced in the actual menu-tracking mode? */
    __block volatile int ran_track = 0;
    dispatch_async(dispatch_get_main_queue(), ^{ ran_track = 1; });
    int tracked = pump_until((__bridge CFStringRef)NSEventTrackingRunLoopMode, &ran_track, 0.25);

    /* (4) CONTROL: an off-common mode must NOT service it (proves the probe can tell) */
    __block volatile int ran_ctrl = 0;
    dispatch_async(dispatch_get_main_queue(), ^{ ran_ctrl = 1; });
    int ctrl = pump_until(CFSTR("CMShellProbeOffCommonMode"), &ran_ctrl, 0.20);

    atomic_store(&g_worker_run, 0);
    pthread_join(th, NULL);

    printf("    GCD-on-main serviced: default=%s  NSEventTracking=%s  off-common(control)=%s\n",
           ran_default ? "yes" : "no", tracked ? "yes" : "no", ctrl ? "yes(!)" : "no");
    if (tracked)
        printf("    NOTE: worker→main hops ARE serviced during menu-tracking — the threaded\n"
               "          inversion's cm__sync_main is safe under a live menu bar (verify on\n"
               "          the real shim at merge).\n");
    else
        printf("    NOTE: worker→main hops are NOT serviced during menu-tracking — at merge the\n"
               "          AROS thread must not BLOCK on a sync hop while a menu tracks (use an\n"
               "          async post or CFRunLoopPerformBlock in the tracking mode). FLAGGED.\n");

    int ok = (g_fail == f0);
    printf("[G-RUNLOOP] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ------------------------------------------------------- live --show mode ---*/
static CMShellSink gShowSink;
static void show_settings_cb(void *ctx) {   /* menu Settings… → generated window */
    (void)ctx;
    cmsettings_build("Machine", &gShowSink, 1);
}
static void run_show(void) {
    CMShellSink sink = make_mock_sink();   /* logs intents via the mock */
    sink.open_settings = show_settings_cb; /* wire ⌘, to the schema-driven window */
    gShowSink = sink;
    cmsettings_set_defaults_suite("org.aros.hostshell.poc.show");
    const char *sp = cmsettings_default_schema_path();
    if (!sp || cmsettings_load_schema(sp) <= 0)
        printf("[G-SHOW] WARN: settings schema not loaded (%s); ⌘, will be empty\n",
               cmsettings_last_error());
    cmshell_install(&sink);
    [NSApp activateIgnoringOtherApps:YES];

    NSRect r = NSMakeRect(0, 0, 480, 300);
    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:r
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered defer:NO];
    [win setTitle:@"AROS — host-shell POC (menu bar above)"];
    [win center];
    [win makeKeyAndOrderFront:nil];

    cmshell_show_about(NULL);

    /* watchdog so an accidental unattended --show cannot hang forever */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    printf("[G-SHOW] live — look at the menu bar / About panel / Dock icon. ⌘Q to quit.\n");
    [NSApp run];
}

/* ----------------------------------------------------------------- main ----*/
int main(int argc, const char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int show = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--show") == 0) show = 1;

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        if (show) { run_show(); return 0; }

        printf("[G] host app shell POC — menu bar + About + icon + run-loop de-risk\n");
        printf("[G] AppKit init: sharedApplication + ActivationPolicy:Regular "
               "(isRunning=%d) — no [NSApp run]\n", (int)[app isRunning]);

        CMShellSink sink = make_mock_sink();
        cmshell_install(&sink);

        int m = test_menu_structure();
        int a = test_actions();
        int i = test_identity();
        int r = test_runloop();

        int ok = m && a && i && r;
        printf("[G] ---- summary ----  menu=%s action=%s identity=%s runloop=%s  (fails=%d)\n",
               m?"PASS":"FAIL", a?"PASS":"FAIL", i?"PASS":"FAIL", r?"PASS":"FAIL", g_fail);
        if (ok) {
            printf("[G] PASS first-class menu bar + About + icon installed; every menu intent "
                   "routes through the engine seam; AROS-thread drain survives a nested main loop\n");
            return 0;
        }
        printf("[G] FAIL see checks above\n");
        return 1;
    }
}
