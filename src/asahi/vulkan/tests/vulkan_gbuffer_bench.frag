#version 450
/* G-buffer content: a smooth gradient plus `noise` of per-pixel hash. */
layout(push_constant) uniform PC { vec4 rect; float z0, dz, noise, alpha; } pc;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
float hash(uvec2 c, uint s)
{
   uint h = c.x * 1973u + c.y * 9277u + s * 26699u;
   h = (h ^ (h >> 13)) * 0x5bd1e995u;
   return float(h >> 8) / 16777216.0;
}
void main()
{
   uvec2 c = uvec2(gl_FragCoord.xy);
   vec2 g = fract(gl_FragCoord.xy / vec2(1024.0, 768.0));
   vec3 v = vec3(g, 0.5 + 0.5 * sin(gl_FragCoord.x / 300.0));
   vec3 n = vec3(hash(c, 1u), hash(c, 2u), hash(c, 3u));
   v = mix(v, n, pc.noise);
   o0 = vec4(v, pc.alpha);
   o1 = vec4(v.zxy, pc.alpha);
   o2 = vec4(v.yzx, pc.alpha);
}
