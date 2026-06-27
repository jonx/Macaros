/* cmshell.h — host app-shell controller for hosted AROS on macOS.
 *
 * The "first-class Mac citizen" layer from docs/features/host-app-shell/: the
 * menu bar, the standard About panel, and the app icon. POC stage — lives in
 * hosted/hostshell/ and touches NONE of the existing cocoametal shim, so it can
 * be developed in parallel and merged later (see README.md).
 *
 * Clean-room: implemented from Apple AppKit/Foundation/CoreGraphics docs [PUB] +
 * the Apple HIG menu-bar/settings/About conventions [PUB] + this project's own
 * spec (docs/features/host-app-shell/spec.md, the cm_* ownership split) [OURS].
 * No GPL emulator source (vAmiga/WinUAE/FS-UAE/Amiberry/E-UAE) was read. UTM is
 * Apache-2.0; only its public menu *layout* informed the structure [PUB-UTM].
 *
 * THE SEAM (engine/shell split, [[aros-embeddable-library-goal]]): the controller
 * never calls Cocoa-into-AROS or the Metal shim directly. Every menu action is
 * translated into a neutral "intent" and handed to a pluggable CMShellSink. The
 * POC supplies a recording mock sink; at merge the sink forwards to the real
 * cm_set_option / cm_capture_png / cm_volume_add / ... ABI. The shell stays
 * host-agnostic; the engine sees no new contract.
 */
#ifndef CMSHELL_H
#define CMSHELL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Option keys — MIRROR of cocoametal.h CMOption (host-acted presentation keys are
 * < 0x10, AROS-facing functional keys are >= 0x10). Kept identical so action code
 * reads exactly like production; at merge, delete this block and #include
 * "cocoametal.h" instead. The new (>=0x14) keys are spec.md R-OPTKEYS. */
enum {
    CM_OPT_EFFECT          = 0x00,   /* host-acted: CM_FX_*            */
    CM_OPT_SCALE_MODE      = 0x01,   /* host-acted: CM_SCALE_*         */
    CM_OPT_FULLSCREEN      = 0x02,   /* host-acted: 0/1                */
    CM_OPT_FILTER          = 0x03,   /* host-acted: CM_FILTER_*        */
    CM_OPT_RETINA          = 0x04,   /* host-acted: 0/1 (new, presentation) */
    CM_OPT_AUDIO_VOLUME    = 0x13,   /* AROS-facing: 0..100 (existing stub) */
    CM_OPT_CLIPBOARD_SHARE = 0x14,   /* AROS-facing: 0/1               */
    CM_OPT_AUDIO_DEVICE    = 0x15,   /* AROS-facing: device index      */
    CM_OPT_VOLUME_ADD      = 0x16,   /* AROS-facing: paired str spec   */
    CM_OPT_VOLUME_REMOVE   = 0x17,   /* AROS-facing: paired str name   */
    CM_OPT_POWER           = 0x18,   /* AROS-facing: CM_POWER_*        */
};

enum { CM_FX_NEAREST = 0, CM_FX_SCANLINE = 1 };
enum { CM_SCALE_FIT = 0, CM_SCALE_INTEGER_NEAREST = 1,
       CM_SCALE_PIXEL_PERFECT = 2, CM_SCALE_ASPECT_FIT = 3 };
enum { CM_FILTER_NEAREST = 0, CM_FILTER_LINEAR = 1 };

/* Graded shutdown, UTM's Power-submenu shape [PUB-UTM] restated [OURS]. */
enum { CM_POWER_REQUEST_DOWN = 0,  /* soft: ask AROS to shut down       */
       CM_POWER_RESET        = 1,  /* reset / reboot                    */
       CM_POWER_FORCE_DOWN   = 2,  /* hard: stop the machine            */
       CM_POWER_FORCE_QUIT   = 3 }; /* last resort: end the host process */

/* The shell→engine seam. Any field may be NULL (the action becomes a no-op). */
typedef struct CMShellSink {
    void *ctx;                                              /* passed back to each cb */
    void (*set_option)(void *ctx, int key, long value);    /* cm_set_option           */
    void (*set_option_str)(void *ctx, int key, const char *str); /* cm_set_option_str  */
    int  (*capture_png)(void *ctx, const char *path);      /* cm_capture_png          */
    int  (*record_start)(void *ctx, const char *path, int fps, int codec);
    int  (*record_stop)(void *ctx);
    int  (*volume_add)(void *ctx, const char *spec);       /* "Name:hostpath[;WRITE]" */
    int  (*volume_remove)(void *ctx, const char *name);
    void (*power)(void *ctx, int request);                 /* CM_POWER_*              */
    void (*set_capture_input)(void *ctx, int on);          /* input grab on/off       */
    void (*open_settings)(void *ctx);                      /* Settings… (⌘,) → cm_open_settings */
} CMShellSink;

/* Build + install the menu bar, About wiring, and the Dock/app icon onto the
 * shared NSApplication. MAIN THREAD ONLY, after [NSApplication sharedApplication]
 * + setActivationPolicy:. `sink` is copied by value (may be NULL). Returns an
 * opaque handle (the controller; kept alive internally). Idempotent-safe to call
 * once per process. */
void *cmshell_install(const CMShellSink *sink);

/* Order the standard About panel front (no-op-safe with no window server). */
void  cmshell_show_about(void *handle);

#ifdef __cplusplus
}
#endif

#endif /* CMSHELL_H */
