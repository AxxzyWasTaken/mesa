// SPDX-License-Identifier: MIT
//
// A fragment function that reads [[front_facing]], the special register GL's
// gl_FrontFacing lowers to (two-sided lighting in gears-style shaders).
// Render pipeline compiled for applegpu_g17g and applegpu_g18g by
// ../g18g/build-reference.py.
#include <metal_stdlib>
using namespace metal;

struct FFV {
   float4 position [[position]];
   float4 color;
};

vertex FFV ffVertex(uint i [[vertex_id]], device const float4 *v [[buffer(0)]])
{
   FFV o;
   o.position = v[2 * i];
   o.color = v[2 * i + 1];
   return o;
}

fragment float4 ffFragment(FFV v [[stage_in]], bool front [[front_facing]])
{
   return front ? v.color : v.color.bgra;
}
