/* SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "agx_state.h"
#include "agx_public.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%u: %s\n", __LINE__, #x); abort(); } } while (0)

static unsigned creates, destroys;
static int create_error, destroy_error;
static int
probe_ioctl(struct agx_device *dev, unsigned request, void *arg)
{
   if (_IOC_NR(request) == _IOC_NR(DRM_IOCTL_ASAHI_QUEUE_CREATE)) {
      struct drm_asahi_queue_create *q = arg;
      CHECK(q->priority == DRM_ASAHI_PRIORITY_HIGH);
      CHECK(q->vm_id == dev->vm_id && q->usc_exec_base == dev->shader_base);
      CHECK(_IOC_SIZE(request) == offsetof(struct drm_asahi_queue_create, render_scs_layout));
      creates++;
      q->queue_id = 42;
      return create_error;
   }
   CHECK(request == DRM_IOCTL_ASAHI_QUEUE_DESTROY);
   CHECK(((struct drm_asahi_queue_destroy *)arg)->queue_id == 42);
   destroys++;
   return destroy_error;
}
int main(void)
{
   struct agx_device isolated = {0};
   isolated.debug = AGX_DBG_1QUEUE;
   isolated.queue_id = 7; /* Probe must not reuse or destroy this shared queue. */
   isolated.ops.simple_ioctl = probe_ioctl;
   CHECK(agx_probe_high_priority(&isolated));
   CHECK(creates == 1 && destroys == 1 && isolated.queue_id == 7);
   create_error = -EACCES;
   CHECK(!agx_probe_high_priority(&isolated));
   CHECK(creates == 2 && destroys == 1);
   create_error = 0; destroy_error = -EIO;
   CHECK(!agx_probe_high_priority(&isolated));
   CHECK(creates == 3 && destroys == 2);

   int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   CHECK(fd >= 0);
   static const driOptionDescription options[] = {
#include "driinfo_asahi.h"
   };
   driOptionCache info, cache;
   driParseOptionInfo(&info, options, ARRAY_SIZE(options));
   struct pipe_screen_config config = {.options = &cache, .options_info = &info};
   struct pipe_screen *screen = agx_screen_create(fd, NULL, &config);
   CHECK(screen);
   const char *policy = getenv("ASAHI_SHIM_HIGH_PRIORITY");
   unsigned mask = PIPE_CONTEXT_PRIORITY_LOW | PIPE_CONTEXT_PRIORITY_MEDIUM;
   if (!policy || strcmp(policy, "deny")) mask |= PIPE_CONTEXT_PRIORITY_HIGH;
   CHECK(screen->caps.context_priority_mask == mask);
   unsigned flags[] = {0, PIPE_CONTEXT_LOW_PRIORITY, PIPE_CONTEXT_HIGH_PRIORITY};
   for (unsigned i = 0; i < ARRAY_SIZE(flags); i++) {
      struct pipe_context *ctx = screen->context_create(screen, NULL, flags[i]);
      CHECK(ctx);
      unsigned expected = i == 1 ? PIPE_CONTEXT_PRIORITY_LOW : PIPE_CONTEXT_PRIORITY_MEDIUM;
      if (i == 2 && !policy) expected = PIPE_CONTEXT_PRIORITY_HIGH;
      CHECK(ctx->context_priority == expected);
      ctx->destroy(ctx);
   }
   screen->destroy(screen);
   close(fd);
   puts("PASS probe cleanup, shared-cache bypass, mask, request flags and effective fallback");
   return 0;
}
