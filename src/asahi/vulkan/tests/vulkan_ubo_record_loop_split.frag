/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Counter-Strike 2's depth-of-field gather (its tap loop, as decompiled from
 * the game's SPIR-V): it reads each tap record's members in separate
 * expressions.
 */
#version 450
/* Counter-Strike 2's layout: the array inside a struct, the scalars around
 * it at explicit offsets. */
struct Records { vec4 v[256]; };
layout(set = 0, binding = 0, std140) uniform Taps
{
   layout(offset = 20) float scale;
   layout(offset = 48) Records tap;
   layout(offset = 4144) int count;
} u;
layout(set = 0, binding = 1) uniform texture2D tex;
layout(set = 0, binding = 2) uniform sampler smp;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
   float limit = texture(sampler2D(tex, smp), uv).a * 2.0;
   float n = 1.0;
   vec3 sum = texture(sampler2D(tex, smp), uv).rgb;
   for (int i = 0; i < u.count; i++) {
      vec2 p = uv + u.tap.v[i].xy;
      vec4 c = texture(sampler2D(tex, smp), p);
      float coc = texture(sampler2D(tex, smp), p * 0.5).a;
      float clamped = coc > limit ? clamp(coc, 0.0, limit) : coc;
      sum += mix(sum / vec3(n), c.rgb,
                 vec3(smoothstep(u.tap.v[i].z, u.tap.v[i].w, clamped * u.scale)));
      n += 1.0;
   }
   o = vec4(sum / vec3(n), 1.0);
}
