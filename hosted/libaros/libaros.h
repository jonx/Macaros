/* libaros.h — flat C ABI for embedding AROS as a library (the ENGINE layer).
 *
 * ┌─ STATUS: DESIGN SKETCH. Nothing behind this header is implemented yet. ─┐
 * │ It exists to pin the embedding *contract* now, so the display/input/    │
 * │ console work already in flight stays on the path to a real libAROS.     │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * For the "why" and what this unlocks once it exists, see IDEAS.md beside it.
 *
 * The goal (project memory: aros-embeddable-library-goal): run AROS as a guest
 * subsystem inside a host process — not a program that owns the machine. The host
 * owns its own main thread / run loop / signal policy; libAROS runs AROS on a
 * dedicated thread it spawns, and hands the host THREE clean seams:
 *
 *   1. LIFECYCLE   create / start / stop / destroy + state & alert notifications.
 *   2. SCREEN+INPUT  "hand the host the screen, take its input" — a chunky
 *                    framebuffer the host reads (or locks), and input events the
 *                    host posts in. This is cocoametal's cm_* ABI, inverted: there
 *                    AROS calls OUT to a host window; here the engine OWNS the
 *                    framebuffer and the host (shell) decides how to present it.
 *   3. CONSOLE     byte streams to/from the AmigaDOS shell (the emul-handler
 *                    stdio seam). "Scriptable" is just a host driving this
 *                    programmatically — it needs no extra architecture.
 *
 * Host-agnostic on purpose: NO Cocoa, Metal, AppKit, or AROS types cross this
 * header. The Cocoa/Metal app becomes "shell #1" — one embedder among several
 * (CLI, CI harness, another app, iOS) on the same engine. cocoametal.h stays the
 * *host shell's* contact surface; this is the *engine's*.
 *
 * Good-library-citizen rules this ABI is shaped to enforce (see the memory):
 *   - Signals CHAIN, never clobber: AROS's SIGSEGV/SIGBUS handlers fall through
 *     to the host's prior handlers (AROSConfig.signal_policy, default CHAIN).
 *   - No unilateral exit(): a guru/halt surfaces as AROS_STATE_HALTED + an alert
 *     callback; aros_stop() ENDS the AROS thread and RETURNS control. The library
 *     never kills the embedding process.
 *   - Display + console are pluggable seams, not hardcoded backends.
 *
 * ABI is APPEND-ONLY once it ships (mirror cocoametal.h §1a): enum members and
 * functions are only ever added at the end, never reordered or removed.
 */
#ifndef LIBAROS_H
#define LIBAROS_H

#include <stddef.h>   /* size_t  */
#include <stdint.h>   /* uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on ANY breaking change. A host checks aros_abi_version() == LIBAROS_ABI
 * right after loading the library and refuses to embed on mismatch. */
#define LIBAROS_ABI 1
int aros_abi_version(void);

/* One embedded AROS machine. Opaque; all state lives behind it, so a host MAY
 * hold several — though one-engine-per-process is the likely first reality (AROS
 * has process-global state; see the header note at the bottom). */
typedef struct AROSInstance AROSInstance;

/* Every call that can fail returns one of these (0 == success). */
typedef enum {
    AROS_OK            =  0,
    AROS_ERR          = -1,   /* unspecified */
    AROS_ERR_BADARG   = -2,   /* NULL / out-of-range argument */
    AROS_ERR_STATE    = -3,   /* not valid in the instance's current state */
    AROS_ERR_NOTREADY = -4,   /* engine not RUNNING yet (still BOOTING) */
    AROS_ERR_KICKSTART= -5,   /* boot dir / module set could not be loaded */
    AROS_ERR_NOBACKEND= -6    /* a live-present call with no display backend attached */
} AROSResult;

/* Lifecycle state. Delivered via AROSConfig.on_state and/or polled with
 * aros_state(). HALTED = an AROS guru/alert stopped the machine but the instance
 * is still inspectable (framebuffer/log readable) until aros_destroy(). */
typedef enum {
    AROS_STATE_CREATED = 0,   /* aros_create done; AROS thread not started */
    AROS_STATE_BOOTING,       /* aros_start called; kickstart running */
    AROS_STATE_RUNNING,       /* reached the Shell / Workbench; accepting input */
    AROS_STATE_HALTED,        /* guru/alert — stopped, still inspectable */
    AROS_STATE_STOPPED        /* aros_stop done; AROS thread joined */
} AROSState;

/* How AROS's fatal-signal handlers behave (rule #1). */
typedef enum {
    AROS_SIG_CHAIN = 0,  /* DEFAULT: handle, then fall through to the host's prior
                            handler — the only safe choice when embedded. */
    AROS_SIG_OWN,        /* install AROS handlers, save & DON'T chain (standalone) */
    AROS_SIG_NONE        /* install nothing — host owns all signal handling */
} AROSSignalPolicy;

/* TrueColor only, matching one GPU pixel format with no swizzle (same world as
 * CMPixelDesc). Default is BGRA8 on little-endian AArch64: B byte 0 … A byte 3. */
typedef enum {
    AROS_PIX_BGRA8 = 0
} AROSPixelFormat;

/* The live chunky framebuffer, as handed to the host by aros_frame_lock(). */
typedef struct {
    const void     *pixels;   /* top-left of the whole framebuffer */
    int             width;    /* logical (pre-scale) pixels */
    int             height;
    int             stride;   /* bytes per row */
    int             scale;    /* backing/logical (HiDPI); 1 if none */
    AROSPixelFormat format;
} AROSFrame;

/* Named non-text keys. PRINTABLE text goes through aros_post_text() (the engine
 * owns char→keystroke, incl. shift), so this enum only needs the keys a string
 * can't express. Raw AROS RAWKEY_* codes are still reachable via aros_post_rawkey().
 * Mirrors what aros-ctl spells out as macOS virtual keycodes today. */
typedef enum {
    AROS_KEY_RETURN = 0, AROS_KEY_ESCAPE, AROS_KEY_TAB, AROS_KEY_BACKSPACE,
    AROS_KEY_DELETE, AROS_KEY_SPACE,
    AROS_KEY_UP, AROS_KEY_DOWN, AROS_KEY_LEFT, AROS_KEY_RIGHT,
    AROS_KEY_LSHIFT, AROS_KEY_RSHIFT, AROS_KEY_CONTROL,
    AROS_KEY_LALT, AROS_KEY_RALT, AROS_KEY_LAMIGA, AROS_KEY_RAMIGA,
    AROS_KEY_F1, AROS_KEY_F2, AROS_KEY_F3, AROS_KEY_F4, AROS_KEY_F5,
    AROS_KEY_F6, AROS_KEY_F7, AROS_KEY_F8, AROS_KEY_F9, AROS_KEY_F10,
    AROS_KEY_HELP
} AROSKey;

/* Boot + presentation + callbacks. Zero-initialise and set what you need; every
 * callback is OPTIONAL (leave NULL and poll instead — see aros_poll). */
typedef struct {
    /* --- boot (what AROSBootstrap consumes today) --- */
    const char  *boot_dir;       /* …/AROS/boot/darwin (holds the conf + modules) */
    const char  *config_path;    /* the AROSBootstrap.conf; NULL → boot_dir default */
    const char *const *env;      /* extra "KEY=VALUE" for the engine, e.g.
                                  * "AROS_HOST_VOLUME=MacRW:/path;WRITE" (host-volume),
                                  * "DYLD_FALLBACK_LIBRARY_PATH=…" — NULL-terminated
                                  * or paired with env_count. */
    size_t       env_count;

    /* --- framebuffer --- */
    int             width, height;   /* initial logical size; 0,0 → driver default */
    AROSPixelFormat pixel_format;    /* 0 → AROS_PIX_BGRA8 */
    int             headless;        /* 1 → engine owns the FB only (no live backend);
                                      * the CI / scripting default. */

    /* --- policy --- */
    AROSSignalPolicy signal_policy;  /* 0 → AROS_SIG_CHAIN */

    /* --- notifications (all optional; delivered on the host thread via aros_poll,
     *     or on an internal dispatcher if the host never polls) --- */
    void *user;                                              /* echoed to callbacks */
    void (*on_state) (void *user, AROSState st);
    void (*on_alert) (void *user, uint32_t code, const char *text); /* guru/recoverable */
    void (*on_console)(void *user, const char *bytes, size_t len);  /* shell stdout */
    void (*on_frame) (void *user, const AROSFrame *f);              /* a frame was presented */
} AROSConfig;

/* ======================================================================== *
 *  SEAM 1 — LIFECYCLE
 * ======================================================================== */

/* Validate cfg and load the kickstart/module set; does NOT start the AROS thread.
 * Returns NULL on failure (bad boot dir, OOM). The instance owns a copy of cfg. */
AROSInstance *aros_create(const AROSConfig *cfg);

/* Spawn the dedicated AROS thread and begin the boot. NON-BLOCKING: the host's
 * thread returns immediately; watch on_state / aros_state() for RUNNING. */
AROSResult    aros_start(AROSInstance *);

/* Request an orderly shutdown: AROS unwinds, its thread ends, control RETURNS
 * here (rule #2 — never exit()). Idempotent; blocks until the thread joins. */
AROSResult    aros_stop(AROSInstance *);

/* Free everything. Calls aros_stop() first if still running. */
void          aros_destroy(AROSInstance *);

AROSState     aros_state(AROSInstance *);

/* Deliver any queued callbacks (state/alert/console/frame) on the CALLING thread
 * and return how many fired. A host with a run loop calls this each tick so
 * callbacks never land on the AROS thread (and never race into a main-thread-only
 * UI). Returns <0 on error. Safe to ignore entirely if you use no callbacks. */
int           aros_poll(AROSInstance *);

/* ======================================================================== *
 *  SEAM 2 — SCREEN + INPUT
 *  Output: the engine owns the framebuffer; the host reads or locks it.
 *  Input:  the host posts events; they enter the SAME stream real device input
 *          would (faithful reproduction), exactly as aros-ctl's injection does.
 *  All input calls are safe from any thread and no-op before RUNNING.
 * ======================================================================== */

/* Current logical/backing geometry (like cm_target_size). */
AROSResult aros_display_size(AROSInstance *, int *w, int *h, int *scale);

/* Copy the last-presented frame into dst as BGRA8 at LOGICAL w×h (the unattended
 * "oracle" — same contract as cm_readback). The screenshot/CI path; needs no
 * window and no live backend. 0 on success. */
AROSResult aros_readback(AROSInstance *, void *dst, int dstStride, int w, int h);

/* Zero-copy access to the live chunky framebuffer for a host that uploads to its
 * own GPU surface. Must be paired with aros_frame_unlock(); hold it briefly. */
AROSResult aros_frame_lock(AROSInstance *, AROSFrame *out);
AROSResult aros_frame_unlock(AROSInstance *);

/* Input in. Coordinates are LOGICAL pixels, top-left origin (cm_* convention). */
AROSResult aros_post_text       (AROSInstance *, const char *utf8);          /* types a string */
AROSResult aros_post_key        (AROSInstance *, AROSKey key, int pressed);  /* named non-text key */
AROSResult aros_post_rawkey     (AROSInstance *, int rawkey, int pressed);   /* AROS RAWKEY_* escape hatch */
AROSResult aros_post_mouse_move (AROSInstance *, int x, int y);
AROSResult aros_post_mouse_button(AROSInstance *, int button, int pressed);  /* 0=left 1=right 2=middle */

/* ======================================================================== *
 *  SEAM 3 — CONSOLE (the scriptable seam)
 *  Bytes to/from the AmigaDOS shell over the emul-handler stdio seam. A host
 *  that writes commands and reads output IS "AROS, scripted" — no more than this.
 * ======================================================================== */

/* Feed bytes to the shell's input (stdin). Returns count accepted, <0 on error. */
long aros_console_write(AROSInstance *, const char *bytes, size_t len);

/* Read up to len bytes the shell has written (stdout), NON-BLOCKING. Returns
 * count read (0 if none pending right now), <0 on error. Or set
 * AROSConfig.on_console for push delivery instead of polling. */
long aros_console_read (AROSInstance *, char *buf, size_t len);

/* ======================================================================== *
 *  CONVENIENCE — pure sugar over the seams above (a libAROS-flavoured aros-ctl).
 *  Provided so the common harness gestures are one call; each is implementable
 *  entirely in terms of the seam functions, nothing new underneath.
 * ======================================================================== */

/* aros_post_text() + a trailing Return — "run this shell line". */
AROSResult aros_shell_exec(AROSInstance *, const char *command);

/* aros_readback() encoded to a binary PPM (P6) at `path` — dependency-free, the
 * same thing aros-ctl's `shot` writes before handing off to sips. 0 on success. */
AROSResult aros_screenshot(AROSInstance *, const char *path);

/* ======================================================================== *
 *  OPTIONAL — pluggable LIVE display backend (APPEND-ONLY extension).
 *  The core above is enough for headless/scripted/CI use. To present a LIVE,
 *  zero-copy window, a host registers a backend whose vtable is exactly
 *  cocoametal's cm_* shape — so cocoametal becomes ONE implementation of this,
 *  wired in by the shell, never linked by the engine. When a backend is attached,
 *  its pump() supplies real device events and the host's aros_post_* injections
 *  MERGE with them (injected drained first), just like cocoametal_control today.
 * ======================================================================== */
typedef struct {
    void *(*open)       (int w, int h, AROSPixelFormat fmt, const char *title);
    void  (*upload_rect)(void *ctx, const void *src, int srcStride,
                         int x, int y, int w, int h);
    void  (*present)    (void *ctx);
    int   (*pump_events)(void *ctx, /* writes neutral events */ void *out, int maxEvents);
    int   (*readback)   (void *ctx, void *dst, int dstStride, int w, int h);
    void  (*close)      (void *ctx);
} AROSDisplayBackend;

/* Attach before aros_start() (or while RUNNING to hand off from headless to live).
 * Pass NULL to detach and fall back to the engine-owned framebuffer. */
AROSResult aros_attach_display(AROSInstance *, const AROSDisplayBackend *);

#ifdef __cplusplus
}
#endif

/* ── A note on multiple instances ───────────────────────────────────────────
 * AROS keeps process-global state (SysBase, the module set), so the honest first
 * target is ONE engine per process with MANY presentation surfaces / scripts, or
 * process-per-instance under a shared host supervisor. True N-engines-in-one-
 * process is the open hard problem; this ABI is written so a host can't tell the
 * difference once that's solved (the instance handle is already the only namespace
 * a caller touches). */
#endif /* LIBAROS_H */
