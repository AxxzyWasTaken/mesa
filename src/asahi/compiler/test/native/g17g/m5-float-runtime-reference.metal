// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

kernel void m5_sub_live(device const float4 *input [[buffer(0)]],
                        device float4 *output [[buffer(1)]],
                        uint id [[thread_position_in_grid]])
{
   float4 v = input[id];
   output[id] = float4(v.x - v.y, v.x, v.y, v.z);
}

kernel void m5_mul_live(device const float4 *input [[buffer(0)]],
                        device float4 *output [[buffer(1)]],
                        uint id [[thread_position_in_grid]])
{
   float4 v = input[id];
   output[id] = float4(saturate(v.x * -abs(v.y)), v.x, v.y, v.z);
}

kernel void m5_fma_live(device const float4 *input [[buffer(0)]],
                        device float4 *output [[buffer(1)]],
                        uint id [[thread_position_in_grid]])
{
   float4 v = input[id];
   output[id] = float4(fma(v.x, abs(v.y), -v.z), v.x, v.y, v.z);
}
