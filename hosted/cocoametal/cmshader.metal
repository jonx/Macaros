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
