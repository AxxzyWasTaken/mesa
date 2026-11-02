/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Thousands of one-copy submits into a compressed R8 image, every command
 * buffer recorded cold and kept until the end of the burst: the shape of a
 * game streaming terrain tiles while no frame is presented, so that nothing is
 * recycled. Each shape overflows a different uploader span or slab, and then
 * releases everything at once. The image is read back and compared, and the
 * cost per submit is printed.
 *
 * usage: aurora-tilesubmit [submits]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define W    34u
#define STEP 32u
#define SIZE 130u
#define GRID ((SIZE - W) / STEP + 1)

#define CHECK(x)                                                               \
   do {                                                                        \
      VkResult r_ = (x);                                                       \
      if (r_) {                                                                \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_);      \
         exit(1);                                                              \
      }                                                                        \
   } while (0)

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties mem_props;

static double
now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void
bind_memory(VkMemoryRequirements req, VkMemoryPropertyFlags want,
            VkDeviceMemory *mem)
{
   uint32_t type = 0;
   while (!(req.memoryTypeBits & (1u << type)) ||
          (mem_props.memoryTypes[type].propertyFlags & want) != want)
      type++;

   VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = type};
   CHECK(vkAllocateMemory(dev, &info, NULL, mem));
}

static VkBuffer
make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *mem,
            uint8_t **map)
{
   VkBuffer buffer;
   VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size, .usage = usage};
   CHECK(vkCreateBuffer(dev, &info, NULL, &buffer));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, buffer, &req);
   bind_memory(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mem);
   CHECK(vkBindBufferMemory(dev, buffer, *mem, 0));
   CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)map));
   return buffer;
}

static void
barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier b = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &b);
}

static void
submit(VkCommandBuffer cmd)
{
   VkSubmitInfo info = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cmd};
   CHECK(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
}

/* One burst: `copies` copy calls per command buffer, from one pool or a pool
 * per command buffer, and only then release. */
static int
burst(uint32_t submits, uint32_t copies, int one_pool)
{
   const uint32_t tile_B = W * W, tiles = 512;
   uint8_t *src_map, *dst_map;
   VkDeviceMemory src_mem, readback_mem;
   VkBuffer src = make_buffer(tile_B * tiles, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              &src_mem, &src_map);
   VkBuffer readback = make_buffer(SIZE * SIZE, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   &readback_mem, &dst_map);
   for (uint32_t i = 0; i < tile_B * tiles; ++i)
      src_map[i] = (uint8_t)(i * 2654435761u >> 13);

   VkImageCreateInfo image_info = {
      VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8_UNORM,
      .extent = {SIZE, SIZE, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
   };
   VkImage image;
   VkDeviceMemory image_mem;
   VkMemoryRequirements req;
   CHECK(vkCreateImage(dev, &image_info, NULL, &image));
   vkGetImageMemoryRequirements(dev, image, &req);
   bind_memory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image_mem);
   CHECK(vkBindImageMemory(dev, image, image_mem, 0));

   VkCommandPool *pools = calloc(submits + 1, sizeof(*pools));
   VkCommandBuffer *cmds = calloc(submits + 1, sizeof(*cmds));
   uint8_t *want = calloc(SIZE, SIZE);
   VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
   VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   const double t0 = now_us();
   uint32_t pos = 0;

   for (uint32_t i = 0; i <= submits; ++i) {
      if (i == 0 || !one_pool) {
         VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
         CHECK(vkCreateCommandPool(dev, &info, NULL, &pools[i]));
      } else {
         pools[i] = pools[0];
      }
      VkCommandBufferAllocateInfo alloc = {
         VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pools[i],
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      CHECK(vkAllocateCommandBuffers(dev, &alloc, &cmds[i]));
      CHECK(vkBeginCommandBuffer(cmds[i], &begin));

      if (i == submits) {
         barrier(cmds[i], image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
         VkBufferImageCopy region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {SIZE, SIZE, 1},
         };
         vkCmdCopyImageToBuffer(cmds[i], image,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback,
                                1, &region);
      } else {
         barrier(cmds[i], image, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
         layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
         for (uint32_t c = 0; c < copies; ++c, ++pos) {
            const uint32_t x = pos % GRID * STEP, y = pos / GRID % GRID * STEP;
            const uint32_t tile = pos % tiles;
            VkBufferImageCopy region = {
               .bufferOffset = tile * tile_B,
               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
               .imageOffset = {x, y, 0},
               .imageExtent = {W, W, 1},
            };
            vkCmdCopyBufferToImage(cmds[i], src, image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
            for (uint32_t r = 0; r < W; ++r)
               memcpy(want + (y + r) * SIZE + x, src_map + tile * tile_B + r * W,
                      W);
         }
      }
      CHECK(vkEndCommandBuffer(cmds[i]));
      submit(cmds[i]);
   }
   const double t1 = now_us();
   CHECK(vkQueueWaitIdle(queue));
   const double t_idle = now_us();

   for (uint32_t i = 0; i <= submits; ++i) {
      if (one_pool)
         vkFreeCommandBuffers(dev, pools[0], 1, &cmds[i]);
      else
         vkDestroyCommandPool(dev, pools[i], NULL);
   }
   if (one_pool)
      vkDestroyCommandPool(dev, pools[0], NULL);
   const double t2 = now_us();

   /* The grid is covered once every tile position has been visited. */
   const uint32_t covered = pos >= GRID * GRID ? SIZE * SIZE : 0;
   uint32_t bad = 0;
   for (uint32_t i = 0; i < covered; ++i)
      bad += want[i] != dst_map[i];

   printf("%5u copies %s: %7.1f us/submit, %6.1f us/submit to release, %u of "
          "%u texels differ\n", copies, one_pool ? "one pool" : "per pool",
          (t1 - t0) / submits, (t2 - t_idle) / submits, bad, covered);
   free(pools);
   free(cmds);
   free(want);
   vkDestroyImage(dev, image, NULL);
   vkFreeMemory(dev, image_mem, NULL);
   vkDestroyBuffer(dev, src, NULL);
   vkFreeMemory(dev, src_mem, NULL);
   vkDestroyBuffer(dev, readback, NULL);
   vkFreeMemory(dev, readback_mem, NULL);
   return bad != 0 || !covered;
}

int
main(int argc, char **argv)
{
   const uint32_t submits = argc > 1 ? strtoul(argv[1], NULL, 0) : 2000;
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                         .pApplicationInfo = &app};
   VkInstance instance;
   CHECK(vkCreateInstance(&instance_info, NULL, &instance));

   uint32_t count = 1;
   VkPhysicalDevice pdev;
   VkResult result = vkEnumeratePhysicalDevices(instance, &count, &pdev);
   if (result < 0 || !count)
      return 77;
   vkGetPhysicalDeviceMemoryProperties(pdev, &mem_props);

   const float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                     .queueCreateInfoCount = 1,
                                     .pQueueCreateInfos = &queue_info};
   CHECK(vkCreateDevice(pdev, &device_info, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &queue);

   /* 2 fits the spans, 64 overflows every one of them into whole BOs. */
   int failed = 0;
   for (int one_pool = 0; one_pool < 2; ++one_pool) {
      failed |= burst(submits, 2, one_pool);
      failed |= burst(submits / 8, 64, one_pool);
   }

   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(instance, NULL);
   return failed;
}
