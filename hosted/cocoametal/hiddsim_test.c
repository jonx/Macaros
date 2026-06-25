/* hiddsim_test.c — a HIDD-shaped behavioral harness for cocoametal.dylib
 * (INTERFACE.md §8 D3 host-support — the de-risk + reference for the AROS HIDD).
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/INTERFACE.md
 * (§2/§2a pixel hand-off + call mapping, §3 threading, §4 function semantics,
 * §6 readback oracle) + cocoametal.h. No GPL emulator source
 * (vAmiga/WinUAE/FS-UAE/Amiberry/E-UAE) was read, searched, or consulted. Apple
 * framework docs + this project's own H7 readback discipline only.
 *
 * WHAT THIS IS, AND HOW IT DIFFERS FROM abi_test.c
 * ------------------------------------------------
 * abi_test.c proves the *load seam* (dlopen + dlsym every frozen symbol,
 * errcount==0, version handshake) and ONE full-frame upload→present→readback.
 * This harness instead drives the shim the way the AROS Intuition/graphics.library
 * path actually will at runtime — beyond that single sequence — so the AROS-side
 * D3 wiring (the bitmap class' UpdateRect hook) has a behavioral reference and the
 * partial-update + pixel-order risks are retired host-side, where the AROS side
 * cannot easily see them:
 *
 *   1. AROS OWNS THE FRAMEBUFFER (§2). We allocate a host-side W*H*4 BGRA8 buffer
 *      — the stand-in for the AROS `AllocMem(W*H*4, MEMF_CLEAR)` framebuffer — and
 *      we maintain a SECOND, independent host reference buffer composed by plain
 *      memcpy (no Metal). The shim never owns pixels; it only ever copies the
 *      dirty sub-rect we hand it (cm_upload_rect) and composes onto the GPU
 *      texture, exactly as moHidd_BitMap_UpdateRect → cm_upload_rect+cm_present.
 *
 *   2. PINNED PIXEL HAND-OFF (§2a). The CMPixelDesc is filled with the EXACT
 *      frozen field values for BGRA8 == MTLPixelFormatBGRA8Unorm on little-endian
 *      AArch64 (blue byte 0, green 1, red 2, alpha 3). These are the values the
 *      AROS gfx class must mirror from its TrueColor StdPixFmt.
 *
 *   3. LAZY OPEN. The first "Show" triggers cm_open (the SDL lazy-window pattern,
 *      §4) — not at allocation time.
 *
 *   4. DIRTY-RECT STREAM (the real pattern). We draw into the AROS framebuffer in
 *      several steps and after EACH change call cm_upload_rect for ONLY the
 *      changed sub-rect + cm_present — partial, incremental, OVERLAPPING updates,
 *      never one full-frame blit. This is the many-small-UpdateRects cadence
 *      Intuition/graphics.library produce.
 *
 *   5. COMPOSE ASSERT (§6 oracle, under realistic usage). After the whole stream,
 *      cm_readback the composed offscreen oracle and assert it is BYTE-EXACT,
 *      logical W×H, against the host reference framebuffer — proving the
 *      accumulation of dirty-rect uploads composes correctly (no stale pixels in
 *      un-re-uploaded regions, overlapping rects land last-writer-wins).
 *
 *   6. SWIZZLE / PIXFMT ROUND-TRIP. Write a known pixel with distinct B/G/R/A
 *      byte values into the framebuffer, upload+present+readback, and confirm each
 *      byte lands in its asserted position — so a swizzle bug (the classic
 *      BGRA/RGBA mix-up) is caught HERE, at the host boundary, not in the AROS
 *      driver where it is much harder to see.
 *
 *   7. EVENTS. cm_pump_events is drained (non-blocking) between presents, like the
 *      VBlank-signalled event task.
 *
 * It is PLAIN C and links NONE of the .m files — it dlopens build/cocoametal.dylib
 * (the REAL boundary, the artifact the AROS HostLib_Open loads) and calls only
 * through the resolved frozen function pointers. Bounded + watchdog; exits clean.
 * Marker: [HIDDSIM] PASS / [HIDDSIM] FAIL.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "cocoametal.h"   /* shared ABI header — types only, no shim code */

/* ---- §1a frozen symbol array (shared with the AROS side, same order). The same
 * names[] HostLib_GetInterface resolves. We only USE a subset here, but we resolve
 * the full set the way the loader does so a missing/renamed symbol still fails. */
static const char *cocoametal_symbols[] = {
    "cm_open", "cm_close", "cm_upload_rect", "cm_present", "cm_set_effect",
    "cm_pump_events", "cm_readback", "cm_target_size",
    "cm_render_effect_readback", "cm_abi_version",
    "cm_set_option", "cm_get_option", "cm_open_settings", NULL
};

/* §1b interface struct, mirroring the array EXACTLY (host stand-in for CMInterface). */
typedef struct {
    CMContext *(*cm_open)(int, int, const CMPixelDesc *, const char *);
    void       (*cm_close)(CMContext *);
    void       (*cm_upload_rect)(CMContext *, const void *, int, int, int, int, int);
    void       (*cm_present)(CMContext *);
    int        (*cm_set_effect)(CMContext *, CMEffect);
    int        (*cm_pump_events)(CMContext *, CMEvent *, int);
    int        (*cm_readback)(CMContext *, void *, int, int, int);
    int        (*cm_target_size)(CMContext *, int *, int *, int *);
    int        (*cm_render_effect_readback)(CMContext *, CMEffect, void *, int, int, int);
    int        (*cm_abi_version)(void);
    int        (*cm_set_option)(CMContext *, int, long);
    int        (*cm_get_option)(CMContext *, int, long *);
    int        (*cm_open_settings)(CMContext *);
} CMInterface;

/* ---- the AROS-owned framebuffer model -------------------------------------
 * The framebuffer is chunky BGRA8 (blue byte 0 .. alpha byte 3), logical W×H,
 * top-left origin — the §2/§2a contract. put_bgra writes a pixel from an
 * 0xAARRGGBB literal into that byte order; the matching readback get_argb reads
 * it back. (Same packing as abi_test.c so the two harnesses agree.) */
static inline void put_bgra(uint8_t *fb, int fbw, int x, int y, uint32_t argb) {
    uint8_t *p = &fb[((size_t)y * fbw + x) * 4];
    p[0] = (uint8_t)(argb);        /* B (byte 0) */
    p[1] = (uint8_t)(argb >> 8);   /* G (byte 1) */
    p[2] = (uint8_t)(argb >> 16);  /* R (byte 2) */
    p[3] = (uint8_t)(argb >> 24);  /* A (byte 3) */
}
static inline uint32_t get_argb(const uint8_t *buf, int stride, int x, int y) {
    const uint8_t *p = &buf[(size_t)y * stride + (size_t)x * 4];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}

/* Fill a solid logical rect in the framebuffer with one colour. The AROS bitmap
 * class' PutImage/FillRect decompose to exactly this kind of region write. */
static void fb_fill_rect(uint8_t *fb, int fbw, int x, int y, int w, int h, uint32_t argb) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_bgra(fb, fbw, xx, yy, argb);
}

/* ---- watchdog: hard-bound the whole run so a hang is a FAIL, never an infinite
 * hang. A detached thread kills the process after N seconds. The main path is
 * non-blocking by contract (§3), so this should never fire. */
static void *watchdog(void *arg) {
    int secs = *(int *)arg;
    sleep((unsigned)secs);
    fprintf(stderr, "[HIDDSIM] FAIL watchdog fired after %ds (a cm_* blocked?)\n", secs);
    _exit(2);
    return NULL;
}

/* The dirty-rect stream descriptor: a sequence of solid-rect draws. Several
 * overlap on purpose so the harness exercises last-writer-wins compositing, not
 * just disjoint tiles. Coordinates are logical, top-left (§2). */
typedef struct { int x, y, w, h; uint32_t argb; const char *what; } DirtyStep;

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int wd_secs = 20;
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, &wd_secs);
    pthread_detach(wd);

    const char *path = (argc > 1) ? argv[1]
                     : getenv("COCOAMETAL_DYLIB") ? getenv("COCOAMETAL_DYLIB")
                     : "build/cocoametal.dylib";
    printf("[HIDDSIM] HIDD-shaped behavioral harness against %s\n", path);
    printf("[HIDDSIM] (AROS-owned framebuffer, lazy open, dirty-rect stream, "
           "compose + swizzle asserts via the §6 oracle)\n");

    /* ---- HostLib_Open + HostLib_GetInterface (the real load path). --------- */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) { printf("[HIDDSIM] FAIL dlopen(%s): %s\n", path, dlerror()); return 1; }

    CMInterface iface;
    void **slots = (void **)&iface;
    int errcount = 0, idx = 0;
    for (const char **name = cocoametal_symbols; *name; ++name, ++idx) {
        dlerror();
        void *sym = dlsym(handle, *name);
        const char *e = dlerror();
        if (!sym || e) { printf("[HIDDSIM] unresolved [%d] %s : %s\n", idx, *name, e ? e : "NULL"); errcount++; slots[idx] = NULL; }
        else           { slots[idx] = sym; }
    }
    if (errcount != 0) { printf("[HIDDSIM] FAIL %d symbol(s) unresolved\n", errcount); dlclose(handle); return 1; }
    if (iface.cm_abi_version() != CM_ABI_VERSION) {
        printf("[HIDDSIM] FAIL cm_abi_version()=%d != header %d\n",
               iface.cm_abi_version(), CM_ABI_VERSION); dlclose(handle); return 1;
    }
    printf("[HIDDSIM] loaded: 13 symbols resolved (errcount=0), cm_abi_version=%d\n",
           iface.cm_abi_version());

    /* ---- §2a: the PINNED pixel hand-off the AROS gfx class must mirror. ----
     * BGRA8 == MTLPixelFormatBGRA8Unorm, little-endian AArch64: blue byte 0,
     * green 1, red 2, alpha 3; bytesPerPixel=4; top-left origin; logical W×H.
     * These EXACT field values are the contract — a swizzle bug here (or on the
     * AROS side) is what the round-trip assert below catches. */
    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift  = 0,           .greenShift = 8,           .redShift  = 16,         .alphaShift = 24,
        .blueMask   = 0x000000FFu, .greenMask  = 0x0000FF00u, .redMask   = 0x00FF0000u, .alphaMask = 0xFF000000u,
    };
    printf("[HIDDSIM] CMPixelDesc: bpp=%d  shift{B=%d,G=%d,R=%d,A=%d}  "
           "mask{B=%08X,G=%08X,R=%08X,A=%08X}  origin=top-left\n",
           fmt.bytesPerPixel, fmt.blueShift, fmt.greenShift, fmt.redShift, fmt.alphaShift,
           fmt.blueMask, fmt.greenMask, fmt.redMask, fmt.alphaMask);

    const int W = 320, H = 200;
    const int stride = W * 4;     /* AROS framebuffer row stride, bytes */

    /* ---- AROS allocates the framebuffer (§2: the H7 model, AllocMem(W*H*4)).
     * We allocate TWO buffers: `fb` is the live AROS framebuffer we hand to the
     * shim (dirty-rect uploads read from it); `ref` is an independent host-side
     * reference we compose the SAME way with plain memcpy/fills (no Metal) — the
     * oracle of record for the compose assert. Both start cleared (MEMF_CLEAR). */
    uint8_t *fb  = (uint8_t *)calloc((size_t)W * H, 4);
    uint8_t *ref = (uint8_t *)calloc((size_t)W * H, 4);
    uint8_t *rb  = (uint8_t *)calloc((size_t)W * H, 4);  /* readback target */
    if (!fb || !ref || !rb) { printf("[HIDDSIM] FAIL calloc framebuffers\n"); dlclose(handle); return 1; }

    int ok = 1;

    /* ---- LAZY OPEN: first "Show" → cm_open (§4 SDL lazy pattern). The AROS gfx
     * class does this at the first moHidd_Gfx_Show, not at allocation. -------- */
    printf("[HIDDSIM] Show(#1) -> lazy cm_open(%d,%d) ...\n", W, H);
    CMContext *cx = iface.cm_open(W, H, &fmt, "hiddsim");
    if (!cx) {
        /* NULL == no Metal device (headless). Per §4 the AROS side treats NULL as
         * a clean SKIP, not a wiring failure; on this Mac we expect a device. */
        printf("[HIDDSIM] SKIP cm_open returned NULL (no Metal device?)\n");
        free(fb); free(ref); free(rb); dlclose(handle); return 0;
    }

    /* drain any startup events (non-blocking, §3) */
    CMEvent evbuf[32];
    int total_events = 0;
    total_events += iface.cm_pump_events(cx, evbuf, 32);

    /* ---- THE DIRTY-RECT STREAM ------------------------------------------------
     * Several solid-rect draws, some DISJOINT, some OVERLAPPING, each followed by
     * a cm_upload_rect of ONLY the changed sub-rect + cm_present + a pump. This is
     * the many-small-UpdateRects cadence; overlaps prove last-writer-wins compose.
     *
     * Colours are 0xAARRGGBB; put_bgra packs them into the BGRA byte order. */
    const DirtyStep steps[] = {
        {   0,   0,  W,   H, 0xFF202020u, "clear bg (dark grey)" },   /* full background */
        {  20,  20,  80,  60, 0xFFFF0000u, "red box" },               /* disjoint */
        { 140,  30,  90,  50, 0xFF00FF00u, "green box" },             /* disjoint */
        {  40,  90, 120,  70, 0xFF0000FFu, "blue box" },              /* overlaps red box's lower edge */
        {  90,  60,  70,  70, 0xFFFFFF00u, "yellow box" },            /* overlaps red+green+blue (3-way) */
        { 200, 120,  90,  60, 0xFF00FFFFu, "cyan box" },              /* disjoint */
        { 110,  80,  40,  40, 0xFFFF00FFu, "magenta box" },           /* overlaps yellow (last writer) */
    };
    const int nsteps = (int)(sizeof(steps) / sizeof(steps[0]));

    printf("[HIDDSIM] dirty-rect stream: %d incremental UpdateRects "
           "(disjoint + overlapping), present+pump after each\n", nsteps);
    for (int i = 0; i < nsteps; i++) {
        const DirtyStep *s = &steps[i];
        /* AROS draws into ITS framebuffer (PutImage/FillRect would do this). */
        fb_fill_rect(fb, W, s->x, s->y, s->w, s->h, s->argb);
        /* and we mirror the same draw into the independent reference. */
        fb_fill_rect(ref, W, s->x, s->y, s->w, s->h, s->argb);

        /* moHidd_BitMap_UpdateRect -> cm_upload_rect(ONLY the dirty sub-rect) +
         * cm_present. We pass a pointer to the WHOLE framebuffer + stride and the
         * sub-rect coords (§2 hand-off), NOT a full-frame blit. */
        iface.cm_upload_rect(cx, fb, stride, s->x, s->y, s->w, s->h);
        iface.cm_present(cx);

        /* VBlank-style event drain between presents (non-blocking). */
        total_events += iface.cm_pump_events(cx, evbuf, 32);

        printf("[HIDDSIM]   step %d/%d UpdateRect(x=%3d,y=%3d,w=%3d,h=%3d) %-22s present ok\n",
               i + 1, nsteps, s->x, s->y, s->w, s->h, s->what);
    }

    /* ---- COMPOSE ASSERT (§6 oracle, under realistic dirty-rect usage). --------
     * Read back the composed offscreen oracle at logical W×H and assert it is
     * BYTE-EXACT against the host reference. This proves the accumulation of
     * partial uploads composed correctly: regions touched once, regions never
     * re-uploaded after the background, and overlapping rects (last-writer-wins)
     * all match the reference, with NO stale pixels. */
    int rbrc = iface.cm_readback(cx, rb, stride, W, H);
    if (rbrc != 0) { printf("[HIDDSIM] FAIL cm_readback returned %d\n", rbrc); ok = 0; }
    else {
        size_t nbytes = (size_t)W * H * 4;
        if (memcmp(rb, ref, nbytes) == 0) {
            printf("[HIDDSIM]   compose assert: oracle == reference framebuffer, "
                   "BYTE-EXACT (%dx%d BGRA8, %zu bytes)  ok\n", W, H, nbytes);
        } else {
            /* Locate + report the first differing pixel for diagnosis. */
            ok = 0;
            int fx = -1, fy = -1;
            for (int y = 0; y < H && fx < 0; y++)
                for (int x = 0; x < W; x++)
                    if (memcmp(&rb[((size_t)y * W + x) * 4], &ref[((size_t)y * W + x) * 4], 4) != 0) {
                        fx = x; fy = y; break;
                    }
            printf("[HIDDSIM]   compose assert: MISMATCH at first diff (%d,%d) "
                   "oracle=%08X reference=%08X\n", fx, fy,
                   get_argb(rb, stride, fx, fy), get_argb(ref, stride, fx, fy));
        }
        /* Spot-check a handful of named positions so the log states the compose
         * result explicitly (which box won each contested pixel). */
        struct { const char *name; int x, y; uint32_t want; } spots[] = {
            { "bg.grey",        5,   5, 0xFF202020u },   /* never overdrawn */
            { "red.only",      30,  30, 0xFFFF0000u },   /* red box, not overlapped */
            { "green.only",   150,  40, 0xFF00FF00u },   /* green box */
            { "blue.only",     50, 140, 0xFF0000FFu },   /* blue box, below the overlaps */
            { "cyan.only",    240, 150, 0xFF00FFFFu },   /* cyan box */
            { "yellow.wins",  130,  70, 0xFFFFFF00u },   /* yellow drawn after red/green/blue */
            { "magenta.wins", 120,  90, 0xFFFF00FFu },   /* magenta drawn last over yellow */
        };
        for (size_t i = 0; i < sizeof(spots)/sizeof(spots[0]); i++) {
            uint32_t got = get_argb(rb, stride, spots[i].x, spots[i].y);
            int pass = (got == spots[i].want);
            if (!pass) ok = 0;
            printf("[HIDDSIM]   spot %-13s (%3d,%3d) want=%08X got=%08X  %s\n",
                   spots[i].name, spots[i].x, spots[i].y, spots[i].want, got,
                   pass ? "ok" : "MISMATCH");
        }
    }

    /* ---- SWIZZLE / PIXFMT ROUND-TRIP. -----------------------------------------
     * Write ONE known pixel with four DISTINCT byte values, upload+present+
     * readback, and confirm each colour byte landed in its asserted BGRA position.
     * Distinct values (B=0x11, G=0x22, R=0x33, A=0xFF) make any swap detectable:
     * an RGBA/BGRA swizzle would show as R and B exchanged in the readback. We
     * assert both the packed 0xAARRGGBB value AND the raw bytes in memory order. */
    {
        const uint8_t B = 0x11, G = 0x22, R = 0x33, A = 0xFF;
        const uint32_t argb = ((uint32_t)A << 24) | ((uint32_t)R << 16) |
                              ((uint32_t)G << 8)  |  (uint32_t)B;        /* 0xFF332211 */
        const int px = 7, py = 11;
        put_bgra(fb, W, px, py, argb);                 /* AROS writes the pixel    */
        put_bgra(ref, W, px, py, argb);                /* keep the reference in sync */
        iface.cm_upload_rect(cx, fb, stride, px, py, 1, 1);   /* a 1×1 dirty rect  */
        iface.cm_present(cx);
        total_events += iface.cm_pump_events(cx, evbuf, 32);

        if (iface.cm_readback(cx, rb, stride, W, H) != 0) {
            printf("[HIDDSIM] FAIL swizzle readback returned nonzero\n"); ok = 0;
        } else {
            const uint8_t *p = &rb[((size_t)py * W + px) * 4];
            int byte_ok = (p[0] == B && p[1] == G && p[2] == R && p[3] == A);
            uint32_t got = get_argb(rb, stride, px, py);
            int packed_ok = (got == argb);
            if (!byte_ok || !packed_ok) ok = 0;
            printf("[HIDDSIM]   swizzle/round-trip @(%d,%d): "
                   "bytes B=%02X G=%02X R=%02X A=%02X (want %02X %02X %02X %02X) %s; "
                   "packed got=%08X want=%08X %s\n",
                   px, py, p[0], p[1], p[2], p[3], B, G, R, A,
                   byte_ok ? "ok" : "SWIZZLED",
                   got, argb, packed_ok ? "ok" : "MISMATCH");
            printf("[HIDDSIM]   -> proves blue@byte0, green@byte1, red@byte2, "
                   "alpha@byte3 survive upload+present+readback (no swizzle)\n");
        }
    }

    /* ---- threading-model sanity: cm_pump_events stayed non-blocking throughout
     * (§3) — the watchdog never fired and the drains returned promptly. */
    printf("[HIDDSIM]   cm_pump_events drained %d event(s) total across the stream "
           "(non-blocking, no watchdog fire)  ok\n", total_events);

    int tw = 0, th = 0, sc = 0;
    iface.cm_target_size(cx, &tw, &th, &sc);
    printf("[HIDDSIM]   cm_target_size -> %dx%d scale=%d (oracle read at logical %dx%d)\n",
           tw, th, sc, W, H);

    free(fb); free(ref); free(rb);
    iface.cm_close(cx);
    dlclose(handle);

    if (ok) {
        printf("[HIDDSIM] PASS dirty-rect stream composes byte-exact vs reference "
               "(%dx%d BGRA8, %d UpdateRects, overlapping last-writer-wins) + "
               "pixfmt round-trips with no swizzle\n", W, H, nsteps);
        return 0;
    }
    printf("[HIDDSIM] FAIL see checks above\n");
    return 1;
}
