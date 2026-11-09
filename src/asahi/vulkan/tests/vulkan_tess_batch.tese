/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#version 450
#ifdef QUAD
layout(quads, fractional_odd_spacing, cw) in;
#else
layout(triangles, equal_spacing, ccw) in;
#endif
void main()
{
#ifdef QUAD
   vec2 p = mix(mix(gl_in[0].gl_Position.xy, gl_in[1].gl_Position.xy,
                    gl_TessCoord.x),
                mix(gl_in[3].gl_Position.xy, gl_in[2].gl_Position.xy,
                    gl_TessCoord.x),
                gl_TessCoord.y);
#else
   vec2 p = gl_TessCoord.x * gl_in[0].gl_Position.xy +
            gl_TessCoord.y * gl_in[1].gl_Position.xy +
            gl_TessCoord.z * gl_in[2].gl_Position.xy;
#endif
   /* Pixels of the 1024x1024 target to clip space. */
   gl_Position = vec4(p / 512.0 - 1.0, 0, 1);
}
