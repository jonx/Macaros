/* cocoametal.h — flat C ABI for the Apple-native Cocoa/Metal display HIDD shim.
 *
 * Implemented from docs/features/cocoa-metal-display/spec.md ("The C ABI
 * (cocoametal.h)"). Independent work: no third-party implementation source —
 * emulator, agent, driver, or otherwise — was read, searched, or consulted in
 * producing it, and any resemblance to existing implementations is coincidental.
 * Apple framework docs + this project's own H7 spike (hosted/display.c) only.
 *
 * Hand-authored, neutral. This header is the *only* contact surface between the
 * AROS-side HIDD (AROS crosstools) and the host shim (Apple clang). The shim
 * pulls no AROS headers; the AROS side pulls no Cocoa headers.
 *
 * ABI is APPEND-ONLY (INTERFACE.md §1a): symbols/enum members are only ever
 * added at the end, never reordered or removed. v2 appended the settings/options
 * ABI (§9). Independent work — no third-party implementation source was read or
 * consulted; any resemblance is coincidental.
 */
#ifndef COCOAMETAL_H
#define COCOAMETAL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CMContext CMContext;

/* TrueColor only; matches one MTLPixelFormat with no swizzle.
 * For BGRA8Unorm on little-endian AArch64: blue byte 0, green 1, red 2, alpha 3. */
typedef struct {
    int  bytesPerPixel;                                /* 4 */
    int  redShift, greenShift, blueShift, alphaShift;  /* bit positions */
    unsigned redMask, greenMask, blueMask, alphaMask;
} CMPixelDesc;

typedef enum {
    CM_EV_NONE = 0,
    CM_EV_MOUSEMOVE,
    CM_EV_MOUSEBTN,
    CM_EV_KEY,
    CM_EV_CLOSE,
    CM_EV_RESIZE,
    /* APPEND-ONLY (INTERFACE.md §1a/§5): a user changed an AROS-facing setting in
     * the native settings panel. PULL-ONLY — the shim never calls into AROS; it
     * enqueues this and the AROS side reads it via cm_pump_events. Packing (§9):
     *   code    = the CMOption key the user changed (an AROS-facing key)
     *   x       = the new value (e.g. CM_OPT_REQUEST_MODE_W -> width in px)
     *   y       = a 2nd value when the key carries a pair (e.g. _MODE_H height)
     *   pressed = 0 (unused);  mods = 0 (unused)
     * Host-owned keys (effect/scale/fullscreen/filter) are acted on directly and do
     * NOT produce a CM_EV_SETTING. */
    CM_EV_SETTING,
    /* APPEND-ONLY (INTERFACE.md §1a/§5): scroll-wheel motion, quantized to whole
     * line steps by the shim. Packing:
     *   x       = horizontal steps (positive = wheel RIGHT)
     *   y       = vertical steps   (positive = wheel DOWN)
     * — the AROS NewMouse/gameport sign convention, NOT a pointer position.
     *   code    = 0 (unused);  pressed = 0 (unused);  mods = CM_MOD_* held.
     * Emitted from real NSEventTypeScrollWheel events and from the control
     * FIFO's "W <dy> [dx]" command. An older AROS driver that predates this
     * kind simply ignores it (unknown-type default), so CM_ABI_VERSION stays 2. */
    CM_EV_WHEEL
} CMEventType;

/* Modifier bitmask (CM_MOD_*) — neutral names, filled from NSEvent.modifierFlags
 * by the input spike (D4/D5). Defined here so the ABI is stable now. */
#define CM_MOD_SHIFT   (1u << 0)
#define CM_MOD_CONTROL (1u << 1)
#define CM_MOD_ALT     (1u << 2)
#define CM_MOD_CMD     (1u << 3)

typedef struct {
    CMEventType type;
    int x, y;        /* logical (pre-scale) pixel coords, top-left origin */
    int code;        /* button index, or macOS virtual keycode */
    int pressed;     /* 1=down 0=up */
    unsigned mods;   /* CM_MOD_* bitmask */
} CMEvent;

/* Present-time fragment-shader effect (selected by cm_set_effect). This is a
 * PRESENTATION-ONLY option: it changes how the framebuffer is drawn into the
 * live drawable, never the offscreen oracle target (which cm_readback reads and
 * which stays fixed at pass-through/nearest). Add new effects by appending to
 * this enum AND the fragment-shader switch in the shim. */
typedef enum {
    CM_FX_NEAREST = 0,   /* pass-through, nearest-sampled — the oracle's effect */
    CM_FX_SCANLINE = 1,  /* example CRT: odd-row darkening + a simple gamma curve */
    CM_FX__COUNT
} CMEffect;

/* ---- Settings & options (INTERFACE.md §9) --------------------------------
 * A host settings panel (cm_open_settings) plus a key/value option ABI
 * (cm_set_option / cm_get_option). Two ownership groups (see CMOption):
 *   HOST-ACTED keys take effect immediately on the LIVE PRESENT path only
 *   (never the offscreen oracle — that stays pass-through/nearest, §6).
 *   AROS-FACING keys are STUBBED on the host: cm_set_option does NOT act on
 *   them; instead the shim enqueues a CM_EV_SETTING (above) for the AROS side
 *   to pull. This keeps the no-callbacks-into-AROS rule (§3). */

/* Value for CM_OPT_SCALE_MODE: how the framebuffer maps onto the drawable. */
typedef enum {
    CM_SCALE_FIT = 0,            /* stretch to fill the drawable (ignores aspect) */
    CM_SCALE_INTEGER_NEAREST = 1,/* largest integer multiple that fits, centred */
    CM_SCALE_PIXEL_PERFECT = 2,  /* 1:1 logical->drawable pixels, centred (no scale) */
    /* APPEND-ONLY (INTERFACE.md §1a/§9): aspect-preserving fill — scale the
     * framebuffer up to the LARGEST size that fits the drawable while keeping the
     * logical aspect ratio, centred, with a BLACK letterbox/pillarbox filling the
     * rest. This is the DEFAULT live-present mode: it FILLS the window/screen as
     * much as possible without distorting 320x200, and the surrounding bars are
     * black (never white). The offscreen oracle is unaffected (§6, always FIT). */
    CM_SCALE_ASPECT_FIT = 3,
    CM_SCALE__COUNT
} CMScaleMode;

/* Value for CM_OPT_FILTER: the sampler used on the LIVE present (oracle stays
 * nearest regardless). */
typedef enum {
    CM_FILTER_NEAREST = 0,       /* crisp pixels (default) */
    CM_FILTER_LINEAR = 1,        /* bilinear smoothing */
    CM_FILTER__COUNT
} CMFilter;

/* Value for CM_OPT_THEME: the host app's appearance. HOST-ACTED — the shim sets
 * NSApp.appearance so all chrome (title bar, menus, Settings, the window status
 * bar) themes coherently. The black Metal area (the guest framebuffer) is never
 * recolored. SYSTEM follows the macOS setting; LIGHT/DARK force it for this app. */
typedef enum {
    CM_THEME_SYSTEM = 0,         /* follow the macOS appearance (default) */
    CM_THEME_LIGHT  = 1,         /* force Aqua (light) */
    CM_THEME_DARK   = 2,         /* force Dark Aqua */
    CM_THEME__COUNT
} CMTheme;

/* Option keys for cm_set_option / cm_get_option. Numbered with deliberate GAPS
 * so each group can grow without renumbering the other (append-only contract).
 *
 *   HOST-ACTED  [0x00..0x0F] — the shim applies these to the live present now.
 *   AROS-FACING [0x10..]     — RESERVED / STUBBED: the host does NOT act on them;
 *                              cm_set_option enqueues a CM_EV_SETTING instead. */
typedef enum {
    /* --- host-owned, acted on immediately (live present only) --- */
    CM_OPT_EFFECT      = 0x00,   /* value = a CMEffect (CM_FX_*)  */
    CM_OPT_SCALE_MODE  = 0x01,   /* value = a CMScaleMode         */
    CM_OPT_FULLSCREEN  = 0x02,   /* value = 0/1                   */
    CM_OPT_FILTER      = 0x03,   /* value = a CMFilter            */
    CM_OPT_RETINA      = 0x04,   /* value = 0/1 (host-shell v3; recorded pref) */
    CM_OPT_THEME       = 0x05,   /* value = a CMTheme (host app appearance) */
    /* 0x06..0x0F reserved for future HOST-acted keys */

    /* --- settings surfaced via CM_EV_SETTING for AROS visibility ---
     * Most are not host-acted; AUDIO_VOLUME is the exception, applying host
     * CoreAudio gain immediately and also emitting a mirrored setting event. */
    CM_OPT_REQUEST_MODE_W = 0x10,/* requested display mode width  (px)  */
    CM_OPT_REQUEST_MODE_H = 0x11,/* requested display mode height (px); paired
                                  * with _MODE_W in CM_EV_SETTING.y      */
    CM_OPT_KEYMAP         = 0x12,/* requested AROS keymap id            */
    CM_OPT_AUDIO_VOLUME   = 0x13,/* host CoreAudio gain percent         */
    /* host-shell v3 (append-only): the menu/settings surface these AROS-facing
     * requests; the AROS side pulls them via cm_pump_events (CM_EV_SETTING). The
     * VOLUME_* keys carry a STRING (the mount spec) read with cm_get_option_str. */
    CM_OPT_CLIPBOARD_SHARE = 0x14,/* share host clipboard 0/1            */
    CM_OPT_AUDIO_DEVICE    = 0x15,/* requested audio output device index */
    CM_OPT_VOLUME_ADD      = 0x16,/* mount a host folder (string spec)   */
    CM_OPT_VOLUME_REMOVE   = 0x17,/* unmount by name (string)            */
    CM_OPT_POWER           = 0x18 /* lifecycle request (a CM_POWER_*)    */
    /* 0x19.. reserved for future AROS-facing keys */
} CMOption;

/* Value for CM_OPT_POWER — a graded lifecycle request (host-shell v3). The host
 * records + relays it; the AROS side decides what to do (clean shutdown vs reset
 * vs forced stop). Replaces the close-button's unilateral exit() at merge. */
typedef enum {
    CM_POWER_REQUEST_DOWN = 0,   /* soft: ask AROS to shut down  */
    CM_POWER_RESET        = 1,   /* reset / reboot               */
    CM_POWER_FORCE_DOWN   = 2,   /* hard: stop the machine       */
    CM_POWER_FORCE_QUIT   = 3    /* last resort: end the process */
} CMPower;

/* Open the display: build device/queue, framebuffer + offscreen textures, and
 * (best-effort) a live window. Returns NULL on failure. */
CMContext *cm_open(int w, int h, const CMPixelDesc *fmt, const char *title);
void       cm_close(CMContext *);

/* Copy a chunky sub-rect from the AROS-owned framebuffer into the GPU texture.
 * src points at the top-left of the WHOLE framebuffer; srcStride = bytes/row. */
void       cm_upload_rect(CMContext *, const void *src, int srcStride,
                          int x, int y, int w, int h);

/* Render the current framebuffer texture into the offscreen target (the source
 * of truth for readback) via the fixed pass-through/nearest pipeline, then
 * best-effort PRESENT it to the live drawable. The present is a render pass that
 * draws the framebuffer texture into the drawable as a color attachment (NOT a
 * blit copy — that is why layer.framebufferOnly = YES is safe: the drawable is a
 * render target, never a copy destination). The present applies the currently
 * selected CMEffect; the oracle target is always pass-through. Synchronous
 * (commit + waitUntilCompleted on the oracle pass), no blocking run loop. */
void       cm_present(CMContext *);

/* Select the present-time fragment-shader effect (default CM_FX_NEAREST).
 * Presentation-only: never affects the offscreen oracle / cm_readback. Returns 0
 * on success, nonzero if the effect index is out of range. */
int        cm_set_effect(CMContext *, CMEffect effect);

/* Drain pending NSEvents (host main thread). Returns count written to out[]. */
int        cm_pump_events(CMContext *, CMEvent *out, int maxEvents);

/* Copy the last-presented offscreen target into dst as BGRA8 (logical w*h, no
 * scale). The unattended oracle — independent of the on-screen window.
 * Returns 0 on success, nonzero on error. */
int        cm_readback(CMContext *, void *dst, int dstStride, int w, int h);

/* Introspection for the D1 oracle: the offscreen target / drawable dimensions
 * (== logical dims * scale). Lets the test assert drawableSize without poking
 * Cocoa types. Returns 0 on success. */
int        cm_target_size(CMContext *, int *outW, int *outH, int *outScale);

/* Verification hook for the shader stage ([D] check): render the current
 * framebuffer texture through the given CMEffect into a temporary offscreen
 * BGRA8 target of (logicalW*scale)x(logicalH*scale), then read it back into dst.
 * dst dims may be either the LOGICAL size (w=logicalW,h=logicalH — scale-block
 * downsampled, same convention as cm_readback) or the NATIVE target size
 * (w=tw,h=th from cm_target_size — identity copy, so a test can inspect real
 * per-target-row effects like scanlines at any scale). Lets the test prove the
 * effect pipeline actually runs and differs from pass-through, WITHOUT touching
 * the live oracle target. Returns 0 on success. */
int        cm_render_effect_readback(CMContext *, CMEffect effect,
                                     void *dst, int dstStride, int w, int h);

/* ABI version (INTERFACE.md §7). Bump on ANY breaking change to this contract.
 * The AROS loader checks CMIFace->cm_abi_version() == CM_ABI_VERSION right after
 * HostLib_GetInterface and refuses to register the driver on mismatch. Per the
 * append-only rule (§1a) cm_abi_version is symbol-array entry 9.
 *
 * v1 -> v2: appended the settings/options ABI (cm_set_option / cm_get_option /
 * cm_open_settings, entries 10/11/12), the CMOption/CMScaleMode/CMFilter enums,
 * and the CM_EV_SETTING event. All append-only — no existing symbol moved.
 *
 * host app shell — STAYS AT 2 ON PURPOSE. The shell appended new symbols
 * (cm_set_option_str / cm_get_option_str, cm_capture_png, cm_record_start/stop) and
 * new CM_OPT_ keys (RETINA / CLIPBOARD_SHARE / AUDIO_DEVICE / VOLUME_ADD/REMOVE /
 * POWER) + the CMPower enum. But the AROS-FACING contract the HIDD resolves (the 13
 * entries 0..12) is UNCHANGED, and the menu / About / icon / settings are HOST-SIDE
 * (they reach AROS only through the existing cm_pump_events channel). The AROS loader
 * checks cm_abi_version() == CM_ABI_VERSION (strict) and the deployed AROS cocoa HIDD
 * expects 2 — so this version, which tracks the AROS-facing contract, MUST stay 2;
 * the appended symbols are an orthogonal host-side extension a v2 driver ignores.
 * Bump to 3 (and rebuild the AROS HIDD to match) only when the AROS side is taught to
 * CONSUME a new symbol — e.g. cm_get_option_str for the clipboard/volume bridge. */
#define CM_ABI_VERSION 2
int        cm_abi_version(void);

/* ---- Settings & options ABI (INTERFACE.md §9) — appended at v2 ------------
 * Append-only symbol-array entries 10/11/12 (after cm_abi_version at 9). */

/* Set an option (key from CMOption, value interpreted per that key).
 *   HOST-ACTED keys (CM_OPT_EFFECT, SCALE_MODE, FULLSCREEN, FILTER,
 *   AUDIO_VOLUME) take effect
 *   immediately on the LIVE present path — never the offscreen oracle (§6).
 *   AROS-FACING keys (CM_OPT_REQUEST_MODE_W/H, KEYMAP) are not acted on by the
 *   host; instead the shim enqueues a CM_EV_SETTING (pull-only) for the AROS
 *   side. Audio volume is also mirrored as a CM_EV_SETTING for UI/log visibility.
 *   Returns 0 on success, nonzero for an unknown key / bad value. */
int        cm_set_option(CMContext *, int key, long value);

/* Read the current host-side value of an option (for the panel + tests). For an
 * AROS-facing key this returns the LAST value the user requested through the
 * panel (the host tracks it for the panel UI; it still does not act on it).
 * Writes *value and returns 0 on success, nonzero for an unknown key. */
int        cm_get_option(CMContext *, int key, long *value);

/* Open the native AppKit settings window (best-effort, like cm_open's window:
 * on the display-server task, serviced by the existing manual CFRunLoop).
 * Degrades to a no-op when there is no window server. Non-blocking. Returns 0
 * if the panel is up (or already up), nonzero if it could not be opened. */
int        cm_open_settings(CMContext *);

/* ---- Host app shell ABI (v3, append-only — entries 13..17) ----------------
 * The menu bar / About / icon are installed host-side when the window comes up
 * (no AROS-facing call). These symbols are the few that AROS *may* also drive. */

/* String-valued option (the side-channel CM_EV_SETTING cannot carry). For
 * CM_OPT_VOLUME_ADD/REMOVE the host records the string and enqueues the matching
 * CM_EV_SETTING (key only); the AROS side pulls the string with cm_get_option_str.
 * Returns 0 on success, nonzero for an unknown key. */
int        cm_set_option_str(CMContext *, int key, const char *value);
int        cm_get_option_str(CMContext *, int key, char *buf, int buflen);

/* Save the current frame as a PNG (cm_readback the offscreen oracle -> ImageIO).
 * No Screen-Recording/TCC — the pixels are ours. Returns 0 on success. */
int        cm_capture_png(CMContext *, const char *path);

/* Movie capture (AVFoundation H.264/HEVC). cm_record_start opens an AVAssetWriter;
 * each cm_present appends the current frame (cm__record_frame); cm_record_stop
 * finalizes the .mov. No Screen-Recording/TCC — the frames are our own offscreen
 * oracle, not a screen grab. Driven from File ▸ Record (menu) and the control FIFO
 * ("V start <path> [fps]" / "V stop"). `codec`: 0 = H.264, 1 = HEVC. Audio is a
 * future drop-in (a second AVAssetWriterInput fed from the CoreAudio capture — see
 * the audio seam in cm_record_start, cocoametal_shell.m). Returns 0 on success. */
int        cm_record_start(CMContext *, const char *path, int fps, int codec);
int        cm_record_stop(CMContext *);

/* ---- GPU compute section (docs/features/gpufx, cocoametal_gpu.m) --------
 *
 * Explicit GPU 2D ops on caller-owned CPU buffers, exported for the
 * AROS-side gpufx.library (which dlsym's ONLY these cm_gpu_* names — they
 * are NOT part of the frozen display CMIFace, so CM_ABI_VERSION is
 * untouched; CM_GPU_ABI versions this section on its own, append-only).
 * The section shares the display's MTLDevice+queue when a window exists
 * (one GPU context for the process); headless callers get a lazy pair.
 * Any-thread callable; every call returns -1 when the GPU is unavailable
 * so callers keep a CPU fallback. Pixel formats are byte-order-agnostic
 * 4 bpp for scale; convert writes RGBA in memory order.
 *
 * ABI 2: the ops take a request STRUCT BY POINTER, not a long argument list.
 * This is deliberate and load-bearing on the hosted port: an AROS->host call
 * passes arguments 9+ on the stack, and the AArch64-ELF (AROS caller) and
 * Apple-arm64 (host dylib) stack-argument layouts differ, so a trailing
 * scalar arg is misread (ABI-1 `cm_gpu_convert_yuv420`'s `fullRange` and
 * `cm_gpu_scale`'s `filter` both landed as garbage through the bridge). A
 * single struct pointer rides entirely in a register, so every field is
 * read correctly. Any host-facing shim call with >8 register-class args has
 * this hazard — prefer a struct pointer. */
#define CM_GPU_ABI 2

typedef struct CmGpuScaleReq {
    const void *src;
    void       *dst;
    int         srcStride, sw, sh;   /* source: bytes, pixels, pixels    */
    int         dstStride, dw, dh;   /* dest:   bytes, pixels, pixels    */
    int         filter;              /* 0 = nearest, 1 = bilinear        */
} CmGpuScaleReq;

typedef struct CmGpuYuvReq {
    const void *y, *u, *v;           /* planar YUV 4:2:0 planes          */
    void       *rgba;
    int         yStride, uStride, vStride;
    int         w, h;
    int         dstStride;
    int         fullRange;           /* 0 = limited (video), 1 = full    */
} CmGpuYuvReq;

int        cm_gpu_open(void);  /* idempotent; 0 = compute section ready */
int        cm_gpu_abi(void);   /* CM_GPU_ABI compiled into this dylib */
/* Point/bilinear scale; src/dst are tightly-rowed-or-wider 4 bpp buffers. */
int        cm_gpu_scale(const CmGpuScaleReq *req);
/* Planar YUV 4:2:0 -> RGBA8888, BT.601. */
int        cm_gpu_convert_yuv420(const CmGpuYuvReq *req);

#ifdef __cplusplus
}
#endif

#endif /* COCOAMETAL_H */
