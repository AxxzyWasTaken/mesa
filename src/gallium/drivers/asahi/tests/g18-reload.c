/* SPDX-License-Identifier: MIT */
/* End-to-end reload/resolve check, written before the driver fix.
 * Failure model: a partial draw can discard the uploaded/cleared left half,
 * fetch the wrong prior tile, or resolve the wrong samples. Retain the exact
 * RGBA8 readback and require every pixel, not only a successful GL return.
 * A drm-shim run exercises compilation only and cannot pass this pixel check.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *where)
{
   GLenum e = glGetError();
   if (e != GL_NO_ERROR) {
      fprintf(stderr, "RELOAD_GL_ERROR at=%s code=%#x\n", where, e);
      exit(2);
   }
}

int main(int argc, char **argv)
{
   const int width = 64, height = 64;
   if (argc != 3) return 2;
   const int samples = atoi(argv[1]);
   const GLenum format = getenv("RELOAD_PACKED_FLOAT") ? GL_R11F_G11F_B10F :
                         getenv("RELOAD_RGBA") ? GL_RGBA8 : GL_RGB8;
   if (samples != 0 && samples != 2 && samples != 4) return 2;
   setvbuf(stdout, NULL, _IOLBF, 0);
   EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                          EGL_DEFAULT_DISPLAY, NULL);
   EGLint major, minor, n = 0;
   if (!eglInitialize(dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) return 3;
   const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                           EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
   EGLConfig cfg;
   if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n) || !n) return 3;
   const EGLint ctxattrs[] = {EGL_CONTEXT_MAJOR_VERSION, 2,
                              EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattrs);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return 3;
   GLint max_samples = -1;
   glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
   printf("RELOAD renderer=%s version=%s max_samples=%d requested_samples=%d\n",
          glGetString(GL_RENDERER), glGetString(GL_VERSION), max_samples, samples);
   check("context");
   if (samples > max_samples) {
      fprintf(stderr, "RELOAD_UNSUPPORTED samples=%d maximum=%d\n", samples, max_samples);
      return 4;
   }
   unsigned char pixels[64 * 64 * 4];
   for (unsigned i = 0; i < sizeof(pixels); i += 4) {
      pixels[i] = pixels[i + 3] = 255;
      pixels[i + 1] = pixels[i + 2] = 0;
   }
   GLuint texture, fbo, rb = 0, resolved = 0;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   /* CPU-uploaded red pixels require a background reload before the draw. */
   glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, pixels);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   if (samples) {
      glGenRenderbuffers(1, &rb);
      glBindRenderbuffer(GL_RENDERBUFFER, rb);
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, width, height);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   } else {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
   }
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 5;
   check("framebuffer");
   if (samples) {
      glClearColor(1, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glFinish();
      check("clear retirement");
   }
   glViewport(0, 0, width, height);
   glDisable(GL_DITHER);
   glMatrixMode(GL_PROJECTION);
   glLoadIdentity();
   glMatrixMode(GL_MODELVIEW);
   glLoadIdentity();
   glColor4f(0, 1, 0, 1);
   glBegin(GL_QUADS);
   glVertex2f(0, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(0, 1);
   glEnd();
   glFinish();
   check("partial draw retirement");
   if (samples) {
      glGenFramebuffers(1, &resolved);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolved);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 5;
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
      glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_FRAMEBUFFER, resolved);
   }
   memset(pixels, 0x5a, sizeof(pixels));
   glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   check("readback");
   FILE *out = fopen(argv[2], "wb");
   if (!out || fwrite(pixels, 1, sizeof(pixels), out) != sizeof(pixels)) return 6;
   if (fclose(out)) return 6;
   unsigned wrong = 0;
   for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
      const unsigned char *p = &pixels[(y * width + x) * 4];
      const unsigned char expected[4] = {x < width / 2 ? 255 : 0,
                                        x < width / 2 ? 0 : 255, 0, 255};
      wrong += memcmp(p, expected, 4) != 0;
   }
   printf("RELOAD_RESULT samples=%d pixels=%d wrong=%u artifact=%s\n",
          samples, width * height, wrong, argv[2]);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   eglTerminate(dpy);
   return wrong ? 1 : 0;
}
