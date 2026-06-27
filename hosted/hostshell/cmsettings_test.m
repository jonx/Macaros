/* cmsettings_test.m — unattended verifier for the schema-driven settings ([GS]).
 *
 * Implemented from Apple AppKit/Foundation docs [PUB] + the project spec [OURS].
 * Independent work — no third-party implementation source was read or consulted;
 * any resemblance is coincidental. Headless (no window server needed): builds
 * views off-screen and asserts the model + generation, never a screenshot.
 *
 *   [GS-SCHEMA]  the descriptor table is well-formed (unique ids, popups have
 *                choices, sliders have a range, every setting binds a store key).
 *   [GS-CONF]    the config-file store edits ONE keyword IN PLACE — module /
 *                arguments / comment lines are preserved; a new key appends one
 *                line. (The "map GUI settings to part of the config file" core.)
 *   [GS-MODEL]   load/set round-trips through the store AND fires the right apply
 *                (host-acted vs AROS-facing vs boot-only) via the CMShellSink seam.
 *   [GS-GEN]     the window generated for a tier has exactly the tabs + controls
 *                the schema declares (no per-control code).
 *
 * VERDICT: [GS] PASS iff all four pass. Run with --show to see a generated window.
 */
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cmsettings.h"

/* ---- recording mock sink (the engine seam) ---- */
typedef struct {
    int opt_calls; int last_key; long last_val;
    int str_calls; int last_str_key; char last_str[256];
} Mock;
static Mock MK;
static void mk_reset(void) { memset(&MK, 0, sizeof MK); }
static void mk_opt(void *c, int k, long v) { (void)c; MK.opt_calls++; MK.last_key=k; MK.last_val=v; }
static void mk_optstr(void *c, int k, const char *s) { (void)c; MK.str_calls++; MK.last_str_key=k; if(s) strncpy(MK.last_str, s, sizeof MK.last_str-1); }
static CMShellSink mock_sink(void) {
    CMShellSink s = {0}; s.set_option = mk_opt; s.set_option_str = mk_optstr; return s;
}

/* ---- helpers ---- */
static int g_fail = 0;
static int check(int cond, const char *what) {
    if (!cond) { g_fail++; printf("    FAIL: %s\n", what); }
    return cond;
}
static const CMSetting *byid(const char *id) {
    int n; const CMSetting *a = cmsettings_all(&n);
    for (int i = 0; i < n; i++) if (strcmp(a[i].ident, id) == 0) return &a[i];
    return NULL;
}
static NSString *gFixture = nil;
static NSString *const kConfComment = @"# aros-host.conf host settings (GUI-edited)";
static NSString *write_fixture(void) {
    NSString *p = [NSTemporaryDirectory() stringByAppendingPathComponent:@"aros-host.conf"];
    NSString *body = [NSString stringWithFormat:@"%@\nmemory 64\nclipboard 0\n", kConfComment];
    [body writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    return p;
}
static NSArray<NSString *> *read_lines(NSString *p) {
    NSString *t = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:NULL];
    NSMutableArray *a = [[t componentsSeparatedByString:@"\n"] mutableCopy];
    if (a.count && [a.lastObject length] == 0) [a removeLastObject];
    return a;
}
static int has(NSArray<NSString *> *ls, NSString *s) {
    for (NSString *l in ls) if ([l isEqualToString:s]) return 1;
    return 0;
}

/* ---- [GS-SCHEMA] ---- */
static int test_schema(void) {
    printf("[GS-SCHEMA] descriptor table well-formed\n");
    int f0 = g_fail, n; const CMSetting *a = cmsettings_all(&n);
    check(n > 0, "schema non-empty");
    for (int i = 0; i < n; i++) {
        const CMSetting *s = &a[i];
        check(s->ident && s->label && s->storeKey && s->tier && s->tab, s->ident ?: "row missing fields");
        if (s->ctl == CMCtlPopup) check(s->choices && s->nchoices > 0, s->ident);
        if (s->ctl == CMCtlSlider) check(s->minV < s->maxV, s->ident);
        for (int j = i + 1; j < n; j++)
            check(strcmp(s->ident, a[j].ident) != 0, "duplicate ident");
    }
    int ok = (g_fail == f0);
    printf("[GS-SCHEMA] %s (%d settings)\n", ok ? "PASS" : "FAIL", n);
    return ok;
}

/* ---- [GS-CONF] the in-place config-file edit ---- */
static int test_conf(void) {
    printf("[GS-CONF] config-file store edits one keyword in place\n");
    int f0 = g_fail;
    CMShellSink sink = mock_sink();
    gFixture = write_fixture();
    cmsettings_set_conf_path(gFixture.UTF8String);

    const CMSetting *mem = byid("system.memory");
    mk_reset();
    cmsettings_set_num(mem, 256, &sink);
    NSArray *ls = read_lines(gFixture);
    check(ls.count == 3, "line count unchanged after editing memory");
    check(has(ls, @"memory 256"), "memory rewritten in place");
    check(has(ls, kConfComment), "comment preserved");
    check(has(ls, @"clipboard 0"), "unrelated host key preserved");
    check(MK.opt_calls == 0 && MK.str_calls == 0, "memory is boot-only — no live apply");
    check(cmsettings_load_num(mem) == 256, "memory reloads from conf as 256");

    const CMSetting *folder = byid("sharing.folder");
    mk_reset();
    cmsettings_set_str(folder, "/tmp/share", &sink);
    ls = read_lines(gFixture);
    check(ls.count == 4, "a new key appends exactly one line");
    check(has(ls, @"hostvolume /tmp/share"), "new key appended");
    check(has(ls, @"memory 256"), "earlier edit still present after append");
    check(MK.str_calls == 1 && MK.last_str_key == CM_OPT_VOLUME_ADD &&
          strcmp(MK.last_str, "/tmp/share") == 0, "folder applies as AROS-facing string");
    check(strcmp(cmsettings_load_str(folder), "/tmp/share") == 0, "folder reloads from conf");

    int ok = (g_fail == f0);
    printf("[GS-CONF] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ---- [GS-MODEL] persist + apply routing ---- */
static int test_model(void) {
    printf("[GS-MODEL] load/set persist to the store and fire the right apply\n");
    int f0 = g_fail;
    CMShellSink sink = mock_sink();

    const CMSetting *sc = byid("display.scaleMode");        /* host-acted */
    mk_reset(); cmsettings_set_num(sc, CM_SCALE_PIXEL_PERFECT, &sink);
    check(MK.opt_calls == 1 && MK.last_key == CM_OPT_SCALE_MODE &&
          MK.last_val == CM_SCALE_PIXEL_PERFECT, "scaleMode → host-acted cm_set_option");
    check(cmsettings_load_num(sc) == CM_SCALE_PIXEL_PERFECT, "scaleMode persisted to defaults");

    const CMSetting *clip = byid("sharing.clipboard");      /* AROS-facing */
    mk_reset(); cmsettings_set_num(clip, 1, &sink);
    check(MK.opt_calls == 1 && MK.last_key == CM_OPT_CLIPBOARD_SHARE && MK.last_val == 1,
          "clipboard → AROS-facing cm_set_option");

    const CMSetting *vol = byid("sound.volume");            /* AROS-facing */
    mk_reset(); cmsettings_set_num(vol, 50, &sink);
    check(MK.opt_calls == 1 && MK.last_key == CM_OPT_AUDIO_VOLUME && MK.last_val == 50,
          "volume → AROS-facing cm_set_option");
    check(cmsettings_load_num(vol) == 50, "volume persisted to defaults");

    int ok = (g_fail == f0);
    printf("[GS-MODEL] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ---- [GS-GEN] the window is generated from the schema ---- */
static int expected_for(const char *tier, int *tabsOut) {
    int n; const CMSetting *a = cmsettings_all(&n);
    NSMutableArray *tabs = [NSMutableArray array]; int controls = 0;
    for (int i = 0; i < n; i++) if (strcmp(a[i].tier, tier) == 0) {
        controls++; NSString *t = @(a[i].tab);
        if (![tabs containsObject:t]) [tabs addObject:t];
    }
    if (tabsOut) *tabsOut = (int)tabs.count;
    return controls;
}
static int test_gen(void) {
    printf("[GS-GEN] generated window matches the schema\n");
    int f0 = g_fail;
    CMShellSink sink = mock_sink();

    int et, ec = expected_for("Machine", &et);
    CMSettingsBuild b = cmsettings_build("Machine", &sink, 0);
    check(b.tabs == et && b.controls == ec && b.window != NULL,
          "Machine window: tabs+controls match schema");
    printf("    Machine: tabs=%d/%d controls=%d/%d window=%s\n",
           b.tabs, et, b.controls, ec, b.window ? "ok" : "NULL");

    int et2, ec2 = expected_for("App", &et2);
    CMSettingsBuild b2 = cmsettings_build("App", &sink, 0);
    check(b2.tabs == et2 && b2.controls == ec2 && b2.window != NULL,
          "App window: tabs+controls match schema");
    printf("    App:     tabs=%d/%d controls=%d/%d window=%s\n",
           b2.tabs, et2, b2.controls, ec2, b2.window ? "ok" : "NULL");

    int ok = (g_fail == f0);
    printf("[GS-GEN] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ---- [GS-VALIDATE] the loader rejects malformed schema (the cost of data-driven) ---- */
static int load_str_schema(NSString *json) {
    NSString *p = [NSTemporaryDirectory() stringByAppendingPathComponent:@"aros-bad-schema.json"];
    [json writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    int rc = cmsettings_load_schema(p.UTF8String);
    [[NSFileManager defaultManager] removeItemAtPath:p error:NULL];
    return rc;
}
static int test_validate(void) {
    printf("[GS-VALIDATE] loader rejects malformed schema with a clear error\n");
    int f0 = g_fail;
    check(load_str_schema(@"{ not json") < 0, "bad JSON rejected");
    check(load_str_schema(@"{\"settings\":[{\"ident\":\"x\",\"tier\":\"App\",\"tab\":\"T\","
                           "\"label\":\"L\",\"store\":\"defaults\",\"key\":\"k\"}]}") < 0,
          "missing 'control' rejected");
    check(load_str_schema(@"{\"settings\":[{\"ident\":\"x\",\"tier\":\"App\",\"tab\":\"T\",\"label\":\"L\","
                           "\"control\":\"bogus\",\"store\":\"defaults\",\"key\":\"k\"}]}") < 0,
          "bad control kind rejected");
    check(strlen(cmsettings_last_error()) > 0, "an error message is reported");
    printf("    last error e.g.: \"%s\"\n", cmsettings_last_error());
    int good = cmsettings_load_schema(cmsettings_default_schema_path());   /* restore */
    check(good > 0, "good schema reloads after rejections");
    int ok = (g_fail == f0);
    printf("[GS-VALIDATE] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static NSString *const kSuite = @"org.aros.hostshell.poc.test";

int main(int argc, const char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int show = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--show") == 0) show = 1;

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        cmsettings_set_defaults_suite(kSuite.UTF8String);   /* throwaway — cleaned below */

        const char *sp = cmsettings_default_schema_path();
        int nloaded = sp ? cmsettings_load_schema(sp) : -1;
        if (nloaded <= 0) {
            printf("[GS] FAIL load schema (%s): %s\n",
                   sp ?: "settings.json not found next to binary", cmsettings_last_error());
            return 1;
        }
        printf("[GS] loaded %d settings from %s\n", nloaded, sp);

        if (show) {
            cmsettings_set_conf_path(write_fixture().UTF8String);
            CMShellSink sink = mock_sink();
            cmsettings_build("Machine", &sink, 1);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
            printf("[GS-SHOW] live — the generated Machine settings window. ⌘Q to quit.\n");
            [NSApp run];
            return 0;
        }

        printf("[GS] schema-driven settings POC — schema, config-file store, model, generation\n");
        int v = test_validate();
        int s = test_schema();
        int c = test_conf();
        int m = test_model();
        int g = test_gen();

        /* cleanup: do not pollute the user's defaults domain or leave the fixture */
        [[NSUserDefaults standardUserDefaults] removePersistentDomainForName:kSuite];
        if (gFixture) [[NSFileManager defaultManager] removeItemAtPath:gFixture error:NULL];

        int ok = v && s && c && m && g;
        printf("[GS] ---- summary ----  validate=%s schema=%s conf=%s model=%s gen=%s  (fails=%d)\n",
               v?"PASS":"FAIL", s?"PASS":"FAIL", c?"PASS":"FAIL", m?"PASS":"FAIL", g?"PASS":"FAIL", g_fail);
        if (ok) {
            printf("[GS] PASS one schema drives the window, persistence, and apply; the config-file "
                   "store edits a keyword in place without disturbing the rest of the file\n");
            return 0;
        }
        printf("[GS] FAIL see checks above\n");
        return 1;
    }
}
