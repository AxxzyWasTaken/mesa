/* SPDX-License-Identifier: MIT */
/* EGL/GLES3 numerical check for byte-offset indexed triangle strips. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
check(int ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "%s: EGL=0x%x GL=0x%x\n", what, eglGetError(), glGetError());
      exit(1);
   }
}

static GLuint
shader(GLenum stage, const char *source)
{
   GLuint s = glCreateShader(stage);
   glShaderSource(s, 1, &source, NULL);
   glCompileShader(s);
   GLint ok;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[4096];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      fprintf(stderr, "%s\n", log);
   }
   check(ok, "compile");
   return s;
}

int
main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   check(get_display != NULL, "EGL platform display extension");
   EGLDisplay d = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   check(d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL), "initialize");
   check(eglBindAPI(EGL_OPENGL_ES_API), "bind API");
   const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
   EGLConfig config;
   EGLint count;
   check(eglChooseConfig(d, attrs, &config, 1, &count) && count == 1, "config");
   const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
   EGLContext ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
   check(ctx != EGL_NO_CONTEXT, "context");
   const EGLint pa[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
   EGLSurface surface = eglCreatePbufferSurface(d, config, pa);
   check(surface != EGL_NO_SURFACE && eglMakeCurrent(d, surface, surface, ctx), "make current");
   printf("vendor=%s\nrenderer=%s\nversion=%s\n", glGetString(GL_VENDOR),
          glGetString(GL_RENDERER), glGetString(GL_VERSION));
   const char *vs = "#version 300 es\n"
      "layout(location=0) in vec2 position;\n"
      "void main(){ gl_Position=vec4(position,0,1); }\n";
   const char *fs = "#version 300 es\n"
      "precision highp float; out vec4 colour;\n"
      "void main(){ colour=vec4(0,1,0,1); }\n";
   GLuint program = glCreateProgram();
   GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
   glAttachShader(program, v);
   glAttachShader(program, f);
   glLinkProgram(program);
   GLint ok;
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   check(ok, "link");
   glUseProgram(program);
   GLuint vao, buffers[3];
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glGenBuffers(3, buffers);
   const float vertices[] = {-1,-1, 1,-1, -1,1, 1,1};
   glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
   glEnableVertexAttribArray(0);
   GLuint texture, framebuffer;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
   glGenFramebuffers(1, &framebuffer);
   glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
   check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
   glViewport(0, 0, 64, 64);
   glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
   unsigned failures = 0, tests = 0;
   for (unsigned size = 1; size <= 4; size *= 2) {
      for (unsigned offset = 0; offset < 4; offset += size) {
         for (unsigned copy = 0; copy < 2; ++copy) {
            uint8_t data[32];
            memset(data, 0xff, sizeof(data));
            for (unsigned i = 0; i < 4; ++i) {
               if (size == 1) data[offset + i] = i;
               if (size == 2) { uint16_t x = i; memcpy(data + offset + i * size, &x, size); }
               if (size == 4) { uint32_t x = i; memcpy(data + offset + i * size, &x, size); }
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(data), copy ? NULL : data, GL_DYNAMIC_DRAW);
            if (copy) {
               glBindBuffer(GL_COPY_READ_BUFFER, buffers[2]);
               glBufferData(GL_COPY_READ_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
               glBindBuffer(GL_COPY_WRITE_BUFFER, buffers[1]);
               glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, sizeof(data));
               /* No finish here: the draw must synchronize the GPU writer. */
            }
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            GLenum type = size == 1 ? GL_UNSIGNED_BYTE : size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
            glDrawElements(GL_TRIANGLE_STRIP, 4, type, (const void *)(uintptr_t)offset);
            uint8_t pixels[64 * 64 * 4];
            glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            check(glGetError() == GL_NO_ERROR, "draw/readback");
            unsigned covered = 0;
            for (unsigned i = 0; i < 64 * 64; ++i)
               covered += pixels[i * 4] == 0 && pixels[i * 4 + 1] == 255 &&
                          pixels[i * 4 + 2] == 0 && pixels[i * 4 + 3] == 255;
            printf("u%u offset=%u writer=%s covered=%u/4096\n",
                   size * 8, offset, copy ? "GPU-copy" : "CPU", covered);
            failures += covered != 4096;
            ++tests;
         }
      }
   }
   printf("tests=%u failures=%u\n", tests, failures);
   glDeleteFramebuffers(1, &framebuffer);
   glDeleteTextures(1, &texture);
   glDeleteBuffers(3, buffers);
   glDeleteVertexArrays(1, &vao);
   glDeleteProgram(program);
   glDeleteShader(v);
   glDeleteShader(f);
   eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(d, ctx);
   eglDestroySurface(d, surface);
   eglTerminate(d);
   return failures != 0;
}
