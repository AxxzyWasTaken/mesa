/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 * Exercise the production sparse binder with a recording VM_BIND backend.
 * The test checks mapping/metadata transactions, not GPU cache coherency.
 */
#undef NDEBUG
#include <assert.h>
#include <string.h>
#include "../hk_queue.c"

static struct drm_asahi_gem_bind_op recorded[64];
static unsigned recorded_count, bind_calls;
static bool fail_bind;

static int
record_bind(struct agx_device *dev, struct drm_asahi_gem_bind_op *ops,
            unsigned count)
{
   assert(count <= ARRAY_SIZE(recorded));
   memcpy(recorded, ops, count * sizeof(*ops));
   recorded_count = count;
   ++bind_calls;
   if (fail_bind) {
      errno = ENOMEM;
      return -1;
   }
   return 0;
}

__typeof__(__vk_errorf) __wrap___vk_errorf;
VkResult
__wrap___vk_errorf(const void *object, VkResult result, const char *file,
                   int line, const char *format, ...)
{
   return result;
}

static void
check_pair(unsigned i, uint64_t addr, uint64_t offset, uint64_t size,
           unsigned handle, const struct hk_device *dev)
{
   assert(i + 1 < recorded_count);
   const struct drm_asahi_gem_bind_op *rw = &recorded[i],
                                      *ro = &recorded[i + 1];
   assert(rw->addr == addr && rw->offset == offset && rw->range == size);
   assert(rw->handle == handle);
   assert(rw->flags == (DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE));
   assert(ro->addr == agx_rw_addr_to_ro(&dev->dev, addr));
   assert(ro->offset == offset && ro->range == size && ro->handle == handle);
   assert(ro->flags == DRM_ASAHI_BIND_READ);
}

static void
buffer_aliases(struct hk_device *dev, struct hk_device_memory *memory)
{
   uint8_t metadata[2][AIL_PAGESIZE] = {{0}};
   struct agx_bo maps[2] = {{0}};
   struct agx_va vas[2] = {{.addr = 0x100000, .size_B = 4 * AIL_PAGESIZE},
                           {.addr = 0x200000, .size_B = 4 * AIL_PAGESIZE}};
   struct hk_buffer buffers[2] = {{0}};
   uint32_t *bits[2];
   for (unsigned i = 0; i < 2; ++i) {
      maps[i] = (struct agx_bo){.dev = &dev->dev,
                                ._map = metadata[i],
                                .size = sizeof(metadata[i])};
      buffers[i].vk.base.type = VK_OBJECT_TYPE_BUFFER;
      buffers[i].va = &vas[i];
      buffers[i].sparse_map = &maps[i];
      buffers[i].sparse_map_entries = 4;
      bits[i] =
         (uint32_t *)(metadata[i] + sizeof(struct hk_sparse_buffer_map_header));
   }
   VkSparseMemoryBind pages[2] = {
      {.resourceOffset = 0,
       .size = AIL_PAGESIZE,
       .memory = hk_device_memory_to_handle(memory),
       .memoryOffset = 2 * AIL_PAGESIZE},
      {.resourceOffset = AIL_PAGESIZE,
       .size = AIL_PAGESIZE,
       .memory = hk_device_memory_to_handle(memory),
       .memoryOffset = 2 * AIL_PAGESIZE},
   };
   VkSparseBufferMemoryBindInfo info = {
      .buffer = hk_buffer_to_handle(&buffers[0]),
      .bindCount = 2,
      .pBinds = pages,
   };
   assert(hk_sparse_buffer_bind_memory(dev, &info) == VK_SUCCESS);
   assert(recorded_count == 4); /* Equal BO offsets must not be merged. */
   check_pair(0, vas[0].addr, pages[0].memoryOffset, AIL_PAGESIZE, 1, dev);
   check_pair(2, vas[0].addr + AIL_PAGESIZE, pages[1].memoryOffset,
              AIL_PAGESIZE, 1, dev);
   assert(*bits[0] == 3 && *bits[1] == 0);

   /* The same backing also appears in a second resource. */
   info.buffer = hk_buffer_to_handle(&buffers[1]);
   info.bindCount = 1;
   assert(hk_sparse_buffer_bind_memory(dev, &info) == VK_SUCCESS);
   check_pair(0, vas[1].addr, pages[0].memoryOffset, AIL_PAGESIZE, 1, dev);
   assert(*bits[0] == 3 && *bits[1] == 1);

   /* Unbinding one alias changes only its own residency bits and shadows. */
   pages[0].memory = VK_NULL_HANDLE;
   assert(hk_sparse_buffer_bind_memory(dev, &info) == VK_SUCCESS);
   assert(recorded_count == 2);
   assert(recorded[0].handle == dev->dev.scratch_bo->uapi_handle);
   assert(recorded[1].handle == dev->dev.zero_bo->uapi_handle);
   assert(recorded[0].flags & DRM_ASAHI_BIND_SINGLE_PAGE);
   assert(recorded[1].flags ==
          (DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_SINGLE_PAGE));
   assert(*bits[0] == 3 && *bits[1] == 0);

   fail_bind = true;
   pages[0].memory = hk_device_memory_to_handle(memory);
   assert(hk_sparse_buffer_bind_memory(dev, &info) ==
          VK_ERROR_OUT_OF_DEVICE_MEMORY);
   assert(*bits[0] == 3 && *bits[1] == 0);
   fail_bind = false;
}

static void
image_aliases(struct hk_device *dev, struct hk_device_memory *memory)
{
   struct hk_image image = {0};
   image.vk.base.type = VK_OBJECT_TYPE_IMAGE;
   image.vk.format = VK_FORMAT_R32_UINT;
   image.plane_count = 1;
   struct ail_layout *layout = &image.planes[0].layout;
   *layout = (struct ail_layout){
      .width_px = 512,
      .height_px = 256,
      .depth_px = 1,
      .levels = 1,
      .sample_count_sa = 1,
      .tiling = AIL_TILING_GPU,
      .format = PIPE_FORMAT_R32_UINT,
   };
   ail_make_miptree(layout);
   struct agx_va va = {.addr = 0x300000, .size_B = layout->size_B};
   image.planes[0].va = &va;
   void *metadata = calloc(1, layout->sparse_table_size_B);
   assert(metadata);
   struct agx_bo map = {.dev = &dev->dev,
                        ._map = metadata,
                        .size = layout->sparse_table_size_B};
   image.planes[0].sparse_map = &map;
   struct ail_tile tile = layout->tilesize_el[0];
   assert(tile.width_el * tile.height_el * 4 == AIL_PAGESIZE);
   VkSparseImageMemoryBind pages[2] = {
      {.subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
       .extent = {2 * tile.width_el, 2 * tile.height_el, 1},
       .memory = hk_device_memory_to_handle(memory)},
      {.subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
       .offset = {2 * tile.width_el, 0, 0},
       .extent = {2 * tile.width_el, 2 * tile.height_el, 1},
       .memory = hk_device_memory_to_handle(memory)},
   };
   VkSparseImageMemoryBindInfo info = {
      .image = hk_image_to_handle(&image),
      .bindCount = 2,
      .pBinds = pages,
   };
   assert(hk_sparse_image_bind_memory(dev, &info) == VK_SUCCESS);
   /* A standard tile contains four hardware pages. The two standard tiles
    * alias those same four backing pages at different resource VAs. */
   unsigned seen[4] = {0};
   for (unsigned i = 0; i < recorded_count; i += 2) {
      const struct drm_asahi_gem_bind_op *op = &recorded[i];
      check_pair(i, op->addr, op->offset, op->range, 1, dev);
      for (uint64_t j = 0; j < op->range; j += AIL_PAGESIZE) {
         unsigned page = (op->offset + j) / AIL_PAGESIZE;
         assert(page < 4);
         ++seen[page];
         unsigned resource_page = (op->addr + j - va.addr) / AIL_PAGESIZE;
         unsigned idx = ail_page_to_sparse_index_el(layout, 0, resource_page);
         agx_unpack(stderr, (uint32_t *)metadata + idx, SPARSE_BLOCK, block);
         assert(block.enabled && block.address == op->addr + j);
      }
   }
   for (unsigned i = 0; i < 4; ++i)
      assert(seen[i] == 2);
   void *before = malloc(map.size);
   assert(before);
   memcpy(before, metadata, map.size);
   fail_bind = true;
   pages[0].memory = VK_NULL_HANDLE;
   assert(hk_sparse_image_bind_memory(dev, &info) ==
          VK_ERROR_OUT_OF_DEVICE_MEMORY);
   assert(memcmp(before, metadata, map.size) == 0);
   fail_bind = false;
   free(before);
   free(metadata);
}

int
main(void)
{
   struct hk_device *dev = calloc(1, sizeof(*dev));
   assert(dev);
   struct agx_bo scratch = {.uapi_handle = 2}, zero = {.uapi_handle = 3};
   struct agx_bo bo = {.uapi_handle = 1, .size = 8 * AIL_PAGESIZE};
   struct hk_device_memory memory = {.bo = &bo};
   memory.vk.base.type = VK_OBJECT_TYPE_DEVICE_MEMORY;
   dev->vk.base.type = VK_OBJECT_TYPE_DEVICE;
   dev->vk.base.device = &dev->vk;
   dev->dev.chip = AGX_CHIP_G17P;
   dev->dev.sparse_ro_offset = UINT64_C(1) << 36;
   dev->dev.scratch_bo = &scratch;
   dev->dev.zero_bo = &zero;
   dev->dev.ops.bo_bind = record_bind;
   buffer_aliases(dev, &memory);
   image_aliases(dev, &memory);
   printf("sparse alias mapping transactions passed (%u VM_BIND batches)\n",
          bind_calls);
   free(dev);
   return 0;
}
