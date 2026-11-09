/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include "drm-uapi/asahi_drm.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%u: %s\n", __LINE__, #x); abort(); } } while (0)

int main(void)
{
   int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   CHECK(fd >= 0);
   struct drm_asahi_params_global params = {0};
   struct drm_asahi_get_params get = {
      .pointer = (uintptr_t)&params, .size = sizeof(params),
   };
   CHECK(!drmIoctl(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &get));
   struct drm_asahi_vm_create vm = {0};
   CHECK(!drmIoctl(fd, DRM_IOCTL_ASAHI_VM_CREATE, &vm));
   struct drm_asahi_gem_create bo = {.size = 0x4000};
   CHECK(!drmIoctl(fd, DRM_IOCTL_ASAHI_GEM_CREATE, &bo));
   struct drm_asahi_gem_bind_op op = {
      .handle = bo.handle, .range = 0x4000,
   };
   struct drm_asahi_vm_bind bind = {
      .vm_id = vm.vm_id, .num_binds = 1,
      .stride = sizeof(op), .userptr = (uintptr_t)&op,
   };
   const uint64_t addresses[] = {params.vm_start, UINT64_C(0x30000000000),
                                 params.vm_end - 0x4000, params.vm_end};
   for (unsigned i = 0; i < 4; ++i) {
      op.addr = addresses[i];
      int ret = drmIoctl(fd, DRM_IOCTL_ASAHI_VM_BIND, &bind);
      CHECK((ret == 0) == (op.addr + op.range <= params.vm_end));
   }
   close(fd);
   puts("PASS reported VM limits match bind admission");
   return 0;
}
