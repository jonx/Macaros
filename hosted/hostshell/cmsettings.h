/* cmsettings.h — schema-driven settings for the host app shell.
 *
 * One declarative table (CMSetting[]) is the single source of truth. From it we
 * GENERATE the settings window, drive PERSISTENCE (into a pluggable store), and
 * route APPLY (to the engine via the CMShellSink). Add a setting = add one row;
 * the GUI / save-load / apply all follow. This is the "map GUI settings to part
 * of the config files" system: a setting bound to the `conf` store edits ONE
 * keyword in a line-oriented config file IN PLACE, leaving every other line
 * (module/arguments/comments) untouched.
 *
 * POC stage (hosted/hostshell) — pure AppKit + Foundation [PUB], no AROS/Metal.
 * Clean-room: Apple docs + the project's own spec, no GPL source. See README.md.
 */
#ifndef CMSETTINGS_H
#define CMSETTINGS_H

#include "cmshell.h"   /* CMShellSink + the CM_OPT_* / CM_* enums */

#ifdef __cplusplus
extern "C" {
#endif

/* --- the descriptor vocabulary --- */
typedef enum { CMCtlCheckbox, CMCtlPopup, CMCtlSlider, CMCtlText, CMCtlPath } CMCtl;
typedef enum { CMStoreDefaults, CMStoreConf } CMStoreKind;   /* where it persists */
typedef enum { CMApplyNone,            /* persist only (host pref)              */
               CMApplyHostOption,      /* live cm_set_option (presentation)     */
               CMApplyArosOption,      /* AROS-facing cm_set_option (relayed)   */
               CMApplyArosOptionStr,   /* AROS-facing cm_set_option_str         */
               CMApplyBootOnly         /* conf only; takes effect next boot     */
             } CMApply;

typedef struct { const char *label; long value; } CMChoice;

/* One row = one setting, fully described. Unused fields stay 0/NULL. */
typedef struct CMSetting {
    const char *ident;        /* stable id, e.g. "display.scaleMode"            */
    const char *tier;         /* "App" (global ⌘,) or "Machine" (per-instance)  */
    const char *tab;          /* pane: "General","Display","System",...         */
    const char *label;        /* UI label                                       */
    CMCtl       ctl;
    int         valueIsStr;   /* 0 = numeric (bool/int/enum), 1 = string         */
    CMStoreKind store;
    const char *storeKey;     /* key in the store ("cocoametal.scaleMode","memory") */
    long        defNum;       /* default (numeric)                              */
    const char *defStr;       /* default (string)                               */
    const CMChoice *choices;  /* popup options                                  */
    int         nchoices;
    long        minV, maxV, stepV;  /* slider range                            */
    CMApply     apply;
    int         applyKey;     /* cm option key for apply (CM_OPT_*)             */
} CMSetting;

/* --- schema loading (data-driven: the table is a JSON file, not C source) ---
 * Parse + validate `path` into the in-memory schema. Returns the number of
 * settings loaded, or -1 on error (see cmsettings_last_error). Replaces any
 * previously loaded schema. cmsettings_default_schema_path() resolves
 * "settings.json" next to the executable (the .app Resources at merge). */
int         cmsettings_load_schema(const char *path);
const char *cmsettings_default_schema_path(void);   /* valid until pool drains */
const char *cmsettings_last_error(void);

/* --- schema access (valid after a successful load) --- */
const CMSetting *cmsettings_all(int *count);

/* --- the model (pure; no NSView). Persist + apply driven by the descriptor. --- */
long        cmsettings_load_num(const CMSetting *s);
const char *cmsettings_load_str(const CMSetting *s);   /* valid until pool drains */
void        cmsettings_set_num(const CMSetting *s, long v, const CMShellSink *sink);
void        cmsettings_set_str(const CMSetting *s, const char *v, const CMShellSink *sink);

/* --- store wiring (the POC/test points these at fixtures) --- */
void cmsettings_set_conf_path(const char *path);       /* the `conf` store file   */
void cmsettings_set_defaults_suite(const char *suite); /* NSUserDefaults suite     */

/* --- generated window. Builds the toolbar-tab window for `tier` from the schema.
 * Returns counts so a test can assert the generation matched the schema; `show`
 * makes it visible. window (out) is the NSWindow* (opaque). --- */
typedef struct { int tabs; int controls; void *window; } CMSettingsBuild;
CMSettingsBuild cmsettings_build(const char *tier, const CMShellSink *sink, int show);

#ifdef __cplusplus
}
#endif

#endif /* CMSETTINGS_H */
