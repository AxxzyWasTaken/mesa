// SPDX-License-Identifier: MIT
// Compiler reference only. These shaders are never substituted into Mesa.
#include <metal_stdlib>
using namespace metal;

kernel void m5_half_widen(device const half4 *input [[buffer(0)]],
                         device float4 *output [[buffer(1)]],
                         uint id [[thread_position_in_grid]])
{
    output[id] = float4(input[id]);
}

kernel void m5_half_widen_modifiers(device const half4 *input [[buffer(0)]],
                                   device float4 *output [[buffer(1)]],
                                   uint id [[thread_position_in_grid]])
{
    half4 h = input[id];
    output[id] = float4(float(h.x), float(-h.y), float(abs(h.z)), float(-abs(h.w)));
}

kernel void m5_half_full_add(device const half4 *input [[buffer(0)]],
                            device const float4 *addend [[buffer(1)]],
                            device float4 *output [[buffer(2)]],
                            uint id [[thread_position_in_grid]])
{
    output[id] = float4(input[id]) + addend[id];
}
