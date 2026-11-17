#version 450
// Large overlapping triangles at varying depth, generated from gl_VertexIndex (no vertex buffers).
layout(push_constant) uniform P { int pass; float alpha; float zbias; } pc;
layout(location = 0) out vec4 col;
void main() {
  int t = gl_VertexIndex / 3, k = gl_VertexIndex % 3;
  float a = float(t) * 2.399 + float(k) * 2.094 + float(pc.pass) * 0.37;
  float r = 0.15 + 0.8 * fract(float(t) * 0.618);
  vec2 c = vec2(fract(float(t) * 0.754 + float(pc.pass) * 0.13), fract(float(t) * 0.371)) * 2.0 - 1.0;
  float vd = fract(float(t) * 0.137 + float(k) * 0.311);
  col = vec4(fract(float(t) * 0.23), vd, fract(float(t + pc.pass) * 0.71), pc.alpha);
  gl_Position = vec4(c + r * vec2(cos(a), sin(a)), clamp(vd + pc.zbias, 0.0, 1.0), 1.0);
}
