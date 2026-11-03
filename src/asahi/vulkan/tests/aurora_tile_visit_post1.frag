#version 450
// One output, no blend: ssao_apply's shape.
layout(location = 1) out vec4 c0;
layout(push_constant) uniform P { float frame; } u;
void main() { c0 = vec4(fract(gl_FragCoord.xy / 256.0 + u.frame / 64.0), 0.5, 1); }
