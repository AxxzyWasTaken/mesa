// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct M5ArrayResources {
   texture2d_array<float> image [[id(0)]];
   sampler filter [[id(1)]];
};

kernel void m5_sample_indexed_array_level_zero(
   constant M5ArrayResources *resources [[buffer(0)]],
   device float4 *output [[buffer(1)]],
   uint2 pixel [[thread_position_in_grid]])
{
   const uint resource = pixel.x & 1u;
   output[pixel.y * 256 + pixel.x] = resources[resource].image.sample(
      resources[resource].filter, float2(pixel) + 0.5f,
      pixel.y & 3u, level(0.0f));
}
