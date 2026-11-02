#version 450
/* A rectangle (x0, y0, x1, y1 in NDC) at depth z0 + dz * x. */
layout(push_constant) uniform PC { vec4 rect; float z0, dz, noise, alpha; } pc;
void main()
{
   vec2 t = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);
   vec2 p = mix(pc.rect.xy, pc.rect.zw, t);
   gl_Position = vec4(p, pc.z0 + pc.dz * p.x, 1.0);
}
