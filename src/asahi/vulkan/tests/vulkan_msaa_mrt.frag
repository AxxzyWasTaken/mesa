/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#version 450
layout(push_constant) uniform Colours { vec4 c[8]; } pc;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;
void main()
{
   o0 = pc.c[0]; o1 = pc.c[1]; o2 = pc.c[2]; o3 = pc.c[3];
   o4 = pc.c[4]; o5 = pc.c[5]; o6 = pc.c[6]; o7 = pc.c[7];
}
