#version 450
// Full-screen quad of two triangles, like a game's post-processing draw:
// nothing here proves coverage.
const vec2 q[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1),
                         vec2(-1, 1), vec2(1, -1), vec2(1, 1));
void main() { gl_Position = vec4(q[gl_VertexIndex], 0.5, 1); }
