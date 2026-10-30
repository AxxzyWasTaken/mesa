#version 450
/* Full-resolution 1x pass: five taps of the resolved colour and depth. */
layout(set = 0, binding = 0) uniform sampler2D colour;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(location = 0) out vec4 o;
void main()
{
   ivec2 c = ivec2(gl_FragCoord.xy);
   ivec2 m = textureSize(colour, 0) - 1;
   vec4 s = vec4(0.0);
   float d = 0.0;
   for (int i = 0; i < 5; i++) {
      ivec2 p = clamp(c + ivec2((i - 2) * 3, (i & 1) * 2 - 1), ivec2(0), m);
      s += texelFetch(colour, p, 0);
      d += texelFetch(depth, p, 0).r;
   }
   o = vec4(s.rgb / 5.0, d / 5.0);
}
