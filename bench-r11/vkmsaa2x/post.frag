#version 450
// Post pass over the resolved image: four bilinear taps.
layout(set = 0, binding = 2) uniform sampler2D src;
layout(location = 0) out vec4 o;
void main() {
  vec2 d = 1.0 / vec2(textureSize(src, 0)), uv = gl_FragCoord.xy * d;
  o = 0.25 * (texture(src, uv + d) + texture(src, uv - d) + texture(src, uv + vec2(d.x, -d.y)) + texture(src, uv - vec2(d.x, -d.y)));
}
