/* cocoametal_media_ui.h — the Media tab's view (see cocoametal_media_ui.m). */
#ifndef COCOAMETAL_MEDIA_UI_H
#define COCOAMETAL_MEDIA_UI_H
#import <AppKit/AppKit.h>

/* A live list of the removable media the Mac has, with a per-medium share
 * control. Owned by the caller's view hierarchy; the controller behind it is
 * kept alive for the process's lifetime so hotplug keeps updating it. */
NSView *cm_media_panel(void);

/* Re-read the media list into an already-built panel (a grant may have been
 * made somewhere else). Safe to call when no panel exists. */
void cm_media_panel_refresh(void);

#endif /* COCOAMETAL_MEDIA_UI_H */
