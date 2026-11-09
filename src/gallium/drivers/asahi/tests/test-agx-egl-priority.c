/* SPDX-License-Identifier: MIT */
/* No GPU submission: exercise the GLVND -> EGL -> DRI -> Gallium query path. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%u: %s\n", __LINE__, #x); abort(); } } while (0)
#define LOAD(name) PFN##name##PROC name = (void *)dlsym(egl, #name)
int main(void)
{
   void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
   CHECK(egl);
   PFNEGLGETPLATFORMDISPLAYPROC get_display = (void *)dlsym(egl, "eglGetPlatformDisplay");
   PFNEGLINITIALIZEPROC initialize = (void *)dlsym(egl, "eglInitialize");
   PFNEGLBINDAPIPROC bind_api = (void *)dlsym(egl, "eglBindAPI");
   PFNEGLCREATECONTEXTPROC create = (void *)dlsym(egl, "eglCreateContext");
   PFNEGLQUERYCONTEXTPROC query = (void *)dlsym(egl, "eglQueryContext");
   PFNEGLDESTROYCONTEXTPROC destroy = (void *)dlsym(egl, "eglDestroyContext");
   PFNEGLTERMINATEPROC terminate = (void *)dlsym(egl, "eglTerminate");
   CHECK(get_display && initialize && bind_api && create && query && destroy && terminate);
   EGLDisplay d = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint major, minor;
   CHECK(initialize(d, &major, &minor));
   CHECK(bind_api(EGL_OPENGL_ES_API));
   EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_CONTEXT_PRIORITY_LEVEL_IMG,
                     EGL_CONTEXT_PRIORITY_HIGH_IMG, EGL_NONE};
   EGLContext c = create(d, NULL, EGL_NO_CONTEXT, attrs);
   CHECK(c != EGL_NO_CONTEXT);
   EGLint actual;
   CHECK(query(d, c, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &actual));
   const char *policy = getenv("ASAHI_SHIM_HIGH_PRIORITY");
   CHECK(actual == (policy ? EGL_CONTEXT_PRIORITY_MEDIUM_IMG : EGL_CONTEXT_PRIORITY_HIGH_IMG));
   printf("PASS EGL queried effective priority %#x (policy=%s)\n", actual, policy ? policy : "allow");
   CHECK(destroy(d, c));
   CHECK(terminate(d));
   dlclose(egl);
   return 0;
}
