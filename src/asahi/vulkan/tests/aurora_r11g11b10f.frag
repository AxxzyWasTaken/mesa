/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#version 450
layout(push_constant) uniform Pass { uint mode; } pc;
layout(location = 0) out vec4 o;

/* Every class of packed float field, and what the pack does past each end. */
const uint values[16] = uint[16](
   0x00000000u, /* zero */
   0x37800000u, /* 2^-16: denormal in eleven bits, 2^-15 in ten */
   0x38000000u, /* 2^-15 */
   0x38800000u, /* 2^-14, the smallest normal */
   0x3e800000u, /* 0.25 */
   0x3f800000u, /* 1 */
   0x3fc00000u, /* 1.5 */
   0x44800000u, /* 1024 */
   0x477e0000u, /* 65024, the largest eleven-bit value */
   0x477f0000u, /* rounds past it */
   0x4e6e6b28u, /* 1e9 */
   0xbf800000u, /* negative */
   0x7f800000u, /* infinity */
   0x7fc00000u, /* NaN */
   0x3eaaaaabu, /* 1/3, rounds */
   0x33d6bf95u  /* 1e-7, rounds to zero */
);

void main()
{
   uvec2 p = uvec2(gl_FragCoord.xy);
   if (pc.mode == 0u) {
      /* Different data in every sample of every pixel. */
      uint i = p.x + p.y * 5u + uint(gl_SampleID) * 3u;
      o = vec4(uintBitsToFloat(values[i % 16u]),
               uintBitsToFloat(values[(i + 5u) % 16u]),
               uintBitsToFloat(values[(i + 11u) % 16u]), 1.0);
   } else if (pc.mode == 1u) {
      /* Two exact samples and their exact mean. */
      o = gl_SampleID == 0 ? vec4(1.0, 0.5, 0.25, 1.0) : vec4(3.0, 1.5, 0.75, 1.0);
   } else {
      o = vec4(0.5, 2.0, 8.0, 1.0);
   }
}
