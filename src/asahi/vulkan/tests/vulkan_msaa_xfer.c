/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Transfer commands on 2x colour and depth/stencil attachments, which M4 Pro
 * compresses. Images A and B are cleared with vkCmdClearColorImage and
 * vkCmdClearDepthStencilImage to different values, vkCmdCopyImage copies a
 * rectangle of A into B, and a render pass reloads B (loadOp LOAD), covers
 * every tile with a draw that writes nothing, and resolves colour (average)
 * and depth/stencil (sample zero) into single-sample images. Inside the
 * rectangle the resolved values must be A's clear values, outside B's.
 *
 *   vulkan-msaa-xfer [depth=d32|d32s8] [format=rgba8|rgb10a2|rgba16f]
 *                    [mode=copy|clear|mixed|mixed-src]
 *
 * `clear` skips the copy (B keeps its clear values everywhere). `mixed` adds
 * input-attachment usage to B, which keeps it uncompressed: the copy then
 * goes from a compressed image to a plain one; `mixed-src` adds it to A.
 *
 * Exit 0 = PASS, 1 = FAIL. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "vulkan_gbuffer_bench_shaders.h"

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_ != VK_SUCCESS) {                                                 \
         printf("FAIL: %s = %d (line %d)\n", #x, r_, __LINE__);               \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

#define W 64
#define H 64
/* Not tile-aligned, so tiles on its edges are partly copied. */
#define RECT_X 9
#define RECT_Y 5
#define RECT_W 37
#define RECT_H 40

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties memprops;

struct img {
   VkImage image;
   VkImageView view;
   VkImageAspectFlags aspect;
};

static VkDeviceMemory
bind_memory(VkMemoryRequirements req, VkMemoryPropertyFlags flags)
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
make_image(VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
           VkImageAspectFlags aspect)
{
   struct img i = {.aspect = aspect};
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt,
      .extent = {W, H, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   };
   CHECK(vkCreateImage(dev, &ci, NULL, &i.image));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, i.image, &req);
   CHECK(vkBindImageMemory(
      dev, i.image, bind_memory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
   VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = i.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = fmt,
      .subresourceRange = {aspect, 0, 1, 0, 1},
   };
   CHECK(vkCreateImageView(dev, &vi, NULL, &i.view));
   return i;
}

static void
barrier(VkCommandBuffer cb)
{
   VkMemoryBarrier2 mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
   };
   VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                          .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
   vkCmdPipelineBarrier2(cb, &di);
}

static void
to_general(VkCommandBuffer cb, const struct img *i)
{
   VkImageMemoryBarrier2 ib = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = i->image,
      .subresourceRange = {i->aspect, 0, 1, 0, 1},
   };
   VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                          .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
   vkCmdPipelineBarrier2(cb, &di);
}

int
main(int argc, char **argv)
{
   const bool d32s8 = argc > 1 && !strcmp(argv[1], "d32s8");
   const char *cname = argc > 2 ? argv[2] : "rgba8";
   VkFormat cfmt;
   unsigned cbpp;
   if (!strcmp(cname, "rgba8")) {
      cfmt = VK_FORMAT_R8G8B8A8_UNORM, cbpp = 4;
   } else if (!strcmp(cname, "rgb10a2")) {
      cfmt = VK_FORMAT_A2R10G10B10_UNORM_PACK32, cbpp = 4;
   } else if (!strcmp(cname, "rgba16f")) {
      cfmt = VK_FORMAT_R16G16B16A16_SFLOAT, cbpp = 8;
   } else {
      fprintf(stderr, "usage: %s [d32|d32s8] [rgba8|rgb10a2|rgba16f] "
                      "[copy|clear|mixed|mixed-src]\n", argv[0]);
      return 2;
   }
   const char *mode = argc > 3 ? argv[3] : "copy";
   const bool do_copy = strcmp(mode, "clear");
   const VkImageUsageFlags b_usage =
      !strcmp(mode, "mixed") ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0;
   const VkImageUsageFlags a_usage =
      !strcmp(mode, "mixed-src") ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0;
   const VkFormat zfmt = d32s8 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
   const VkImageAspectFlags zaspect =
      VK_IMAGE_ASPECT_DEPTH_BIT | (d32s8 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
   printf("2x %s + %s, %ux%u, %s, copied rectangle (%u,%u) %ux%u\n", cname,
          d32s8 ? "D32S8" : "D32", W, H, mode, RECT_X, RECT_Y, RECT_W, RECT_H);

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
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = 0, .queueCount = 1,
                                  .pQueuePriorities = &prio};
   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = &f13, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci};
   CHECK(vkCreateDevice(pdev, &dci, NULL, &dev));
   VkQueue queue;
   vkGetDeviceQueue(dev, 0, 0, &queue);

   const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   const VkImageUsageFlags zu = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
   struct img ca = make_image(cfmt, VK_SAMPLE_COUNT_2_BIT, cu | a_usage,
                              VK_IMAGE_ASPECT_COLOR_BIT);
   struct img cb_ = make_image(cfmt, VK_SAMPLE_COUNT_2_BIT, cu | b_usage,
                               VK_IMAGE_ASPECT_COLOR_BIT);
   struct img cres = make_image(cfmt, VK_SAMPLE_COUNT_1_BIT, cu, VK_IMAGE_ASPECT_COLOR_BIT);
   struct img za = make_image(zfmt, VK_SAMPLE_COUNT_2_BIT, zu | a_usage, zaspect);
   struct img zb = make_image(zfmt, VK_SAMPLE_COUNT_2_BIT, zu | b_usage, zaspect);
   struct img zres = make_image(zfmt, VK_SAMPLE_COUNT_1_BIT, zu, zaspect);

   /* A draw that writes nothing but covers every tile, so the reload and
    * resolve run everywhere. */
   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, 32};
   VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pcr};
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));
   VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof(gbuffer_bench_vert),
                                    .pCode = gbuffer_bench_vert};
   VkShaderModule vs;
   CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
   VkPipelineShaderStageCreateInfo stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"};
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
   VkViewport vp = {0, 0, W, H, 0, 1};
   VkRect2D scissor = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &scissor};
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_2_BIT};
   VkPipelineDepthStencilStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0};
   VkPipelineColorBlendStateCreateInfo cbs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba};
   VkPipelineRenderingCreateInfo rci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1, .pColorAttachmentFormats = &cfmt,
      .depthAttachmentFormat = zfmt,
      .stencilAttachmentFormat = d32s8 ? zfmt : VK_FORMAT_UNDEFINED};
   VkGraphicsPipelineCreateInfo gci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rci, .stageCount = 1, .pStages = &stage,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia,
      .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pDepthStencilState = &ds,
      .pColorBlendState = &cbs, .layout = layout};
   VkPipeline pipeline;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipeline));

   /* Readback: resolved colour, depth and (D32S8) stencil. */
   const VkDeviceSize plane_B = (VkDeviceSize)W * H * 8;
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = plane_B * 3,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements breq;
   vkGetBufferMemoryRequirements(dev, buf, &breq);
   VkDeviceMemory bmem = bind_memory(breq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECK(vkBeginCommandBuffer(cb, &cbi));
   const struct img *all[] = {&ca, &cb_, &cres, &za, &zb, &zres};
   for (unsigned i = 0; i < 6; i++)
      to_general(cb, all[i]);

   const VkClearColorValue col_a = {{1.0f, 0.0f, 0.0f, 1.0f}};
   const VkClearColorValue col_b = {{0.0f, 0.0f, 1.0f, 1.0f}};
   const VkClearDepthStencilValue z_a = {0.25f, 7}, z_b = {0.75f, 200};
   const VkImageSubresourceRange crange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   const VkImageSubresourceRange zrange = {zaspect, 0, 1, 0, 1};
   vkCmdClearColorImage(cb, ca.image, VK_IMAGE_LAYOUT_GENERAL, &col_a, 1, &crange);
   vkCmdClearColorImage(cb, cb_.image, VK_IMAGE_LAYOUT_GENERAL, &col_b, 1, &crange);
   vkCmdClearDepthStencilImage(cb, za.image, VK_IMAGE_LAYOUT_GENERAL, &z_a, 1, &zrange);
   vkCmdClearDepthStencilImage(cb, zb.image, VK_IMAGE_LAYOUT_GENERAL, &z_b, 1, &zrange);
   barrier(cb);

   VkImageCopy copy = {
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffset = {RECT_X, RECT_Y, 0},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffset = {RECT_X, RECT_Y, 0},
      .extent = {RECT_W, RECT_H, 1},
   };
   if (do_copy) {
      vkCmdCopyImage(cb, ca.image, VK_IMAGE_LAYOUT_GENERAL, cb_.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
      copy.srcSubresource.aspectMask = copy.dstSubresource.aspectMask = zaspect;
      vkCmdCopyImage(cb, za.image, VK_IMAGE_LAYOUT_GENERAL, zb.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
   }
   barrier(cb);

   VkRenderingAttachmentInfo catt = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = cb_.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
      .resolveImageView = cres.view,
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
   };
   VkRenderingAttachmentInfo zatt = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = zb.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
      .resolveImageView = zres.view,
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
   };
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                         .renderArea = {{0, 0}, {W, H}},
                         .layerCount = 1,
                         .colorAttachmentCount = 1,
                         .pColorAttachments = &catt,
                         .pDepthAttachment = &zatt,
                         .pStencilAttachment = d32s8 ? &zatt : NULL};
   vkCmdBeginRendering(cb, &ri);
   const float push[8] = {-1, -1, 1, 1, 0.5f, 0, 0, 1};
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdPushConstants(cb, layout, pcr.stageFlags, 0, sizeof(push), push);
   vkCmdDraw(cb, 4, 1, 0, 0);
   vkCmdEndRendering(cb);
   barrier(cb);

   VkBufferImageCopy bic = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {W, H, 1}};
   vkCmdCopyImageToBuffer(cb, cres.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
   bic.bufferOffset = plane_B;
   bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
   vkCmdCopyImageToBuffer(cb, zres.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
   if (d32s8) {
      bic.bufferOffset = plane_B * 2;
      bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      vkCmdCopyImageToBuffer(cb, zres.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
   }
   barrier(cb);
   r = vkEndCommandBuffer(cb);
   if (r != VK_SUCCESS) {
      printf("vkEndCommandBuffer = %d\nFAIL\n", r);
      return 1;
   }
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1, .pCommandBuffers = &cb};
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));

   /* The clear values exactly representable in every format here. */
   uint8_t want_a[8], want_b[8];
   if (cfmt == VK_FORMAT_R8G8B8A8_UNORM) {
      memcpy(want_a, (uint8_t[]){0xff, 0, 0, 0xff}, 4);
      memcpy(want_b, (uint8_t[]){0, 0, 0xff, 0xff}, 4);
   } else if (cfmt == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
      const uint32_t a = 3u << 30 | 0x3ffu << 20, b = 3u << 30 | 0x3ffu;
      memcpy(want_a, &a, 4);
      memcpy(want_b, &b, 4);
   } else {
      const uint16_t a[4] = {0x3c00, 0, 0, 0x3c00}, b[4] = {0, 0, 0x3c00, 0x3c00};
      memcpy(want_a, a, 8);
      memcpy(want_b, b, 8);
   }

   void *map;
   CHECK(vkMapMemory(dev, bmem, 0, plane_B * 3, 0, &map));
   const uint8_t *col = map;
   const float *depth = (const float *)((const uint8_t *)map + plane_B);
   const uint8_t *stencil = (const uint8_t *)map + plane_B * 2;
   unsigned bad_c = 0, bad_z = 0, bad_s = 0;
   for (unsigned y = 0; y < H; y++) {
      for (unsigned x = 0; x < W; x++) {
         const unsigned px = y * W + x;
         const bool in = do_copy && x >= RECT_X && x < RECT_X + RECT_W && y >= RECT_Y &&
                         y < RECT_Y + RECT_H;
         bad_c += !!memcmp(col + px * cbpp, in ? want_a : want_b, cbpp);
         bad_z += depth[px] != (in ? z_a.depth : z_b.depth);
         bad_s += d32s8 && stencil[px] != (in ? z_a.stencil : z_b.stencil);
      }
   }
   printf("wrong pixels: colour %u, depth %u, stencil %u of %u\n", bad_c, bad_z,
          bad_s, W * H);
   const bool pass = !bad_c && !bad_z && !bad_s;
   printf("%s\n", pass ? "PASS" : "FAIL (contents)");
   return pass ? 0 : 1;
}
