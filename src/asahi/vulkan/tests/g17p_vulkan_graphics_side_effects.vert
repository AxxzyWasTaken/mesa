#version 450

layout(location = 0) in vec2 position;
layout(set = 0, binding = 0, std430) buffer SideEffects {
   uint vertex_atomic;
   uint vertex_values[3];
   uint fragment_atomic;
   uint fragment_value;
} side;

void main()
{
   atomicAdd(side.vertex_atomic, 1u);
   side.vertex_values[gl_VertexIndex] = 0x17000000u + uint(gl_VertexIndex);
   gl_Position = vec4(position, 0.0, 1.0);
}
