// SPDX-License-Identifier: MIT
//
// GL 4.x shapes for the G17G (agx3c) packer, and through the transcoder for
// G18G: bitfieldReverse (reverse_bits) on a dead, a live and a loaded word,
// and fragment functions updating a device atomic -- GL's atomic counters
// and SSBO atomics in a fragment shader -- plainly and behind
// simd_is_helper_thread(). Both read the active-thread special register the
// helper-invocation predicate lowers to. Compiled for applegpu_g17g and
// applegpu_g18g by ../g18g/build-reference.py.
#include <metal_stdlib>
using namespace metal;

kernel void rbit32_dead(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = reverse_bits(x); }
kernel void rbit32_live(device const uint4 *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ uint x = in[id].x * in[id].y; o[id] = reverse_bits(x) ^ x; }
kernel void rbit32_loaded(device const uint *in [[buffer(0)]], device uint *o [[buffer(1)]], uint id [[thread_position_in_grid]])
{ o[id] = reverse_bits(in[id]); }

struct SV {
   float4 position [[position]];
   float4 color;
};

static SV sx_vertex(uint i, device const float4 *v)
{
   SV o;
   o.position = v[2 * i];
   o.color = v[2 * i + 1];
   return o;
}

vertex SV sxAtomicVertex(uint i [[vertex_id]], device const float4 *v [[buffer(0)]])
{ return sx_vertex(i, v); }

vertex SV sxHelperVertex(uint i [[vertex_id]], device const float4 *v [[buffer(0)]])
{ return sx_vertex(i, v); }

fragment float4 sxAtomic(SV v [[stage_in]], device atomic_uint *c [[buffer(0)]])
{
   atomic_fetch_add_explicit(&c[0], 1u, memory_order_relaxed);
   return v.color;
}

fragment float4 sxHelper(SV v [[stage_in]], device atomic_uint *c [[buffer(0)]])
{
   if (!simd_is_helper_thread())
      atomic_fetch_add_explicit(&c[1], 3u, memory_order_relaxed);
   return v.color;
}
