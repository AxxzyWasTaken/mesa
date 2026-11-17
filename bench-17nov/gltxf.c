// gltxf: full-screen GL passes made of texelFetch reads, the shape of a shader pack's TAA / filter passes,
// timed off-screen (EGL surfaceless, GL 4.6 core). Each frame is one pass that reads four textures with
// texelFetch over a (2K+1)^2 neighbourhood plus the previous frame's output, ping-ponging between two
// RGBA16F targets so every pass is its own render pass. All fetches are in range (the sources carry a
// K-texel border), so the image must not depend on whether the driver predicates them.
// usage: gltxf <w> <h> <frames> <K> [robust]   "robust" asks for a robust-access context
// output: shader stats come from the driver (AGX_MESA_DEBUG=shaderdb); stdout has the image hash and ms/frame.
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

#ifndef EGL_CONTEXT_OPENGL_ROBUST_ACCESS
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS 0x31B2
#endif

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
static GLuint sh(GLenum t, const char *s) {
  GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, 0); glCompileShader(o); GLint ok; glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
  if (!ok) { char b[4000]; glGetShaderInfoLog(o, sizeof b, 0, b); fprintf(stderr, "compile: %s\n", b); exit(1); } return o; }

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: gltxf w h frames K [robust]\n"); return 1; }
  int W = atoi(argv[1]), H = atoi(argv[2]), F = atoi(argv[3]), K = atoi(argv[4]);
  int robust = argc > 5 && !strcmp(argv[5], "robust");
  int SW = W + 2 * K, SH = H + 2 * K;

  PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL); eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_API);
  EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                 EGL_CONTEXT_OPENGL_ROBUST_ACCESS, robust ? EGL_TRUE : EGL_FALSE, EGL_NONE};
  EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
  if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "context failed\n"); return 1; }
  GLint flags = 0; glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
  printf("driver %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

  // Sources: colour (RGBA16F), material (RGBA8), linear depth (R32F), all SW x SH, deterministic contents.
  size_t n = (size_t)SW * SH; uint32_t *rnd = malloc(n * 16); uint32_t s = 12345;
  for (size_t i = 0; i < n * 4; i++) { s = s * 1664525u + 1013904223u; rnd[i] = s; }
  uint16_t *h16 = malloc(n * 8); for (size_t i = 0; i < n * 4; i++) h16[i] = 0x3000 | (rnd[i] >> 22);  // halves in [0.125, 0.25)
  float *zf = malloc(n * 4); for (size_t i = 0; i < n; i++) zf[i] = (rnd[i] >> 8) * (1.0f / 16777216.0f);
  GLuint tex[3]; glGenTextures(3, tex);
  glBindTexture(GL_TEXTURE_2D, tex[0]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, SW, SH); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SW, SH, GL_RGBA, GL_HALF_FLOAT, h16);
  glBindTexture(GL_TEXTURE_2D, tex[1]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SW, SH); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SW, SH, GL_RGBA, GL_UNSIGNED_BYTE, rnd);
  glBindTexture(GL_TEXTURE_2D, tex[2]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, SW, SH); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SW, SH, GL_RED, GL_FLOAT, zf);
  // Two W x H RGBA16F targets: each pass reads the other one as history.
  GLuint out[2], fb[2]; glGenTextures(2, out); glGenFramebuffers(2, fb);
  for (int i = 0; i < 2; i++) {
    glBindTexture(GL_TEXTURE_2D, out[i]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[i]); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out[i], 0);
    glClearColor(0.2f, 0.2f, 0.2f, 1); glClear(GL_COLOR_BUFFER_BIT);
  }

  const char *vs = "#version 460 core\nvoid main(){ vec2 v = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(v * 2.0 - 1.0, 0, 1); }\n";
  char fs[2048];
  snprintf(fs, sizeof fs,
    "#version 460 core\n#define K %d\n"
    "layout(binding = 0) uniform sampler2D c0; layout(binding = 1) uniform sampler2D c1;\n"
    "layout(binding = 2) uniform sampler2D dz; layout(binding = 3) uniform sampler2D prev;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  ivec2 f = ivec2(gl_FragCoord.xy), p = f + ivec2(K);\n"
    "  vec4 mn = vec4(1e9), mx = vec4(-1e9), acc = vec4(0); float zmin = 1.0;\n"
    "  for (int y = -K; y <= K; y++) for (int x = -K; x <= K; x++) {\n"
    "    ivec2 q = p + ivec2(x, y);\n"
    "    vec4 a = texelFetch(c0, q, 0); vec4 b = texelFetch(c1, q, 0); float z = texelFetch(dz, q, 0).r;\n"
    "    mn = min(mn, a); mx = max(mx, a); acc += a * b; zmin = min(zmin, z);\n"
    "  }\n"
    "  vec4 h = texelFetch(prev, f, 0);\n"
    "  o = mix(clamp(h, mn, mx), acc * (1.0 / float((2 * K + 1) * (2 * K + 1))), 0.1 + 0.05 * zmin);\n"
    "}\n", K);
  GLuint pr = glCreateProgram(); glAttachShader(pr, sh(GL_VERTEX_SHADER, vs)); glAttachShader(pr, sh(GL_FRAGMENT_SHADER, fs)); glLinkProgram(pr);
  GLint ok; glGetProgramiv(pr, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "link failed\n"); return 1; }
  glUseProgram(pr);
  GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
  for (int i = 0; i < 3; i++) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, tex[i]); }
  glViewport(0, 0, W, H); glDisable(GL_DEPTH_TEST);

  double t0 = 0;
  for (int fr = 0; fr < F + 20; fr++) {
    if (fr == 20) { glFinish(); t0 = now(); }
    glBindFramebuffer(GL_FRAMEBUFFER, fb[fr & 1]);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, out[(fr + 1) & 1]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFlush();
  }
  glFinish(); double t = now() - t0;

  uint16_t *px = malloc((size_t)W * H * 8);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[(F + 19) & 1]); glReadPixels(0, 0, W, H, GL_RGBA, GL_HALF_FLOAT, px);
  printf("gltxf %dx%d K=%d taps/pixel=%d context=%s(flags 0x%x) frames=%d: %.3f ms/frame image=%016llx glerr=0x%x\n",
         W, H, K, 3 * (2 * K + 1) * (2 * K + 1) + 1, robust ? "robust" : "plain", flags, F, t * 1000 / F,
         (unsigned long long)fnv(px, (size_t)W * H * 8), glGetError());
  return 0;
}
