#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
struct FxParams { uint effect; uint targetW; uint targetH; uint _pad; };
vertex VOut vs_fulltri(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv  = float2(p.x, 1.0 - p.y);
    return o;
}
fragment float4 fs_sample(VOut in [[stage_in]],
                          texture2d<float> tex [[texture(0)]],
                          sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv);
}
fragment float4 fs_effect(VOut in [[stage_in]],
                          texture2d<float> tex [[texture(0)]],
                          sampler smp [[sampler(0)]],
                          constant FxParams& fx [[buffer(0)]]) {
    float4 c = tex.sample(smp, in.uv);
    if (fx.effect == 1u) {
        uint row = uint(in.pos.y);
        float scan = (row & 1u) ? 0.55 : 1.0;
        c.rgb *= scan;
        c.rgb = pow(c.rgb, float3(1.0 / 1.25));
    }
    return c;
}

/* ---- GPU compute section (gpufx.library / cm_gpu_*) --------------------
 * Byte-buffer kernels (arbitrary row strides, no texture alignment rules).
 * The sampling arithmetic is mirrored, op for op, by the CPU references in
 * gpu_test.c and by gpufx consumers' fallbacks — keep them in sync. */

struct CsScaleParams {
    uint sw, sh, dw, dh;
    uint srcStride, dstStride;
    uint filter;               /* 0 nearest, 1 bilinear */
    uint _pad;
};

kernel void cs_scale_rgba(device const uchar *src [[buffer(0)]],
                          device uchar       *dst [[buffer(1)]],
                          constant CsScaleParams &p [[buffer(2)]],
                          uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.dw || gid.y >= p.dh)
        return;
    float u = (gid.x + 0.5f) * (float)p.sw / (float)p.dw;
    float v = (gid.y + 0.5f) * (float)p.sh / (float)p.dh;
    uint di = gid.y * p.dstStride + (gid.x << 2);

    if (p.filter == 0u) {
        int ix = clamp(int(u), 0, int(p.sw) - 1);
        int iy = clamp(int(v), 0, int(p.sh) - 1);
        uint si = uint(iy) * p.srcStride + (uint(ix) << 2);
        for (uint c = 0u; c < 4u; c++)
            dst[di + c] = src[si + c];
        return;
    }

    float fu = u - 0.5f, fv = v - 0.5f;
    int x0 = clamp(int(floor(fu)), 0, int(p.sw) - 1);
    int y0 = clamp(int(floor(fv)), 0, int(p.sh) - 1);
    int x1 = min(x0 + 1, int(p.sw) - 1);
    int y1 = min(y0 + 1, int(p.sh) - 1);
    float ax = clamp(fu - (float)x0, 0.0f, 1.0f);
    float ay = clamp(fv - (float)y0, 0.0f, 1.0f);
    uint i00 = uint(y0) * p.srcStride + (uint(x0) << 2);
    uint i10 = uint(y0) * p.srcStride + (uint(x1) << 2);
    uint i01 = uint(y1) * p.srcStride + (uint(x0) << 2);
    uint i11 = uint(y1) * p.srcStride + (uint(x1) << 2);
    for (uint c = 0u; c < 4u; c++) {
        float top = mix((float)src[i00 + c], (float)src[i10 + c], ax);
        float bot = mix((float)src[i01 + c], (float)src[i11 + c], ax);
        dst[di + c] = (uchar)clamp(mix(top, bot, ay) + 0.5f, 0.0f, 255.0f);
    }
}

struct CsYuvParams {
    uint w, h;
    uint yStride, uStride, vStride, dstStride;
    uint fullRange;            /* 0 = BT.601 limited (video), 1 = full */
    uint _pad;
};

kernel void cs_yuv420_rgba(device const uchar *yp [[buffer(0)]],
                           device const uchar *up [[buffer(1)]],
                           device const uchar *vp [[buffer(2)]],
                           device uchar       *dst [[buffer(3)]],
                           constant CsYuvParams &p [[buffer(4)]],
                           uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.w || gid.y >= p.h)
        return;
    float Y = (float)yp[gid.y * p.yStride + gid.x];
    float U = (float)up[(gid.y >> 1) * p.uStride + (gid.x >> 1)] - 128.0f;
    float V = (float)vp[(gid.y >> 1) * p.vStride + (gid.x >> 1)] - 128.0f;
    float r, g, b;
    if (p.fullRange != 0u) {
        r = Y + 1.402000f * V;
        g = Y - 0.344136f * U - 0.714136f * V;
        b = Y + 1.772000f * U;
    } else {
        float C = 1.164383f * (Y - 16.0f);
        r = C + 1.596027f * V;
        g = C - 0.391762f * U - 0.812968f * V;
        b = C + 2.017232f * U;
    }
    uint di = gid.y * p.dstStride + (gid.x << 2);
    dst[di + 0] = (uchar)clamp(r + 0.5f, 0.0f, 255.0f);
    dst[di + 1] = (uchar)clamp(g + 0.5f, 0.0f, 255.0f);
    dst[di + 2] = (uchar)clamp(b + 0.5f, 0.0f, 255.0f);
    dst[di + 3] = 255;
}
