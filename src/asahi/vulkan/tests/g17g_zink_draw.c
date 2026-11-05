/* SPDX-License-Identifier: MIT
 * Exercise a real OpenGL draw through Zink/Honeykrisp without a display server.
 * Context creation alone is not a rendering result. --submit-only explicitly
 * skips pixel validation so transfer failures can be diagnosed separately.
 */
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static volatile sig_atomic_t readback_requested;

static void request_readback(int signal_number)
{
    (void)signal_number;
    readback_requested = 1;
}

static void fail(const char *message)
{
    fprintf(stderr, "M5_ZINK_FAIL %s EGL=%#x GL=%#x\n", message,
            eglGetError(), glGetError());
    exit(1);
}

static GLuint shader(GLenum stage, const char *source)
{
    GLuint object = glCreateShader(stage);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint success = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[4096];
        glGetShaderInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
        fail("shader compilation");
    }
    return object;
}

int main(int argc, char **argv)
{
    const int submit_only = argc == 2 && !strcmp(argv[1], "--submit-only");
    const int blit_only = argc == 2 && !strcmp(argv[1], "--blit-only");
    if (argc > 1 && !submit_only && !blit_only) {
        fprintf(stderr, "Usage: %s [--submit-only|--blit-only]\n", argv[0]);
        return 2;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    const int defer_readback = !submit_only && getenv("AGX_GL_DEFER_READBACK");
    if (defer_readback && signal(SIGUSR1, request_readback) == SIG_ERR)
        fail("readback signal handler");
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                               EGL_DEFAULT_DISPLAY, NULL);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL))
        fail("EGL initialization");
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE,
    };
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
        fail("EGL config");
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    if (!eglBindAPI(EGL_OPENGL_API)) fail("OpenGL API");
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                          context_attributes);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context))
        fail("OpenGL context");
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    fprintf(stderr, "M5_ZINK_RENDERER %s\n", renderer ? renderer : "missing");
    if (!renderer || !strstr(renderer, "zink") || !strstr(renderer, "Apple M5"))
        fail("expected Zink on Apple M5");
    GLuint vertex = shader(GL_VERTEX_SHADER,
        "#version 330 core\nlayout(location=0) in vec2 position;\n"
        "void main(){gl_Position=vec4(position,0.0,1.0);}\n");
    GLuint fragment = shader(GL_FRAGMENT_SHADER,
        "#version 330 core\nlayout(location=0) out vec4 color;\n"
        "void main(){color=vec4(1.0,71.0/255.0,13.0/255.0,1.0);}\n");
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
        fail("program link");
    }
    static const GLfloat vertices[] = {-1, -1, 3, -1, -1, 3};
    GLuint array, buffer;
    glGenVertexArrays(1, &array);
    glBindVertexArray(array);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glUseProgram(program);
    glViewport(0, 0, 256, 256);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    fprintf(stderr, "M5_ZINK_DRAW_RECORDED\n");
    glFinish();
    if (glGetError() != GL_NO_ERROR) fail("draw completion");
    fprintf(stderr, "M5_ZINK_GL_FINISH_RETURNED pixels_untested=1\n");
    if (defer_readback) {
        fprintf(stderr, "M5_ZINK_HOLD pid=%ld waiting_readback=1 pixels_tested=0\n",
                (long)getpid());
        while (!readback_requested) sleep(1);
        fprintf(stderr, "M5_ZINK_READBACK_REQUESTED\n");
    }
    unsigned exact = 0;
    if (blit_only) {
        GLuint image, framebuffer;
        glGenTextures(1, &image);
        glBindTexture(GL_TEXTURE_2D, image);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, image, 0);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fail("blit framebuffer");
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, 256, 256, 0, 0, 256, 256,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        fprintf(stderr, "M5_ZINK_BLIT_RECORDED pixels_untested=1\n");
        glFinish();
        if (glGetError() != GL_NO_ERROR) fail("blit completion");
        fprintf(stderr, "M5_ZINK_BLIT_GL_FINISH_RETURNED pixels_untested=1\n");
    } else if (!submit_only) {
        unsigned char *pixels = malloc(256 * 256 * 4);
        if (!pixels) fail("readback allocation");
        memset(pixels, 0xa5, 256 * 256 * 4);
        glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if (glGetError() != GL_NO_ERROR) fail("pixel readback");
        for (unsigned i = 0; i < 256 * 256; ++i)
            exact += !memcmp(pixels + 4 * i, "\xff\x47\x0d\xff", 4);
        fprintf(stderr, "M5_ZINK_PIXELS exact=%u/65536 first=%02x%02x%02x%02x\n",
                exact, pixels[0], pixels[1], pixels[2], pixels[3]);
        free(pixels);
    }
    if (getenv("AGX_GL_HOLD")) {
        fprintf(stderr, "M5_ZINK_HOLD pid=%ld pixels_exact=%u/65536 pixels_tested=%d\n",
                (long)getpid(), exact, !submit_only && !blit_only);
        for (;;) sleep(60);
    }
    if (!submit_only && !blit_only && exact != 65536) return 1;
    fprintf(stderr, "%s\n", blit_only ? "M5_ZINK_BLIT_ONLY_DONE pixels_untested=1"
                           : submit_only ? "M5_ZINK_SUBMIT_ONLY_DONE pixels_untested=1"
                                         : "M5_ZINK_PIXEL_PASS");
    return 0;
}
