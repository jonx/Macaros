/* cocoametal_gpu.m -- the GPU compute section (docs/features/gpufx).
 *
 * Explicit GPU 2D ops (scale, YUV420->RGBA convert) on caller-owned CPU
 * buffers, exported as cm_gpu_* for the AROS-side gpufx.library front door.
 * The section SHARES the display's MTLDevice + MTLCommandQueue when a
 * display context exists (cocoametal.m adopts its pair via cm__gpu_adopt at
 * cm_open) so the library and the window drive one GPU context, never two
 * competing devices; headless callers (no window yet) get a lazily created
 * pair of their own.
 *
 * All entry points are callable from any thread (compute encoding needs no
 * main-thread hop; MTLCommandQueue is thread-safe) and degrade to -1 when
 * Metal is unavailable -- callers keep their CPU fallback.
 *
 * The kernels live in cmshader.metal (embedded metallib); kCsMSL below is
 * the byte-identical runtime-compile fallback for environments where the
 * embedded library fails to load. KEEP THE THREE IN SYNC: cmshader.metal,
 * kCsMSL, and the CPU references in gpu_test.c / consumer fallbacks.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <string.h>

#include "cocoametal.h"

/* Embedded shader library: compiled from cmshader.metal (see the Makefile
 * cocoametal-shader rule). Defined once in cocoametal.m; declared here. */
extern unsigned char cmshader_metallib[];
extern unsigned int cmshader_metallib_len;

static id<MTLDevice> gpu_device;
static id<MTLCommandQueue> gpu_queue;
static id<MTLComputePipelineState> gpu_scale_pso;
static id<MTLComputePipelineState> gpu_yuv_pso;
static int gpu_state; /* 0 = untried, 1 = ready, -1 = failed */
static NSLock *gpu_lock;

static NSString *const kCsMSL = @"#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct CsScaleParams { uint sw, sh, dw, dh; uint srcStride, dstStride; uint filter; uint _pad; };\n"
    "kernel void cs_scale_rgba(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]],\n"
    "                          constant CsScaleParams &p [[buffer(2)]], uint2 gid [[thread_position_in_grid]]) {\n"
    "    if (gid.x >= p.dw || gid.y >= p.dh) return;\n"
    "    float u = (gid.x + 0.5f) * (float)p.sw / (float)p.dw;\n"
    "    float v = (gid.y + 0.5f) * (float)p.sh / (float)p.dh;\n"
    "    uint di = gid.y * p.dstStride + (gid.x << 2);\n"
    "    if (p.filter == 0u) {\n"
    "        int ix = clamp(int(u), 0, int(p.sw) - 1);\n"
    "        int iy = clamp(int(v), 0, int(p.sh) - 1);\n"
    "        uint si = uint(iy) * p.srcStride + (uint(ix) << 2);\n"
    "        for (uint c = 0u; c < 4u; c++) dst[di + c] = src[si + c];\n"
    "        return;\n"
    "    }\n"
    "    float fu = u - 0.5f, fv = v - 0.5f;\n"
    "    int x0 = clamp(int(floor(fu)), 0, int(p.sw) - 1);\n"
    "    int y0 = clamp(int(floor(fv)), 0, int(p.sh) - 1);\n"
    "    int x1 = min(x0 + 1, int(p.sw) - 1);\n"
    "    int y1 = min(y0 + 1, int(p.sh) - 1);\n"
    "    float ax = clamp(fu - (float)x0, 0.0f, 1.0f);\n"
    "    float ay = clamp(fv - (float)y0, 0.0f, 1.0f);\n"
    "    uint i00 = uint(y0) * p.srcStride + (uint(x0) << 2);\n"
    "    uint i10 = uint(y0) * p.srcStride + (uint(x1) << 2);\n"
    "    uint i01 = uint(y1) * p.srcStride + (uint(x0) << 2);\n"
    "    uint i11 = uint(y1) * p.srcStride + (uint(x1) << 2);\n"
    "    for (uint c = 0u; c < 4u; c++) {\n"
    "        float top = mix((float)src[i00 + c], (float)src[i10 + c], ax);\n"
    "        float bot = mix((float)src[i01 + c], (float)src[i11 + c], ax);\n"
    "        dst[di + c] = (uchar)clamp(mix(top, bot, ay) + 0.5f, 0.0f, 255.0f);\n"
    "    }\n"
    "}\n"
    "struct CsYuvParams { uint w, h; uint yStride, uStride, vStride, dstStride; uint fullRange; uint _pad; };\n"
    "kernel void cs_yuv420_rgba(device const uchar *yp [[buffer(0)]], device const uchar *up [[buffer(1)]],\n"
    "                           device const uchar *vp [[buffer(2)]], device uchar *dst [[buffer(3)]],\n"
    "                           constant CsYuvParams &p [[buffer(4)]], uint2 gid [[thread_position_in_grid]]) {\n"
    "    if (gid.x >= p.w || gid.y >= p.h) return;\n"
    "    float Y = (float)yp[gid.y * p.yStride + gid.x];\n"
    "    float U = (float)up[(gid.y >> 1) * p.uStride + (gid.x >> 1)] - 128.0f;\n"
    "    float V = (float)vp[(gid.y >> 1) * p.vStride + (gid.x >> 1)] - 128.0f;\n"
    "    float r, g, b;\n"
    "    if (p.fullRange != 0u) {\n"
    "        r = Y + 1.402000f * V;\n"
    "        g = Y - 0.344136f * U - 0.714136f * V;\n"
    "        b = Y + 1.772000f * U;\n"
    "    } else {\n"
    "        float C = 1.164383f * (Y - 16.0f);\n"
    "        r = C + 1.596027f * V;\n"
    "        g = C - 0.391762f * U - 0.812968f * V;\n"
    "        b = C + 2.017232f * U;\n"
    "    }\n"
    "    uint di = gid.y * p.dstStride + (gid.x << 2);\n"
    "    dst[di + 0] = (uchar)clamp(r + 0.5f, 0.0f, 255.0f);\n"
    "    dst[di + 1] = (uchar)clamp(g + 0.5f, 0.0f, 255.0f);\n"
    "    dst[di + 2] = (uchar)clamp(b + 0.5f, 0.0f, 255.0f);\n"
    "    dst[di + 3] = 255;\n"
    "}\n";

typedef struct {
    unsigned sw, sh, dw, dh;
    unsigned srcStride, dstStride;
    unsigned filter;
    unsigned _pad;
} CsScaleParams;

typedef struct {
    unsigned w, h;
    unsigned yStride, uStride, vStride, dstStride;
    unsigned fullRange;
    unsigned _pad;
} CsYuvParams;

/* Called by cm_open (cocoametal.m) right after it creates the display's
 * device + queue: the display pair becomes the process's shared GPU
 * context, unless the compute section already stood one up. */
void cm__gpu_adopt(id<MTLDevice> device, id<MTLCommandQueue> queue)
{
    if (!gpu_lock)
        gpu_lock = [NSLock new];
    [gpu_lock lock];
    if (!gpu_device && device && queue) {
        gpu_device = device;
        gpu_queue = queue;
    }
    [gpu_lock unlock];
}

static int gpu_setup_locked(void)
{
    if (gpu_state != 0)
        return gpu_state;

    if (!gpu_device) {
        gpu_device = MTLCreateSystemDefaultDevice();
        gpu_queue = [gpu_device newCommandQueue];
    }
    if (!gpu_device || !gpu_queue) {
        gpu_state = -1;
        return gpu_state;
    }

    NSError *err = nil;
    dispatch_data_t libdata = dispatch_data_create(
        cmshader_metallib, cmshader_metallib_len,
        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> lib = [gpu_device newLibraryWithData:libdata error:&err];
    if (!lib || ![lib newFunctionWithName:@"cs_scale_rgba"]) {
        /* Older embedded library (no compute kernels) or load failure:
         * compile the fallback source at runtime. */
        err = nil;
        lib = [gpu_device newLibraryWithSource:kCsMSL options:nil error:&err];
    }
    id<MTLFunction> fscale = [lib newFunctionWithName:@"cs_scale_rgba"];
    id<MTLFunction> fyuv = [lib newFunctionWithName:@"cs_yuv420_rgba"];
    if (fscale)
        gpu_scale_pso = [gpu_device newComputePipelineStateWithFunction:fscale
                                                                  error:&err];
    if (fyuv)
        gpu_yuv_pso = [gpu_device newComputePipelineStateWithFunction:fyuv
                                                                error:&err];
    gpu_state = (gpu_scale_pso && gpu_yuv_pso) ? 1 : -1;
    if (gpu_state < 0)
        fprintf(stderr, "cm_gpu: pipeline setup failed (%s)\n",
                err ? err.localizedDescription.UTF8String : "?");
    return gpu_state;
}

int cm_gpu_open(void)
{
    if (!gpu_lock) {
        /* Racy only before the first cm_open/cm_gpu_open; both run once
         * from a single caller in practice. */
        gpu_lock = [NSLock new];
    }
    [gpu_lock lock];
    int state = gpu_setup_locked();
    [gpu_lock unlock];
    return state == 1 ? 0 : -1;
}

int cm_gpu_abi(void)
{
    return CM_GPU_ABI;
}

/* Dispatch a compute pass over a dw x dh grid and wait for it. */
static int gpu_run(id<MTLComputePipelineState> pso, NSArray *buffers,
                   const void *params, int paramsLen, int dw, int dh)
{
    id<MTLCommandBuffer> cb = [gpu_queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    if (!cb || !enc)
        return -1;
    [enc setComputePipelineState:pso];
    NSUInteger index = 0;
    for (id<MTLBuffer> buffer in buffers)
        [enc setBuffer:buffer offset:0 atIndex:index++];
    [enc setBytes:params length:paramsLen atIndex:index];
    /* Apple silicon supports non-uniform threadgroup sizes. */
    [enc dispatchThreads:MTLSizeMake(dw, dh, 1)
        threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted ? 0 : -1;
}

int cm_gpu_scale(const void *src, int srcStride, int sw, int sh, void *dst,
                 int dstStride, int dw, int dh, int filter)
{
    if (!src || !dst || sw < 1 || sh < 1 || dw < 1 || dh < 1
        || srcStride < sw * 4 || dstStride < dw * 4)
        return -1;
    if (cm_gpu_open() != 0)
        return -1;

    @autoreleasepool {
        id<MTLBuffer> sbuf =
            [gpu_device newBufferWithBytes:src
                                    length:(NSUInteger)sh * srcStride
                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> dbuf =
            [gpu_device newBufferWithLength:(NSUInteger)dh * dstStride
                                    options:MTLResourceStorageModeShared];
        if (!sbuf || !dbuf)
            return -1;
        CsScaleParams p = { (unsigned)sw, (unsigned)sh, (unsigned)dw,
                            (unsigned)dh, (unsigned)srcStride,
                            (unsigned)dstStride, (unsigned)(filter ? 1 : 0),
                            0 };
        if (gpu_run(gpu_scale_pso, @[ sbuf, dbuf ], &p, sizeof(p), dw, dh)
            != 0)
            return -1;
        memcpy(dst, dbuf.contents, (size_t)dh * dstStride);
    }
    return 0;
}

int cm_gpu_convert_yuv420(const void *y, int yStride, const void *u,
                          int uStride, const void *v, int vStride, int w,
                          int h, void *rgba, int dstStride, int fullRange)
{
    if (!y || !u || !v || !rgba || w < 1 || h < 1 || yStride < w
        || uStride < (w + 1) / 2 || vStride < (w + 1) / 2
        || dstStride < w * 4)
        return -1;
    if (cm_gpu_open() != 0)
        return -1;

    @autoreleasepool {
        int ch = (h + 1) / 2;
        id<MTLBuffer> ybuf =
            [gpu_device newBufferWithBytes:y
                                    length:(NSUInteger)h * yStride
                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> ubuf =
            [gpu_device newBufferWithBytes:u
                                    length:(NSUInteger)ch * uStride
                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> vbuf =
            [gpu_device newBufferWithBytes:v
                                    length:(NSUInteger)ch * vStride
                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> dbuf =
            [gpu_device newBufferWithLength:(NSUInteger)h * dstStride
                                    options:MTLResourceStorageModeShared];
        if (!ybuf || !ubuf || !vbuf || !dbuf)
            return -1;
        CsYuvParams p = { (unsigned)w, (unsigned)h, (unsigned)yStride,
                          (unsigned)uStride, (unsigned)vStride,
                          (unsigned)dstStride,
                          (unsigned)(fullRange ? 1 : 0), 0 };
        if (gpu_run(gpu_yuv_pso, @[ ybuf, ubuf, vbuf, dbuf ], &p, sizeof(p),
                    w, h)
            != 0)
            return -1;
        memcpy(rgba, dbuf.contents, (size_t)h * dstStride);
    }
    return 0;
}
