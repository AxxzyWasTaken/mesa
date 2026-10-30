/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#version 450
layout(push_constant) uniform PC { ivec4 rect; uint rgba; float level; uint quad; } pc;
void main()
{
   /* Two triangles, or one quad, covering the draw's rectangle in pixels. */
   const vec2 tri[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1),
                              vec2(1, 0), vec2(1, 1), vec2(0, 1));
   const vec2 quad[4] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 1));
   vec2 c = pc.quad != 0 ? quad[gl_VertexIndex % 4] : tri[gl_VertexIndex % 6];
   gl_Position = vec4(mix(vec2(pc.rect.xy), vec2(pc.rect.zw), c), 0, 1);
}
