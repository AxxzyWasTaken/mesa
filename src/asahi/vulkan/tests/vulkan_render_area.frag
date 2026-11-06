/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#version 450
layout(push_constant) uniform Colour { uint rgba; } pc;
layout(location = 0) out vec4 o;
void main()
{
   o = unpackUnorm4x8(pc.rgba);
}
