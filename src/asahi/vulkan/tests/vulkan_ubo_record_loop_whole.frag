/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * The same loop with each tap record read once and the scale read before
 * the loop.
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
   const float scale = u.scale;
   float limit = texture(sampler2D(tex, smp), uv).a * 2.0;
   float n = 1.0;
   vec3 sum = texture(sampler2D(tex, smp), uv).rgb;
   for (int i = 0; i < u.count; i++) {
      const vec4 t = u.tap.v[i];
      vec2 p = uv + t.xy;
      vec4 c = texture(sampler2D(tex, smp), p);
      float coc = texture(sampler2D(tex, smp), p * 0.5).a;
      float clamped = coc > limit ? clamp(coc, 0.0, limit) : coc;
      sum += mix(sum / vec3(n), c.rgb,
                 vec3(smoothstep(t.z, t.w, clamped * scale)));
      n += 1.0;
   }
   o = vec4(sum / vec3(n), 1.0);
}
