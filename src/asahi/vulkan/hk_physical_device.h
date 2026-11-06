/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "asahi/lib/agx_device.h"
#include <sys/types.h>
#include "hk_private.h"
#include "vk_physical_device.h"
#include "vk_sync.h"
#include "wsi_common.h"

/*
 * User-visible driver identity, reported in VkPhysicalDeviceDriverProperties
 * by every physical device this driver opens.  It does not depend on the chip,
 * so a newly enabled GPU reports it with no further change.  The driverID is
 * VK_DRIVER_ID_AURORA_BOREALIS, registered by Khronos for Aurora Silicon.
 */
#define HK_DRIVER_NAME "Borealis"
#define HK_DRIVER_INFO_SUFFIX ", Borealis, derived from Honeykrisp"

struct hk_instance;
struct hk_physical_device;

struct hk_queue_family {
   VkQueueFlags queue_flags;
   uint32_t queue_count;
};

/* VK_EXT_memory_budget: "A heap's usage is an estimate of how much memory
 * the process is currently using in that heap." Every VkInstance enumerates
 * its own hk_physical_device, so the counters live in one record per render
 * node, shared by every physical device of that node in the process.
 */
struct hk_heap_usage {
   struct hk_heap_usage *next;
   dev_t render_dev;
   unsigned refcount;
   uint64_t used[3];
   p_atomic_uint64_t cached[3];
};

struct hk_memory_heap {
   uint64_t size;
   /* Point into the physical device's hk_heap_usage. */
   uint64_t *used;
   p_atomic_uint64_t *cached;
   VkMemoryHeapFlags flags;
};

struct hk_physical_device {
   struct vk_physical_device vk;
   dev_t render_dev;
   int master_fd;

   /* Only used for VK_EXT_memory_budget */
   struct agx_device dev;

   struct wsi_device wsi_device;

   uint8_t device_uuid[VK_UUID_SIZE];

   // TODO: add mapable VRAM heap if possible
   struct hk_heap_usage *heap_usage;
   struct hk_memory_heap mem_heaps[3];
   VkMemoryType mem_types[3];
   uint8_t mem_heap_count;
   uint8_t mem_type_count;
   uint64_t sysmem;

   struct hk_queue_family queue_families[3];
   uint8_t queue_family_count;

   struct vk_sync_type syncobj_sync_type;
   const struct vk_sync_type *sync_types[2];

   simple_mtx_t debug_compile_lock;

   /* Whether any [SU]SCALED format has sampled-image (or input-attachment)
    * features. Only then can a valid application descriptor carry a nonzero
    * hk_sampled_image_descriptor_sw::scaled_conversion kind. */
   bool scaled_sampling;
   bool scaled_storage;

   /* Set once a device of this GPU creates a sampler whose border colour G17P
    * shaders must emulate (hk_CreateSampler). Shaders compiled before then
    * leave the emulation out and are replaced by emulating twins when bound
    * (hk_border_twin); the shader cache key carries it. Per physical device
    * because the cache key is computed from it (hk_hash_graphics_state). */
   uint32_t border_emulation;
};

VK_DEFINE_HANDLE_CASTS(hk_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

static inline struct hk_instance *
hk_physical_device_instance(struct hk_physical_device *pdev)
{
   return (struct hk_instance *)pdev->vk.instance;
}

VkResult hk_create_drm_physical_device(struct vk_instance *vk_instance,
                                       struct _drmDevice *drm_device,
                                       struct vk_physical_device **pdev_out);

void hk_physical_device_destroy(struct vk_physical_device *vk_device);

void hk_border_emulation_recall(struct hk_physical_device *pdev);
void hk_border_emulation_remember(struct hk_physical_device *pdev);

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) ||                                    \
   defined(VK_USE_PLATFORM_XCB_KHR) || defined(VK_USE_PLATFORM_XLIB_KHR) ||    \
   defined(VK_USE_PLATFORM_DISPLAY_KHR)
#define HK_USE_WSI_PLATFORM
#endif
