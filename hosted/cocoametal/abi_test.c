/* abi_test.c — the dlopen-based ABI conformance test for cocoametal.dylib
 * (INTERFACE.md §8 item 2 — the highest-value de-risk of the seam).
 *
 * Implemented from docs/features/cocoa-metal-display/INTERFACE.md (§1 load
 * sequence, §1a frozen symbol array, §2 pixel/geometry, §6 readback oracle, §7
 * versioning) + cocoametal.h. Independent work: no third-party implementation
 * source — emulator, agent, driver, or otherwise — was read, searched, or consulted
 * in producing it, and any resemblance to existing implementations is coincidental.
 *
 * This file is PLAIN C and links NONE of the .m files. It exercises the shim the
 * exact way the AROS side does at runtime:
 *
 *     HostLibBase = OpenResource("hostlib.resource");
 *     handle = HostLib_Open("cocoametal.dylib", &errstr);            // dlopen
 *     CMIFace = HostLib_GetInterface(handle, cocoametal_symbols, &errcount);
 *     // errcount MUST be 0 — it counts symbols that failed to resolve.
 *
 * We model HostLib_Open as dlopen() and HostLib_GetInterface as: walk the
 * NULL-terminated cocoametal_symbols[] array, dlsym each name into a struct of
 * function pointers IN ARRAY ORDER, and count unresolved names into errcount.
 * That is precisely hostlib.resource's contract (the struct mirrors the array,
 * append-only). If errcount != 0 the AROS loader would refuse the driver, so the
 * whole point of this test is to assert errcount == 0 against the REAL dylib.
 *
 * Then it drives the frozen call sequence through the resolved function pointers
 * (never against directly-linked symbols) and asserts the §6 readback oracle.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cocoametal.h"   /* the shared ABI header — types only, no shim code */

/* ---- §1a: the frozen, NULL-terminated symbol array (shared with the AROS side).
 * Order = cocoametal.h declaration order. Append-only; cm_abi_version is entry 9,
 * then the v2 settings/options ABI (§9) at 10/11/12. 13 names + NULL terminator.
 * This is the literal names[] HostLib_GetInterface resolves. */
static const char *cocoametal_symbols[] =
{
    "cm_open",                     /* 0 */
    "cm_close",                    /* 1 */
    "cm_upload_rect",              /* 2 */
    "cm_present",                  /* 3 */
    "cm_set_effect",               /* 4 */
    "cm_pump_events",              /* 5 */
    "cm_readback",                 /* 6 */
    "cm_target_size",              /* 7 */
    "cm_render_effect_readback",   /* 8 */
    "cm_abi_version",              /* 9 — appended per §7 */
    "cm_set_option",               /* 10 — appended at v2 per §9 */
    "cm_get_option",               /* 11 — appended at v2 per §9 */
    "cm_open_settings",            /* 12 — appended at v2 per §9 */
    NULL
};

/* ---- §1b: the interface struct, mirroring the array EXACTLY (same order). This
 * is the host-side stand-in for the AROS CMInterface struct. */
typedef struct {
    CMContext *(*cm_open)(int w, int h, const CMPixelDesc *fmt, const char *title);
    void       (*cm_close)(CMContext *);
    void       (*cm_upload_rect)(CMContext *, const void *src, int srcStride,
                                 int x, int y, int w, int h);
    void       (*cm_present)(CMContext *);
    int        (*cm_set_effect)(CMContext *, CMEffect effect);
    int        (*cm_pump_events)(CMContext *, CMEvent *out, int maxEvents);
    int        (*cm_readback)(CMContext *, void *dst, int dstStride, int w, int h);
    int        (*cm_target_size)(CMContext *, int *outW, int *outH, int *outScale);
    int        (*cm_render_effect_readback)(CMContext *, CMEffect effect,
                                            void *dst, int dstStride, int w, int h);
    int        (*cm_abi_version)(void);
    int        (*cm_set_option)(CMContext *, int key, long value);       /* v2 §9 */
    int        (*cm_get_option)(CMContext *, int key, long *value);      /* v2 §9 */
    int        (*cm_open_settings)(CMContext *);                         /* v2 §9 */
} CMInterface;

/* The four-quadrant + marker scene the §6 oracle verifies (same colours as the
 * D1 test, packed BGRA at upload time, read back as 0xAARRGGBB). */
#define C_TL   0xFFFF0000u   /* top-left:     red    */
#define C_TR   0xFF00FF00u   /* top-right:    green  */
#define C_BL   0xFF0000FFu   /* bottom-left:  blue   */
#define C_BR   0xFFFFFF00u   /* bottom-right: yellow */
#define C_MARK 0xFFFF00FFu   /* marker pixel: magenta */

static inline void put_bgra(uint8_t *fb, int fbw, int x, int y, uint32_t argb) {
    uint8_t *p = &fb[((size_t)y * fbw + x) * 4];
    p[0] = (uint8_t)(argb);        /* B */
    p[1] = (uint8_t)(argb >> 8);   /* G */
    p[2] = (uint8_t)(argb >> 16);  /* R */
    p[3] = (uint8_t)(argb >> 24);  /* A */
}
static inline uint32_t get_argb(const uint8_t *buf, int stride, int x, int y) {
    const uint8_t *p = &buf[(size_t)y * stride + (size_t)x * 4];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}
static void build_scene(uint8_t *fb, int w, int h, int markX, int markY) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = (x < w/2) ? ((y < h/2) ? C_TL : C_BL)
                                   : ((y < h/2) ? C_TR : C_BR);
            put_bgra(fb, w, x, y, c);
        }
    put_bgra(fb, w, markX, markY, C_MARK);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* The dlopen path is the one the AROS HostLib_Open uses. Default to the
     * build artifact relative to the repo root (where the harness runs); allow an
     * override as argv[1] or $COCOAMETAL_DYLIB for flexibility. */
    const char *path = (argc > 1) ? argv[1]
                     : getenv("COCOAMETAL_DYLIB") ? getenv("COCOAMETAL_DYLIB")
                     : "build/cocoametal.dylib";

    printf("[ABI] dlopen-based conformance test against %s\n", path);

    /* ---- HostLib_Open: dlopen the shim. ---------------------------------- */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        printf("[ABI] FAIL dlopen(%s): %s\n", path, dlerror());
        return 1;
    }

    /* ---- HostLib_GetInterface: resolve every name in cocoametal_symbols[]
     * into the interface struct IN ARRAY ORDER; errcount counts failures. This
     * is the literal contract — errcount MUST be 0. ----------------------- */
    CMInterface iface;
    void **slots = (void **)&iface;   /* fill the struct slot-by-slot, in order */
    int errcount = 0, idx = 0;
    for (const char **name = cocoametal_symbols; *name; ++name, ++idx) {
        dlerror();                                  /* clear */
        void *sym = dlsym(handle, *name);
        const char *e = dlerror();
        if (!sym || e) {
            printf("[ABI]   unresolved [%d] %-26s : %s\n", idx, *name, e ? e : "NULL");
            errcount++;
            slots[idx] = NULL;
        } else {
            printf("[ABI]   resolved   [%d] %-26s -> %p\n", idx, *name, sym);
            slots[idx] = sym;
        }
    }
    printf("[ABI]   errcount = %d (HostLib_GetInterface contract: MUST be 0)\n", errcount);
    if (errcount != 0) {
        printf("[ABI] FAIL %d symbol(s) unresolved\n", errcount);
        dlclose(handle);
        return 1;
    }

    /* ---- §7: ABI version handshake. The AROS loader refuses on mismatch. - */
    int ver = iface.cm_abi_version();
    printf("[ABI]   cm_abi_version() = %d (header CM_ABI_VERSION = %d)  %s\n",
           ver, CM_ABI_VERSION, (ver == CM_ABI_VERSION) ? "ok" : "MISMATCH");
    if (ver != CM_ABI_VERSION) {
        printf("[ABI] FAIL cm_abi_version mismatch\n");
        dlclose(handle);
        return 1;
    }

    /* ---- the real frozen sequence, all through the resolved pointers. ---- */
    const int w = 320, h = 200;
    const int markX = w / 7, markY = h * 7 / 15;   /* arbitrary, in-bounds */

    /* §2 pixel contract: BGRA8, blue byte 0 .. alpha byte 3. */
    CMPixelDesc fmt = {
        .bytesPerPixel = 4,
        .blueShift = 0, .greenShift = 8, .redShift = 16, .alphaShift = 24,
        .blueMask = 0x000000FF, .greenMask = 0x0000FF00,
        .redMask  = 0x00FF0000, .alphaMask = 0xFF000000,
    };

    CMContext *cx = iface.cm_open(w, h, &fmt, "abi");
    if (!cx) {
        /* NULL == no Metal device (headless). Per §4 the caller treats NULL as a
         * clean SKIP, not a wiring failure — but on this Mac we expect a device. */
        printf("[ABI] SKIP cm_open returned NULL (no Metal device?)\n");
        dlclose(handle);
        return 0;
    }

    uint8_t *fb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!fb) { printf("[ABI] FAIL calloc framebuffer\n"); iface.cm_close(cx); dlclose(handle); return 1; }
    build_scene(fb, w, h, markX, markY);

    /* §4: upload the whole framebuffer rect, then present (oracle pass). */
    iface.cm_upload_rect(cx, fb, w * 4, 0, 0, w, h);
    iface.cm_present(cx);

    /* §6 oracle: read back the offscreen target at logical w*h and assert the
     * four quadrants + the marker are EXACT. This is the verification contract. */
    int stride = w * 4;
    uint8_t *rb = (uint8_t *)calloc((size_t)w * h, 4);
    if (!rb) { printf("[ABI] FAIL calloc readback\n"); free(fb); iface.cm_close(cx); dlclose(handle); return 1; }
    int rbrc = iface.cm_readback(cx, rb, stride, w, h);
    if (rbrc != 0) {
        printf("[ABI] FAIL cm_readback returned %d\n", rbrc);
        free(fb); free(rb); iface.cm_close(cx); dlclose(handle); return 1;
    }

    int ok = 1;
    struct { const char *name; int x, y; uint32_t want; } checks[] = {
        { "TL.red",     w/4,   h/4,   C_TL },
        { "TR.green", 3*w/4,   h/4,   C_TR },
        { "BL.blue",    w/4, 3*h/4,   C_BL },
        { "BR.yellow",3*w/4, 3*h/4,   C_BR },
        { "marker",   markX,  markY,  C_MARK },
    };
    for (size_t i = 0; i < sizeof(checks)/sizeof(checks[0]); i++) {
        uint32_t got = get_argb(rb, stride, checks[i].x, checks[i].y);
        int pass = (got == checks[i].want);
        if (!pass) ok = 0;
        printf("[ABI]   oracle %-10s (%4d,%4d) want=%08X got=%08X  %s\n",
               checks[i].name, checks[i].x, checks[i].y,
               checks[i].want, got, pass ? "ok" : "MISMATCH");
    }

    /* §3: cm_pump_events is a NON-BLOCKING drain that must return (no events on a
     * headless/quiet context). We just confirm it returns a sane count and does
     * not block the test. */
    CMEvent evbuf[16];
    int nev = iface.cm_pump_events(cx, evbuf, 16);
    if (nev < 0) { ok = 0; printf("[ABI]   cm_pump_events returned %d (MISMATCH)\n", nev); }
    else         { printf("[ABI]   cm_pump_events drained %d event(s) (non-blocking)  ok\n", nev); }

    /* §4 sanity: cm_target_size reports (tw,th,scale) with tw==w*scale etc., and
     * cm_set_effect accepts a valid effect and rejects an out-of-range one. */
    int tw = 0, th = 0, scale = 0;
    int tsz = iface.cm_target_size(cx, &tw, &th, &scale);
    int tsz_ok = (tsz == 0 && scale >= 1 && tw == w * scale && th == h * scale);
    if (!tsz_ok) ok = 0;
    printf("[ABI]   cm_target_size -> %dx%d scale=%d (expect %dx%d)  %s\n",
           tw, th, scale, w * scale, h * scale, tsz_ok ? "ok" : "MISMATCH");

    int fx_ok  = (iface.cm_set_effect(cx, CM_FX_NEAREST) == 0);
    int fx_bad = (iface.cm_set_effect(cx, (CMEffect)999) != 0);   /* must reject */
    if (!fx_ok || !fx_bad) ok = 0;
    printf("[ABI]   cm_set_effect(NEAREST)=accepted:%s  cm_set_effect(999)=rejected:%s\n",
           fx_ok ? "yes" : "NO", fx_bad ? "yes" : "NO");

    /* §9 (v2) sanity through the resolved pointers: a host-owned option round-trips
     * (set then get), an unknown key is rejected, and cm_open_settings is callable
     * (best-effort: 0 if a panel came up, nonzero if no window server — both fine). */
    long ov = -1;
    int opt_set = (iface.cm_set_option(cx, CM_OPT_SCALE_MODE, CM_SCALE_INTEGER_NEAREST) == 0);
    int opt_get = (iface.cm_get_option(cx, CM_OPT_SCALE_MODE, &ov) == 0 &&
                   ov == CM_SCALE_INTEGER_NEAREST);
    int opt_bad = (iface.cm_set_option(cx, 0x7FFF, 1) != 0);     /* unknown key rejected */
    if (!opt_set || !opt_get || !opt_bad) ok = 0;
    printf("[ABI]   cm_set/get_option(SCALE_MODE)=roundtrip:%s(%ld)  unknownKey=rejected:%s\n",
           (opt_set && opt_get) ? "yes" : "NO", ov, opt_bad ? "yes" : "NO");
    int os = iface.cm_open_settings(cx);   /* best-effort; either result is acceptable */
    printf("[ABI]   cm_open_settings -> %d (%s)\n", os,
           os == 0 ? "panel up" : "no window server / no-op");

    free(fb);
    free(rb);
    iface.cm_close(cx);

    /* HostLib_Close. */
    dlclose(handle);

    if (ok) {
        printf("[ABI] PASS errcount=0 (13 symbols), cm_abi_version=%d, oracle quadrants+marker "
               "exact, pump/target/effect + set/get_option/open_settings sane\n", ver);
        return 0;
    }
    printf("[ABI] FAIL see checks above\n");
    return 1;
}
