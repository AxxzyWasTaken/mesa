// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

struct M5ReadResources {
   texture2d_array<uint, access::read> image [[id(0)]];
};

kernel void m5_read_array_uniform(
   constant M5ReadResources &resources [[buffer(0)]],
   device uint4 *output [[buffer(1)]],
   uint2 pixel [[thread_position_in_grid]])
{
   output[pixel.y * 256 + pixel.x] = resources.image.read(pixel, 0u, 0u);
}

kernel void m5_read_array_indexed(
   constant M5ReadResources *resources [[buffer(0)]],
   device uint4 *output [[buffer(1)]],
   uint3 pixel [[thread_position_in_grid]])
{
   const uint resource = pixel.x & 1u;
   output[pixel.y * 256 + pixel.x] =
      resources[resource].image.read(pixel.xy, pixel.z, 0u);
}
