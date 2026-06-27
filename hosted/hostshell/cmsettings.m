/* cmsettings.m — schema-driven settings: schema loader, stores, model, window.
 *
 * See cmsettings.h. Pure AppKit/Foundation [PUB]; no AROS/Metal. The schema is a
 * JSON DATA FILE (settings.json) loaded + validated at runtime; the whole window
 * is GENERATED from it — there is no per-setting C code.
 */
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cmsettings.h"

/* ===================================================== the schema (loaded) ==
 * Parsed from settings.json at runtime into a heap array. Strings are strdup'd
 * and live for the process (a one-shot load). Add a setting = add a JSON entry —
 * no C change. */

static CMSetting *gSettings = NULL;
static int        gCount    = 0;
static char       gErr[256] = "";

const CMSetting *cmsettings_all(int *count) { if (count) *count = gCount; return gSettings; }
const char      *cmsettings_last_error(void) { return gErr; }

const char *cmsettings_default_schema_path(void) {
    NSString *p = [[NSBundle mainBundle] pathForResource:@"settings" ofType:@"json"];
    return p ? [p UTF8String] : NULL;   /* the .app Resources path at merge */
}

static char *dupc(NSString *s) { return s ? strdup([s UTF8String]) : NULL; }
static int   failf(NSString *m) { snprintf(gErr, sizeof gErr, "%s", m.UTF8String ?: "error"); return -1; }

/* "value"/"default"/"applyKey" may be a JSON number OR a named CM_* constant */
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

int cmsettings_load_schema(const char *pathC) {
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
    free(gSettings);                 /* replace any prior schema (one-shot; prior strdups leak) */
    gSettings = out; gCount = k;
    return k;
}

/* ======================================================= the stores ========
 * Two backends. The conf store is the load-bearing one: it edits a single
 * keyword in a line-oriented `keyword value` file IN PLACE, preserving every
 * other line and the order (the AROSBootstrap.conf grammar). */

static NSString *gConfPath = nil;
static NSString *gSuite    = nil;

void cmsettings_set_conf_path(const char *p)      { gConfPath = p ? @(p) : nil; }
void cmsettings_set_defaults_suite(const char *s) { gSuite    = s ? @(s) : nil; }

static NSUserDefaults *du(void) {
    return gSuite ? [[NSUserDefaults alloc] initWithSuiteName:gSuite]
                  : [NSUserDefaults standardUserDefaults];
}

static NSMutableArray<NSString *> *conf_lines(void) {
    NSString *txt = gConfPath ? [NSString stringWithContentsOfFile:gConfPath
                                                          encoding:NSUTF8StringEncoding error:NULL]
                              : nil;
    if (!txt) return [NSMutableArray array];
    NSMutableArray *a = [[txt componentsSeparatedByString:@"\n"] mutableCopy];
    /* drop a single trailing empty element from a final newline */
    if (a.count && [a.lastObject length] == 0) [a removeLastObject];
    return a;
}

/* first whitespace-delimited token, or nil for a comment/blank line */
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
            : [[t substringFromIndex:sp.location]
                  stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    }
    return nil;
}

static void conf_set(NSString *key, NSString *val) {
    if (!gConfPath) return;
    NSMutableArray<NSString *> *lines = conf_lines();
    NSString *repl = [NSString stringWithFormat:@"%@ %@", key, val];
    BOOL found = NO;
    for (NSUInteger i = 0; i < lines.count; i++) {
        if ([conf_token(lines[i]) isEqualToString:key]) { lines[i] = repl; found = YES; break; }
    }
    if (!found) [lines addObject:repl];                 /* append, don't disturb others */
    NSString *out = [[lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"];
    [out writeToFile:gConfPath atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

/* ======================================================= the model =========*/

static void cmsettings_apply(const CMSetting *s, long num, const char *str,
                             const CMShellSink *sink) {
    if (!sink) return;
    switch (s->apply) {
        case CMApplyHostOption:
        case CMApplyArosOption:
            if (sink->set_option) sink->set_option(sink->ctx, s->applyKey, num);
            break;
        case CMApplyArosOptionStr:
            if (sink->set_option_str) sink->set_option_str(sink->ctx, s->applyKey, str ?: "");
            break;
        case CMApplyNone:
        case CMApplyBootOnly:
            break;   /* persist only (boot-only takes effect next launch) */
    }
}

long cmsettings_load_num(const CMSetting *s) {
    if (s->store == CMStoreConf) {
        NSString *v = conf_get(@(s->storeKey));
        return v ? (long)[v integerValue] : s->defNum;
    }
    NSString *k = @(s->storeKey);
    return [du() objectForKey:k] ? (long)[du() integerForKey:k] : s->defNum;
}

const char *cmsettings_load_str(const CMSetting *s) {
    NSString *v = (s->store == CMStoreConf) ? conf_get(@(s->storeKey))
                                            : [du() stringForKey:@(s->storeKey)];
    if (!v) v = s->defStr ? @(s->defStr) : @"";
    return [v UTF8String];   /* valid until the current autorelease pool drains */
}

void cmsettings_set_num(const CMSetting *s, long v, const CMShellSink *sink) {
    if (s->store == CMStoreConf) conf_set(@(s->storeKey), [@(v) stringValue]);
    else                         [du() setInteger:v forKey:@(s->storeKey)];
    cmsettings_apply(s, v, NULL, sink);
}

void cmsettings_set_str(const CMSetting *s, const char *v, const CMShellSink *sink) {
    NSString *val = v ? @(v) : @"";
    if (s->store == CMStoreConf) conf_set(@(s->storeKey), val);
    else                         [du() setObject:val forKey:@(s->storeKey)];
    cmsettings_apply(s, 0, v, sink);
}

/* ================================================ the generated window ======
 * Walks SETTINGS[] for the tier, groups by tab, and builds a toolbar-tab window
 * (HIG settings pattern) — one control per descriptor, wired to the model. */

@interface CMSettingsWC : NSObject <NSToolbarDelegate>
@property (nonatomic) CMShellSink sink;
@property (nonatomic) BOOL hasSink;
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSMutableArray<NSString *> *tabs;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSView *> *tabViews;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, NSTextField *> *pathFields;
@end

@implementation CMSettingsWC

- (const CMShellSink *)sinkPtr { return _hasSink ? &_sink : NULL; }

- (NSControl *)controlFor:(const CMSetting *)s index:(int)idx {
    switch (s->ctl) {
        case CMCtlCheckbox: {
            NSButton *b = [NSButton checkboxWithTitle:@"" target:self action:@selector(changed:)];
            b.state = cmsettings_load_num(s) ? NSControlStateValueOn : NSControlStateValueOff;
            b.tag = idx; return b;
        }
        case CMCtlPopup: {
            NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            for (int i = 0; i < s->nchoices; i++) {
                [p addItemWithTitle:@(s->choices[i].label)];
                p.lastItem.tag = (NSInteger)s->choices[i].value;
            }
            long cur = cmsettings_load_num(s);
            for (NSMenuItem *it in p.itemArray) if (it.tag == cur) { [p selectItem:it]; break; }
            p.target = self; p.action = @selector(changed:); p.tag = idx; return p;
        }
        case CMCtlSlider: {
            NSSlider *sl = [NSSlider sliderWithValue:cmsettings_load_num(s)
                                            minValue:s->minV maxValue:s->maxV
                                              target:self action:@selector(changed:)];
            sl.numberOfTickMarks = (s->stepV > 0) ? (int)((s->maxV - s->minV) / s->stepV) + 1 : 0;
            sl.allowsTickMarkValuesOnly = (s->stepV > 0);
            sl.tag = idx;
            [sl.widthAnchor constraintGreaterThanOrEqualToConstant:180].active = YES;
            sl.toolTip = [NSString stringWithFormat:@"%ld–%ld", s->minV, s->maxV];
            return sl;
        }
        case CMCtlText: {
            NSTextField *t = [NSTextField textFieldWithString:@(cmsettings_load_str(s))];
            t.target = self; t.action = @selector(changed:); t.tag = idx;
            [t.widthAnchor constraintGreaterThanOrEqualToConstant:220].active = YES;
            return t;
        }
        case CMCtlPath: {  /* the text field is the value-carrying control */
            NSTextField *t = [NSTextField textFieldWithString:@(cmsettings_load_str(s))];
            t.target = self; t.action = @selector(changed:); t.tag = idx;
            [t.widthAnchor constraintGreaterThanOrEqualToConstant:220].active = YES;
            self.pathFields[@(idx)] = t;
            return t;
        }
    }
    return nil;
}

/* the path "Browse…" button (live mode); set next to the field in the grid */
- (NSButton *)browseButtonForIndex:(int)idx {
    NSButton *b = [NSButton buttonWithTitle:@"Browse…" target:self action:@selector(browse:)];
    b.tag = idx; return b;
}

- (void)changed:(id)sender {
    int idx = (int)[(NSControl *)sender tag];
    const CMSetting *s = &gSettings[idx];
    if (s->valueIsStr) {
        cmsettings_set_str(s, [[(NSControl *)sender stringValue] UTF8String], [self sinkPtr]);
    } else {
        long v;
        if (s->ctl == CMCtlCheckbox)   v = ([(NSButton *)sender state] == NSControlStateValueOn);
        else if (s->ctl == CMCtlPopup) v = [[(NSPopUpButton *)sender selectedItem] tag];
        else                           v = [(NSControl *)sender integerValue];
        cmsettings_set_num(s, v, [self sinkPtr]);
    }
}

- (void)browse:(id)sender {
    int idx = (int)[(NSButton *)sender tag];
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.canChooseDirectories = YES; op.canChooseFiles = NO;
    if ([op runModal] == NSModalResponseOK) {
        NSTextField *f = self.pathFields[@(idx)];
        f.stringValue = op.URL.path;
        cmsettings_set_str(&gSettings[idx], [op.URL.path UTF8String], [self sinkPtr]);
    }
}

- (NSView *)gridForTab:(NSString *)tab tier:(NSString *)tier {
    int n = 0; const CMSetting *all = cmsettings_all(&n);
    NSMutableArray *rows = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        const CMSetting *s = &all[i];
        if (![@(s->tier) isEqualToString:tier] || ![@(s->tab) isEqualToString:tab]) continue;
        NSTextField *lbl = [NSTextField labelWithString:[NSString stringWithFormat:@"%s:", s->label]];
        NSControl *ctl = [self controlFor:s index:i];
        NSView *right = ctl;
        if (s->ctl == CMCtlPath) {   /* field + Browse… */
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
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor constraintEqualToAnchor:pad.topAnchor constant:20],
        [grid.leadingAnchor constraintEqualToAnchor:pad.leadingAnchor constant:20],
        [grid.trailingAnchor constraintEqualToAnchor:pad.trailingAnchor constant:-20],
        [grid.bottomAnchor constraintEqualToAnchor:pad.bottomAnchor constant:-20],
    ]];
    return pad;
}

- (void)selectTab:(NSString *)tab {
    NSView *v = self.tabViews[tab];
    if (!v) return;
    self.window.contentView = v;
    self.window.title = [NSString stringWithFormat:@"AROS Settings — %@", tab];
    [self.window setContentSize:v.fittingSize];
    self.window.toolbar.selectedItemIdentifier = tab;
}
- (void)switchTab:(id)sender { [self selectTab:[(NSToolbarItem *)sender itemIdentifier]]; }

/* NSToolbarDelegate — one selectable item per tab */
- (NSToolbarItem *)toolbar:(NSToolbar *)tb itemForItemIdentifier:(NSToolbarItemIdentifier)id
 willBeInsertedIntoToolbar:(BOOL)flag {
    NSToolbarItem *it = [[NSToolbarItem alloc] initWithItemIdentifier:id];
    it.label = id; it.target = self; it.action = @selector(switchTab:);
    it.image = [NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:nil];
    return it;
}
- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)tb { return self.tabs; }
- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)tb { return self.tabs; }
- (NSArray<NSToolbarItemIdentifier> *)toolbarSelectableItemIdentifiers:(NSToolbar *)tb { return self.tabs; }
@end

static CMSettingsWC *gWC = nil;   /* static-strong; keeps the window alive */

CMSettingsBuild cmsettings_build(const char *tierC, const CMShellSink *sink, int show) {
    CMSettingsBuild r = {0, 0, NULL};
    NSString *tier = @(tierC);

    CMSettingsWC *wc = [CMSettingsWC new];
    if (sink) { wc.sink = *sink; wc.hasSink = YES; }
    wc.tabs = [NSMutableArray array];
    wc.tabViews = [NSMutableDictionary dictionary];
    wc.pathFields = [NSMutableDictionary dictionary];

    int n = 0; const CMSetting *all = cmsettings_all(&n);
    for (int i = 0; i < n; i++) {
        const CMSetting *s = &all[i];
        if (![@(s->tier) isEqualToString:tier]) continue;
        r.controls++;
        NSString *tab = @(s->tab);
        if (![wc.tabs containsObject:tab]) {
            [wc.tabs addObject:tab];
            wc.tabViews[tab] = [wc gridForTab:tab tier:tier];
        }
    }
    r.tabs = (int)wc.tabs.count;
    if (r.tabs == 0) return r;

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 420, 240)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                    backing:NSBackingStoreBuffered defer:NO];
    NSToolbar *tb = [[NSToolbar alloc] initWithIdentifier:
                        [NSString stringWithFormat:@"AROSSettings.%@", tier]];
    tb.delegate = wc; tb.displayMode = NSToolbarDisplayModeIconAndLabel;
    tb.allowsUserCustomization = NO;
    win.toolbar = tb;
    if (@available(macOS 11.0, *)) win.toolbarStyle = NSWindowToolbarStylePreference;
    wc.window = win;
    [wc selectTab:wc.tabs.firstObject];
    win.releasedWhenClosed = NO;
    [win center];

    if (show) { [win makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; }
    gWC = wc;            /* retain */
    r.window = (__bridge void *)win;
    return r;
}
