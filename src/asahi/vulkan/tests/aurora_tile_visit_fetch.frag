#version 450
// Read-back: the exact bits of one sample into a 1x RGBA32F target.
layout(location = 0) out vec4 c;
layout(set = 0, binding = 0) uniform sampler2DMS t;
layout(push_constant) uniform P { float frame; } u;
void main() { c = texelFetch(t, ivec2(gl_FragCoord.xy), int(u.frame)); }
