/* cocoametal_settings_schema.m — schema-driven settings (production).
 *
 * Replaces the hand-coded cocoametal_settings.m panel in the dylib. ONE declarative
 * schema — a JSON DATA FILE (settings.json) loaded + validated at runtime — drives a
 * GENERATED toolbar-tab window; there is no per-setting C code. Merged from the
 * verified POC in hosted/hostshell/ (cmsettings.*), adapted to the cocoametal world:
 * it uses cocoametal.h directly and applies via the cm_* ABI (no test sink).
 *
 * Provides the strong overrides of the weak stubs in cocoametal.m:
 *   cm__open_settings_appkit  — load schema (lazy) + build the window (the menu's
 *                               Settings… / cm_open_settings reaches here)
 *   cm__apply_persisted_options — re-apply persisted host-acted display options at
 *                               cm_open (schema-independent: the cocoametal.* keys).
 *
 * Each setting binds (1) presentation, (2) a store — NSUserDefaults (cocoametal.*)
 * or the dedicated host config file aros-host.conf, edited IN PLACE — and (3) an
 * apply: hostOption (live cm_set_option), arosOption/arosOptionStr (AROS-facing,
 * relayed as CM_EV_SETTING), or bootOnly (conf only, next launch).
 *
 * Clean-room: Apple AppKit/Foundation docs [PUB] + the project spec [OURS]. No GPL.
 */
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "cocoametal.h"

/* ---- the descriptor vocabulary (was cmsettings.h in the POC) ---- */
typedef enum { CMCtlCheckbox, CMCtlPopup, CMCtlSlider, CMCtlText, CMCtlPath } CMCtl;
typedef enum { CMStoreDefaults, CMStoreConf } CMStoreKind;
typedef enum { CMApplyNone, CMApplyHostOption, CMApplyArosOption,
               CMApplyArosOptionStr, CMApplyBootOnly } CMApply;
typedef struct { const char *label; long value; } CMChoice;
typedef struct {
    const char *ident, *tier, *tab, *label;
    CMCtl ctl; int valueIsStr;
    CMStoreKind store; const char *storeKey;
    long defNum; const char *defStr;
    const CMChoice *choices; int nchoices;
    long minV, maxV, stepV;
    CMApply apply; int applyKey;
} CMSetting;

/* ===================================================== the schema (loaded) == */
static CMSetting *gSettings = NULL;
static int        gCount    = 0;
static char       gErr[256] = "";

static char *dupc(NSString *s) { return s ? strdup([s UTF8String]) : NULL; }
static int   failf(NSString *m) { snprintf(gErr, sizeof gErr, "%s", m.UTF8String ?: "error"); return -1; }

static long const_num(id v, long deflt) {
    if ([v isKindOfClass:[NSNumber class]]) return [v longValue];
    if (![v isKindOfClass:[NSString class]]) return deflt;
    static NSDictionary *K = nil;
    if (!K) K = @{
        @"CM_OPT_EFFECT":@(CM_OPT_EFFECT), @"CM_OPT_SCALE_MODE":@(CM_OPT_SCALE_MODE),
        @"CM_OPT_FULLSCREEN":@(CM_OPT_FULLSCREEN), @"CM_OPT_FILTER":@(CM_OPT_FILTER),
        @"CM_OPT_RETINA":@(CM_OPT_RETINA), @"CM_OPT_AUDIO_VOLUME":@(CM_OPT_AUDIO_VOLUME),
        @"CM_OPT_CLIPBOARD_SHARE":@(CM_OPT_CLIPBOARD_SHARE), @"CM_OPT_AUDIO_DEVICE":@(CM_OPT_AUDIO_DEVICE),
        @"CM_OPT_VOLUME_ADD":@(CM_OPT_VOLUME_ADD), @"CM_OPT_VOLUME_REMOVE":@(CM_OPT_VOLUME_REMOVE),
        @"CM_OPT_POWER":@(CM_OPT_POWER),
        @"CM_FX_NEAREST":@(CM_FX_NEAREST), @"CM_FX_SCANLINE":@(CM_FX_SCANLINE),
        @"CM_SCALE_FIT":@(CM_SCALE_FIT), @"CM_SCALE_INTEGER_NEAREST":@(CM_SCALE_INTEGER_NEAREST),
        @"CM_SCALE_PIXEL_PERFECT":@(CM_SCALE_PIXEL_PERFECT), @"CM_SCALE_ASPECT_FIT":@(CM_SCALE_ASPECT_FIT),
        @"CM_FILTER_NEAREST":@(CM_FILTER_NEAREST), @"CM_FILTER_LINEAR":@(CM_FILTER_LINEAR),
    };
    NSNumber *n = K[v]; return n ? n.longValue : deflt;
}
static int ctl_from(NSString *s, CMCtl *out) {
    NSDictionary *M = @{@"checkbox":@(CMCtlCheckbox), @"popup":@(CMCtlPopup), @"slider":@(CMCtlSlider),
                        @"text":@(CMCtlText), @"path":@(CMCtlPath)};
    NSNumber *n = M[s ?: @""]; if (!n) return 0; *out = (CMCtl)n.intValue; return 1;
}
static int store_from(NSString *s, CMStoreKind *out) {
    if ([s isEqual:@"defaults"]) { *out = CMStoreDefaults; return 1; }
    if ([s isEqual:@"conf"])     { *out = CMStoreConf;     return 1; }
    return 0;
}
static int apply_from(NSString *s, CMApply *out) {
    NSDictionary *M = @{@"none":@(CMApplyNone), @"hostOption":@(CMApplyHostOption),
                        @"arosOption":@(CMApplyArosOption), @"arosOptionStr":@(CMApplyArosOptionStr),
                        @"bootOnly":@(CMApplyBootOnly)};
    NSNumber *n = M[s ?: @"none"]; if (!n) return 0; *out = (CMApply)n.intValue; return 1;
}

static int load_schema(const char *pathC) {
    gErr[0] = 0;
    if (!pathC) return failf(@"no schema path");
    NSData *data = [NSData dataWithContentsOfFile:@(pathC)];
    if (!data) return failf([NSString stringWithFormat:@"cannot read schema: %s", pathC]);
    NSError *e = nil;
    id root = [NSJSONSerialization JSONObjectWithData:data options:0 error:&e];
    if (!root) return failf([NSString stringWithFormat:@"JSON parse error: %@", e.localizedDescription]);
    NSArray *arr = [root isKindOfClass:[NSDictionary class]] ? root[@"settings"] : root;
    if (![arr isKindOfClass:[NSArray class]]) return failf(@"schema has no 'settings' array");

    CMSetting *out = calloc(arr.count, sizeof(CMSetting));
    int k = 0;
    for (NSDictionary *d in arr) {
        if (![d isKindOfClass:[NSDictionary class]]) { free(out); return failf(@"a settings entry is not an object"); }
        CMSetting s; memset(&s, 0, sizeof s);
        NSString *ident=d[@"ident"], *tier=d[@"tier"], *tab=d[@"tab"], *label=d[@"label"],
                 *ctl=d[@"control"], *store=d[@"store"], *key=d[@"key"];
        if (!ident||!tier||!tab||!label||!ctl||!store||!key) { free(out);
            return failf([NSString stringWithFormat:@"entry '%@' missing a required field", ident ?: @"?"]); }
        if (!ctl_from(ctl, &s.ctl))       { free(out); return failf([NSString stringWithFormat:@"%@: bad control '%@'", ident, ctl]); }
        if (!store_from(store, &s.store)) { free(out); return failf([NSString stringWithFormat:@"%@: bad store '%@'", ident, store]); }
        if (!apply_from(d[@"apply"], &s.apply)) { free(out); return failf([NSString stringWithFormat:@"%@: bad apply", ident]); }
        s.ident=dupc(ident); s.tier=dupc(tier); s.tab=dupc(tab); s.label=dupc(label); s.storeKey=dupc(key);
        s.valueIsStr = (s.ctl == CMCtlPath || s.ctl == CMCtlText) ? 1 : 0;
        if (d[@"applyKey"]) s.applyKey = (int)const_num(d[@"applyKey"], 0);
        if (s.valueIsStr) s.defStr = dupc(d[@"default"] ?: @"");
        else              s.defNum = const_num(d[@"default"], 0);
        s.minV = [d[@"min"] longValue]; s.maxV = [d[@"max"] longValue]; s.stepV = [d[@"step"] longValue];
        NSArray *ch = d[@"choices"];
        if ([ch isKindOfClass:[NSArray class]] && ch.count) {
            CMChoice *cc = calloc(ch.count, sizeof(CMChoice)); int j = 0;
            for (NSDictionary *co in ch) { cc[j].label = dupc(co[@"label"]); cc[j].value = const_num(co[@"value"], 0); j++; }
            s.choices = cc; s.nchoices = (int)ch.count;
        }
        if (s.ctl == CMCtlPopup && s.nchoices <= 0) { free(out); return failf([NSString stringWithFormat:@"%@: popup needs choices", ident]); }
        if (s.ctl == CMCtlSlider && !(s.minV < s.maxV)) { free(out); return failf([NSString stringWithFormat:@"%@: slider needs min<max", ident]); }
        for (int i = 0; i < k; i++) if (strcmp(out[i].ident, s.ident) == 0) { free(out); return failf([NSString stringWithFormat:@"duplicate ident '%@'", ident]); }
        out[k++] = s;
    }
    free(gSettings); gSettings = out; gCount = k;
    return k;
}

/* ======================================================= the stores ========= */
static NSString *gConfPath = nil;
static NSString *gSchemaPath = nil;   /* where settings.json was loaded from (shown in the window) */
static void set_conf_path(NSString *p) { gConfPath = p; }
static NSUserDefaults *du(void) { return [NSUserDefaults standardUserDefaults]; }

static NSMutableArray<NSString *> *conf_lines(void) {
    NSString *txt = gConfPath ? [NSString stringWithContentsOfFile:gConfPath
                                                          encoding:NSUTF8StringEncoding error:NULL] : nil;
    if (!txt) return [NSMutableArray array];
    NSMutableArray *a = [[txt componentsSeparatedByString:@"\n"] mutableCopy];
    if (a.count && [a.lastObject length] == 0) [a removeLastObject];
    return a;
}
static NSString *conf_token(NSString *line) {
    NSString *t = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (t.length == 0 || [t hasPrefix:@"#"]) return nil;
    NSRange sp = [t rangeOfCharacterFromSet:[NSCharacterSet whitespaceCharacterSet]];
    return sp.location == NSNotFound ? t : [t substringToIndex:sp.location];
}
static NSString *conf_get(NSString *key) {
    for (NSString *line in conf_lines()) {
        if (![conf_token(line) isEqualToString:key]) continue;
        NSString *t = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSRange sp = [t rangeOfCharacterFromSet:[NSCharacterSet whitespaceCharacterSet]];
        return sp.location == NSNotFound ? @""
            : [[t substringFromIndex:sp.location] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    }
    return nil;
}
static void conf_set(NSString *key, NSString *val) {
    if (!gConfPath) return;
    NSMutableArray<NSString *> *lines = conf_lines();
    NSString *repl = [NSString stringWithFormat:@"%@ %@", key, val];
    BOOL found = NO;
    for (NSUInteger i = 0; i < lines.count; i++)
        if ([conf_token(lines[i]) isEqualToString:key]) { lines[i] = repl; found = YES; break; }
    if (!found) [lines addObject:repl];
    NSString *out = [[lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"];
    [[NSFileManager defaultManager] createDirectoryAtPath:[gConfPath stringByDeletingLastPathComponent]
                              withIntermediateDirectories:YES attributes:nil error:NULL];
    [out writeToFile:gConfPath atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

/* ======================================================= the model ========= */
static void apply_setting(const CMSetting *s, long num, const char *str, CMContext *cx) {
    switch (s->apply) {
    case CMApplyHostOption:
    case CMApplyArosOption:    cm_set_option(cx, s->applyKey, num); break;
    case CMApplyArosOptionStr: cm_set_option_str(cx, s->applyKey, str ?: ""); break;
    case CMApplyNone:
    case CMApplyBootOnly:      break;
    }
}
static long load_num(const CMSetting *s) {
    if (s->store == CMStoreConf) { NSString *v = conf_get(@(s->storeKey)); return v ? (long)[v integerValue] : s->defNum; }
    NSString *k = @(s->storeKey);
    return [du() objectForKey:k] ? (long)[du() integerForKey:k] : s->defNum;
}
static const char *load_str(const CMSetting *s) {
    NSString *v = (s->store == CMStoreConf) ? conf_get(@(s->storeKey)) : [du() stringForKey:@(s->storeKey)];
    if (!v) v = s->defStr ? @(s->defStr) : @"";
    return [v UTF8String];
}
static void set_num(const CMSetting *s, long v, CMContext *cx) {
    if (s->store == CMStoreConf) conf_set(@(s->storeKey), [@(v) stringValue]);
    else                         [du() setInteger:v forKey:@(s->storeKey)];
    apply_setting(s, v, NULL, cx);
}
static void set_str(const CMSetting *s, const char *v, CMContext *cx) {
    NSString *val = v ? @(v) : @"";
    if (s->store == CMStoreConf) conf_set(@(s->storeKey), val);
    else                         [du() setObject:val forKey:@(s->storeKey)];
    apply_setting(s, 0, v, cx);
}

/* ================================================ the generated window ====== */
/* A footer shown on every tab so the user can see WHERE the schema + config were
 * loaded from (they can come from several places: AROS_SETTINGS_SCHEMA, next to the
 * dylib, or ~/Library/Application Support/AROS). Abbreviated with ~, full in tooltip. */
static NSString *abbrev_path(NSString *p) {
    return p ? [p stringByAbbreviatingWithTildeInPath] : @"(built-in)";
}
static NSString *footer_text(void) {
    return [NSString stringWithFormat:@"Schema: %@      ·      Config: %@",
            abbrev_path(gSchemaPath), abbrev_path(gConfPath)];
}
static NSString *footer_tooltip(void) {
    return [NSString stringWithFormat:@"settings.json loaded from:\n  %@\n\naros-host.conf:\n  %@",
            gSchemaPath ?: @"(none)", gConfPath ?: @"(none)"];
}

@interface CMSettingsWC : NSObject <NSToolbarDelegate>
@property (nonatomic, assign) CMContext *cx;
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSMutableArray<NSString *> *tabs;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSView *> *tabViews;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, NSTextField *> *pathFields;
@end

@implementation CMSettingsWC

- (NSControl *)controlFor:(const CMSetting *)s index:(int)idx {
    switch (s->ctl) {
        case CMCtlCheckbox: {
            NSButton *b = [NSButton checkboxWithTitle:@"" target:self action:@selector(changed:)];
            b.state = load_num(s) ? NSControlStateValueOn : NSControlStateValueOff;
            b.tag = idx; return b;
        }
        case CMCtlPopup: {
            NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            for (int i = 0; i < s->nchoices; i++) {
                [p addItemWithTitle:@(s->choices[i].label)];
                p.lastItem.tag = (NSInteger)s->choices[i].value;
            }
            long cur = load_num(s);
            for (NSMenuItem *it in p.itemArray) if (it.tag == cur) { [p selectItem:it]; break; }
            p.target = self; p.action = @selector(changed:); p.tag = idx; return p;
        }
        case CMCtlSlider: {
            NSSlider *sl = [NSSlider sliderWithValue:load_num(s) minValue:s->minV maxValue:s->maxV
                                              target:self action:@selector(changed:)];
            sl.numberOfTickMarks = (s->stepV > 0) ? (int)((s->maxV - s->minV) / s->stepV) + 1 : 0;
            sl.allowsTickMarkValuesOnly = (s->stepV > 0); sl.tag = idx;
            [sl.widthAnchor constraintGreaterThanOrEqualToConstant:180].active = YES;
            sl.toolTip = [NSString stringWithFormat:@"%ld–%ld", s->minV, s->maxV];
            return sl;
        }
        case CMCtlText:
        case CMCtlPath: {
            NSTextField *t = [NSTextField textFieldWithString:@(load_str(s))];
            t.target = self; t.action = @selector(changed:); t.tag = idx;
            [t.widthAnchor constraintGreaterThanOrEqualToConstant:220].active = YES;
            if (s->ctl == CMCtlPath) self.pathFields[@(idx)] = t;
            return t;
        }
    }
    return nil;
}
- (NSButton *)browseButtonForIndex:(int)idx {
    NSButton *b = [NSButton buttonWithTitle:@"Browse…" target:self action:@selector(browse:)];
    b.tag = idx; return b;
}
- (void)changed:(id)sender {
    int idx = (int)[(NSControl *)sender tag];
    const CMSetting *s = &gSettings[idx];
    if (s->valueIsStr) {
        set_str(s, [[(NSControl *)sender stringValue] UTF8String], _cx);
    } else {
        long v;
        if (s->ctl == CMCtlCheckbox)   v = ([(NSButton *)sender state] == NSControlStateValueOn);
        else if (s->ctl == CMCtlPopup) v = [[(NSPopUpButton *)sender selectedItem] tag];
        else                           v = [(NSControl *)sender integerValue];
        set_num(s, v, _cx);
    }
}
- (void)browse:(id)sender {
    int idx = (int)[(NSButton *)sender tag];
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.canChooseDirectories = YES; op.canChooseFiles = NO;
    if ([op runModal] == NSModalResponseOK) {
        NSTextField *f = self.pathFields[@(idx)];
        f.stringValue = op.URL.path;
        set_str(&gSettings[idx], [op.URL.path UTF8String], _cx);
    }
}
- (NSView *)gridForTab:(NSString *)tab {
    NSMutableArray *rows = [NSMutableArray array];
    for (int i = 0; i < gCount; i++) {
        const CMSetting *s = &gSettings[i];
        if (![@(s->tab) isEqualToString:tab]) continue;
        NSTextField *lbl = [NSTextField labelWithString:[NSString stringWithFormat:@"%s:", s->label]];
        NSControl *ctl = [self controlFor:s index:i];
        NSView *right = ctl;
        if (s->ctl == CMCtlPath) {
            NSStackView *h = [NSStackView stackViewWithViews:@[ctl, [self browseButtonForIndex:i]]];
            h.orientation = NSUserInterfaceLayoutOrientationHorizontal; h.spacing = 6;
            right = h;
        }
        [rows addObject:@[lbl, right]];
    }
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 10; grid.columnSpacing = 12;
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    [[grid columnAtIndex:0] setXPlacement:NSGridCellPlacementTrailing];
    NSView *pad = [[NSView alloc] initWithFrame:NSZeroRect];
    [pad addSubview:grid];
    /* footer: where the schema + config were loaded from */
    NSTextField *foot = [NSTextField labelWithString:footer_text()];
    foot.font = [NSFont systemFontOfSize:9];
    foot.textColor = [NSColor secondaryLabelColor];
    foot.lineBreakMode = NSLineBreakByTruncatingMiddle;
    foot.toolTip = footer_tooltip();
    foot.translatesAutoresizingMaskIntoConstraints = NO;
    [pad addSubview:foot];
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor constraintEqualToAnchor:pad.topAnchor constant:20],
        [grid.leadingAnchor constraintEqualToAnchor:pad.leadingAnchor constant:20],
        [grid.trailingAnchor constraintEqualToAnchor:pad.trailingAnchor constant:-20],
        [foot.topAnchor constraintEqualToAnchor:grid.bottomAnchor constant:16],
        [foot.leadingAnchor constraintEqualToAnchor:pad.leadingAnchor constant:20],
        [foot.trailingAnchor constraintLessThanOrEqualToAnchor:pad.trailingAnchor constant:-20],
        [foot.bottomAnchor constraintEqualToAnchor:pad.bottomAnchor constant:-12],
    ]];
    return pad;
}
- (void)selectTab:(NSString *)tab {
    NSView *v = self.tabViews[tab]; if (!v) return;
    self.window.contentView = v;
    self.window.title = [NSString stringWithFormat:@"AROS Settings — %@", tab];
    [self.window setContentSize:v.fittingSize];
    self.window.toolbar.selectedItemIdentifier = tab;
}
- (void)switchTab:(id)sender { [self selectTab:[(NSToolbarItem *)sender itemIdentifier]]; }
- (NSToolbarItem *)toolbar:(NSToolbar *)tb itemForItemIdentifier:(NSToolbarItemIdentifier)ident
 willBeInsertedIntoToolbar:(BOOL)flag {
    NSToolbarItem *it = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    it.label = ident; it.target = self; it.action = @selector(switchTab:);
    it.image = [NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:nil];
    return it;
}
- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)tb { return self.tabs; }
- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)tb { return self.tabs; }
- (NSArray<NSToolbarItemIdentifier> *)toolbarSelectableItemIdentifiers:(NSToolbar *)tb { return self.tabs; }
@end

static CMSettingsWC *gWC = nil;   /* static-strong: keeps the window alive */

static int build_window(CMContext *cx) {
    if (gWC.window) { [gWC.window makeKeyAndOrderFront:nil]; return 0; }   /* re-front */
    CMSettingsWC *wc = [CMSettingsWC new];
    wc.cx = cx;
    wc.tabs = [NSMutableArray array];
    wc.tabViews = [NSMutableDictionary dictionary];
    wc.pathFields = [NSMutableDictionary dictionary];
    for (int i = 0; i < gCount; i++) {
        NSString *tab = @(gSettings[i].tab);
        if (![wc.tabs containsObject:tab]) { [wc.tabs addObject:tab]; wc.tabViews[tab] = [wc gridForTab:tab]; }
    }
    if (wc.tabs.count == 0) return 1;

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 420, 240)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                    backing:NSBackingStoreBuffered defer:NO];
    NSToolbar *tb = [[NSToolbar alloc] initWithIdentifier:@"AROSSettings"];
    tb.delegate = wc; tb.displayMode = NSToolbarDisplayModeIconAndLabel; tb.allowsUserCustomization = NO;
    win.toolbar = tb;
    if (@available(macOS 11.0, *)) win.toolbarStyle = NSWindowToolbarStylePreference;
    win.releasedWhenClosed = NO;
    wc.window = win;
    [wc selectTab:wc.tabs.firstObject];
    [win center];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    gWC = wc;
    return 0;
}

/* ------------------------------------------- schema + conf path resolution -- */
static NSString *dylib_dir(void) {
    Dl_info info;
    if (dladdr((const void *)&build_window, &info) && info.dli_fname)
        return [@(info.dli_fname) stringByDeletingLastPathComponent];
    return nil;
}
static const char *resolve_schema_path(void) {
    NSFileManager *fm = [NSFileManager defaultManager];
    const char *env = getenv("AROS_SETTINGS_SCHEMA");           /* 1. explicit (the .app sets this) */
    if (env && *env) return env;
    NSString *dir = dylib_dir();                                /* 2. next to the dylib (~/lib, Frameworks, build/) */
    if (dir) {
        NSString *p = [dir stringByAppendingPathComponent:@"settings.json"];
        if ([fm fileExistsAtPath:p]) return [p UTF8String];
    }
    NSArray *as = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (as.firstObject) {                                       /* 3. ~/Library/Application Support/AROS/ */
        NSString *p = [[as.firstObject stringByAppendingPathComponent:@"AROS"]
                          stringByAppendingPathComponent:@"settings.json"];
        if ([fm fileExistsAtPath:p]) return [p UTF8String];
    }
    return NULL;
}
static NSString *resolve_conf_path(void) {
    const char *env = getenv("AROS_HOST_CONF");
    if (env && *env) return @(env);
    NSArray *as = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *base = as.firstObject ?: NSHomeDirectory();
    return [base stringByAppendingPathComponent:@"AROS/aros-host.conf"];
}

/* ------------------------------------------- the strong weak-stub overrides - */
int cm__open_settings_appkit(CMContext *cx) {
    @autoreleasepool {
        if (gCount == 0) {                                  /* lazy schema load on first open */
            set_conf_path(resolve_conf_path());
            const char *sp = resolve_schema_path();
            int n = sp ? load_schema(sp) : -1;
            if (n <= 0) { NSLog(@"[shell] settings schema not loaded: %s", gErr); return 1; }
            gSchemaPath = @(sp);          /* record the resolved source for the window footer */
        }
        return build_window(cx);
    }
}

/* Re-apply persisted host-acted display options at cm_open (schema-independent so
 * cm_open stays robust even before settings.json is located). Same cocoametal.*
 * keys the schema's `defaults` store uses, so the schema window and this agree. */
void cm__apply_persisted_options(CMContext *cx) {
    @autoreleasepool {
        NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
        if ([d objectForKey:@"cocoametal.effect"])     cm_set_option(cx, CM_OPT_EFFECT,    [d integerForKey:@"cocoametal.effect"]);
        if ([d objectForKey:@"cocoametal.scaleMode"])  cm_set_option(cx, CM_OPT_SCALE_MODE, [d integerForKey:@"cocoametal.scaleMode"]);
        if ([d objectForKey:@"cocoametal.filter"])     cm_set_option(cx, CM_OPT_FILTER,     [d integerForKey:@"cocoametal.filter"]);
        if ([d objectForKey:@"cocoametal.fullscreen"]) cm_set_option(cx, CM_OPT_FULLSCREEN, [d integerForKey:@"cocoametal.fullscreen"]);
    }
}
