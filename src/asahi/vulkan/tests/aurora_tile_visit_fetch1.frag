#version 450
// Read-back: the exact bits of one sample into a 1x RGBA32F target.
layout(location = 0) out vec4 c;
layout(set = 0, binding = 0) uniform sampler2D t;
void main() { c = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }
