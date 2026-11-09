/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#version 450
#ifdef QUAD
layout(vertices = 4) out;
#else
layout(vertices = 3) out;
#endif
layout(push_constant) uniform PC { ivec4 rect; uint rgba; float level; uint quad; } pc;
void main()
{
   gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
   if (gl_InvocationID == 0) {
      gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = pc.level;
      gl_TessLevelOuter[2] = gl_TessLevelOuter[3] = pc.level;
      gl_TessLevelInner[0] = gl_TessLevelInner[1] = pc.level;
   }
}
