#version 450
// Reads both samples of the 2x colour and depth attachments, bit-exact, for the image hash.
layout(set = 0, binding = 0) uniform sampler2DMS msc;
layout(set = 0, binding = 1) uniform sampler2DMS msz;
layout(location = 0) out uvec4 oc;
layout(location = 1) out uvec4 oz;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 a = texelFetch(msc, p, 0), b = texelFetch(msc, p, 1);
  oc = uvec4(packHalf2x16(a.xy), packHalf2x16(a.zw), packHalf2x16(b.xy), packHalf2x16(b.zw));
  oz = uvec4(floatBitsToUint(texelFetch(msz, p, 0).r), floatBitsToUint(texelFetch(msz, p, 1).r), 0u, 0u);
}
