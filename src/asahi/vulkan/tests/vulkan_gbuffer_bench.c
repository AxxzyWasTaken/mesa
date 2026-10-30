/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Cost of reloading and storing a 2x multisample G-buffer, in the shape of
 * Transport Fever 3's frame: three colour targets (RGBA8 standing in for
 * R11G11B10F, A2R10G10B10, RGBA8) and a depth attachment at 3456x2234,
 * with the attachments' own usages (colour and transfer, depth and
 * transfer; never sampled).
 *
 * `rt0=r11g11b10f` uses the game's own format, which stays uncompressed.
 *
 * A frame clears the G-buffer and draws one full-screen layer whose content
 * is a smooth gradient mixed with `noise` of per-pixel hash. It then visits
 * the G-buffer `visits` times: each visit is a 2x pass with loadOp LOAD and
 * storeOp STORE on every attachment that blends `draws` rectangles covering
 * `cover` of the screen, and resolves colour 0 and depth (`resolve=all`:
 * every attachment) into single-sample images at the end of the pass. A
 * barrier, then a full-resolution 1x pass samples the resolved colour and
 * depth.
 *
 *   aurora-gbuffer-bench [noise=0.3] [visits=6] [frames=10] [draws=2]
 *                        [cover=0.25] [resolve=1|all] [depth=d32|d32s8]
 *                        [rt0=rgba8|r11g11b10f] [plain=-|c|z|cz]
 *
 * `plain` adds input-attachment usage to the colour (c) and/or depth (z)
 * attachments, which keeps them uncompressed on G17P: the same driver then
 * times plain against compressed, and must give the same hashes.
 *
 * Take per-job GPU time from the asahi_g17_complete tracepoint. The program
 * prints the wall-clock time per frame as a cross-check, and a hash of every
 * resolved image and of the 1x output after the last frame: plain and
 * compressed attachments must give the same hashes. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

#define W 3456
#define H 2234
#define NUM_RT 3

static VkFormat rt_formats[NUM_RT] = {
   VK_FORMAT_R8G8B8A8_UNORM,
   VK_FORMAT_A2R10G10B10_UNORM_PACK32,
   VK_FORMAT_R8G8B8A8_UNORM,
};

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties memprops;

struct img {
   VkImage image;
   VkImageView view;
   VkFormat format;
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
   struct img i = {.format = fmt, .aspect = aspect};
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt,
      .extent = {W, H, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
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

static VkShaderModule
make_module(const uint32_t *code, size_t size)
{
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = size, .pCode = code};
   VkShaderModule m;
   CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
   return m;
}

static VkPipeline
make_pipeline(VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs,
              unsigned samples, unsigned rts, const VkFormat *formats,
              VkFormat depth, bool blend, bool depth_write)
{
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
   };
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
      .rasterizationSamples = samples};
   VkPipelineDepthStencilStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = depth != VK_FORMAT_UNDEFINED,
      .depthWriteEnable = depth_write,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
   VkPipelineColorBlendAttachmentState cba[NUM_RT];
   for (unsigned i = 0; i < rts; i++) {
      cba[i] = (VkPipelineColorBlendAttachmentState){
         .blendEnable = blend,
         .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
         .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .colorBlendOp = VK_BLEND_OP_ADD,
         .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
         .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
         .alphaBlendOp = VK_BLEND_OP_ADD,
         .colorWriteMask = 0xf};
   }
   VkPipelineColorBlendStateCreateInfo cbs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = rts, .pAttachments = cba};
   VkPipelineRenderingCreateInfo rci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = rts, .pColorAttachmentFormats = formats,
      .depthAttachmentFormat = depth};
   VkGraphicsPipelineCreateInfo gci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rci, .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia,
      .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pDepthStencilState = &ds,
      .pColorBlendState = &cbs, .layout = layout};
   VkPipeline pipeline;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipeline));
   return pipeline;
}

static double
now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
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

struct push {
   float rect[4];
   float z0, dz, noise, alpha;
};

int
main(int argc, char **argv)
{
   const float noise = argc > 1 ? atof(argv[1]) : 0.3f;
   const unsigned visits = argc > 2 ? atoi(argv[2]) : 6;
   const unsigned frames = argc > 3 ? atoi(argv[3]) : 10;
   const unsigned draws = argc > 4 ? atoi(argv[4]) : 2;
   const float cover = argc > 5 ? atof(argv[5]) : 0.25f;
   const bool resolve_all = argc > 6 && !strcmp(argv[6], "all");
   const bool d32s8 = argc > 7 && !strcmp(argv[7], "d32s8");
   if (argc > 8 && !strcmp(argv[8], "r11g11b10f"))
      rt_formats[0] = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
   const char *plain = argc > 9 ? argv[9] : "-";
   const VkImageUsageFlags plain_c =
      strchr(plain, 'c') ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0;
   const VkImageUsageFlags plain_z =
      strchr(plain, 'z') ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0;
   const VkFormat zfmt = d32s8 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
   if (!frames || cover < 0 || cover > 1) {
      fprintf(stderr, "usage: %s [noise] [visits] [frames] [draws] [cover] "
                      "[resolve=1|all] [depth=d32|d32s8] [rt0=rgba8|r11g11b10f] "
                      "[plain=-|c|z|cz]\n",
              argv[0]);
      return 2;
   }
   printf("%ux%u@2, noise %.2f, %u visits x %u draws covering %.2f, resolve %s, "
          "%s, rt0 %s, plain %s, %u frames\n", W, H, noise, visits, draws, cover,
          resolve_all ? "all" : "rt0+depth", d32s8 ? "D32S8" : "D32",
          rt_formats[0] == VK_FORMAT_R8G8B8A8_UNORM ? "RGBA8" : "R11G11B10F",
          plain, frames);

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

   /* The game's usages: 0x13 for colour, 0x23 for depth. */
   const VkImageUsageFlags xfer =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   struct img rt[NUM_RT], res[NUM_RT];
   for (unsigned i = 0; i < NUM_RT; i++) {
      rt[i] = make_image(rt_formats[i], VK_SAMPLE_COUNT_2_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | xfer | plain_c,
                         VK_IMAGE_ASPECT_COLOR_BIT);
      res[i] = make_image(rt_formats[i], VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT | xfer,
                          VK_IMAGE_ASPECT_COLOR_BIT);
   }
   const VkImageAspectFlags zaspect =
      VK_IMAGE_ASPECT_DEPTH_BIT | (d32s8 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
   struct img depth = make_image(zfmt, VK_SAMPLE_COUNT_2_BIT,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | xfer |
                                    plain_z,
                                 zaspect);
   struct img zres = make_image(zfmt, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT | xfer,
                                zaspect);
   struct img out = make_image(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | xfer,
                               VK_IMAGE_ASPECT_COLOR_BIT);
   /* The 1x pass samples depth only. */
   VkImageView zres_depth;
   VkImageViewCreateInfo zvi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = zres.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = zfmt,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
   };
   CHECK(vkCreateImageView(dev, &zvi, NULL, &zres_depth));

   VkSampler sampler;
   VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST};
   CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));
   VkDescriptorSetLayoutBinding binds[2] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler},
   };
   VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2, .pBindings = binds};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dpool;
   CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
   VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = dpool, .descriptorSetCount = 1,
                                       .pSetLayouts = &dsl};
   VkDescriptorSet set;
   CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
   VkDescriptorImageInfo dii[2] = {
      {sampler, res[0].view, VK_IMAGE_LAYOUT_GENERAL},
      {sampler, zres_depth, VK_IMAGE_LAYOUT_GENERAL},
   };
   VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = set, .dstBinding = 0, .descriptorCount = 2,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = dii};
   vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(struct push)};
   VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pcr};
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));

   VkShaderModule vs = make_module(gbuffer_bench_vert, sizeof(gbuffer_bench_vert));
   VkShaderModule fs = make_module(gbuffer_bench_frag, sizeof(gbuffer_bench_frag));
   VkShaderModule post = make_module(gbuffer_bench_post_frag, sizeof(gbuffer_bench_post_frag));
   VkPipeline scene = make_pipeline(layout, vs, fs, 2, NUM_RT, rt_formats, zfmt,
                                    false, true);
   VkPipeline decal = make_pipeline(layout, vs, fs, 2, NUM_RT, rt_formats, zfmt,
                                    true, false);
   const VkFormat out_fmt = VK_FORMAT_R8G8B8A8_UNORM;
   VkPipeline postp = make_pipeline(layout, vs, post, 1, 1, &out_fmt,
                                    VK_FORMAT_UNDEFINED, false, false);

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 3};
   VkCommandBuffer cbs[3];
   CHECK(vkAllocateCommandBuffers(dev, &cai, cbs));

   /* Command buffer 0 moves everything to GENERAL; 1 is the frames; 2 reads
    * back the resolved images and the output. */
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECK(vkBeginCommandBuffer(cbs[0], &cbi));
   for (unsigned i = 0; i < NUM_RT; i++) {
      to_general(cbs[0], &rt[i]);
      to_general(cbs[0], &res[i]);
   }
   to_general(cbs[0], &depth);
   to_general(cbs[0], &zres);
   to_general(cbs[0], &out);
   CHECK(vkEndCommandBuffer(cbs[0]));

   VkCommandBuffer cb = cbs[1];
   CHECK(vkBeginCommandBuffer(cb, &cbi));
   for (unsigned f = 0; f < frames; f++) {
      for (unsigned v = 0; v <= visits; v++) {
         const bool first = v == 0;
         VkRenderingAttachmentInfo att[NUM_RT];
         for (unsigned i = 0; i < NUM_RT; i++) {
            const bool resolve = !first && (i == 0 || resolve_all);
            att[i] = (VkRenderingAttachmentInfo){
               .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
               .imageView = rt[i].view,
               .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
               .resolveMode = resolve ? VK_RESOLVE_MODE_AVERAGE_BIT
                                      : VK_RESOLVE_MODE_NONE,
               .resolveImageView = resolve ? res[i].view : VK_NULL_HANDLE,
               .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
               .loadOp = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
               .clearValue.color = {{0.25f, 0.5f, 0.75f, 1.0f}},
            };
         }
         VkRenderingAttachmentInfo datt = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depth.view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = first ? VK_RESOLVE_MODE_NONE : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
            .resolveImageView = first ? VK_NULL_HANDLE : zres.view,
            .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.depthStencil = {1.0f, 0},
         };
         VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .renderArea = {{0, 0}, {W, H}},
                               .layerCount = 1,
                               .colorAttachmentCount = NUM_RT,
                               .pColorAttachments = att,
                               .pDepthAttachment = &datt,
                               .pStencilAttachment = d32s8 ? &datt : NULL};
         barrier(cb);
         vkCmdBeginRendering(cb, &ri);
         if (first) {
            /* A tilted full-screen layer, so depth varies across the screen. */
            struct push p = {{-1, -1, 1, 1}, 0.6f, 0.2f, noise, 1.0f};
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scene);
            vkCmdPushConstants(cb, layout, pcr.stageFlags, 0, sizeof(p), &p);
            vkCmdDraw(cb, 4, 1, 0, 0);
         } else {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, decal);
            for (unsigned d = 0; d < draws; d++) {
               /* Rectangles of `cover` of the screen at varying places. */
               const float s = 2.0f * sqrtf(cover);
               const float x = -1.0f + (2.0f - s) * ((v * 3 + d) % 5) / 4.0f;
               const float y = -1.0f + (2.0f - s) * ((v + d * 2) % 3) / 2.0f;
               struct push p = {{x, y, x + s, y + s}, 0.3f, 0.0f, noise, 0.25f};
               vkCmdPushConstants(cb, layout, pcr.stageFlags, 0, sizeof(p), &p);
               vkCmdDraw(cb, 4, 1, 0, 0);
            }
         }
         vkCmdEndRendering(cb);
         if (first)
            continue;

         barrier(cb);
         VkRenderingAttachmentInfo oatt = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = out.view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         };
         VkRenderingInfo ori = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                .renderArea = {{0, 0}, {W, H}},
                                .layerCount = 1,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &oatt};
         vkCmdBeginRendering(cb, &ori);
         struct push p = {{-1, -1, 1, 1}, 0.0f, 0.0f, 0.0f, 1.0f};
         vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postp);
         vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                 &set, 0, NULL);
         vkCmdPushConstants(cb, layout, pcr.stageFlags, 0, sizeof(p), &p);
         vkCmdDraw(cb, 4, 1, 0, 0);
         vkCmdEndRendering(cb);
      }
   }
   CHECK(vkEndCommandBuffer(cb));

   /* Readback: every resolved image, the resolved depth and the output. */
   const unsigned nread = NUM_RT + 2;
   const VkDeviceSize img_B = (VkDeviceSize)W * H * 4;
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = img_B * nread,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer rb;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &rb));
   VkMemoryRequirements breq;
   vkGetBufferMemoryRequirements(dev, rb, &breq);
   VkDeviceMemory rbmem = bind_memory(breq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   CHECK(vkBindBufferMemory(dev, rb, rbmem, 0));
   CHECK(vkBeginCommandBuffer(cbs[2], &cbi));
   barrier(cbs[2]);
   const struct img *reads[] = {&res[0], &res[1], &res[2], &zres, &out};
   for (unsigned i = 0; i < nread; i++) {
      VkBufferImageCopy bic = {
         .bufferOffset = img_B * i,
         .imageSubresource = {reads[i]->aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                                 ? VK_IMAGE_ASPECT_DEPTH_BIT
                                 : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {W, H, 1},
      };
      vkCmdCopyImageToBuffer(cbs[2], reads[i]->image, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &bic);
   }
   barrier(cbs[2]);
   CHECK(vkEndCommandBuffer(cbs[2]));

   /* Warm-up (shaders and allocation), then the timed run. */
   VkSubmitInfo si0 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 2, .pCommandBuffers = cbs};
   CHECK(vkQueueSubmit(queue, 1, &si0, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 2, .pCommandBuffers = &cbs[1]};
   const double t0 = now_ms();
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));
   printf("wall-clock: %.3f ms per frame\n", (now_ms() - t0) / frames);

   void *map;
   CHECK(vkMapMemory(dev, rbmem, 0, img_B * nread, 0, &map));
   static const char *names[] = {"rt0", "rt1", "rt2", "depth", "out"};
   for (unsigned i = 0; i < nread; i++) {
      /* FNV-1a over the image. rt1/rt2 are only resolved with resolve=all. */
      const uint8_t *p = (const uint8_t *)map + img_B * i;
      uint64_t h = 0xcbf29ce484222325ull;
      for (VkDeviceSize b = 0; b < img_B; b++)
         h = (h ^ p[b]) * 0x100000001b3ull;
      printf("hash %s %016llx\n", names[i], (unsigned long long)h);
   }
   return 0;
}
