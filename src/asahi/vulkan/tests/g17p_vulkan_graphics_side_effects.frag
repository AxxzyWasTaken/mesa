#version 450

layout(location = 0) out vec4 colour;
layout(set = 0, binding = 0, std430) buffer SideEffects {
   uint vertex_atomic;
   uint vertex_values[3];
   uint fragment_atomic;
   uint fragment_value;
} side;

void main()
{
   atomicAdd(side.fragment_atomic, 1u);
   side.fragment_value = 0xf17e0001u;
   colour = vec4(1.0, 71.0 / 255.0, 13.0 / 255.0, 1.0);
}
