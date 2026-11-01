/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * R11G11B10F attachments, which M4 Pro compresses through the format's own
 * PBE class. Each step runs on a compressed image and on a plain twin (with
 * STORAGE usage at 1x and input-attachment usage at 2x) and they must agree
 * bit for bit, except that the compressed class reads a NaN field as zero:
 *
 *   values   a draw stores every class of field, in every sample
 *   reload   a LOAD pass keeps what it does not cover
 *   clear    vkCmdClearColorImage, then a LOAD pass
 *   copy     vkCmdCopyImage both ways, partial, from and to the plain twin
 *   mips     every level rendered, then read back (1x)
 *
 * and at 2x, against the known mean of the two samples:
 *
 *   resolve  one target, storeOp DONT_CARE (end-of-tile resolve) and STORE,
 *            and vkCmdResolveImage
 *
 * The printed hash of the plain image lets two drivers be compared.
 *
 *   aurora-r11g11b10f [samples=1|2]
 *
 * Exit 0 = PASS, 1 = FAIL. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "aurora_r11g11b10f_shaders.h"

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_ != VK_SUCCESS) {                                                 \
         printf("FAIL: %s = %d (line %d)\n", #x, r_, __LINE__);               \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

#define W      96
#define H      64
#define LEVELS 5 /* the last two are below the compressed 16x16 */
#define FORMAT VK_FORMAT_B10G11R11_UFLOAT_PACK32

static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties memprops;
static VkPipelineLayout layout;
static VkPipeline pipes[2];
static VkBuffer readback;
static uint32_t *readback_map;
static unsigned failures;

struct img {
   VkImage image;
   VkImageView view[LEVELS];
};

struct push {
   uint32_t mode;
   /* Width the draw covers: the rest must survive the pass. Zero is all. */
   uint32_t width;
};

static VkDeviceMemory
memory(VkMemoryRequirements req, VkMemoryPropertyFlags flags)
{
   uint32_t type = 0;
   while (!(req.memoryTypeBits & (1u << type)) ||
          (memprops.memoryTypes[type].propertyFlags & flags) != flags)
      type++;
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL,
                              req.size, type};
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
   return mem;
}

static struct img
make_image(VkSampleCountFlagBits samples, unsigned levels, VkImageUsageFlags extra)
{
   struct img i;
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = FORMAT,
      .extent = {W, H, 1},
      .mipLevels = levels,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra,
   };
   CHECK(vkCreateImage(dev, &ci, NULL, &i.image));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, i.image, &req);
   CHECK(vkBindImageMemory(
      dev, i.image, memory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
   for (unsigned l = 0; l < levels; l++) {
      VkImageViewCreateInfo vi = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = i.image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = FORMAT,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1},
      };
      CHECK(vkCreateImageView(dev, &vi, NULL, &i.view[l]));
   }
   return i;
}

static VkCommandBuffer
begin(void)
{
   VkCommandBufferAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECK(vkBeginCommandBuffer(cb, &bi));
   return cb;
}

static void
submit(VkCommandBuffer cb)
{
   CHECK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1,
                      .pCommandBuffers = &cb};
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));
   vkFreeCommandBuffers(dev, pool, 1, &cb);
}

static void
barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, unsigned levels)
{
   VkImageMemoryBarrier2 ib = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
      .oldLayout = from,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1},
   };
   VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                          .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &ib};
   vkCmdPipelineBarrier2(cb, &di);
}

/* A pass over one level of `t`. resolve is a 1x image or NULL. */
static void
pass(VkCommandBuffer cb, const struct img *t, unsigned level, unsigned pipe,
     VkAttachmentLoadOp load, VkAttachmentStoreOp store, const struct img *resolve,
     struct push p)
{
   VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = t->view[level],
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = resolve ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
      .resolveImageView = resolve ? resolve->view[0] : VK_NULL_HANDLE,
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = load,
      .storeOp = store,
   };
   const VkExtent2D extent = {W >> level, H >> level};
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                         .renderArea = {{0, 0}, extent},
                         .layerCount = 1,
                         .colorAttachmentCount = 1,
                         .pColorAttachments = &att};
   VkViewport vp = {0, 0, extent.width, extent.height, 0, 1};
   VkRect2D sc = {{0, 0}, {p.width ? p.width : extent.width, extent.height}};
   vkCmdBeginRendering(cb, &ri);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[pipe]);
   vkCmdSetViewport(cb, 0, 1, &vp);
   vkCmdSetScissor(cb, 0, 1, &sc);
   vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &p.mode);
   vkCmdDraw(cb, 3, 1, 0, 0);
   vkCmdEndRendering(cb);
}

static void
readback_1x(VkCommandBuffer cb, VkImage image, unsigned level, VkDeviceSize offset)
{
   VkBufferImageCopy c = {
      .bufferOffset = offset,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
      .imageExtent = {W >> level, H >> level, 1},
   };
   vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, readback, 1, &c);
}

/* Resolve a 2x image to 1x through the sample-exact path or read a 1x one. */
static void
read_image(VkCommandBuffer cb, const struct img *i, const struct img *scratch,
           VkDeviceSize offset)
{
   if (!scratch) {
      readback_1x(cb, i->image, 0, offset);
      return;
   }
   VkImageResolve r = {
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .extent = {W, H, 1},
   };
   vkCmdResolveImage(cb, i->image, VK_IMAGE_LAYOUT_GENERAL, scratch->image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &r);
   barrier(cb, scratch->image, VK_IMAGE_LAYOUT_GENERAL, 1);
   readback_1x(cb, scratch->image, 0, offset);
}

/* A word with its NaN fields zeroed. */
static uint32_t
without_nan(uint32_t w)
{
   if ((w & 0x7c0) == 0x7c0 && (w & 0x03f))
      w &= ~0x7ffu;
   if ((w & 0x3e0000) == 0x3e0000 && (w & 0x01f800))
      w &= ~0x3ff800u;
   if ((w & 0xf8000000) == 0xf8000000 && (w & 0x07c00000))
      w &= ~0xffc00000u;
   return w;
}

static void
compare(const char *what, unsigned words)
{
   const uint32_t *a = readback_map, *b = readback_map + W * H;
   unsigned bad = 0, set = 0, nan = 0;
   uint64_t h = 0xcbf29ce484222325ull;
   for (unsigned i = 0; i < words; i++) {
      if (a[i] != without_nan(b[i]) && !bad++)
         printf("  word %u: compressed %08x plain %08x\n", i, a[i], b[i]);
      nan += b[i] != without_nan(b[i]);
      set += b[i] != 0;
      h = (h ^ b[i]) * 0x100000001b3ull;
   }
   printf("%-10s %5u of %u words differ, %u set, %u with NaN, plain %016llx\n",
          what, bad, words, set, nan, (unsigned long long)h);
   failures += bad != 0 || !set;
}

static void
expect_mean(const char *what)
{
   /* (1, 0.5, 0.25) and (3, 1.5, 0.75) average to (2, 1, 0.5). */
   const uint32_t want = 0x1c0u << 22 | 0x3c0u << 11 | 0x400u;
   unsigned bad = 0;
   for (unsigned i = 0; i < W * H; i++)
      bad += readback_map[i] != want;
   printf("%-10s %5u of %u pixels are not the mean, first %08x want %08x\n",
          what, bad, W * H, readback_map[0], want);
   failures += bad != 0;
}

int
main(int argc, char **argv)
{
   const VkSampleCountFlagBits samples = argc > 1 ? atoi(argv[1]) : 1;
   const bool ms = samples > 1;
   if (samples != VK_SAMPLE_COUNT_1_BIT && samples != VK_SAMPLE_COUNT_2_BIT) {
      fprintf(stderr, "usage: %s [samples=1|2]\n", argv[0]);
      return 2;
   }

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkPhysicalDevice pdev;
   VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pdev);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) {
      printf("FAIL: no physical device\n");
      return 1;
   }
   vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);

   float prio = 1;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio};
   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
      .synchronization2 = VK_TRUE,
   };
   VkPhysicalDeviceFeatures2 features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &f13,
      .features = {.sampleRateShading = VK_TRUE},
   };
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = &features,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci};
   CHECK(vkCreateDevice(pdev, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &queue);
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = 2 * 4 * W * H,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   CHECK(vkCreateBuffer(dev, &bci, NULL, &readback));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, readback, &req);
   VkDeviceMemory rm = memory(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   CHECK(vkBindBufferMemory(dev, readback, rm, 0));
   CHECK(vkMapMemory(dev, rm, 0, VK_WHOLE_SIZE, 0, (void **)&readback_map));

   VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
   VkPipelineLayoutCreateInfo pl = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &range,
   };
   CHECK(vkCreatePipelineLayout(dev, &pl, NULL, &layout));
   VkShaderModuleCreateInfo vm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = sizeof(r11_vert),
                                  .pCode = r11_vert};
   VkShaderModuleCreateInfo fm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = sizeof(r11_frag),
                                  .pCode = r11_frag};
   VkShaderModule vs, fs;
   CHECK(vkCreateShaderModule(dev, &vm, NULL, &vs));
   CHECK(vkCreateShaderModule(dev, &fm, NULL, &fs));
   VkFormat format = FORMAT;
   VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   for (unsigned p = 0; p < 2; p++) {
      /* pipes[0] matches the image, pipes[1] the 1x levels. */
      VkSampleCountFlagBits pipe_samples = p ? VK_SAMPLE_COUNT_1_BIT : samples;
      VkGraphicsPipelineCreateInfo gp = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext = &(VkPipelineRenderingCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &format},
         .stageCount = 2,
         .pStages = (VkPipelineShaderStageCreateInfo[]){
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}},
         .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
         .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
         .pViewportState = &(VkPipelineViewportStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1},
         .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .lineWidth = 1},
         .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = pipe_samples,
            .sampleShadingEnable = pipe_samples > 1,
            .minSampleShading = 1},
         .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &(VkPipelineColorBlendAttachmentState){
               .colorWriteMask = 0xf}},
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn},
         .layout = layout,
      };
      CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipes[p]));
   }

   const VkImageUsageFlags plain_usage = ms ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
                                            : VK_IMAGE_USAGE_STORAGE_BIT;
   struct img t[2] = {make_image(samples, 1, 0), make_image(samples, 1, plain_usage)};
   struct img res[2] = {make_image(1, 1, 0), make_image(1, 1, VK_IMAGE_USAGE_STORAGE_BIT)};
   struct img mips[2] = {make_image(1, LEVELS, 0),
                         make_image(1, LEVELS, VK_IMAGE_USAGE_STORAGE_BIT)};
   VkCommandBuffer cb = begin();
   for (unsigned i = 0; i < 2; i++) {
      barrier(cb, t[i].image, VK_IMAGE_LAYOUT_UNDEFINED, 1);
      barrier(cb, res[i].image, VK_IMAGE_LAYOUT_UNDEFINED, 1);
      barrier(cb, mips[i].image, VK_IMAGE_LAYOUT_UNDEFINED, LEVELS);
   }
   submit(cb);

#define READ_TWINS(what)                                                      \
   do {                                                                       \
      cb = begin();                                                           \
      for (unsigned i = 0; i < 2; i++) {                                      \
         barrier(cb, t[i].image, VK_IMAGE_LAYOUT_GENERAL, 1);                 \
         read_image(cb, &t[i], ms ? &res[i] : NULL, 4 * W * H * i);           \
      }                                                                       \
      submit(cb);                                                             \
      compare(what, W * H);                                                   \
   } while (0)

   const VkAttachmentLoadOp clear = VK_ATTACHMENT_LOAD_OP_CLEAR;
   const VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD;
   const VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;

   cb = begin();
   for (unsigned i = 0; i < 2; i++)
      pass(cb, &t[i], 0, 0, clear, store, NULL, (struct push){0, 0});
   submit(cb);
   READ_TWINS("values");

   cb = begin();
   for (unsigned i = 0; i < 2; i++)
      pass(cb, &t[i], 0, 0, load, store, NULL, (struct push){2, 21});
   submit(cb);
   READ_TWINS("reload");

   const VkClearColorValue colour = {{0.5f, 4.0f, 0.125f, 1.0f}};
   const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   cb = begin();
   for (unsigned i = 0; i < 2; i++) {
      vkCmdClearColorImage(cb, t[i].image, VK_IMAGE_LAYOUT_GENERAL, &colour, 1, &all);
      barrier(cb, t[i].image, VK_IMAGE_LAYOUT_GENERAL, 1);
      pass(cb, &t[i], 0, 0, load, store, NULL, (struct push){2, 11});
   }
   submit(cb);
   READ_TWINS("clear");

   cb = begin();
   for (unsigned i = 0; i < 2; i++)
      pass(cb, &t[i], 0, 0, clear, store, NULL, (struct push){0, 0});
   submit(cb);
   cb = begin();
   /* Not tile-aligned, so tiles on the edges are partly written. Only the
    * compressed image is the target of the copy that matters, so the plain
    * twin takes the same region from itself and stays the reference. */
   VkImageCopy region = {
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffset = {5, 3, 0},
      .dstOffset = {37, 19, 0},
      .extent = {41, 29, 1},
   };
   vkCmdCopyImage(cb, t[1].image, VK_IMAGE_LAYOUT_GENERAL, t[0].image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &region);
   vkCmdCopyImage(cb, t[1].image, VK_IMAGE_LAYOUT_GENERAL, t[1].image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &region);
   submit(cb);
   READ_TWINS("copy");

   if (ms) {
      for (unsigned i = 0; i < 3; i++) {
         cb = begin();
         for (unsigned k = 0; k < 2; k++)
            barrier(cb, res[k].image, VK_IMAGE_LAYOUT_GENERAL, 1);
         /* Same image and resolve target on both twins: compressed source
          * into a compressed target, then plain into plain. */
         for (unsigned k = 0; k < 2; k++) {
            if (i < 2) {
               pass(cb, &t[k], 0, 0, clear,
                    i ? store : VK_ATTACHMENT_STORE_OP_DONT_CARE, &res[k],
                    (struct push){1, 0});
            } else {
               pass(cb, &t[k], 0, 0, clear, store, NULL, (struct push){1, 0});
               barrier(cb, t[k].image, VK_IMAGE_LAYOUT_GENERAL, 1);
               VkImageResolve r2 = {
                  .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                  .extent = {W, H, 1},
               };
               vkCmdResolveImage(cb, t[k].image, VK_IMAGE_LAYOUT_GENERAL,
                                 res[k].image, VK_IMAGE_LAYOUT_GENERAL, 1, &r2);
            }
            barrier(cb, res[k].image, VK_IMAGE_LAYOUT_GENERAL, 1);
            readback_1x(cb, res[k].image, 0, 4 * W * H * k);
         }
         submit(cb);
         static const char *names[] = {"res-eot", "res-store", "res-image"};
         compare(names[i], W * H);
         expect_mean(names[i]);
      }
   } else {
      for (unsigned l = 0; l < LEVELS; l++) {
         cb = begin();
         for (unsigned i = 0; i < 2; i++) {
            pass(cb, &mips[i], l, 1, clear, store, NULL, (struct push){0, 0});
            barrier(cb, mips[i].image, VK_IMAGE_LAYOUT_GENERAL, LEVELS);
            readback_1x(cb, mips[i].image, l, 4 * W * H * i);
         }
         submit(cb);
         char name[16];
         snprintf(name, sizeof(name), "mip %u", l);
         compare(name, (W >> l) * (H >> l));
      }
   }

   printf(failures ? "FAIL\n" : "PASS\n");
   return failures != 0;
}
