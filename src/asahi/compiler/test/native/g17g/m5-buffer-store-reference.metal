// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct M5Buffers {
   device const uint4 *input [[id(0)]];
   device uint4 *output [[id(1)]];
};

kernel void m5_copy_indexed_buffers(
   constant M5Buffers *buffers [[buffer(0)]],
   uint id [[thread_position_in_grid]])
{
   const uint resource = id & 1u;
   const uint4 value = buffers[resource].input[id];
   buffers[resource].output[id] = value;
   buffers[resource].output[id + 512u] = value;
}
