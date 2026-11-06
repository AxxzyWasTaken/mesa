// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

kernel void m5_half_select(device const uint4 *input [[buffer(0)]],
                          device ushort4 *output [[buffer(1)]],
                          uint id [[thread_position_in_grid]])
{
   uint4 v = input[id];
   output[id] = ushort4(v.x < v.y, v.x > v.y, v.z < v.w, v.z == v.w);
}

kernel void m5_half_select_nested(device const uint4 *input [[buffer(0)]],
                                 device ushort4 *output [[buffer(1)]],
                                 uint id [[thread_position_in_grid]])
{
   uint4 v = input[id];
   output[id] = ushort4(v.x < v.y && v.z < v.w,
                       v.x < v.y ? ushort(v.z) : ushort(0),
                       v.x < v.y ? ushort(v.z) : ushort(v.w),
                       v.x < v.y ? ushort(1) : ushort(v.z));
}
