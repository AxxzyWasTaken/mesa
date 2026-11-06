#version 450
/* Samples a 4x4 texture across three times its extent, so the outer ring of
 * the target reads the sampler's border colour. The arithmetic only makes
 * compiling the shader take long enough to time. */
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 o;
void main()
{
   vec2 uv = gl_FragCoord.xy / 256.0 * 3.0 - 1.0;
   vec4 a = vec4(gl_FragCoord.xy * 0.004, 0.3, 1.0);
   for (int i = 0; i < 48; i++)
      a = sin(a * 1.31 + vec4(float(i))) * cos(a.yzwx * 0.7) + a.wxyz * 0.1;
   o = texture(tex, uv) + a * 1e-6;
}
