// SPDX-License-Identifier: MIT
// Shaders for the native Metal state oracle (oracle.m).
//
// Compiled offline by build.sh into oracle.metallib, which is embedded in the
// oracle binary (__DATA,__metallib). The same metallib is the input for
// offline cross-compilation to applegpu_g18g (g18g-offline.sh), so the host
// run, the binary archives and the G18G binaries all name the same functions.
//
// Vertex buffer(0) holds interleaved float4 pairs: position, colour.
#include <metal_stdlib>
using namespace metal;

struct VP { float4 p [[position]]; float4 c; };
struct VPS { float4 p [[position]]; float4 c; float size [[point_size]]; };
struct VT { float4 p [[position]]; float2 uv; };

vertex VP vertexPlain(uint i [[vertex_id]], device const float4 *v [[buffer(0)]])
{
    return { v[2 * i], v[2 * i + 1] };
}

// Only the point topology may write a point size.
vertex VPS vertexPoint(uint i [[vertex_id]], device const float4 *v [[buffer(0)]])
{
    return { v[2 * i], v[2 * i + 1], 16.0f };
}

// Texture coordinates from clip-space position: (0,0) top left, (1,1) bottom
// right, so the sampled texel is a function of the pixel alone.
vertex VT vertexTextured(uint i [[vertex_id]], device const float4 *v [[buffer(0)]])
{
    const float4 p = v[2 * i];
    return { p, float2(p.x * 0.5f + 0.5f, 0.5f - p.y * 0.5f) };
}

fragment float4 fragmentPlain(VP v [[stage_in]]) { return v.c; }

fragment float4 fragmentPoint(VPS v [[stage_in]]) { return v.c; }

fragment float4 fragmentDiscard(VP v [[stage_in]])
{
    if (v.c.r > 0.5f)
        discard_fragment();
    return v.c;
}

fragment uint4 fragmentUint(VP v [[stage_in]])
{
    return uint4(uint(v.c.r * 255.0f), 0, 0, 1);
}

fragment float4 fragmentTextured(VT v [[stage_in]],
                                 texture2d<float> t [[texture(0)]],
                                 sampler s [[sampler(0)]])
{
    return t.sample(s, v.uv);
}

kernel void computeAdd(device const float *a [[buffer(0)]],
                       device const float *b [[buffer(1)]],
                       device float *c [[buffer(2)]],
                       uint i [[thread_position_in_grid]])
{
    c[i] = a[i] + b[i];
}
