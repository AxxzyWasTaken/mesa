// sodiumbench: Sodium 0.9's real terrain shaders on Sodium's 20-byte vertex format, drawn the way Sodium
// draws (glMultiDrawElementsBaseVertex, one draw per section, shared quad index buffer), off-screen.
// usage: sodiumbench <vert> <frag> <sections> <quads per section> <frames> [w h] [mode]
//   mode "real" = Sodium shaders; "trivial" = same vertex format and draws, minimal shaders
//   quads are sized so most are a few pixels on screen, like distant terrain.
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static char *slurp(const char *p) { FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); } fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); char *b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0; fclose(f); return b; }
static GLuint sh(GLenum t, const char *s) { GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, 0); glCompileShader(o);
  GLint ok; glGetShaderiv(o, GL_COMPILE_STATUS, &ok); if (!ok) { char b[4000]; glGetShaderInfoLog(o, 4000, 0, b); fprintf(stderr, "%s\n", b); exit(1); } return o; }
static const char *TRIV_VS = "#version 460 core\nlayout(location=0) in uvec2 a_Position; layout(location=1) in vec4 a_Color;\n"
  "layout(std140) uniform u_Globals { mat4 u_ProjectionMatrix; mat4 u_ModelViewMatrix; };\nuniform vec3 u_RegionOffset;\nout vec4 v_Color;\n"
  "void main(){ vec3 p = vec3(uvec3(a_Position.x, a_Position.x >> 10, a_Position.x >> 20) & 0x3FFu) * (32.0/1024.0) - 8.0;\n"
  " gl_Position = u_ProjectionMatrix * u_ModelViewMatrix * vec4(p + u_RegionOffset, 1.0); v_Color = a_Color; }\n";
static const char *NOUBO_VS = "#version 460 core\nlayout(location=0) in uvec2 a_Position; layout(location=1) in vec4 a_Color;\n"
  "uniform vec3 u_RegionOffset;\nout vec4 v_Color;\n"
  "void main(){ vec3 p = vec3(uvec3(a_Position.x, a_Position.x >> 10, a_Position.x >> 20) & 0x3FFu) * (32.0/1024.0) - 8.0 + u_RegionOffset;\n"
  " const float f = 1.4281480, a = 1.548387, n = 0.05, fa = 1000.0;\n"
  " gl_Position = vec4(p.x * f / a, p.y * f, p.z * (fa+n)/(n-fa) + 2.0*fa*n/(n-fa), -p.z); v_Color = a_Color; }\n";
static const char *PREMUL_VS = "#version 460 core\nlayout(location=0) in uvec2 a_Position; layout(location=1) in vec4 a_Color;\n"
  "layout(std140) uniform u_Globals { mat4 u_ProjectionMatrix; mat4 u_ModelViewMatrix; };\nuniform vec3 u_RegionOffset;\nout vec4 v_Color;\n"
  "void main(){ vec3 p = vec3(uvec3(a_Position.x, a_Position.x >> 10, a_Position.x >> 20) & 0x3FFu) * (32.0/1024.0) - 8.0;\n"
  " gl_Position = u_ProjectionMatrix * vec4(p + u_RegionOffset, 1.0); v_Color = a_Color; }\n";
static const char *NOVARY_VS = "#version 460 core\nlayout(location=0) in uvec2 a_Position;\nuniform vec3 u_RegionOffset;\n"
  "void main(){ vec3 p = vec3(uvec3(a_Position.x, a_Position.x >> 10, a_Position.x >> 20) & 0x3FFu) * (32.0/1024.0) - 8.0 + u_RegionOffset;\n"
  " gl_Position = vec4(p.x * 0.92, p.y * 1.43, p.z * -1.0001 - 0.1, -p.z); }\n";
static const char *VID_VS = "#version 460 core\nuniform vec3 u_RegionOffset;\n"
  "void main(){ uint i = uint(gl_VertexID); vec3 p = vec3(float(i & 63u), float((i >> 6) & 31u), float((i >> 11) & 255u)) * 0.5 + u_RegionOffset;\n"
  " gl_Position = vec4(p.x * 0.92, p.y * 1.43, p.z * -1.0001 - 0.1, -p.z); }\n";
static const char *CONST_FS = "#version 460 core\nout vec4 fragColor; void main(){ fragColor = vec4(1.0, 0.5, 0.2, 1.0); }\n";
static const char *TRIV_FS = "#version 460 core\nin vec4 v_Color; out vec4 fragColor; void main(){ fragColor = v_Color; }\n";
int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage\n"); return 1; }
  int S = atoi(argv[3]), Q = atoi(argv[4]), frames = atoi(argv[5]);
  int W = argc > 7 ? atoi(argv[6]) : 3456, H = argc > 7 ? atoi(argv[7]) : 2234;
  const char *mode = argc > 8 ? argv[8] : "real";
  PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL); eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_API);
  EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
  if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "ctx failed\n"); return 1; }
  GLuint fb, ct, dt; glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glGenTextures(1, &ct); glBindTexture(GL_TEXTURE_2D, ct); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ct, 0);
  glGenTextures(1, &dt); glBindTexture(GL_TEXTURE_2D, dt); glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, W, H);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dt, 0);
  GLuint pr = glCreateProgram();
  if (!strcmp(mode, "real")) { glAttachShader(pr, sh(GL_VERTEX_SHADER, slurp(argv[1]))); glAttachShader(pr, sh(GL_FRAGMENT_SHADER, slurp(argv[2]))); }
  else if (!strcmp(mode, "novary")) { glAttachShader(pr, sh(GL_VERTEX_SHADER, NOVARY_VS)); glAttachShader(pr, sh(GL_FRAGMENT_SHADER, CONST_FS)); }
  else if (!strcmp(mode, "vid")) { glAttachShader(pr, sh(GL_VERTEX_SHADER, VID_VS)); glAttachShader(pr, sh(GL_FRAGMENT_SHADER, CONST_FS)); }
  else if (!strcmp(mode, "noubo")) { glAttachShader(pr, sh(GL_VERTEX_SHADER, NOUBO_VS)); glAttachShader(pr, sh(GL_FRAGMENT_SHADER, TRIV_FS)); }
  else if (!strcmp(mode, "onemat")) { glAttachShader(pr, sh(GL_VERTEX_SHADER, PREMUL_VS)); glAttachShader(pr, sh(GL_FRAGMENT_SHADER, TRIV_FS)); }
  else { glAttachShader(pr, sh(GL_VERTEX_SHADER, TRIV_VS)); glAttachShader(pr, sh(GL_FRAGMENT_SHADER, TRIV_FS)); }
  glLinkProgram(pr); GLint ok; glGetProgramiv(pr, GL_LINK_STATUS, &ok); if (!ok) { char b[4000]; glGetProgramInfoLog(pr, 4000, 0, b); fprintf(stderr, "link: %s\n", b); return 1; }
  glUseProgram(pr);
  // globals UBO: projection (perspective, 70 deg, near 0.05), modelview looking down +z, fog far away
  float aspect = (float)W / H, f = 1.0f / tanf(35.0f * 3.14159f / 180.0f), n = 0.05f, fa = 1000.0f;
  float G[64] = {0};
  float P[16] = { f / aspect,0,0,0, 0,f,0,0, 0,0,(fa+n)/(n-fa),-1, 0,0,2*fa*n/(n-fa),0 };
  float M[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
  memcpy(G, P, 64); memcpy(G + 16, M, 64);
  G[32] = 0.6f; G[33] = 0.7f; G[34] = 0.9f; G[35] = 1.0f;   // fog colour
  G[36] = 900; G[37] = 1000; G[38] = 900; G[39] = 1000;      // environment + render fog start/end
  G[40] = 1.0f / 1024; G[41] = 1.0f / 1024; G[42] = 0.999f; G[43] = 0.999f; // texel size, shrink
  G[44] = 1.0f; ((int *)G)[45] = 0;                           // fade period inv, RGSS off
  GLuint ubo; glGenBuffers(1, &ubo); glBindBuffer(GL_UNIFORM_BUFFER, ubo); glBufferData(GL_UNIFORM_BUFFER, sizeof G, G, GL_STATIC_DRAW);
  GLuint bi = glGetUniformBlockIndex(pr, "u_Globals"); glUniformBlockBinding(pr, bi, 0); glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
  // textures: block atlas (1024^2 RGBA8 with mips), light map 16x16, section time buffer
  GLuint tex[3]; glGenTextures(3, tex);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex[0]); glTexStorage2D(GL_TEXTURE_2D, 11, GL_RGBA8, 1024, 1024);
  uint32_t *px = malloc(1024 * 1024 * 4); for (int i = 0; i < 1024 * 1024; i++) px[i] = 0xff000000u | (i * 2654435761u >> 8);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1024, GL_RGBA, GL_UNSIGNED_BYTE, px); glGenerateMipmap(GL_TEXTURE_2D);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex[1]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
  GLuint tbo; glGenBuffers(1, &tbo); glBindBuffer(GL_TEXTURE_BUFFER, tbo); int *tinfo = calloc(256 * 64, 4); for (int i = 0; i < 256 * 64; i++) tinfo[i] = -1;
  glBufferData(GL_TEXTURE_BUFFER, 256 * 64 * 4, tinfo, GL_STATIC_DRAW);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_BUFFER, tex[2]); glTexBuffer(GL_TEXTURE_BUFFER, GL_R32I, tbo);
  glUniform1i(glGetUniformLocation(pr, "u_BlockTex"), 0); glUniform1i(glGetUniformLocation(pr, "u_LightTex"), 1); glUniform1i(glGetUniformLocation(pr, "u_SectionTimeInfo"), 2);
  glUniform1i(glGetUniformLocation(pr, "u_CurrentTime"), 1000); glUniform1ui(glGetUniformLocation(pr, "u_RegionID"), 0);
  // geometry: S sections laid out on a wide field in front of the camera, Q small quads each
  size_t verts = (size_t)S * Q * 4; uint8_t *vb = calloc(verts, 20);
  unsigned seed = 1;
  for (int s = 0; s < S; s++) for (int q = 0; q < Q; q++) {
    seed = seed * 1103515245 + 12345; unsigned x0 = seed >> 22 & 1023; seed = seed * 1103515245 + 12345; unsigned y0 = seed >> 22 & 1023; seed = seed * 1103515245 + 12345; unsigned z0 = seed >> 22 & 1023;
    for (int v = 0; v < 4; v++) {
      unsigned e = getenv("SB_BIG") ? atoi(getenv("SB_BIG")) : 10; unsigned x = x0 + (v & 1) * e, y = y0 + (v >> 1) * e, z = z0;  // 10/1024 of 32 blocks = ~0.3 block quads
      uint8_t *o = vb + ((size_t)(s * Q + q) * 4 + v) * 20;
      uint32_t hi = (x >> 10 & 0x3ff) | (y >> 10 & 0x3ff) << 10 | (z >> 10 & 0x3ff) << 20, lo = (x & 0x3ff) | (y & 0x3ff) << 10 | (z & 0x3ff) << 20;
      memcpy(o, &hi, 4); memcpy(o + 4, &lo, 4);
      o[8] = 200; o[9] = 180; o[10] = 160; o[11] = 255;
      uint16_t tc[2] = {(uint16_t)((q * 37) & 0x7fff), (uint16_t)((q * 91) & 0x7fff)}; memcpy(o + 12, tc, 4);
      o[16] = 240; o[17] = 240; o[18] = 0; o[19] = (uint8_t)(s & 255);
    }
  }
  int noidx = getenv("SB_NOINDEX") != NULL;
  if (noidx) {
    uint8_t *vb6 = malloc((size_t)S * Q * 6 * 20); static const int map[6] = {0, 1, 2, 2, 1, 3};
    for (size_t qq = 0; qq < (size_t)S * Q; qq++) for (int k = 0; k < 6; k++) memcpy(vb6 + (qq * 6 + k) * 20, vb + (qq * 4 + map[k]) * 20, 20);
    vb = vb6; verts = (size_t)S * Q * 6;
  }
  GLuint vao, vbo, ibo; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
  glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferStorage(GL_ARRAY_BUFFER, verts * 20, vb, 0);
  glVertexAttribIPointer(0, 2, GL_UNSIGNED_INT, 20, (void *)0); glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 20, (void *)8); glEnableVertexAttribArray(1);
  glVertexAttribIPointer(2, 2, GL_UNSIGNED_SHORT, 20, (void *)12); glEnableVertexAttribArray(2);
  glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, 20, (void *)16); glEnableVertexAttribArray(3);
  int i16 = getenv("SB_IDX16") != NULL;
  unsigned *idx = malloc(Q * 6 * 4); uint16_t *idx16 = malloc(Q * 6 * 2);
  for (int q = 0; q < Q; q++) { unsigned b = q * 4; unsigned t[6] = {b, b + 1, b + 2, b + 2, b + 1, b + 3}; memcpy(idx + q * 6, t, 24); for (int k = 0; k < 6; k++) idx16[q * 6 + k] = t[k]; }
  glGenBuffers(1, &ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  if (i16) glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, Q * 12, idx16, 0); else glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, Q * 24, idx, 0);
  GLsizei *cnt = malloc(S * sizeof *cnt); const void **off = malloc(S * sizeof *off); GLint *base = malloc(S * sizeof *base);
  for (int s = 0; s < S; s++) { cnt[s] = Q * 6; off[s] = 0; base[s] = s * Q * 4; }
  // sections spread over a 160 x 40 x 240-block field 4..244 blocks ahead: most quads end up a few pixels big
  glViewport(0, 0, W, H); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(getenv("SB_CULL") ? GL_CCW : GL_CW);
  GLint ro = glGetUniformLocation(pr, "u_RegionOffset");
  int game = getenv("SB_GAME") != NULL;
  GLuint cprog = 0, cbuf = 0, bprog = 0, bvao = 0, bfb[4], btex[4]; int bw[4] = {2048, 1024, 512, 256}, bh[4] = {1024, 512, 256, 128};
  if (game) {
    const char *cs = "#version 460 core\nlayout(local_size_x = 256) in;\nlayout(std430, binding = 3) buffer B { uint o[]; };\nuniform uint fr;\n"
                     "void main(){ uint i = gl_GlobalInvocationID.x; o[i] = o[i] * 1664525u + fr + i; }\n";
    cprog = glCreateProgram(); glAttachShader(cprog, sh(GL_COMPUTE_SHADER, cs)); glLinkProgram(cprog);
    glGenBuffers(1, &cbuf); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, cbuf); glBufferData(GL_SHADER_STORAGE_BUFFER, 256 * 1024, 0, GL_DYNAMIC_COPY);
    const char *bvs = "#version 460 core\nout vec2 uv; void main(){ vec2 p = vec2((gl_VertexID & 1) * 4 - 1, (gl_VertexID >> 1) * 4 - 1); uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0, 1); }\n";
    const char *bfs = "#version 460 core\nin vec2 uv; uniform sampler2D src; out vec4 c; void main(){ vec2 d = 1.0 / vec2(textureSize(src, 0));\n"
                      " c = 0.25 * (texture(src, uv + d) + texture(src, uv - d) + texture(src, uv + vec2(d.x, -d.y)) + texture(src, uv - vec2(d.x, -d.y))); }\n";
    bprog = glCreateProgram(); glAttachShader(bprog, sh(GL_VERTEX_SHADER, bvs)); glAttachShader(bprog, sh(GL_FRAGMENT_SHADER, bfs)); glLinkProgram(bprog);
    glGenVertexArrays(1, &bvao); glGenFramebuffers(4, bfb); glGenTextures(4, btex);
    for (int k = 0; k < 4; k++) { glBindTexture(GL_TEXTURE_2D, btex[k]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, bw[k], bh[k]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glBindFramebuffer(GL_FRAMEBUFFER, bfb[k]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, btex[k], 0); }
    glBindFramebuffer(GL_FRAMEBUFFER, fb); glUseProgram(pr); glBindVertexArray(vao);
  }
  double t0 = 0, cpu = 0, busy = 0, fb0 = 0; int pace_us = getenv("SB_PACE_US") ? atoi(getenv("SB_PACE_US")) : 0;
  for (int fr = 0; fr < frames + 10; fr++) {
    if (fr == 10) { glFinish(); t0 = now(); fb0 = t0; }
    glClearColor(0.5f, 0.7f, 1, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUniform3f(ro, -60.0f, -20.0f, getenv("SB_OFFSCREEN") ? 40.0f : -244.0f);
    double c0 = now();
    if (game) {
      glUseProgram(cprog); glUniform1ui(glGetUniformLocation(cprog, "fr"), fr);
      for (int k = 0; k < 3; k++) { glDispatchCompute(256 * 1024 / 4 / 256, 1, 1); glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT); }
      glUseProgram(pr); glBindVertexArray(vao); glBindFramebuffer(GL_FRAMEBUFFER, fb); glViewport(0, 0, W, H);
      glEnable(GL_DEPTH_TEST); glClearColor(0.5f, 0.7f, 1, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    if (noidx) { static GLint *first; static GLsizei *c6; if (!first) { first = malloc(S * sizeof *first); c6 = malloc(S * sizeof *c6); for (int k = 0; k < S; k++) { first[k] = k * Q * 6; c6[k] = Q * 6; } }
      glMultiDrawArrays(GL_TRIANGLES, first, c6, S); }
    else glMultiDrawElementsBaseVertex(GL_TRIANGLES, cnt, i16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, off, S, base);
    if (game) {
      glDisable(GL_DEPTH_TEST); glUseProgram(bprog); glBindVertexArray(bvao); glActiveTexture(GL_TEXTURE0);
      for (int k = 0; k < 4; k++) { glBindFramebuffer(GL_FRAMEBUFFER, bfb[k]); glViewport(0, 0, bw[k], bh[k]);
        glBindTexture(GL_TEXTURE_2D, k ? btex[k - 1] : ct); glDrawArrays(GL_TRIANGLES, 0, 3); }
      glBindFramebuffer(GL_FRAMEBUFFER, fb); glUseProgram(pr); glBindVertexArray(vao); glEnable(GL_DEPTH_TEST);
    }
    glFlush(); if (fr >= 10) cpu += now() - c0; if (!game && fr % 3 == 2 && !pace_us) glFinish();
    if (pace_us) { glFinish(); if (fr >= 10) { busy += now() - fb0; } usleep(pace_us); fb0 = now(); }
  }
  glFinish(); double t = now() - t0;
  { uint8_t *px = malloc((size_t)W * H * 4); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    uint64_t hsh = 1469598103934665603ull; size_t lit = 0;
    for (size_t k = 0; k < (size_t)W * H * 4; k++) { hsh = (hsh ^ px[k]) * 1099511628211ull; }
    for (size_t k = 0; k < (size_t)W * H; k++) lit += !(px[k*4] == 128 && px[k*4+1] == 179 && px[k*4+2] == 255);
    printf("image hash=%016llx non-sky pixels=%zu\n", (unsigned long long)hsh, lit); }
  printf("%s sections=%d quads=%d tris/frame=%.2fM verts/frame=%.2fM %dx%d: %.1f fps (%.3f ms/frame, %.0f Mtris/s) cpu=%.3f ms/frame\n",
         mode, S, Q, S * Q * 2 / 1e6, verts / 1e6, W, H, frames / t, t * 1000 / frames, S * Q * 2.0 * frames / t / 1e6, cpu * 1000 / frames);
  if (pace_us) printf("paced %d us: busy %.3f ms/frame\n", pace_us, busy * 1000 / frames);
  return 0;
}
