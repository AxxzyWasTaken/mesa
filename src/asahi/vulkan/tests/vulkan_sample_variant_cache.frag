#version 450
/* An alpha-tested material: discard makes the shader's coverage depend on the
 * sample count, so a dynamic-sample-count pipeline builds its multisample
 * variant at the first draw. The arithmetic only makes compiling it take
 * long enough to time. */
layout(location = 0) out vec4 o;
layout(push_constant) uniform P { float k; } p;
void main()
{
   vec4 a = vec4(gl_FragCoord.xy * 0.004, p.k, 1.0);
   for (int i = 0; i < 48; i++)
      a = sin(a * 1.31 + vec4(float(i))) * cos(a.yzwx * 0.7) + a.wxyz * 0.1;
   if (fract(gl_FragCoord.x * 0.05) > 0.5)
      discard;
   o = a * 0.5 + 0.5;
}
