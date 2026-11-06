// SPDX-License-Identifier: MIT
//
// Structured control flow that Apple's compiler cannot if-convert: every
// guarded body is a store or a loop, so the texts keep their exec-mask stack
// ops (cndst/cndlt/cndcf/cndef/cndend), compares and b/bn branches. Compiled
// for applegpu_g17g and applegpu_g18g by ../g18g/build-reference.py, these
// are the G18G control-flow witnesses of agx3_transcode.c.
#include <metal_stdlib>
using namespace metal;

kernel void cf_if_store(device const uint4 *in [[buffer(0)]],
                        device uint *out [[buffer(1)]],
                        uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   if (v.x < v.y)
      out[id] = v.z * 3u + v.w;
}

kernel void cf_if_else_store(device const uint4 *in [[buffer(0)]],
                             device uint *out [[buffer(1)]],
                             uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   if (v.x < v.y)
      out[id] = v.z * 3u;
   else
      out[id + 64] = v.w + 7u;
}

kernel void cf_float_if(device const float4 *in [[buffer(0)]],
                        device float *out [[buffer(1)]],
                        uint id [[thread_position_in_grid]])
{
   float4 v = in[id];
   if (v.x < v.y)
      out[id] = v.z * v.w;
}

kernel void cf_loop_divergent(device const uint4 *in [[buffer(0)]],
                              device uint *out [[buffer(1)]],
                              uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   uint acc = 0;
   for (uint i = 0; i < (v.x & 15u); i++)
      acc = acc * v.y + i;
   out[id] = acc;
}

kernel void cf_loop_uniform(device const uint4 *in [[buffer(0)]],
                            device uint *out [[buffer(1)]],
                            constant uint &n [[buffer(2)]],
                            uint id [[thread_position_in_grid]])
{
   uint acc = 0;
   for (uint i = 0; i < n; i++)
      acc += in[i].x * (id + 1u);
   out[id] = acc;
}

kernel void cf_loop_break(device const uint4 *in [[buffer(0)]],
                          device uint *out [[buffer(1)]],
                          uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   uint acc = v.x;
   for (uint i = 0; i < v.w; i++) {
      acc = acc * 3u + v.y;
      if (acc > v.z)
         break;
   }
   out[id] = acc;
}

kernel void cf_loop_continue_nested(device const uint4 *in [[buffer(0)]],
                                    device uint *out [[buffer(1)]],
                                    uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   uint acc = 0;
   for (uint i = 0; i < (v.x & 7u); i++) {
      if ((i & 1u) == (v.y & 1u))
         continue;
      for (uint j = 0; j < i; j++)
         acc += j * v.z;
   }
   out[id] = acc;
}

kernel void cf_switch(device const uint4 *in [[buffer(0)]],
                      device uint *out [[buffer(1)]],
                      uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   switch (v.x & 3u) {
   case 0:
      out[id] = v.y;
      break;
   case 1:
      out[id + 64] = v.z;
      break;
   case 2:
      out[id + 128] = v.w;
      break;
   default:
      break;
   }
}

kernel void cf_two_flags(device const uint4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   bool c1 = v.x < v.y, c2 = v.z < v.w, c3 = v.x == v.z;
   if (c1) atomic_fetch_add_explicit(&out[0], v.z, memory_order_relaxed);
   if (c2) atomic_fetch_add_explicit(&out[1], v.x, memory_order_relaxed);
   if (c3) atomic_fetch_add_explicit(&out[2], v.y, memory_order_relaxed);
   if (c1 != c2) atomic_fetch_add_explicit(&out[3], v.w, memory_order_relaxed);
}

kernel void cf_nested_break(device const uint4 *in [[buffer(0)]], device uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   uint acc = v.x;
   for (uint i = 0; i < v.w; i++) {
      if (acc > v.y) {
         acc ^= i;
         if (acc > v.z)
            break;
         acc += 5u;
      }
      acc = acc * 3u + i;
   }
   out[id] = acc;
}

kernel void cf_nested_loops_break(device const uint4 *in [[buffer(0)]], device uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   uint acc = 0;
   for (uint i = 0; i < v.x; i++) {
      for (uint j = 0; j < v.y; j++) {
         acc += i * j;
         if (acc > v.z)
            break;
      }
      if (acc > v.w)
         break;
   }
   out[id] = acc;
}

kernel void cf_fcmp_forms(device const float4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
   float4 v = in[id];
   if (v.x == v.y) atomic_fetch_add_explicit(&out[0], 1u, memory_order_relaxed);
   if (v.x >= v.z) atomic_fetch_add_explicit(&out[1], 1u, memory_order_relaxed);
   if (v.y != v.w) atomic_fetch_add_explicit(&out[2], 1u, memory_order_relaxed);
   if (v.z > 1.0f) atomic_fetch_add_explicit(&out[3], 1u, memory_order_relaxed);
   if (v.w < 0.0f) atomic_fetch_add_explicit(&out[4], 1u, memory_order_relaxed);
}

kernel void cf_icmp_forms(device const int4 *in [[buffer(0)]], device atomic_uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
   int4 v = in[id];
   if (v.x < v.y) atomic_fetch_add_explicit(&out[0], 1u, memory_order_relaxed);
   if (uint(v.x) < 17u) atomic_fetch_add_explicit(&out[1], 1u, memory_order_relaxed);
   if (v.z != 0) atomic_fetch_add_explicit(&out[2], 1u, memory_order_relaxed);
   if (v.w > 5) atomic_fetch_add_explicit(&out[3], 1u, memory_order_relaxed);
   if (uint(v.y) >= uint(v.w)) atomic_fetch_add_explicit(&out[4], 1u, memory_order_relaxed);
}

kernel void cf_deep_break(device const uint4 *in [[buffer(0)]], device uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
   uint4 v = in[id];
   uint acc = v.x;
   for (uint i = 0; i < v.w; i++) {
      acc = acc * 5u + i;
      if (acc > v.y) {
         acc ^= 3u;
         if (acc < v.z) {
            acc += 7u;
            if (acc != v.x) {
               acc *= 9u;
               if ((acc & 15u) == 3u)
                  break;
               acc += 1u;
            }
            acc -= 2u;
         }
         acc ^= i;
      }
   }
   out[id] = acc;
}

kernel void cf_loops_breaks(device const uint4 *in [[buffer(0)]], device uint *out [[buffer(1)]], constant uint *n [[buffer(2)]], uint id [[thread_position_in_grid]])
{
   uint acc = id;
   for (uint a = 0; a < n[0]; a++) {
      for (uint b = 0; b < n[1] + (acc & 1u); b++) {
         for (uint c = 0; c < n[2] + (acc & 2u); c++) {
            for (uint d = 0; d < n[3] + (acc & 4u); d++) {
               acc = acc * 3u + a + b + c + d;
               if (acc == in[d].x) break;
            }
            if (acc == in[c].y) break;
         }
         if (acc == in[b].z) break;
      }
      if (acc == in[a].w) break;
   }
   out[id] = acc;
}

kernel void cf_float_alu_if(device const float4 *in [[buffer(0)]],
                            device float *out [[buffer(1)]],
                            uint id [[thread_position_in_grid]])
{
   float4 v = in[id];
   float a = v.x * 2.0f + v.z, b = v.y * v.w;
   if (a < b)
      out[id] = a - b;
   if (a == b + 1.0f)
      out[id + 64] = b;
}
