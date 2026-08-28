/* cocoametal_media_ui.m — the Media tab of the Settings window.
 *
 * Lists the removable media the Mac currently has (never its own disk) and lets
 * the user say, per medium, whether AROS sees nothing, may read, or may read and
 * write. The list follows the hardware: plugging a stick in or pulling it out
 * updates it while the window is open.
 *
 * The decisions themselves live in cocoametal_media.m; this file is only the
 * presentation. Sources: Apple AppKit documentation [PUB].
 */
#import <AppKit/AppKit.h>

#include "cocoametal_media.h"
#include "cocoametal_media_ui.h"

#define CM_MEDIA_MAX 32

@interface CMMediaController : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, strong) NSTableView *table;
@property (nonatomic, strong) NSTextField *status;
@property (nonatomic, assign) int count;
@end

@implementation CMMediaController {
    CMMediaItem _items[CM_MEDIA_MAX];
}

static CMMediaController *gMedia = nil;      /* static-strong: outlives the tab view */

static NSString *human_size(unsigned long long bytes) {
    static const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
    double size = (double)bytes;
    int i = 0;
    while (size >= 1024.0 && i < 4) { size /= 1024.0; i++; }
    return [NSString stringWithFormat:@"%.1f %s", size, unit[i]];
}

- (void)reload {
    self.count = cm_media_scan(_items, CM_MEDIA_MAX);
    [self.table reloadData];
    if (self.count == 0)
        self.status.stringValue = @"No removable media attached.";
    else if (![self.status.stringValue hasPrefix:@"Could not"])
        self.status.stringValue = @"Granting a disk unmounts it on the Mac first, so both "
                                   "systems never write it at once.";
}

/* DiskArbitration tells us a medium appeared or disappeared. */
static void media_changed(void *ctx) {
    CMMediaController *self = (__bridge CMMediaController *)ctx;
    [self reload];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { (void)table; return self.count; }

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table;
    if (row < 0 || row >= self.count) return nil;
    const CMMediaItem *it = &_items[row];

    if ([column.identifier isEqualToString:@"access"]) {
        NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [popup addItemWithTitle:@"Not shared"];   popup.lastItem.tag = CM_MEDIA_NONE;
        [popup addItemWithTitle:@"Read only"];    popup.lastItem.tag = CM_MEDIA_READONLY;
        [popup addItemWithTitle:@"Read & write"]; popup.lastItem.tag = CM_MEDIA_READWRITE;
        for (NSMenuItem *item in popup.itemArray)
            if (item.tag == it->grant) { [popup selectItem:item]; break; }
        if (!it->writable)
            [popup itemAtIndex:2].enabled = NO;   /* the medium itself refuses writes */
        popup.enabled = it->supported != 0;
        popup.toolTip = it->supported ? nil
            : [NSString stringWithFormat:@"AROS has no handler for %s volumes", it->fs];
        popup.target = self; popup.action = @selector(accessChanged:); popup.tag = row;
        popup.bezelStyle = NSBezelStyleRounded;
        return popup;
    }

    NSString *text;
    if ([column.identifier isEqualToString:@"volume"])
        text = [NSString stringWithFormat:@"%s", it->label[0] ? it->label : it->bsd];
    else if ([column.identifier isEqualToString:@"where"])
        text = it->grant ? [NSString stringWithFormat:@"AROS %s:", it->aros]
                         : (it->mounted ? @"in the Finder" : @"not mounted");
    else if ([column.identifier isEqualToString:@"size"])
        text = human_size(it->size);
    else
        text = @(it->fs);

    NSTextField *label = [NSTextField labelWithString:text];
    label.font = [NSFont systemFontOfSize:11];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if ([column.identifier isEqualToString:@"volume"])
        label.toolTip = [NSString stringWithFormat:@"/dev/%s", it->bsd];
    else if (!it->supported)
        label.textColor = [NSColor secondaryLabelColor];
    return label;
}

- (void)accessChanged:(id)sender {
    NSPopUpButton *popup = sender;
    NSInteger row = popup.tag;
    if (row < 0 || row >= self.count) return;
    char err[192] = "";
    int wanted = (int)popup.selectedTag;
    if (cm_media_set_grant(_items[row].bsd, wanted, err, sizeof err) != 0) {
        self.status.stringValue = [NSString stringWithFormat:@"Could not share %s: %s",
                                   _items[row].label[0] ? _items[row].label : _items[row].bsd,
                                   err[0] ? err : "unknown error"];
    } else if (wanted == CM_MEDIA_NONE) {
        self.status.stringValue = @"Returned to the Mac.";
    } else {
        self.status.stringValue = (wanted == CM_MEDIA_READWRITE)
            ? @"Shared with AROS. It may write to this disk."
            : @"Shared with AROS, read only.";
    }
    [self reload];
}
@end

void cm_media_panel_refresh(void) {
    [gMedia reload];
}

NSView *cm_media_panel(void) {
    CMMediaController *ctl = gMedia ?: [CMMediaController new];
    gMedia = ctl;

    NSTableView *table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    struct { NSString *ident, *title; CGFloat width; } cols[] = {
        { @"volume", @"Volume",  150 },
        { @"size",   @"Size",     70 },
        { @"format", @"Format",   60 },
        { @"where",  @"Where",   110 },
        { @"access", @"Share",   130 },
    };
    for (unsigned i = 0; i < sizeof cols / sizeof cols[0]; i++) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:cols[i].ident];
        c.title = cols[i].title; c.width = cols[i].width;
        [table addTableColumn:c];
    }
    table.dataSource = ctl; table.delegate = ctl;
    table.usesAlternatingRowBackgroundColors = YES;
    table.rowHeight = 24;
    table.allowsColumnReordering = NO;
    table.style = NSTableViewStyleInset;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView = table;
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *status = [NSTextField labelWithString:@""];
    status.font = [NSFont systemFontOfSize:10];
    status.textColor = [NSColor secondaryLabelColor];
    status.lineBreakMode = NSLineBreakByTruncatingTail;
    status.translatesAutoresizingMaskIntoConstraints = NO;

    ctl.table = table; ctl.status = status;
    [ctl reload];
    cm_media_watch(media_changed, (__bridge void *)ctl);

    NSView *panel = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 560, 260)];
    [panel addSubview:scroll];
    [panel addSubview:status];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:panel.topAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:panel.trailingAnchor],
        [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:180],
        [status.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:8],
        [status.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor],
        [status.trailingAnchor constraintEqualToAnchor:panel.trailingAnchor],
        [status.bottomAnchor constraintEqualToAnchor:panel.bottomAnchor],
        [panel.widthAnchor constraintGreaterThanOrEqualToConstant:540],
    ]];
    return panel;
}
