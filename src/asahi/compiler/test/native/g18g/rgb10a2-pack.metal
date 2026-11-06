#include <metal_stdlib>
using namespace metal;

struct Varyings {
    float4 position [[position]];
    float4 color;
};

vertex Varyings rgb_vertex(uint index [[vertex_id]],
                          const device float4 *values [[buffer(0)]])
{
    Varyings out;
    out.position = values[2 * index];
    out.color = values[2 * index + 1];
    return out;
}

fragment float4 rgb_registers(Varyings in [[stage_in]]) { return in.color; }
fragment float4 rgb_alpha_one(Varyings in [[stage_in]])
{
    return float4(in.color.xyz, 1.0f);
}
fragment float4 rgb_alpha_zero(Varyings in [[stage_in]])
{
    return float4(in.color.xyz, 0.0f);
}
fragment float4 rgb_modifiers(Varyings in [[stage_in]])
{
    return float4(-abs(in.color.x), abs(in.color.y), -in.color.z, in.color.w);
}

struct TwoColors {
    float4 packed [[color(0)]];
    float4 retained [[color(1)]];
};
fragment TwoColors rgb_retained(Varyings in [[stage_in]])
{
    return {in.color, in.color};
}
fragment float4 rgb_texture(Varyings in [[stage_in]],
                           texture2d<float> image [[texture(0)]],
                           sampler state [[sampler(0)]])
{
    return image.sample(state, in.color.xy);
}
fragment float4 rgb_constant()
{
    return float4(1.0f, 0.5f, 0.25f, 0.0f);
}
