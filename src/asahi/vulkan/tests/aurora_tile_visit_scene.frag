#version 450
// Every target, different at each sample.
layout(location = 0) out vec4 c0;
layout(location = 1) out vec4 c1;
layout(location = 2) out vec4 c2;
float h(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.yzx + 33.33); return fract((p.x + p.y) * p.z); }
void main() {
   vec2 uv = gl_FragCoord.xy / vec2(3456, 2234);
   float n = h(vec3(gl_FragCoord.xy, gl_SampleID));
   vec3 g = vec3(uv, 0.5 + 0.5 * sin(uv.x * 6.0));
   c0 = vec4(mix(g, vec3(n), 0.3), 1);
   c1 = vec4(mix(g.zxy, vec3(n), 0.3), 1);
   c2 = vec4(mix(g.yzx, vec3(n), 0.3), 1);
}
