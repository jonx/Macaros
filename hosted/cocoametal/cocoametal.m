/* cocoametal.m — Apple-native Cocoa/Metal display HIDD host shim.
 *
 * Implemented clean-room from docs/features/cocoa-metal-display/spec.md
 * ("Metal pipeline (host shim internals)"). No GPL emulator source
 * (vAmiga/WinUAE/FS-UAE/Amiberry/E-UAE) was read, searched, or consulted — the
 * two MSL shaders below were authored from scratch. Sources: Apple's
 * Metal/QuartzCore/Foundation docs [PUB] + this project's H7 spike
 * (hosted/display.c) for the readback/PNG/pixel-assert discipline [OURS].
 *
 * This is the D1-phase host shim: it owns every Cocoa/Metal object and exposes
 * the flat C ABI in cocoametal.h. It pulls no AROS headers. The offscreen
 * BGRA8 target is the source of truth for unattended verification (cm_readback)
 * and is ALWAYS rendered with the fixed pass-through/nearest pipeline; the
 * on-screen CAMetalLayer/NSWindow present is a best-effort, non-fatal bonus and
 * is the home of the selectable fragment-effect stage (cm_set_effect). The live
 * present is a RENDER PASS that draws the framebuffer texture into the drawable
 * as a color attachment (not a blit copy), which is why layer.framebufferOnly =
 * YES is correct — the drawable is a render target, never a copy destination.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* Precompiled shader library, generated from cmshader.metal at build time. Used
 * so cm_open does not need the runtime Metal shader compiler (MTLCompilerService),
 * which is unreachable when AROS runs in AROSBootstrap's fork()ed child. */
#include "cmshader_metallib.h"

#include <signal.h>
#include <pthread.h>
/* Hop a cm_* call onto the main thread (AppKit/Metal require it; under the AROS
 * "inversion" cm_* are called from AROS's dedicated thread). ALL signals are
 * blocked on the calling (AROS) thread for the duration of the sync: AROS's
 * scheduler is signal-driven, and a SIGALRM landing mid-dispatch_sync would make
 * AROS context-switch tasks from inside a host syscall and corrupt the task
 * stack. The main thread (which runs the block) is unaffected. */
static void cm__sync_main(dispatch_block_t block) {
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    dispatch_sync(dispatch_get_main_queue(), block);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}
#import <QuartzCore/QuartzCore.h>

#include <string.h>
#include "cocoametal.h"

/* The full-screen-triangle pipeline, MSL, embedded and compiled at cm_open.
 * Authored from scratch (no shader was copied from any reference):
 *   - vertex vs_fulltri: emit one of three vertices covering the whole clip
 *     space from vertex_id (the standard "oversized triangle" trick), and the
 *     matching UV. We flip V so texture row 0 (framebuffer top) lands at the top
 *     of the target (Metal's render-target origin is top-left, clip +Y is up).
 *   - fragment fs_sample: nearest pass-through. This is the ORACLE fragment —
 *     the offscreen target that cm_readback verifies always uses exactly this.
 *   - fragment fs_effect: the present-time effect stage. It takes the target
 *     pixel size + an effect selector in a small constant buffer and branches:
 *       CM_FX_NEAREST (0)  -> identical pass-through (so effect 0 == oracle).
 *       CM_FX_SCANLINE (1) -> a simple CRT look: darken odd target rows and
 *                             apply a gamma curve. Authored from scratch; the
 *                             constants are deliberate and explained inline.
 *     The effect runs ONLY on the present/[D]-check path, never on the oracle.
 *
 * The selector values below must stay in sync with enum CMEffect in cocoametal.h.
 */
static NSString *const kMSL =
@"#include <metal_stdlib>\n"
@"using namespace metal;\n"
@"struct VOut { float4 pos [[position]]; float2 uv; };\n"
@"struct FxParams { uint effect; uint targetW; uint targetH; uint _pad; };\n"
@"vertex VOut vs_fulltri(uint vid [[vertex_id]]) {\n"
@"    float2 p = float2((vid << 1) & 2, vid & 2);\n"   /* (0,0),(2,0),(0,2) */
@"    VOut o;\n"
@"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"        /* -> (-1,-1)..(3,-1)..(-1,3) */
@"    o.uv  = float2(p.x, 1.0 - p.y);\n"                 /* flip V: top row at top */
@"    return o;\n"
@"}\n"
@"fragment float4 fs_sample(VOut in [[stage_in]],\n"
@"                          texture2d<float> tex [[texture(0)]],\n"
@"                          sampler smp [[sampler(0)]]) {\n"
@"    return tex.sample(smp, in.uv);\n"
@"}\n"
@"fragment float4 fs_effect(VOut in [[stage_in]],\n"
@"                          texture2d<float> tex [[texture(0)]],\n"
@"                          sampler smp [[sampler(0)]],\n"
@"                          constant FxParams& fx [[buffer(0)]]) {\n"
@"    float4 c = tex.sample(smp, in.uv);\n"
@"    if (fx.effect == 1u) {\n"                          /* CM_FX_SCANLINE */
@"        // Darken every other DESTINATION row (a CRT scanline gap). in.pos.y is\n"
@"        // the target pixel centre (0.5, 1.5, ...); its integer floor parity\n"
@"        // selects odd rows to dim. 0.55 leaves the gap clearly darker but not\n"
@"        // black, so colour is still readable.\n"
@"        uint row = uint(in.pos.y);\n"
@"        float scan = (row & 1u) ? 0.55 : 1.0;\n"
@"        c.rgb *= scan;\n"
@"        // Simple gamma lift (encode with 1/1.25) to mimic a CRT's brighter\n"
@"        // midtones. Applied after scanline so the gap stays a true ratio.\n"
@"        c.rgb = pow(c.rgb, float3(1.0 / 1.25));\n"
@"    }\n"
@"    return c;\n"                                        /* effect 0: pass-through */
@"}\n";

/* Mirror of the MSL FxParams constant buffer (same layout/order). */
typedef struct { uint32_t effect, targetW, targetH, _pad; } CMFxParams;

struct CMContext {
    int w, h;            /* logical framebuffer dimensions */
    int scale;           /* integer upscale (backing factor; 1 if no window) */
    int tw, th;          /* offscreen target dimensions = w*scale x h*scale */
    CMPixelDesc fmt;

    CMEffect                 effect;      /* present-time effect (default CM_FX_NEAREST) */

    /* Host-owned options (INTERFACE.md §9). Acted on the LIVE present path only;
     * the offscreen oracle is never touched by any of these (§6). */
    CMScaleMode              scaleMode;   /* CM_OPT_SCALE_MODE (default CM_SCALE_FIT) */
    int                      fullscreen;  /* CM_OPT_FULLSCREEN  (0/1)               */
    CMFilter                 filter;      /* CM_OPT_FILTER      (present sampler)   */

    /* Last AROS-facing values the user requested in the panel. The host does NOT
     * act on these (they are surfaced via CM_EV_SETTING); it only records them so
     * cm_get_option can drive the panel UI and tests. */
    long                     reqModeW, reqModeH, reqKeymap, reqAudioVolume;

    /* Pending CM_EV_SETTING queue. cm_set_option of an AROS-facing key enqueues
     * {key,x,y}; cm_pump_events drains it (pull-only — never a callback into AROS,
     * §3). One (main) thread touches this, like the close/resize flags. */
    struct { int key, x, y; } setq[32];
    int                      setHead, setTail;   /* ring indices (count = tail-head) */

    id<MTLDevice>            device;
    id<MTLCommandQueue>      queue;
    id<MTLRenderPipelineState> pipeline;        /* oracle: pass-through nearest */
    id<MTLRenderPipelineState> effectPipeline;  /* present/[D]: effect-capable */
    id<MTLSamplerState>      sampler;       /* nearest (oracle + default present) */
    id<MTLSamplerState>      linearSampler; /* CM_FILTER_LINEAR (present only)    */
    id<MTLTexture>           fbTex;       /* w x h, framebuffer (shared, CPU writes) */
    id<MTLTexture>           offTex;      /* tw x th, render target + shaderRead (shared) */

    /* Live present path (best-effort; nil when headless / no window server). */
    CAMetalLayer            *layer;       /* weak-ish: owned by the window's view */
    void                    *window;      /* NSWindow* held as void* (no AppKit in header) */
    int                      haveWindow;

    /* Diagnostics for the D2t threading-model probe ONLY (not part of the frozen
     * ABI; reached via the internal cm__present_stats accessor, never exported in
     * the dylib). Count cm_present calls and how many acquired a live drawable —
     * so D2t can report, honestly, whether nextDrawable works under the
     * hand-pumped CFRunLoop model (no NSApplicationMain / no [NSApp run]). */
    int                      presentCount;
    int                      drawableCount;

    /* Input: window-management transitions surfaced by the AppKit delegate
     * (cocoametal_window.m) and drained as CMEvents by cm_pump_events. A close-
     * button click / resize is a delegate callback or notification, NOT a plain
     * dequeuable NSEvent, so the delegate sets these and the pump turns them into
     * CM_EV_CLOSE / CM_EV_RESIZE. Plain ints toggled on one (main) thread. */
    int                      closePending;
    int                      resizePending;
};

/* Live-window helpers: weak default (headless) here, strong override in
 * cocoametal_window.m when AppKit is linked. Forward-declared so cm_open can
 * call them before their definitions. */
void cm_try_window(CMContext *cx, const char *title);
void cm_destroy_window(CMContext *cx);

/* §9: load persisted host-owned options from NSUserDefaults at the end of
 * cm_open. Strong override in cocoametal_settings.m; weak no-op stub below.
 * Forward-declared here so cm_open can call it before the stub's definition. */
void cm__apply_persisted_options(CMContext *cx);

/* §9 CM_OPT_FULLSCREEN: enter/exit REAL AppKit native fullscreen on the live
 * window. Strong override in cocoametal_window.m (does the toggleFullScreen: under
 * the hand-pumped CFRunLoop, §3); weak no-op stub below for the headless / no-
 * AppKit build. cm_set_option calls it after recording the flag. Async + non-
 * blocking: it REQUESTS the (animated) transition and returns — it never waits for
 * the animation to finish (no cm_* may block, §3). cm__window_is_fullscreen
 * reports the window's CURRENT styleMask state (so tests/the present path can see
 * whether the transition has completed). Both are forward-declared here so
 * cm_set_option can call them before the stubs' definitions. */
void cm__set_fullscreen_appkit(CMContext *cx, int on);
int  cm__window_is_fullscreen(CMContext *cx);

/* Resync the live CAMetalLayer's drawableSize/contentsScale to the content view's
 * current backing-pixel size (INTERFACE.md §2a: the layer-fills-the-content-view
 * contract). Strong override in cocoametal_window.m; weak no-op stub below for the
 * headless / no-AppKit build. cm_present calls it just before nextDrawable so the
 * live drawable always matches the window/screen even if AppKit deferred a -layout
 * pass under the hand-pumped run loop (which is what left the framebuffer a small
 * rect after a fullscreen enter). Non-blocking; pure geometry. */
void cm__resync_layer(CMContext *cx);

/* Build a texture descriptor for a BGRA8 shared texture. */
static MTLTextureDescriptor *bgra8_desc(int w, int h, MTLTextureUsage usage) {
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModeShared;   /* unified memory: CPU + GPU, no copy */
    return d;
}

CMContext *cm_open(int w, int h, const CMPixelDesc *fmt, const char *title) {
    /* The cm_* entry points may be called from AROS's thread (the "inversion":
     * a Cocoa-app host owns the main thread + run loop; AROS runs on a dedicated
     * thread). AppKit/Metal work must run on the main thread, so a non-main
     * caller hops there. This keeps cocoametal's original single-display-server-
     * thread model intact -- that thread is now simply the main thread. */
    if (![NSThread isMainThread]) {
        __block CMContext *r = NULL;
        cm__sync_main(^{ r = cm_open(w, h, fmt, title); });
        return r;
    }
    (void)title;
    if (w <= 0 || h <= 0) return NULL;

    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) return NULL;                 /* caller treats NULL as SKIP */

    CMContext *cx = (CMContext *)calloc(1, sizeof(*cx));
    if (!cx) return NULL;
    cx->device = dev;
    cx->w = w; cx->h = h;
    if (fmt) cx->fmt = *fmt;

    cx->queue = [dev newCommandQueue];
    if (!cx->queue) { cm_close(cx); return NULL; }

    /* Build the render pipeline from the PRECOMPILED shader library. Loading a
     * prebuilt .metallib needs no runtime shader compiler, so it works in a
     * fork()ed child where MTLCompilerService (XPC) is unreachable. Fall back to
     * compiling kMSL at runtime if the embedded library can't be loaded. */
    NSError *err = nil;
    dispatch_data_t libdata =
        dispatch_data_create(cmshader_metallib, cmshader_metallib_len,
                             dispatch_get_main_queue(),
                             DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> lib = [dev newLibraryWithData:libdata error:&err];
    if (!lib) {
        fprintf(stderr, "cm_open: embedded metallib load failed (%s); trying runtime MSL\n",
                err ? err.localizedDescription.UTF8String : "?");
        err = nil;
        lib = [dev newLibraryWithSource:kMSL options:nil error:&err];
    }
    if (!lib) {
        fprintf(stderr, "cm_open: shader library unavailable: %s\n",
                err ? err.localizedDescription.UTF8String : "?");
        cm_close(cx); return NULL;
    }
    id<MTLFunction> vfn = [lib newFunctionWithName:@"vs_fulltri"];
    id<MTLFunction> ffn = [lib newFunctionWithName:@"fs_sample"];
    id<MTLFunction> efn = [lib newFunctionWithName:@"fs_effect"];

    /* Oracle pipeline: fixed pass-through nearest -> offscreen target. */
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    cx->pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!cx->pipeline) {
        fprintf(stderr, "cm_open: pipeline failed: %s\n",
                err ? err.localizedDescription.UTF8String : "?");
        cm_close(cx); return NULL;
    }

    /* Present/effect pipeline: same vertex, effect-capable fragment. Used by the
     * render-pass present (into the live drawable) and the [D] readback check.
     * Same BGRA8 color attachment as the oracle and the drawable. */
    MTLRenderPipelineDescriptor *ped = [[MTLRenderPipelineDescriptor alloc] init];
    ped.vertexFunction = vfn;
    ped.fragmentFunction = efn;
    ped.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    cx->effectPipeline = [dev newRenderPipelineStateWithDescriptor:ped error:&err];
    if (!cx->effectPipeline) {
        fprintf(stderr, "cm_open: effect pipeline failed: %s\n",
                err ? err.localizedDescription.UTF8String : "?");
        cm_close(cx); return NULL;
    }
    cx->effect = CM_FX_NEAREST;

    /* Nearest sampler, clamp — integer-crisp pixels, no blur. The oracle pass and
     * the default present both use this. */
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    cx->sampler = [dev newSamplerStateWithDescriptor:sd];

    /* Linear sampler for CM_OPT_FILTER == CM_FILTER_LINEAR. Present path only —
     * the oracle pass always uses cx->sampler (nearest), so readback is unchanged
     * (§6). */
    MTLSamplerDescriptor *ld = [[MTLSamplerDescriptor alloc] init];
    ld.minFilter = MTLSamplerMinMagFilterLinear;
    ld.magFilter = MTLSamplerMinMagFilterLinear;
    ld.sAddressMode = MTLSamplerAddressModeClampToEdge;
    ld.tAddressMode = MTLSamplerAddressModeClampToEdge;
    cx->linearSampler = [dev newSamplerStateWithDescriptor:ld];

    /* Host-owned option defaults. Then load any persisted values from
     * NSUserDefaults (§9: load at cm_open, save on change) — the strong override
     * in cocoametal_settings.m applies them via cm_set_option; the headless weak
     * stub is a no-op so the defaults above stand. */
    /* Default live-present scaling: aspect-preserving fill with a BLACK letterbox
     * (§2a). FILLS the window/screen as much as possible without distorting the
     * logical 320x200; the bars are black, never white. The offscreen oracle is
     * never touched by this (§6 — the oracle pass always uses the full-target FIT). */
    cx->scaleMode = CM_SCALE_ASPECT_FIT;
    cx->fullscreen = 0;
    cx->filter = CM_FILTER_NEAREST;
    cx->setHead = cx->setTail = 0;

    /* Try to bring up a live window + CAMetalLayer (non-fatal). Sets cx->scale
     * to the backing factor if it succeeds; otherwise scale stays 0. */
    cm_try_window(cx, title);
    if (cx->scale <= 0) cx->scale = 1;     /* headless / no window: 1x */
    cx->tw = w * cx->scale;
    cx->th = h * cx->scale;

    /* Framebuffer texture: shared, CPU writes via replaceRegion. */
    cx->fbTex = [dev newTextureWithDescriptor:bgra8_desc(w, h, MTLTextureUsageShaderRead)];
    /* Offscreen target: render-to + shaderRead, shared so the CPU can read it. */
    cx->offTex = [dev newTextureWithDescriptor:
        bgra8_desc(cx->tw, cx->th, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)];
    if (!cx->fbTex || !cx->offTex) { cm_close(cx); return NULL; }

    /* §9: load persisted host-owned options now (no-op headless). */
    cm__apply_persisted_options(cx);

    return cx;
}

void cm_close(CMContext *cx) {
    if (!cx) return;
    if (![NSThread isMainThread]) { cm__sync_main(^{ cm_close(cx); }); return; }
    /* ARC releases the Obj-C objects when the struct fields are cleared / freed;
     * but we must drop the window explicitly (it's held as void*). */
    if (cx->window) { cm_destroy_window(cx); }
    cx->fbTex = nil; cx->offTex = nil; cx->pipeline = nil;
    cx->effectPipeline = nil; cx->sampler = nil; cx->linearSampler = nil;
    cx->queue = nil; cx->device = nil; cx->layer = nil;
    free(cx);
}

void cm_upload_rect(CMContext *cx, const void *src, int srcStride,
                    int x, int y, int w, int h) {
    if (!cx || !src) return;
    if (![NSThread isMainThread]) {
        cm__sync_main(^{ cm_upload_rect(cx, src, srcStride, x, y, w, h); });
        return;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > cx->w || y + h > cx->h) return;
    /* src points at the top-left of the WHOLE framebuffer; offset to the rect. */
    const uint8_t *base = (const uint8_t *)src + (size_t)y * srcStride + (size_t)x * 4;
    MTLRegion region = MTLRegionMake2D((NSUInteger)x, (NSUInteger)y,
                                       (NSUInteger)w, (NSUInteger)h);
    [cx->fbTex replaceRegion:region
                 mipmapLevel:0
                   withBytes:base
                 bytesPerRow:(NSUInteger)srcStride];
}

/* Downsample a tw x th BGRA8 buffer to logical w x h (defined below; used by
 * cm_readback before its definition). */
static void downsample_logical(const uint8_t *full, int tw,
                               uint8_t *dst, int dstStride, int w, int h, int s);

/* Encode one full-screen-triangle pass: sample fbTex through `pipeline` and draw
 * the result, upscaled, into `dst` (the offscreen target or a drawable texture).
 * The destination's pixel size is passed as FxParams so the effect shader can do
 * destination-row math; the oracle pipeline ignores it. dstW/dstH are the target
 * size in pixels (so a drawable that differs from offTex still scales correctly).
 */
static void encode_pass(CMContext *cx, id<MTLCommandBuffer> cb,
                        id<MTLRenderPipelineState> pipeline,
                        id<MTLTexture> dst, CMEffect effect,
                        int dstW, int dstH) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pipeline];
    [enc setFragmentTexture:cx->fbTex atIndex:0];
    [enc setFragmentSamplerState:cx->sampler atIndex:0];
    CMFxParams fx = { (uint32_t)effect, (uint32_t)dstW, (uint32_t)dstH, 0 };
    [enc setFragmentBytes:&fx length:sizeof(fx) atIndex:0];  /* ignored by oracle pipeline */
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

/* Compute the LIVE-present viewport for the selected scale mode. The full-screen
 * triangle maps clip space to the whole render-target by default (CM_SCALE_FIT —
 * stretch). For the other modes we centre a sub-rect of the drawable and let the
 * MTLLoadActionClear fill the surrounding pixels black (letterbox/pillarbox):
 *   CM_SCALE_INTEGER_NEAREST -> largest integer multiple of the LOGICAL size that
 *                               fits the drawable, centred.
 *   CM_SCALE_PIXEL_PERFECT   -> exactly LOGICAL size (1 drawable px per logical
 *                               px), centred — no upscale at all.
 * Host-owned, present-only: the oracle pass always passes CM_SCALE_FIT (full
 * drawable) so cm_readback is unaffected (§6). */
static MTLViewport scale_viewport(CMScaleMode mode, int logW, int logH,
                                  int dstW, int dstH) {
    int vw = dstW, vh = dstH;     /* CM_SCALE_FIT: stretch to fill the whole drawable */
    if (mode == CM_SCALE_INTEGER_NEAREST && logW > 0 && logH > 0) {
        int kx = dstW / logW, ky = dstH / logH;
        int k = (kx < ky) ? kx : ky;
        if (k < 1) k = 1;
        vw = logW * k; vh = logH * k;
    } else if (mode == CM_SCALE_PIXEL_PERFECT && logW > 0 && logH > 0) {
        vw = logW; vh = logH;
        if (vw > dstW) vw = dstW;
        if (vh > dstH) vh = dstH;
    } else if (mode == CM_SCALE_ASPECT_FIT && logW > 0 && logH > 0) {
        /* Largest rect with the LOGICAL aspect ratio that fits the drawable, centred.
         * Compare logW*dstH vs dstW*logH (cross-multiply, no float) to decide which
         * dimension is the binding constraint; the clear MTLLoadActionClear fills the
         * surrounding letterbox/pillarbox BLACK. This is the default — it fills the
         * window/screen as much as possible with no distortion and no white. */
        if ((long)logW * dstH <= (long)dstW * logH) {
            vh = dstH;                          /* height-bound: pillarbox L/R */
            vw = (int)((long)logW * dstH / logH);
        } else {
            vw = dstW;                          /* width-bound: letterbox T/B */
            vh = (int)((long)logH * dstW / logW);
        }
        if (vw < 1) vw = 1;
        if (vh < 1) vh = 1;
    }
    MTLViewport vp;
    vp.originX = (double)((dstW - vw) / 2);
    vp.originY = (double)((dstH - vh) / 2);
    vp.width   = (double)vw;
    vp.height  = (double)vh;
    vp.znear = 0.0; vp.zfar = 1.0;
    return vp;
}

/* Live-present encode: like encode_pass but honours the host-owned scale mode
 * (viewport) and filter (sampler). LIVE PRESENT ONLY — never the oracle. */
static void encode_present_pass(CMContext *cx, id<MTLCommandBuffer> cb,
                                id<MTLTexture> dst, CMEffect effect,
                                int dstW, int dstH) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:cx->effectPipeline];
    MTLViewport vp = scale_viewport(cx->scaleMode, cx->w, cx->h, dstW, dstH);
    [enc setViewport:vp];
    [enc setFragmentTexture:cx->fbTex atIndex:0];
    id<MTLSamplerState> smp = (cx->filter == CM_FILTER_LINEAR && cx->linearSampler)
                                ? cx->linearSampler : cx->sampler;
    [enc setFragmentSamplerState:smp atIndex:0];
    CMFxParams fx = { (uint32_t)effect, (uint32_t)dstW, (uint32_t)dstH, 0 };
    [enc setFragmentBytes:&fx length:sizeof(fx) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

void cm_present(CMContext *cx) {
    if (!cx) return;
    if (![NSThread isMainThread]) { cm__sync_main(^{ cm_present(cx); }); return; }
    cx->presentCount++;

    /* 1) Render the framebuffer into the offscreen target (the oracle). Always
     *    the fixed pass-through/nearest pipeline — this is what cm_readback
     *    verifies, so it is never touched by the selected effect. */
    id<MTLCommandBuffer> cb = [cx->queue commandBuffer];
    encode_pass(cx, cb, cx->pipeline, cx->offTex, CM_FX_NEAREST, cx->tw, cx->th);
    [cb commit];
    [cb waitUntilCompleted];               /* synchronous; no blocking run loop */

    /* 2) Best-effort PRESENT: a render pass that draws the framebuffer texture
     *    INTO the drawable as a color attachment (not a blit copy), so it is
     *    valid with layer.framebufferOnly = YES — the drawable is a render
     *    target, never a copy destination. This pass applies the selected
     *    effect; the live window is presentation-only. Non-fatal. */
    if (cx->layer) {
        @autoreleasepool {
            /* Keep the live drawable glued to the content view BEFORE acquiring it
             * (§2a). CMContentView resyncs on -layout / fullscreen, but under the
             * hand-pumped run loop a layout pass can be deferred; doing it here
             * guarantees the drawable is window/screen-sized this frame — the fix
             * for the small-rect-in-fullscreen bug. No-op headless (weak stub). */
            cm__resync_layer(cx);
            id<CAMetalDrawable> d = [cx->layer nextDrawable];
            if (d) {
                cx->drawableCount++;       /* D2t diagnostic: nextDrawable worked */
                int dw = (int)d.texture.width, dh = (int)d.texture.height;
                id<MTLCommandBuffer> cb2 = [cx->queue commandBuffer];
                /* Live present honours the host-owned scale mode + filter
                 * (encode_present_pass); the oracle pass above used encode_pass
                 * with the fixed full-drawable/nearest path, so readback is
                 * unchanged (§6). */
                encode_present_pass(cx, cb2, d.texture, cx->effect, dw, dh);
                [cb2 presentDrawable:d];
                [cb2 commit];
            }
        }
    }
}

int cm_set_effect(CMContext *cx, CMEffect effect) {
    if (!cx) return 1;
    if (effect < 0 || effect >= CM_FX__COUNT) return 2;
    cx->effect = effect;
    return 0;
}

/* ---- TEST-ONLY live-drawable readback (NOT the frozen ABI, NOT exported) -----
 * Resync the layer, acquire the live drawable, compose the framebuffer into it via
 * the SAME present pass cm_present uses (so this reads exactly what the user would
 * see), getBytes it into a freshly-malloc'd *dst (BGRA8, caller frees), and report
 * the drawable's pixel size. The live layer must have framebufferOnly=NO (set by
 * cm__live_set_framebuffer_only in the test). Returns 0 on success; nonzero if there
 * is no live layer / nextDrawable yielded nil. Used only by the [LIVE] readback test
 * to prove the present FILLS the drawable — the offscreen oracle alone was blind to
 * the small-rect-in-fullscreen bug. */
int cm__live_readback(CMContext *cx, void **dst, int *dw, int *dh) {
    if (dst) *dst = NULL; if (dw) *dw = 0; if (dh) *dh = 0;
    if (!cx || !cx->layer || !dst) return 1;
    cm__resync_layer(cx);
    @autoreleasepool {
        id<CAMetalDrawable> d = [cx->layer nextDrawable];
        if (!d) return 2;
        int tw = (int)d.texture.width, th = (int)d.texture.height;
        id<MTLCommandBuffer> cb = [cx->queue commandBuffer];
        encode_present_pass(cx, cb, d.texture, cx->effect, tw, th);   /* same as present */
        [cb commit];
        [cb waitUntilCompleted];      /* synchronous so getBytes sees the composed image */

        size_t stride = (size_t)tw * 4;
        uint8_t *buf = (uint8_t *)malloc(stride * (size_t)th);
        if (!buf) return 3;
        MTLRegion all = MTLRegionMake2D(0, 0, (NSUInteger)tw, (NSUInteger)th);
        [d.texture getBytes:buf bytesPerRow:stride fromRegion:all mipmapLevel:0];
        if (dst) *dst = buf;
        if (dw) *dw = tw;
        if (dh) *dh = th;
    }
    return 0;
}

/* ---- settings / options (INTERFACE.md §9) ---------------------------------
 * cm_set_option dispatches by key group:
 *   HOST-ACTED keys are validated + applied to the live present state now.
 *   AROS-FACING keys are NOT acted on; the host records the requested value (so
 *   cm_get_option can drive the panel) and enqueues a CM_EV_SETTING for the AROS
 *   side to pull (no callback into AROS — §3). */

/* Enqueue a pending CM_EV_SETTING. One (main) thread touches the ring (§3). */
static void cm__enqueue_setting(CMContext *cx, int key, int x, int y) {
    int next = (cx->setTail + 1) % (int)(sizeof(cx->setq)/sizeof(cx->setq[0]));
    if (next == cx->setHead) return;           /* full: drop oldest-overwrite-free, keep simple */
    cx->setq[cx->setTail].key = key;
    cx->setq[cx->setTail].x   = x;
    cx->setq[cx->setTail].y   = y;
    cx->setTail = next;
}

/* Dequeue one pending CM_EV_SETTING into key/x/y. Returns 1 if one was taken,
 * 0 if the queue is empty. Drained by cm_pump_events (the AppKit override and the
 * headless weak stub both call this). */
int cm__take_setting(CMContext *cx, int *key, int *x, int *y) {
    if (!cx || cx->setHead == cx->setTail) return 0;
    if (key) *key = cx->setq[cx->setHead].key;
    if (x)   *x   = cx->setq[cx->setHead].x;
    if (y)   *y   = cx->setq[cx->setHead].y;
    cx->setHead = (cx->setHead + 1) % (int)(sizeof(cx->setq)/sizeof(cx->setq[0]));
    return 1;
}

int cm_set_option(CMContext *cx, int key, long value) {
    if (!cx) return 1;
    switch (key) {
    /* ---- HOST-ACTED: validate + apply to the live present now ---- */
    case CM_OPT_EFFECT:
        if (value < 0 || value >= CM_FX__COUNT) return 2;
        cx->effect = (CMEffect)value;
        return 0;
    case CM_OPT_SCALE_MODE:
        if (value < 0 || value >= CM_SCALE__COUNT) return 2;
        cx->scaleMode = (CMScaleMode)value;
        return 0;
    case CM_OPT_FULLSCREEN:
        cx->fullscreen = value ? 1 : 0;
        /* REAL native AppKit fullscreen (§9): record the flag (above, so
         * cm_get_option/the panel reflect it and it persists), then REQUEST the
         * window enter/exit native fullscreen. The toggle is async (an animated
         * transition) and non-blocking — it returns immediately; the present path
         * keeps composing to whatever the live drawable size becomes. Headless /
         * no window: the weak stub below is a no-op, so the flag is still recorded
         * (visually a stored flag, as before) without any window work. */
        cm__set_fullscreen_appkit(cx, cx->fullscreen);
        return 0;
    case CM_OPT_FILTER:
        if (value < 0 || value >= CM_FILTER__COUNT) return 2;
        cx->filter = (CMFilter)value;
        return 0;

    /* ---- AROS-FACING: NOT host-acted. Record + enqueue CM_EV_SETTING ----
     * REQUEST_MODE_W and _H form a pair: each carries the LAST-known partner in
     * the event's y field so the AROS side gets a complete W×H request. */
    case CM_OPT_REQUEST_MODE_W:
        cx->reqModeW = value;
        cm__enqueue_setting(cx, CM_OPT_REQUEST_MODE_W, (int)value, (int)cx->reqModeH);
        return 0;
    case CM_OPT_REQUEST_MODE_H:
        cx->reqModeH = value;
        cm__enqueue_setting(cx, CM_OPT_REQUEST_MODE_H, (int)value, (int)cx->reqModeW);
        return 0;
    case CM_OPT_KEYMAP:
        cx->reqKeymap = value;
        cm__enqueue_setting(cx, CM_OPT_KEYMAP, (int)value, 0);
        return 0;
    case CM_OPT_AUDIO_VOLUME:
        cx->reqAudioVolume = value;
        cm__enqueue_setting(cx, CM_OPT_AUDIO_VOLUME, (int)value, 0);
        return 0;

    default:
        return 3;                              /* unknown key */
    }
}

int cm_get_option(CMContext *cx, int key, long *value) {
    if (!cx) return 1;
    long v;
    switch (key) {
    case CM_OPT_EFFECT:           v = (long)cx->effect;          break;
    case CM_OPT_SCALE_MODE:       v = (long)cx->scaleMode;       break;
    case CM_OPT_FULLSCREEN:       v = (long)cx->fullscreen;      break;
    case CM_OPT_FILTER:           v = (long)cx->filter;          break;
    case CM_OPT_REQUEST_MODE_W:   v = cx->reqModeW;              break;
    case CM_OPT_REQUEST_MODE_H:   v = cx->reqModeH;              break;
    case CM_OPT_KEYMAP:           v = cx->reqKeymap;             break;
    case CM_OPT_AUDIO_VOLUME:     v = cx->reqAudioVolume;        break;
    default:                      return 3;     /* unknown key */
    }
    if (value) *value = v;
    return 0;
}

int cm_readback(CMContext *cx, void *dst, int dstStride, int w, int h) {
    if (!cx || !dst) return 1;
    if (![NSThread isMainThread]) {
        __block int r = 0;
        cm__sync_main(^{ r = cm_readback(cx, dst, dstStride, w, h); });
        return r;
    }
    if (w != cx->w || h != cx->h) return 2;   /* readback is logical w*h */

    /* The offscreen target is tw x th (= w*scale x h*scale). For the logical
     * readback we sample one source texel per logical pixel (nearest, the
     * top-left of each scale block) so the oracle sees the unscaled image. */
    size_t srcStride = (size_t)cx->tw * 4;
    uint8_t *full = (uint8_t *)malloc(srcStride * (size_t)cx->th);
    if (!full) return 3;
    MTLRegion all = MTLRegionMake2D(0, 0, (NSUInteger)cx->tw, (NSUInteger)cx->th);
    [cx->offTex getBytes:full
             bytesPerRow:srcStride
              fromRegion:all
             mipmapLevel:0];

    downsample_logical(full, cx->tw, (uint8_t *)dst, dstStride, w, h, cx->scale);
    free(full);
    return 0;
}

int cm_target_size(CMContext *cx, int *outW, int *outH, int *outScale) {
    if (!cx) return 1;
    if (outW) *outW = cx->tw;
    if (outH) *outH = cx->th;
    if (outScale) *outScale = cx->scale;
    return 0;
}

/* Downsample a tw x th BGRA8 buffer to logical w x h (nearest, top-left of each
 * scale block) into dst — the same convention cm_readback uses. */
static void downsample_logical(const uint8_t *full, int tw,
                               uint8_t *dst, int dstStride, int w, int h, int s) {
    size_t srcStride = (size_t)tw * 4;
    for (int yy = 0; yy < h; yy++) {
        const uint8_t *srow = full + (size_t)(yy * s) * srcStride;
        uint8_t *drow = dst + (size_t)yy * dstStride;
        for (int xx = 0; xx < w; xx++) {
            const uint8_t *sp = srow + (size_t)(xx * s) * 4;
            uint8_t *dp = drow + (size_t)xx * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
}

int cm_render_effect_readback(CMContext *cx, CMEffect effect,
                              void *dst, int dstStride, int w, int h) {
    if (!cx || !dst) return 1;
    /* Accept either the LOGICAL grid (w*h, scale-block downsampled — matches
     * cm_readback) or the NATIVE target grid (tw*th, identity copy — lets a test
     * inspect real per-target-row effects like scanlines at any scale). */
    int s;
    if (w == cx->w && h == cx->h)            s = cx->scale;  /* logical */
    else if (w == cx->tw && h == cx->th)     s = 1;          /* native target */
    else                                     return 2;
    if (effect < 0 || effect >= CM_FX__COUNT) return 3;

    /* A throwaway offscreen target the same size as the oracle target, so the
     * effect's destination-row math matches what the live drawable would see. */
    id<MTLTexture> tmp = [cx->device newTextureWithDescriptor:
        bgra8_desc(cx->tw, cx->th,
                   MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)];
    if (!tmp) return 4;

    id<MTLCommandBuffer> cb = [cx->queue commandBuffer];
    /* Same encode path as the present, but render-to-texture so we can read it. */
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tmp;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:cx->effectPipeline];
    [enc setFragmentTexture:cx->fbTex atIndex:0];
    [enc setFragmentSamplerState:cx->sampler atIndex:0];
    CMFxParams fx = { (uint32_t)effect, (uint32_t)cx->tw, (uint32_t)cx->th, 0 };
    [enc setFragmentBytes:&fx length:sizeof(fx) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    size_t srcStride = (size_t)cx->tw * 4;
    uint8_t *full = (uint8_t *)malloc(srcStride * (size_t)cx->th);
    if (!full) { tmp = nil; return 5; }
    MTLRegion all = MTLRegionMake2D(0, 0, (NSUInteger)cx->tw, (NSUInteger)cx->th);
    [tmp getBytes:full bytesPerRow:srcStride fromRegion:all mipmapLevel:0];
    downsample_logical(full, cx->tw, (uint8_t *)dst, dstStride, w, h, s);
    free(full);
    tmp = nil;   /* ARC releases the throwaway target */
    return 0;
}

/* ---- live-window helpers (AppKit) ----------------------------------------
 * Defined in cocoametal_window.m when AppKit is linked, else stubbed here so
 * the shim builds and runs headless without AppKit. The standalone D1 build
 * links AppKit and provides the real implementations. */
__attribute__((weak)) void cm_try_window(CMContext *cx, const char *title) {
    (void)cx; (void)title;   /* default: no window (headless) */
}
__attribute__((weak)) void cm_destroy_window(CMContext *cx) {
    (void)cx;
}

/* cm_open_settings: the real native AppKit panel lives in cocoametal_settings.m
 * (cm__open_settings_appkit, strong override of the weak stub below) so this
 * AppKit-free TU pulls no AppKit headers — the same weak/strong split as
 * cm_try_window. Headless / no AppKit: the stub returns nonzero (no-op). */
int cm__open_settings_appkit(CMContext *cx);

__attribute__((weak)) int cm__open_settings_appkit(CMContext *cx) {
    (void)cx;
    return 1;   /* headless / no AppKit: no panel */
}

int cm_open_settings(CMContext *cx) {
    if (!cx) return 1;
    return cm__open_settings_appkit(cx);
}

/* Weak no-op for cm__apply_persisted_options (forward-declared near the top so
 * cm_open can call it). Strong override in cocoametal_settings.m loads from
 * NSUserDefaults; headless / no AppKit keeps the cm_open defaults. */
__attribute__((weak)) void cm__apply_persisted_options(CMContext *cx) {
    (void)cx;
}

/* Weak stubs for the REAL fullscreen toggle (forward-declared near the top so
 * cm_set_option can call them). Strong override in cocoametal_window.m does the
 * [window toggleFullScreen:] under the hand-pumped CFRunLoop; headless / no
 * AppKit (no window) is a no-op so the flag is merely recorded and the window
 * is reported as never-fullscreen. */
__attribute__((weak)) void cm__set_fullscreen_appkit(CMContext *cx, int on) {
    (void)cx; (void)on;
}
__attribute__((weak)) int cm__window_is_fullscreen(CMContext *cx) {
    (void)cx;
    return 0;
}

/* Weak no-op for cm__resync_layer (forward-declared near the top so cm_present can
 * call it). Strong override in cocoametal_window.m glues the live drawable to the
 * content view; headless / no AppKit has no layer so this is a no-op. */
__attribute__((weak)) void cm__resync_layer(CMContext *cx) {
    (void)cx;
}

/* cm_pump_events: the real non-blocking NSEvent drain lives in the AppKit file
 * (cocoametal_window.m, cm__pump_events_appkit) so this AppKit-free translation
 * unit pulls no AppKit headers — exactly the cm_try_window weak/strong split.
 * When AppKit is linked (the dylib, d2t, the input test) the strong override
 * supplies the real implementation; the weak stub below keeps a headless build
 * (no AppKit) link-complete and returns 0 events. The drain is non-blocking and
 * runs on the single AROS/main thread (INTERFACE.md §3) — the only caller. */
int cm__pump_events_appkit(CMContext *cx, CMEvent *out, int maxEvents);

__attribute__((weak)) int cm__pump_events_appkit(CMContext *cx, CMEvent *out, int maxEvents) {
    (void)cx; (void)out; (void)maxEvents;
    return 0;   /* headless / no AppKit: no event source */
}

int cm_pump_events(CMContext *cx, CMEvent *out, int maxEvents) {
    if (!cx || !out || maxEvents <= 0) return 0;
    /* Dequeues NSEvents -> must run on the main thread (AROS calls from its own
     * thread under the inversion). */
    if (![NSThread isMainThread]) {
        __block int r = 0;
        cm__sync_main(^{ r = cm_pump_events(cx, out, maxEvents); });
        return r;
    }

    /* Drain pending CM_EV_SETTING first (pull surface for AROS-facing options,
     * §9). These are produced by cm_set_option of an AROS-facing key (including
     * from the settings panel), NOT dequeuable NSEvents — so they are drained
     * here in the AppKit-free TU, which keeps the headless build (weak pump stub)
     * delivering settings events too. */
    int n = 0;
    int k, x, y;
    while (n < maxEvents && cm__take_setting(cx, &k, &x, &y)) {
        CMEvent *e = &out[n++];
        e->type = CM_EV_SETTING;
        e->code = k;                 /* the CMOption key */
        e->x = x;                    /* the new value */
        e->y = y;                    /* a 2nd value (e.g. paired mode W/H) */
        e->pressed = 0; e->mods = 0;
    }
    if (n >= maxEvents) return n;

    /* Then drain NSEvents / window-management transitions (AppKit override or the
     * headless weak stub) into the remaining slots. */
    n += cm__pump_events_appkit(cx, out + n, maxEvents - n);
    return n;
}

/* cm_abi_version: report the frozen ABI version (INTERFACE.md §7). The AROS
 * loader compares this against its compile-time CM_ABI_VERSION right after
 * HostLib_GetInterface and refuses to register the driver on mismatch — so a
 * stale dylib can't silently bind to a newer loader (or vice versa). */
int cm_abi_version(void) {
    return CM_ABI_VERSION;
}

/* ---- controlled accessors for cocoametal_window.m -------------------------
 * Let the AppKit file reach the few fields it needs without duplicating the
 * CMContext struct definition (which lives here, the source of truth). */
id   cm__device(CMContext *cx)    { return cx ? cx->device : nil; }
int  cm__logical_w(CMContext *cx) { return cx ? cx->w : 0; }
int  cm__logical_h(CMContext *cx) { return cx ? cx->h : 0; }
void cm__set_window(CMContext *cx, void *window, void *layer, int scale) {
    if (!cx) return;
    cx->window = window;
    cx->layer  = (__bridge CAMetalLayer *)layer;
    cx->haveWindow = (window != NULL);
    if (scale > 0) cx->scale = scale;
}
void *cm__get_window(CMContext *cx) { return cx ? cx->window : NULL; }

/* Input window-management flags (set by the AppKit delegate, drained by the pump).
 * cm__take_* returns the current value and clears it (one-shot, edge-triggered). */
void cm__set_close_pending(CMContext *cx)  { if (cx) cx->closePending  = 1; }
void cm__set_resize_pending(CMContext *cx) { if (cx) cx->resizePending = 1; }
int  cm__take_close_pending(CMContext *cx) {
    if (!cx || !cx->closePending) return 0;
    cx->closePending = 0; return 1;
}
int  cm__take_resize_pending(CMContext *cx) {
    if (!cx || !cx->resizePending) return 0;
    cx->resizePending = 0; return 1;
}

/* Internal diagnostic accessor for the D2t threading-model probe. NOT part of the
 * frozen cm_* ABI and NOT exported in the dylib (absent from cocoametal.exports).
 * Reports whether the live-window path exists and how many of presentCount calls
 * acquired a CAMetalDrawable — so D2t can state, from evidence, whether
 * cm_present/nextDrawable work under the hand-pumped CFRunLoop model. */
void cm__present_stats(CMContext *cx, int *haveWindow, int *presents, int *drawables) {
    if (haveWindow) *haveWindow = cx ? cx->haveWindow : 0;
    if (presents)   *presents   = cx ? cx->presentCount : 0;
    if (drawables)  *drawables  = cx ? cx->drawableCount : 0;
}
