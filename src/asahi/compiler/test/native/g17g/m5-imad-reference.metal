// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

kernel void m5_imad_imm_live(device const uint4 *input [[buffer(0)]],
                            device uint4 *output [[buffer(1)]],
                            uint id [[thread_position_in_grid]])
{
   uint4 v = input[id];
   output[id] = uint4(v.y * 37u + v.x, v.x, v.y, v.z);
}

kernel void m5_imad_reg_live(device const uint4 *input [[buffer(0)]],
                            device uint4 *output [[buffer(1)]],
                            uint id [[thread_position_in_grid]])
{
   uint4 v = input[id];
   output[id] = uint4(v.y * v.z + v.x, v.x, v.y, v.z);
}

kernel void m5_imad_const_live(device const uint4 *input [[buffer(0)]],
                              device uint4 *output [[buffer(1)]],
                              uint id [[thread_position_in_grid]])
{
   uint4 v = input[id];
   output[id] = uint4(v.x * 37u + 1u, v.x, v.y, v.z);
}
