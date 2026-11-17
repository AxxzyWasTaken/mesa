// gldepthcomp: depth (Z32F) through every path a game uses, hashed so compressed and
// uncompressed runs can be compared byte for byte. EGL surfaceless, GL 4.6 core.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static uint64_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
static GLuint prog(const char *vs, const char *fs) {
  GLuint p = glCreateProgram(); const char *s[2] = {vs, fs}; GLenum t[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
  for (int i = 0; i < 2; i++) { GLuint sh = glCreateShader(t[i]); glShaderSource(sh, 1, &s[i], 0); glCompileShader(sh); GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok); if (!ok) { char b[2000]; glGetShaderInfoLog(sh, 2000, 0, b); fprintf(stderr, "compile: %s\n", b); exit(1); } glAttachShader(p, sh); }
  glLinkProgram(p); return p; }
int main(int argc, char **argv) {
  int W = argc > 1 ? atoi(argv[1]) : 1024, H = argc > 2 ? atoi(argv[2]) : 1024;
  PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL); eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_API);
  EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca); if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "ctx\n"); return 1; }
  printf("renderer %s\n", glGetString(GL_RENDERER));
  GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
  // Scene: many overlapping triangles at varying depth (gl_VertexID driven, no buffers).
  const char *vs = "#version 460\nuniform int pass; out float vd;\nvoid main(){ int t=gl_VertexID/3, k=gl_VertexID%3; float a=float(t)*2.399+float(k)*2.094+float(pass)*0.37; float r=0.15+0.8*fract(float(t)*0.618);\n vec2 c=vec2(fract(float(t)*0.754)*2.0-1.0, fract(float(t)*0.371)*2.0-1.0); vd=fract(float(t)*0.137+float(k)*0.311);\n gl_Position=vec4(c+r*vec2(cos(a),sin(a)), vd*2.0-1.0, 1.0);}";
  const char *fs = "#version 460\nin float vd; out vec4 o; void main(){ o=vec4(vd, gl_FragCoord.z, 0.25, 1.0); }";
  GLuint p = prog(vs, fs);
  GLuint z, col, fb; glGenTextures(1, &z); glBindTexture(GL_TEXTURE_2D, z); glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, W, H);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenTextures(1, &col); glBindTexture(GL_TEXTURE_2D, col); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, W, H);
  glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, z, 0); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo\n"); return 1; }
  glViewport(0, 0, W, H); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glClearDepth(1.0); glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glUseProgram(p);
  // Pass 0, then a second batch on top (depth load + test against stored depth).
  glUniform1i(glGetUniformLocation(p, "pass"), 0); glDrawArrays(GL_TRIANGLES, 0, 3 * 3000); glFlush(); glFinish();
  glUniform1i(glGetUniformLocation(p, "pass"), 1); glDepthFunc(GL_LEQUAL); glDrawArrays(GL_TRIANGLES, 0, 3 * 3000); glFinish();
  size_t n = (size_t)W * H; float *buf = malloc(n * 16);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, buf); printf("color after 2 passes  %016llx\n", (unsigned long long)fnv(buf, n * 16));
  glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, buf); printf("depth readpixels      %016llx\n", (unsigned long long)fnv(buf, n * 4));
  // Sample the depth texture (plain and shadow compare) into an R32F target.
  GLuint out, fb2; glGenTextures(1, &out); glBindTexture(GL_TEXTURE_2D, out); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, W, H);
  glGenFramebuffers(1, &fb2); glBindFramebuffer(GL_FRAMEBUFFER, fb2); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
  const char *qvs = "#version 460\nvoid main(){ vec2 v=vec2((gl_VertexID<<1)&2, gl_VertexID&2); gl_Position=vec4(v*2.0-1.0,0,1);}";
  const char *sfs = "#version 460\nuniform sampler2D zt; uniform sampler2DShadow zs; out vec4 o;\nvoid main(){ ivec2 q=ivec2(gl_FragCoord.xy); float a=texelFetch(zt,q,0).r; vec2 uv=(vec2(q)+0.5)/vec2(textureSize(zt,0));\n float b=texture(zt,uv).r; float s=texture(zs,vec3(uv,0.5)); float s2=texture(zs,vec3(uv,b)); o=vec4(a,b,s,s2); }";
  GLuint sp = prog(qvs, sfs); glUseProgram(sp); glDisable(GL_DEPTH_TEST);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, z); glUniform1i(glGetUniformLocation(sp, "zt"), 0);
  GLuint smp; glGenSamplers(1, &smp); glSamplerParameteri(smp, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE); glSamplerParameteri(smp, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, z); glBindSampler(1, smp); glUniform1i(glGetUniformLocation(sp, "zs"), 1);
  glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
  glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, buf); printf("depth sampled+shadow  %016llx\n", (unsigned long long)fnv(buf, n * 16));
  // Blit depth to a second depth texture and read it.
  GLuint z2, fb3; glGenTextures(1, &z2); glBindTexture(GL_TEXTURE_2D, z2); glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, W, H);
  glGenFramebuffers(1, &fb3); glBindFramebuffer(GL_FRAMEBUFFER, fb3); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, z2, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fb); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb3); glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_DEPTH_BUFFER_BIT, GL_NEAREST); glFinish();
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fb3); glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, buf); printf("depth blit copy       %016llx\n", (unsigned long long)fnv(buf, n * 4));
  // Partial clear + more draws (fast-clear metadata path), then read.
  glBindFramebuffer(GL_FRAMEBUFFER, fb); glEnable(GL_SCISSOR_TEST); glScissor(W / 4, H / 4, W / 2, H / 3); glClearDepth(0.75); glClear(GL_DEPTH_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
  glUseProgram(p); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glUniform1i(glGetUniformLocation(p, "pass"), 2); glDrawArrays(GL_TRIANGLES, 0, 3 * 2000); glFinish();
  glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, buf); printf("depth scissor clear   %016llx\n", (unsigned long long)fnv(buf, n * 4));
  glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, buf); printf("color final           %016llx\n", (unsigned long long)fnv(buf, n * 16));
  printf("gl error %x\n", glGetError()); return 0;
}
