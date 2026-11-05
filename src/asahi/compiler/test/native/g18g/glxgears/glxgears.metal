// SPDX-License-Identifier: MIT
//
// glxgears-shaped instruction probes for G17G/G18G.
//
// glxgears draws with fixed-function GL state: one directional light
// (GL_LIGHT0 at (5, 5, 10, 0)), GL_LIGHTING, GL_NORMALIZE, per-gear material
// colour, and GL_FLAT for the gear faces. Mesa turns that state into a vertex
// shader that transforms the position by the modelview-projection matrix,
// transforms and renormalises the normal, takes max(N.L, 0) against the
// eye-space light direction, accumulates ambient + diffuse (+ the LIT
// specular select, whose pow() is 1 at shininess 0) and clamps the colour,
// and into a fragment shader that returns the flat-interpolated colour.
//
// These are the same computations written as Metal, so that metal-nt can
// compile them for applegpu_g17g and applegpu_g18g and the two builds can be
// compared instruction by instruction. The vertex inputs are plain device
// loads indexed by vertex_id, as Mesa's AGX vertex fetch lowers them.
#include <metal_stdlib>
using namespace metal;

struct GearsUniforms {
    float4x4 mvp;        // projection * modelview
    float4x4 modelview;  // rigid: its upper 3x3 is the normal matrix
    float4 light_dir;    // eye-space direction towards the light, normalised
    float4 ambient;      // scene ambient + light ambient * material
    float4 diffuse;      // light diffuse * material diffuse
    float4 specular;     // light specular * material specular
};

struct GearsVertexOut {
    float4 position [[position]];
    float4 color [[flat]];
};

vertex GearsVertexOut gearsVertex(uint vid [[vertex_id]],
                                  device const packed_float3 *position [[buffer(0)]],
                                  device const packed_float3 *normal [[buffer(1)]],
                                  constant GearsUniforms &u [[buffer(2)]])
{
    const float4 p = float4(float3(position[vid]), 1.0f);
    const float3 n = normalize((u.modelview * float4(float3(normal[vid]), 0.0f)).xyz);
    const float ndotl = dot(n, u.light_dir.xyz);
    const float diffuse = max(ndotl, 0.0f);
    const float facing = ndotl > 0.0f ? 1.0f : 0.0f;

    GearsVertexOut out;
    out.position = u.mvp * p;
    out.color = saturate(u.ambient + diffuse * u.diffuse + facing * u.specular);
    return out;
}

fragment float4 gearsFragmentFlat(GearsVertexOut in [[stage_in]])
{
    return in.color;
}
