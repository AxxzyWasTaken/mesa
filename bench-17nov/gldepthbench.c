// gldepthbench: a depth-heavy GL frame with Z32F depth, timed off-screen (EGL surfaceless, GL 4.6 core).
// Per frame: (1) shadow map pass, S x S Z32F, depth only; (2) main pass, W x H RGBA8 + Z32F, depth-tested
// geometry whose fragment shader samples the shadow map with a compare sampler; (3) an AO-style full-screen
// pass that reads the main depth with 8 texelFetch taps and one filtered tap; (4) a translucent pass back
// on the main framebuffer (colour and depth are loaded again), depth-tested without depth writes.
// So depth is stored, loaded, sampled with compare, fetched and filtered every frame, as in a game.
// usage: gldepthbench <w> <h> <frames> [shadow size=2048] [triangles=3000] [composite]
//   composite: (5) a full-screen pass samples the main colour and the AO image into a final RGBA8 image.
//   Without it, the next frame's shadow pass (a program without samplers) leaves the previous pass's
//   sampler view of the shadow map bound in Gallium, and the GL driver's feedback-loop rule decompresses
//   the shadow map after the first frame. With it, the shadow map stays compressed.
// stdout: ms/frame and hashes of the final colour, AO and depth images (and the composite, if drawn).
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
static GLuint prog(const char *vs, const char *fs) {
  GLuint p = glCreateProgram(); const char *s[2] = {vs, fs}; GLenum t[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
  for (int i = 0; i < 2; i++) {
    GLuint o = glCreateShader(t[i]); glShaderSource(o, 1, &s[i], 0); glCompileShader(o); GLint ok; glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[2000]; glGetShaderInfoLog(o, sizeof b, 0, b); fprintf(stderr, "compile: %s\n", b); exit(1); }
    glAttachShader(p, o);
  }
  glLinkProgram(p); GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "link failed\n"); exit(1); } return p; }
static GLuint tex2d(GLenum fmt, int w, int h, GLenum filter) {
  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t); glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); return t; }

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: gldepthbench w h frames [shadow] [tris]\n"); return 1; }
  int W = atoi(argv[1]), H = atoi(argv[2]), F = atoi(argv[3]);
  int S = argc > 4 ? atoi(argv[4]) : 2048, T = argc > 5 ? atoi(argv[5]) : 3000;
  int composite = argc > 6 && !strcmp(argv[6], "composite");
  PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL); eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_API);
  EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
  if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "context failed\n"); return 1; }
  printf("driver %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);

  // Geometry: T large overlapping triangles at varying depth, generated from gl_VertexID (no buffers).
  const char *geo_vs =
    "#version 460 core\nuniform int pass; uniform float zbias; out float vd; out vec2 sp;\n"
    "void main(){ int t = gl_VertexID / 3, k = gl_VertexID % 3; float a = float(t) * 2.399 + float(k) * 2.094 + float(pass) * 0.37;\n"
    " float r = 0.15 + 0.8 * fract(float(t) * 0.618); vec2 c = vec2(fract(float(t) * 0.754), fract(float(t) * 0.371)) * 2.0 - 1.0;\n"
    " vd = fract(float(t) * 0.137 + float(k) * 0.311); vec2 p = c + r * vec2(cos(a), sin(a)); sp = p * 0.5 + 0.5;\n"
    " gl_Position = vec4(p, (vd + zbias) * 2.0 - 1.0, 1.0); }\n";
  const char *shadow_fs = "#version 460 core\nvoid main(){}\n";
  const char *main_fs =
    "#version 460 core\nin float vd; in vec2 sp; layout(binding = 0) uniform sampler2DShadow sm; uniform float alpha; out vec4 o;\n"
    "void main(){ float lit = texture(sm, vec3(sp, vd + 0.002)); o = vec4(vd, lit, 1.0 - vd, alpha); }\n";
  const char *quad_vs = "#version 460 core\nvoid main(){ vec2 v = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(v * 2.0 - 1.0, 0, 1); }\n";
  const char *ao_fs =
    "#version 460 core\nlayout(binding = 1) uniform sampler2D zt; out vec4 o;\n"
    "void main(){ ivec2 q = ivec2(gl_FragCoord.xy), sz = textureSize(zt, 0) - 1; float z = texelFetch(zt, q, 0).r, occ = 0.0;\n"
    " const ivec2 off[8] = ivec2[8](ivec2(-3,-1), ivec2(2,-3), ivec2(3,2), ivec2(-1,3), ivec2(-6,2), ivec2(5,-5), ivec2(1,6), ivec2(-4,-6));\n"
    " for (int i = 0; i < 8; i++) occ += step(texelFetch(zt, clamp(q + off[i], ivec2(0), sz), 0).r, z - 0.001);\n"
    " float zf = texture(zt, (vec2(q) + 0.75) / vec2(sz + 1)).r; o = vec4(occ / 8.0, z, zf, 1.0); }\n";
  const char *comp_fs =
    "#version 460 core\nlayout(binding = 0) uniform sampler2D col; layout(binding = 1) uniform sampler2D ao; out vec4 o;\n"
    "void main(){ ivec2 q = ivec2(gl_FragCoord.xy); vec4 c = texelFetch(col, q, 0); o = vec4(c.rgb * (1.0 - 0.5 * texelFetch(ao, q, 0).r), 1.0); }\n";
  GLuint pgeo_shadow = prog(geo_vs, shadow_fs), pgeo = prog(geo_vs, main_fs), pao = prog(quad_vs, ao_fs), pcomp = prog(quad_vs, comp_fs);

  GLuint smap = tex2d(GL_DEPTH_COMPONENT32F, S, S, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  GLuint zmain = tex2d(GL_DEPTH_COMPONENT32F, W, H, GL_LINEAR), cmain = tex2d(GL_RGBA8, W, H, GL_NEAREST), aotex = tex2d(GL_RGBA8, W, H, GL_NEAREST);
  GLuint fintex = tex2d(GL_RGBA8, W, H, GL_NEAREST);
  GLuint fb[4]; glGenFramebuffers(4, fb);
  glBindFramebuffer(GL_FRAMEBUFFER, fb[3]); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fintex, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, fb[0]); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, smap, 0); glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
  glBindFramebuffer(GL_FRAMEBUFFER, fb[1]); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cmain, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, zmain, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, fb[2]); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, aotex, 0);
  for (int i = 0; i < 3; i++) { glBindFramebuffer(GL_FRAMEBUFFER, fb[i]); if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo %d\n", i); return 1; } }
  // Textures are bound only for the pass that reads them, so no pass has its own attachment bound for sampling.
#define BIND(unit, t) do { glActiveTexture(GL_TEXTURE0 + (unit)); glBindTexture(GL_TEXTURE_2D, (t)); } while (0)
  GLint ps_pass = glGetUniformLocation(pgeo_shadow, "pass"), ps_bias = glGetUniformLocation(pgeo_shadow, "zbias");
  GLint pg_pass = glGetUniformLocation(pgeo, "pass"), pg_bias = glGetUniformLocation(pgeo, "zbias"), pg_alpha = glGetUniformLocation(pgeo, "alpha");

  double t0 = 0;
  for (int fr = 0; fr < F + 10; fr++) {
    if (fr == 10) { glFinish(); t0 = now(); }
    // (1) shadow map
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]); glViewport(0, 0, S, S); glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
    glDisable(GL_BLEND); glClearDepth(1.0); glClear(GL_DEPTH_BUFFER_BIT); BIND(0, 0); BIND(1, 0);
    glUseProgram(pgeo_shadow); glUniform1i(ps_pass, 7); glUniform1f(ps_bias, 0.0f); glDrawArrays(GL_TRIANGLES, 0, 3 * T);
    // (2) main pass, samples the shadow map
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]); glViewport(0, 0, W, H); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); BIND(0, smap);
    glUseProgram(pgeo); glUniform1i(pg_pass, 0); glUniform1f(pg_bias, 0.0f); glUniform1f(pg_alpha, 1.0f); glDrawArrays(GL_TRIANGLES, 0, 3 * T);
    // (3) AO-style pass, fetches and filters the main depth
    glBindFramebuffer(GL_FRAMEBUFFER, fb[2]); glDisable(GL_DEPTH_TEST); BIND(1, zmain); glUseProgram(pao); glDrawArrays(GL_TRIANGLES, 0, 3);
    // (4) translucent pass on the main framebuffer: loads colour and depth, tests without writing depth
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]); BIND(1, 0); glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(pgeo); glUniform1i(pg_pass, 1); glUniform1f(pg_bias, -0.05f); glUniform1f(pg_alpha, 0.5f); glDrawArrays(GL_TRIANGLES, 0, 3 * (T / 4));
    glDepthMask(GL_TRUE); glDisable(GL_BLEND);
    // (5) optional composite pass
    if (composite) { glBindFramebuffer(GL_FRAMEBUFFER, fb[3]); glDisable(GL_DEPTH_TEST); BIND(0, cmain); BIND(1, aotex); glUseProgram(pcomp); glDrawArrays(GL_TRIANGLES, 0, 3); }
    glFlush();
  }
  glFinish(); double t = now() - t0;

  size_t n = (size_t)W * H; uint8_t *buf = malloc(n * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf); uint64_t hc = fnv(buf, n * 4);
  glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, buf); uint64_t hz = fnv(buf, n * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[2]); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf); uint64_t ha = fnv(buf, n * 4);
  if (composite) { glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[3]); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf); printf("composite=%016llx\n", (unsigned long long)fnv(buf, n * 4)); }
  printf("gldepthbench %dx%d shadow %d^2 tris %d+%d+%d frames=%d: %.3f ms/frame colour=%016llx ao=%016llx depth=%016llx glerr=0x%x\n",
         W, H, S, T, T, T / 4, F, t * 1000 / F, (unsigned long long)hc, (unsigned long long)ha, (unsigned long long)hz, glGetError());
  return 0;
}
