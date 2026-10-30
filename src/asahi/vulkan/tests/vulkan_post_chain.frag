/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Pass { float noise; uint pass_index; uint generate; } pc;
layout(location = 0) out vec4 o;

float hash(uvec3 v)
{
   v = v * 1664525u + 1013904223u;
   v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
   v ^= v >> 16u;
   v.x += v.y * v.z;
   return float(v.x & 0xffffu) / 65535.0;
}

void main()
{
   ivec2 p = ivec2(gl_FragCoord.xy);
   vec4 n = vec4(hash(uvec3(p, pc.pass_index)), hash(uvec3(p, pc.pass_index + 7u)),
                 hash(uvec3(p, pc.pass_index + 13u)), 1.0);
   if (pc.generate != 0u) {
      /* Smooth gradient plus optional noise. */
      vec2 uv = gl_FragCoord.xy / vec2(textureSize(src, 0));
      o = mix(vec4(uv, 1.0 - uv.x * uv.y, 1.0), n, pc.noise);
      return;
   }
   ivec2 hi = textureSize(src, 0) - 1;
   vec4 c = 4.0 * texelFetch(src, p, 0) +
            texelFetch(src, clamp(p + ivec2(1, 0), ivec2(0), hi), 0) +
            texelFetch(src, clamp(p - ivec2(1, 0), ivec2(0), hi), 0) +
            texelFetch(src, clamp(p + ivec2(0, 1), ivec2(0), hi), 0) +
            texelFetch(src, clamp(p - ivec2(0, 1), ivec2(0), hi), 0);
   o = mix(c * 0.125, n, pc.noise);
}
