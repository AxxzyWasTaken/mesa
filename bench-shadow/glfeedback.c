// glfeedback: sampler views and render targets in the same and in later draws, EGL surfaceless, GL 4.6 core.
// Covers the asahi GL driver's feedback-loop rule: real loops (a draw's own shaders sample one of its attachments)
// and stale views (a view of the attachment is still bound from an earlier pass, but nothing in the draw reads it).
// usage: glfeedback check <w> <h>
//          one step per case, fresh resources each; stdout: a hash of every image the step makes;
//          stderr: "STEP <name>" before each step, so the driver's perf lines can be attributed to it.
//          real loops (each after a glTextureBarrier, so the earlier draw's writes are visible as GL requires):
//          loop-colour (texelFetch of the colour target), loop-depth (sampled read-only depth attachment),
//          loop-vs (vertex shader fetches texels of its colour target that the draw does not write),
//          switch (a stale view first, then a program that samples it while rendering into it).
//          stale views: stale-fs (map rendered by a sampler-less program after the main pass sampled it),
//          stale-vs (vertex-shader view left bound), stale-cs (texture sampled by a dispatch, then rendered to).
//        glfeedback bench-fs <w> <h> <frames> [map size=2048] [triangles=3000]
//          per frame: an RGBA16F moments map (S x S, + Z32F) drawn by a program without samplers, then the main
//          pass (W x H RGBA8 + Z32F) samples it filtered. The map pass leaves the main pass's view bound.
//        glfeedback bench-cs <w> <h> <frames> [triangles=3000]
//          per frame: the scene into W x H RGBA16F + Z32F (its program samples a small albedo texture), a compute
//          pass sums 4x4 blocks of the scene with texelFetch into an SSBO, a post pass tone-maps the scene with
//          the block sums into RGBA8. The dispatch leaves its view of the scene bound.
// bench stdout: ms/frame and hashes of every image.
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

static int W, H;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
static GLuint shader(GLenum type, const char *src) {
  GLuint o = glCreateShader(type); glShaderSource(o, 1, &src, 0); glCompileShader(o); GLint ok; glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
  if (!ok) { char b[2000]; glGetShaderInfoLog(o, sizeof b, 0, b); fprintf(stderr, "compile: %s\n", b); exit(1); } return o; }
static GLuint link(GLuint p) { glLinkProgram(p); GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "link failed\n"); exit(1); } return p; }
static GLuint prog(const char *vs, const char *fs) {
  GLuint p = glCreateProgram(); glAttachShader(p, shader(GL_VERTEX_SHADER, vs)); glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs)); return link(p); }
static GLuint cprog(const char *cs) { GLuint p = glCreateProgram(); glAttachShader(p, shader(GL_COMPUTE_SHADER, cs)); return link(p); }
static GLuint tex2d(GLenum fmt, int w, int h, GLenum filter) {
  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t); glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); return t; }
static GLuint fbo(GLuint colour, GLuint depth) {
  GLuint f; glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
  if (colour) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0); else { glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE); }
  if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete\n"); exit(1); } return f; }
static void bind(int unit, GLuint t) { glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, t); }
// Hash of a colour (RGBA, float or ubyte) or depth image attached to fb, w x h.
static uint64_t hash_fb(GLuint fb, int w, int h, GLenum format, GLenum type, int bpp) {
  size_t n = (size_t)w * h * bpp; void *buf = malloc(n); glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
  glReadPixels(0, 0, w, h, format, type, buf); uint64_t r = fnv(buf, n); free(buf); return r; }
#define HC8(fb, w, h) hash_fb(fb, w, h, GL_RGBA, GL_UNSIGNED_BYTE, 4)
#define HCF(fb, w, h) hash_fb(fb, w, h, GL_RGBA, GL_FLOAT, 16)
#define HZ(fb, w, h) hash_fb(fb, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, 4)
static void step(const char *name) { glFinish(); fprintf(stderr, "STEP %s\n", name); fflush(stderr); }

// Geometry: T large overlapping triangles at varying depth, generated from gl_VertexID (no buffers), as in gldepthbench.
static const char *geo_vs =
  "#version 460 core\nuniform int pass; uniform float zbias; out float vd; out vec2 sp;\n"
  "void main(){ int t = gl_VertexID / 3, k = gl_VertexID % 3; float a = float(t) * 2.399 + float(k) * 2.094 + float(pass) * 0.37;\n"
  " float r = 0.15 + 0.8 * fract(float(t) * 0.618); vec2 c = vec2(fract(float(t) * 0.754), fract(float(t) * 0.371)) * 2.0 - 1.0;\n"
  " vd = fract(float(t) * 0.137 + float(k) * 0.311); vec2 p = c + r * vec2(cos(a), sin(a)); sp = p * 0.5 + 0.5;\n"
  " gl_Position = vec4(p, clamp(vd + zbias, 0.0, 1.0) * 2.0 - 1.0, 1.0); }\n";
static const char *quad_vs = "#version 460 core\nvoid main(){ vec2 v = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(v * 2.0 - 1.0, 0, 1); }\n";
static const char *plain_fs = "#version 460 core\nin float vd; in vec2 sp; out vec4 o;\nvoid main(){ o = vec4(vd, sp, 1.0); }\n";
static const char *moments_fs = "#version 460 core\nin float vd; out vec4 o;\nvoid main(){ float z = gl_FragCoord.z; o = vec4(z, z * z, vd, 1.0); }\n";
static const char *lit_fs =      // samples the moments map at unit 0 (Chebyshev upper bound, as variance shadow maps do)
  "#version 460 core\nin float vd; in vec2 sp; layout(binding = 0) uniform sampler2D m; out vec4 o;\n"
  "void main(){ vec2 mo = texture(m, sp).xy; float var = max(mo.y - mo.x * mo.x, 1e-5), d = vd - mo.x;\n"
  " float lit = d <= 0.0 ? 1.0 : var / (var + d * d); o = vec4(vd * lit, lit, 1.0 - vd, 1.0); }\n";
static const char *fetch_fs =    // texelFetch of unit 0 at this pixel
  "#version 460 core\nlayout(binding = 0) uniform sampler2D t; out vec4 o;\n"
  "void main(){ vec4 c = texelFetch(t, ivec2(gl_FragCoord.xy), 0); o = vec4(fract(c.rgb * 1.37 + 0.11), 1.0); }\n";

static const char *scene_fs =    // samples a small albedo texture at unit 0, so the stage's view is replaced every frame
  "#version 460 core\nin float vd; in vec2 sp; layout(binding = 0) uniform sampler2D alb; out vec4 o;\n"
  "void main(){ vec3 a = texture(alb, sp * 4.0).rgb; o = vec4(a * (0.25 + 2.0 * vd), 1.0); }\n";
static GLuint albedo(void) {
  GLuint t = tex2d(GL_RGBA8, 64, 64, GL_LINEAR); uint8_t *px = malloc(64 * 64 * 4);
  for (int i = 0; i < 64 * 64; i++) { px[4 * i] = (i * 37) & 255; px[4 * i + 1] = (i * 91) & 255; px[4 * i + 2] = (i * 13) & 255; px[4 * i + 3] = 255; }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px); free(px); return t; }

static void draw_geo(GLuint p, int pass, float zbias, int tris) {
  glUseProgram(p); glUniform1i(glGetUniformLocation(p, "pass"), pass); glUniform1f(glGetUniformLocation(p, "zbias"), zbias);
  glDrawArrays(GL_TRIANGLES, 0, 3 * tris); }

static int check(void) {
  GLuint pplain = prog(geo_vs, plain_fs), pfetch = prog(quad_vs, fetch_fs), pmom = prog(geo_vs, moments_fs), plit = prog(geo_vs, lit_fs);
  glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glClearColor(0, 0, 0, 1); glClearDepth(1.0);
  { // loop-colour: four full-screen draws that texelFetch the colour target they write, a texture barrier before each.
    step("loop-colour"); GLuint t = tex2d(GL_RGBA8, W, H, GL_NEAREST), f = fbo(t, 0); glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT); draw_geo(pplain, 0, 0, 2000); bind(0, t); glUseProgram(pfetch);
    for (int i = 0; i < 4; i++) { glTextureBarrier(); glDrawArrays(GL_TRIANGLES, 0, 3); }
    printf("loop-colour target=%016llx\n", (unsigned long long)HC8(f, W, H)); }
  { // loop-depth: depth written, then tested without writes while the fragment shader fetches it (soft particles).
    step("loop-depth"); GLuint z = tex2d(GL_DEPTH_COMPONENT32F, W, H, GL_NEAREST), c = tex2d(GL_RGBA8, W, H, GL_NEAREST), f = fbo(c, z);
    const char *soft_fs = "#version 460 core\nin float vd; layout(binding = 0) uniform sampler2D zt; out vec4 o;\n"
      "void main(){ float d = texelFetch(zt, ivec2(gl_FragCoord.xy), 0).r; o = vec4(clamp((d - gl_FragCoord.z) * 16.0, 0.0, 1.0), d, vd, 1.0); }\n";
    GLuint psoft = prog(geo_vs, soft_fs); glViewport(0, 0, W, H); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); draw_geo(pplain, 0, 0, 3000);
    bind(0, z); glTextureBarrier(); glDepthMask(GL_FALSE); glDepthFunc(GL_LEQUAL); draw_geo(psoft, 1, -0.05f, 1000); glDepthMask(GL_TRUE); glDisable(GL_DEPTH_TEST);
    printf("loop-depth colour=%016llx depth=%016llx\n", (unsigned long long)HC8(f, W, H), (unsigned long long)HZ(f, W, H)); }
  { // loop-vs: the vertex shader fetches texels of the target it renders into, from its top row, which the draw does not
    // write (scissored to the lower half): disjoint texels, so the result is defined.
    step("loop-vs"); GLuint t = tex2d(GL_RGBA8, W, H, GL_NEAREST), f = fbo(t, 0);
    const char *vs = "#version 460 core\nlayout(binding = 0) uniform sampler2D t; out vec4 col;\n"
      "void main(){ int i = gl_VertexID / 3, k = gl_VertexID % 3; ivec2 sz = textureSize(t, 0); vec4 c = texelFetch(t, ivec2((i * 37) % sz.x, sz.y - 1), 0);\n"
      " vec2 p = vec2(fract(float(i) * 0.754), fract(float(i) * 0.371)) * 2.0 - 1.0 + 0.3 * vec2(cos(float(k) * 2.094 + c.r * 6.0), sin(float(k) * 2.094 + c.g * 6.0));\n"
      " col = vec4(c.bgr, 1.0); gl_Position = vec4(p, 0.0, 1.0); }\n";
    const char *fs = "#version 460 core\nin vec4 col; out vec4 o;\nvoid main(){ o = col; }\n";
    GLuint pvs = prog(vs, fs); glViewport(0, 0, W, H); glClear(GL_COLOR_BUFFER_BIT); draw_geo(pplain, 2, 0, 2000);
    bind(0, t); glTextureBarrier(); glEnable(GL_SCISSOR_TEST); glScissor(0, 0, W, H / 2); glUseProgram(pvs); glDrawArrays(GL_TRIANGLES, 0, 3 * 500);
    glDisable(GL_SCISSOR_TEST); printf("loop-vs target=%016llx\n", (unsigned long long)HC8(f, W, H)); }
  { // switch: the target's view is bound from a pass into another image; a program without samplers draws into the
    // target (stale view); then a program that fetches the target draws into it (a real loop, after a texture barrier).
    step("switch"); GLuint t = tex2d(GL_RGBA8, W, H, GL_NEAREST), ft = fbo(t, 0), c = tex2d(GL_RGBA8, W, H, GL_NEAREST), fc = fbo(c, 0);
    glViewport(0, 0, W, H); glBindFramebuffer(GL_FRAMEBUFFER, ft); glClear(GL_COLOR_BUFFER_BIT); draw_geo(pplain, 3, 0, 1500);
    bind(0, t); glBindFramebuffer(GL_FRAMEBUFFER, fc); glClear(GL_COLOR_BUFFER_BIT); glUseProgram(pfetch); glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, ft); draw_geo(pplain, 4, 0, 1500);
    fprintf(stderr, "STEP switch: sampling program\n"); glTextureBarrier(); glUseProgram(pfetch); glDrawArrays(GL_TRIANGLES, 0, 3);
    printf("switch target=%016llx other=%016llx\n", (unsigned long long)HC8(ft, W, H), (unsigned long long)HC8(fc, W, H)); }
  { // stale-fs: three frames of a moments map drawn without samplers, then sampled by the main pass.
    step("stale-fs"); int S = 1024; GLuint m = tex2d(GL_RGBA16F, S, S, GL_LINEAR), fm = fbo(m, 0), c = tex2d(GL_RGBA8, W, H, GL_NEAREST), fc = fbo(c, 0);
    for (int fr = 0; fr < 3; fr++) {
      glBindFramebuffer(GL_FRAMEBUFFER, fm); glViewport(0, 0, S, S); glClear(GL_COLOR_BUFFER_BIT); bind(0, 0); draw_geo(pmom, 5 + fr, 0, 1500);
      glBindFramebuffer(GL_FRAMEBUFFER, fc); glViewport(0, 0, W, H); glClear(GL_COLOR_BUFFER_BIT); bind(0, m); draw_geo(plit, fr, 0, 1500); }
    printf("stale-fs map=%016llx colour=%016llx\n", (unsigned long long)HCF(fm, S, S), (unsigned long long)HC8(fc, W, H)); }
  { // stale-vs: a vertex shader samples the image, then programs without vertex samplers draw into it.
    step("stale-vs"); GLuint t = tex2d(GL_RGBA8, W, H, GL_NEAREST), ft = fbo(t, 0), c = tex2d(GL_RGBA8, W, H, GL_NEAREST), fc = fbo(c, 0);
    const char *vs = "#version 460 core\nlayout(binding = 0) uniform sampler2D t; out vec4 col;\n"
      "void main(){ int i = gl_VertexID / 3, k = gl_VertexID % 3; ivec2 sz = textureSize(t, 0); vec4 c = texelFetch(t, ivec2((i * 53) % sz.x, (i * 31) % sz.y), 0);\n"
      " vec2 p = vec2(fract(float(i) * 0.618), fract(float(i) * 0.291)) * 2.0 - 1.0 + 0.25 * vec2(cos(float(k) * 2.094), sin(float(k) * 2.094));\n"
      " col = vec4(c.rgb, 1.0); gl_Position = vec4(p, 0.0, 1.0); }\n";
    const char *fs = "#version 460 core\nin vec4 col; out vec4 o;\nvoid main(){ o = col; }\n";
    GLuint pvs = prog(vs, fs); glViewport(0, 0, W, H);
    for (int fr = 0; fr < 3; fr++) {
      glBindFramebuffer(GL_FRAMEBUFFER, ft); glClear(GL_COLOR_BUFFER_BIT); draw_geo(pplain, 6 + fr, 0, 1500);
      glBindFramebuffer(GL_FRAMEBUFFER, fc); glClear(GL_COLOR_BUFFER_BIT); bind(0, t); glUseProgram(pvs); glDrawArrays(GL_TRIANGLES, 0, 3 * 500); bind(0, 0); }
    printf("stale-vs image=%016llx colour=%016llx\n", (unsigned long long)HC8(ft, W, H), (unsigned long long)HC8(fc, W, H)); }
  { // stale-cs: three frames of: draw into the image (the program samples an albedo texture, so the fragment stage's
    // view is replaced), a dispatch texelFetches the image, a pass samples it into another image.
    step("stale-cs"); GLuint t = tex2d(GL_RGBA8, W, H, GL_NEAREST), ft = fbo(t, 0), c = tex2d(GL_RGBA8, W, H, GL_NEAREST), fc = fbo(c, 0);
    const char *cs = "#version 460 core\nlayout(local_size_x = 8, local_size_y = 8) in; layout(binding = 0) uniform sampler2D t;\n"
      "layout(std430, binding = 0) buffer B { float s[]; };\n"
      "void main(){ ivec2 q = ivec2(gl_GlobalInvocationID.xy), sz = textureSize(t, 0) / 4; if (any(greaterThanEqual(q, sz))) return; float a = 0.0;\n"
      " for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) a += dot(texelFetch(t, q * 4 + ivec2(x, y), 0).rgb, vec3(0.3, 0.6, 0.1));\n"
      " s[q.y * sz.x + q.x] = a; }\n";
    GLuint pcs = cprog(cs), pscene = prog(geo_vs, scene_fs), alb = albedo(), ssbo; int bw = W / 4, bh = H / 4; glGenBuffers(1, &ssbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (size_t)bw * bh * 4, NULL, GL_DYNAMIC_COPY); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    glViewport(0, 0, W, H);
    for (int fr = 0; fr < 3; fr++) {
      glBindFramebuffer(GL_FRAMEBUFFER, ft); glClear(GL_COLOR_BUFFER_BIT); bind(0, alb); draw_geo(pscene, 9 + fr, 0, 1500);
      bind(0, t); glUseProgram(pcs); glDispatchCompute((bw + 7) / 8, (bh + 7) / 8, 1); glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
      glBindFramebuffer(GL_FRAMEBUFFER, fc); glClear(GL_COLOR_BUFFER_BIT); glUseProgram(pfetch); glDrawArrays(GL_TRIANGLES, 0, 3); }
    float *s = malloc((size_t)bw * bh * 4); glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (size_t)bw * bh * 4, s);
    printf("stale-cs image=%016llx colour=%016llx sums=%016llx\n", (unsigned long long)HC8(ft, W, H), (unsigned long long)HC8(fc, W, H),
           (unsigned long long)fnv(s, (size_t)bw * bh * 4)); free(s); }
  step("end");
  printf("glfeedback check %dx%d glerr=0x%x\n", W, H, glGetError()); return 0;
}

static int bench_fs(int F, int S, int T) {
  GLuint pmom = prog(geo_vs, moments_fs), plit = prog(geo_vs, lit_fs);
  GLuint m = tex2d(GL_RGBA16F, S, S, GL_LINEAR), mz = tex2d(GL_DEPTH_COMPONENT32F, S, S, GL_NEAREST), fm = fbo(m, mz);
  GLuint c = tex2d(GL_RGBA8, W, H, GL_NEAREST), z = tex2d(GL_DEPTH_COMPONENT32F, W, H, GL_NEAREST), fc = fbo(c, z);
  glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glClearColor(0, 0, 0, 1); glClearDepth(1.0);
  double t0 = 0;
  for (int fr = 0; fr < F + 10; fr++) {
    if (fr == 10) { glFinish(); t0 = now(); }
    glBindFramebuffer(GL_FRAMEBUFFER, fm); glViewport(0, 0, S, S); glClearColor(1, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bind(0, 0); draw_geo(pmom, 7, 0, T);
    glBindFramebuffer(GL_FRAMEBUFFER, fc); glViewport(0, 0, W, H); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bind(0, m); draw_geo(plit, 0, 0, T); glFlush();
  }
  glFinish(); double t = now() - t0;
  printf("glfeedback bench-fs %dx%d map %d^2 tris %d+%d frames=%d: %.3f ms/frame colour=%016llx depth=%016llx samples=%016llx glerr=0x%x\n",
         W, H, S, T, T, F, t * 1000 / F, (unsigned long long)HC8(fc, W, H), (unsigned long long)HZ(fc, W, H),
         (unsigned long long)HCF(fm, S, S), glGetError());
  return 0;
}

static int bench_cs(int F, int T) {
  const char *cs = "#version 460 core\nlayout(local_size_x = 8, local_size_y = 8) in; layout(binding = 0) uniform sampler2D t;\n"
    "layout(std430, binding = 0) buffer B { float s[]; };\n"
    "void main(){ ivec2 q = ivec2(gl_GlobalInvocationID.xy), sz = textureSize(t, 0) / 4; if (any(greaterThanEqual(q, sz))) return; float a = 0.0;\n"
    " for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) a += dot(texelFetch(t, q * 4 + ivec2(x, y), 0).rgb, vec3(0.3, 0.6, 0.1));\n"
    " s[q.y * sz.x + q.x] = a * (1.0 / 16.0); }\n";
  const char *post_fs = "#version 460 core\nlayout(binding = 0) uniform sampler2D sc; layout(std430, binding = 0) readonly buffer B { float s[]; };\n"
    "uniform int bw; out vec4 o;\n"
    "void main(){ ivec2 q = ivec2(gl_FragCoord.xy); float e = s[(q.y / 4) * bw + q.x / 4]; vec3 c = texelFetch(sc, q, 0).rgb;\n"
    " o = vec4(c / (c + vec3(0.5 + e)), 1.0); }\n";
  GLuint pscene = prog(geo_vs, scene_fs), pcs = cprog(cs), ppost = prog(quad_vs, post_fs);
  GLuint alb = albedo();
  GLuint sc = tex2d(GL_RGBA16F, W, H, GL_NEAREST), z = tex2d(GL_DEPTH_COMPONENT32F, W, H, GL_NEAREST), fs = fbo(sc, z);
  GLuint fin = tex2d(GL_RGBA8, W, H, GL_NEAREST), ff = fbo(fin, 0);
  int bw = W / 4, bh = H / 4; GLuint ssbo; glGenBuffers(1, &ssbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER, (size_t)bw * bh * 4, NULL, GL_DYNAMIC_COPY); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
  glUseProgram(ppost); glUniform1i(glGetUniformLocation(ppost, "bw"), bw);
  glClearColor(0, 0, 0, 1); glClearDepth(1.0); glViewport(0, 0, W, H);
  double t0 = 0;
  for (int fr = 0; fr < F + 10; fr++) {
    if (fr == 10) { glFinish(); t0 = now(); }
    glBindFramebuffer(GL_FRAMEBUFFER, fs); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bind(0, alb); draw_geo(pscene, 0, 0, T);
    bind(0, sc); glUseProgram(pcs); glDispatchCompute((bw + 7) / 8, (bh + 7) / 8, 1); glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, ff); glDisable(GL_DEPTH_TEST); glUseProgram(ppost); glDrawArrays(GL_TRIANGLES, 0, 3); glFlush();
  }
  glFinish(); double t = now() - t0;
  float *s = malloc((size_t)bw * bh * 4); glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (size_t)bw * bh * 4, s);
  printf("glfeedback bench-cs %dx%d tris %d frames=%d: %.3f ms/frame image=%016llx colour=%016llx depth=%016llx post=%016llx glerr=0x%x\n",
         W, H, T, F, t * 1000 / F, (unsigned long long)fnv(s, (size_t)bw * bh * 4), (unsigned long long)HCF(fs, W, H),
         (unsigned long long)HZ(fs, W, H), (unsigned long long)HC8(ff, W, H), glGetError());
  free(s); return 0;
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: glfeedback check|bench-fs|bench-cs w h [frames ...]\n"); return 1; }
  W = atoi(argv[2]); H = atoi(argv[3]);
  PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL); eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_API);
  EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
  if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "context failed\n"); return 1; }
  printf("driver %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
  if (!strcmp(argv[1], "check")) return check();
  if (argc < 5) { fprintf(stderr, "bench needs frames\n"); return 1; }
  int F = atoi(argv[4]);
  if (!strcmp(argv[1], "bench-fs")) return bench_fs(F, argc > 5 ? atoi(argv[5]) : 2048, argc > 6 ? atoi(argv[6]) : 3000);
  if (!strcmp(argv[1], "bench-cs")) return bench_cs(F, argc > 5 ? atoi(argv[5]) : 3000);
  fprintf(stderr, "unknown mode %s\n", argv[1]); return 1;
}
