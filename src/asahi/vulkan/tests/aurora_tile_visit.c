/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * A multisample G-buffer visit that leaves some targets alone, in the shape of
 * Transport Fever 3's post-processing: 3456x2234, colour targets loaded and
 * stored, one vkQueueSubmit per pass, a fragment shader writing one target or
 * none. Each visit must leave the targets it does not write exactly as they
 * were, sample by sample, and write the others as a one-target pass does.
 * A layout that spills is not elided, and carries that path's own bugs.
 *
 *   aurora-tile-visit [ms=2] [fmts=rgba8,rgb10]
 *
 * fmts names one to three targets from r11f, rgb10, rgba8, bgra8, rgba16f,
 * rgba32f, rg16f, r32f and r8. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "aurora_tile_visit_shaders.h"

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_) {                                                               \
         printf("FAIL: %s = %d (line %d)\n", #x, r_, __LINE__);               \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

#define W        3456
#define H        2234
#define MAX_RT   3
#define MAX_MS   4
#define ALL_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)

enum shape {
   SHAPE_RT0,          /* writes RT0 */
   SHAPE_TRIANGLE,     /* writes RT0, full-screen triangle */
   SHAPE_RESOLVE,      /* writes RT0, resolves it */
   SHAPE_RESOLVE_NOSTORE, /* the same with RT0 storeOp DONT_CARE */
   SHAPE_TRIANGLE_RESOLVE_NOSTORE, /* the same, full-screen triangle */
   SHAPE_RESOLVE_ALL,  /* writes RT0, resolves every target */
   SHAPE_NONE,         /* writes nothing, under an occlusion query */
   SHAPE_TWO_DRAWS,    /* writes RT0, then RT1 */
   SHAPE_COUNT,
};
static const char *shape_names[] = {
   "rt0", "triangle", "resolve", "resolve-nostore",
   "triangle-resolve-nostore", "resolve-all", "none", "two-draws",
};

struct img {
   VkImage image;
   VkImageView view;
};

static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties memprops;
static VkPipelineLayout push_layout, sampled_layout;
static VkDescriptorSetLayout set_layout;
static VkDescriptorPool set_pool;
static VkSampler sampler;
static VkPipeline fetch_pipe, fetch1_pipe;
static struct img readback;
static VkBuffer buffer;
static void *buffer_map;
static int samples = 2;

static const struct {
   const char *name;
   VkFormat format;
} formats[] = {
   {"r11f", VK_FORMAT_B10G11R11_UFLOAT_PACK32},
   {"rgb10", VK_FORMAT_A2R10G10B10_UNORM_PACK32},
   {"rgba8", VK_FORMAT_R8G8B8A8_UNORM},
   {"bgra8", VK_FORMAT_B8G8R8A8_UNORM},
   {"rgba16f", VK_FORMAT_R16G16B16A16_SFLOAT},
   {"rgba32f", VK_FORMAT_R32G32B32A32_SFLOAT},
   {"rg16f", VK_FORMAT_R16G16_SFLOAT},
   {"r32f", VK_FORMAT_R32_SFLOAT},
   {"r8", VK_FORMAT_R8_UNORM},
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
make_image(VkFormat format, int image_samples, VkImageUsageFlags usage)
{
   struct img i;
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {W, H, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = image_samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
   };
   CHECK(vkCreateImage(dev, &ci, NULL, &i.image));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, i.image, &req);
   CHECK(vkBindImageMemory(
      dev, i.image, memory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
   VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = i.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   CHECK(vkCreateImageView(dev, &vi, NULL, &i.view));
   return i;
}

static VkShaderModule
module(const uint32_t *code, size_t size)
{
   VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  NULL, 0, size, code};
   VkShaderModule m;
   CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
   return m;
}
#define MODULE(name) module(name, sizeof(name))

static VkPipeline
pipeline(VkShaderModule vs, VkShaderModule fs, unsigned rts,
         const VkFormat *rt_formats, int pipe_samples, bool sample_shading,
         VkPipelineLayout layout)
{
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo vertex = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport viewport = {0, 0, W, H, 0, 1};
   VkRect2D scissor = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &viewport,
      .scissorCount = 1, .pScissors = &scissor};
   VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = pipe_samples,
      .sampleShadingEnable = sample_shading, .minSampleShading = 1};
   VkPipelineDepthStencilStateCreateInfo depth = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
   VkPipelineColorBlendAttachmentState blend[MAX_RT] = {
      {.colorWriteMask = 0xf}, {.colorWriteMask = 0xf}, {.colorWriteMask = 0xf}};
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = rts, .pAttachments = blend};
   VkPipelineRenderingCreateInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = rts, .pColorAttachmentFormats = rt_formats};
   VkGraphicsPipelineCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering, .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
      .pViewportState = &vp, .pRasterizationState = &raster,
      .pMultisampleState = &ms, .pDepthStencilState = &depth,
      .pColorBlendState = &cb, .layout = layout};
   VkPipeline p;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
   return p;
}

static VkCommandBuffer
begin(void)
{
   VkCommandBufferAllocateInfo ai = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   return cmd;
}

static void
barrier(VkCommandBuffer cmd)
{
   VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
                        VK_ACCESS_MEMORY_WRITE_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                        0, NULL);
}

static void
submit(VkCommandBuffer cmd)
{
   barrier(cmd);
   CHECK(vkEndCommandBuffer(cmd));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1, .pCommandBuffers = &cmd};
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));
   vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

static void
to_general(VkCommandBuffer cmd, VkImage image)
{
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                        1, &b);
}

static void
draw_pipe(VkCommandBuffer cmd, VkPipeline pipe, float frame, unsigned vertices)
{
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdPushConstants(cmd, push_layout, ALL_STAGES, 0, sizeof(frame), &frame);
   vkCmdDraw(cmd, vertices, 1, 0, 0);
}

/* The exact bits of an image, through a 1x RGBA32F target. */
static uint64_t
hash_image(VkImageView view, int sample)
{
   VkDescriptorSetAllocateInfo ai = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, set_pool, 1,
      &set_layout};
   VkDescriptorSet set;
   CHECK(vkResetDescriptorPool(dev, set_pool, 0));
   CHECK(vkAllocateDescriptorSets(dev, &ai, &set));
   VkDescriptorImageInfo info = {sampler, view, VK_IMAGE_LAYOUT_GENERAL};
   VkWriteDescriptorSet w = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &info};
   vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

   VkCommandBuffer cmd = begin();
   VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = readback.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                         .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                         .colorAttachmentCount = 1, .pColorAttachments = &att};
   vkCmdBeginRendering(cmd, &ri);
   const float fetched = sample;
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     sample < 0 ? fetch1_pipe : fetch_pipe);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sampled_layout,
                           0, 1, &set, 0, NULL);
   vkCmdPushConstants(cmd, sampled_layout, ALL_STAGES, 0, sizeof(fetched),
                      &fetched);
   vkCmdDraw(cmd, 6, 1, 0, 0);
   vkCmdEndRendering(cmd);
   barrier(cmd);
   VkBufferImageCopy region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {W, H, 1}};
   vkCmdCopyImageToBuffer(cmd, readback.image, VK_IMAGE_LAYOUT_GENERAL, buffer,
                          1, &region);
   submit(cmd);

   uint64_t h = 0xcbf29ce484222325ull;
   const uint32_t *d = buffer_map;
   for (size_t i = 0; i < (size_t)W * H * 4; i++) {
      h ^= d[i];
      h *= 0x100000001b3ull;
   }
   return h;
}

int
main(int argc, char **argv)
{
   char fmts_arg[64] = "rgba8,rgb10";
   for (int i = 1; i < argc; i++) {
      if (!strncmp(argv[i], "ms=", 3))
         samples = atoi(argv[i] + 3);
      else if (!strncmp(argv[i], "fmts=", 5))
         snprintf(fmts_arg, sizeof(fmts_arg), "%s", argv[i] + 5);
   }
   VkFormat rt_formats[MAX_RT];
   unsigned rts = 0;
   for (char *t = strtok(fmts_arg, ","); t && rts < MAX_RT;
        t = strtok(NULL, ",")) {
      for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
         if (!strcmp(t, formats[i].name))
            rt_formats[rts++] = formats[i].format;
   }
   if (!rts || samples > MAX_MS) {
      printf("FAIL: bad arguments\n");
      return 1;
   }

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &app};
   VkInstance instance;
   CHECK(vkCreateInstance(&ii, NULL, &instance));
   uint32_t count = 1;
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(instance, &count, &pd);
   vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
   const float priority = 1;
   VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 NULL, 0, 0, 1, &priority};
   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = 1};
   VkPhysicalDeviceFeatures2 f2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13,
      .features = {.sampleRateShading = 1, .occlusionQueryPrecise = 1}};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                            .pNext = &f2, .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qi};
   CHECK(vkCreateDevice(pd, &di, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &queue);
   VkCommandPoolCreateInfo pi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   CHECK(vkCreateCommandPool(dev, &pi, NULL, &pool));

   VkPushConstantRange range = {ALL_STAGES, 0, 4};
   VkPipelineLayoutCreateInfo li = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
   CHECK(vkCreatePipelineLayout(dev, &li, NULL, &push_layout));
   VkDescriptorSetLayoutBinding binding = {
      0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
      VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo dl = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &binding};
   CHECK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &set_layout));
   li.setLayoutCount = 1;
   li.pSetLayouts = &set_layout;
   CHECK(vkCreatePipelineLayout(dev, &li, NULL, &sampled_layout));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dp = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
      .poolSizeCount = 1, .pPoolSizes = &ps};
   CHECK(vkCreateDescriptorPool(dev, &dp, NULL, &set_pool));
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
   CHECK(vkCreateSampler(dev, &si, NULL, &sampler));

   /* The attachments carry the game's usages, plus sampled for the read-back. */
   const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT;
   /* reference[i]: target i written through a one-target pass. */
   const unsigned references = rts < 2 ? rts : 2;
   struct img rt[MAX_RT], resolved[MAX_RT], reference[2];
   for (unsigned i = 0; i < rts; i++) {
      rt[i] = make_image(rt_formats[i], samples, usage);
      resolved[i] = make_image(rt_formats[i], 1, usage);
   }
   for (unsigned i = 0; i < references; i++)
      reference[i] = make_image(rt_formats[i], samples, usage);
   const VkFormat readback_format = VK_FORMAT_R32G32B32A32_SFLOAT;
   readback = make_image(readback_format, 1,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = (VkDeviceSize)W * H * 16,
                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   CHECK(vkCreateBuffer(dev, &bi, NULL, &buffer));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, buffer, &req);
   VkDeviceMemory buffer_memory =
      memory(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
   CHECK(vkBindBufferMemory(dev, buffer, buffer_memory, 0));
   CHECK(vkMapMemory(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, &buffer_map));

   VkShaderModule quad = MODULE(visit_quad_vert);
   VkShaderModule tri = MODULE(visit_tri_vert);
   VkPipeline scene = pipeline(quad, MODULE(visit_scene_frag), rts, rt_formats,
                               samples, true, push_layout);
   VkPipeline post = pipeline(quad, MODULE(visit_post_frag), rts, rt_formats,
                              samples, false, push_layout);
   VkPipeline post1 = pipeline(quad, MODULE(visit_post1_frag), rts, rt_formats,
                               samples, false, push_layout);
   VkPipeline post_tri = pipeline(tri, MODULE(visit_post_frag), rts, rt_formats,
                                  samples, false, push_layout);
   VkPipeline post_one[2];
   for (unsigned i = 0; i < references; i++)
      post_one[i] = pipeline(quad, MODULE(visit_post_frag), 1, &rt_formats[i],
                             samples, false, push_layout);
   VkPipeline hist = pipeline(quad, MODULE(visit_hist_frag), rts, rt_formats,
                              samples, false, push_layout);
   fetch_pipe = pipeline(quad, MODULE(visit_fetch_frag), 1, &readback_format,
                         1, false, sampled_layout);
   fetch1_pipe = pipeline(quad, MODULE(visit_fetch1_frag), 1, &readback_format,
                          1, false, sampled_layout);
   VkQueryPoolCreateInfo qp = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                               .queryType = VK_QUERY_TYPE_OCCLUSION,
                               .queryCount = 1};
   VkQueryPool query_pool;
   CHECK(vkCreateQueryPool(dev, &qp, NULL, &query_pool));

   VkCommandBuffer cmd = begin();
   for (unsigned i = 0; i < rts; i++) {
      to_general(cmd, rt[i].image);
      to_general(cmd, resolved[i].image);
   }
   for (unsigned i = 0; i < references; i++)
      to_general(cmd, reference[i].image);
   to_general(cmd, readback.image);
   submit(cmd);

   unsigned fails = 0;
   for (unsigned s = 0; s < SHAPE_COUNT; s++) {
      if (s == SHAPE_TWO_DRAWS && rts < 2)
         continue;
      const float frame = s + 1;

      /* A fresh scene, and what each target and its resolve must then be. */
      VkRenderingAttachmentInfo at[MAX_RT];
      for (unsigned i = 0; i < rts; i++)
         at[i] = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = rt[i].view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
      VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                            .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                            .colorAttachmentCount = rts,
                            .pColorAttachments = at};
      cmd = begin();
      vkCmdBeginRendering(cmd, &ri);
      draw_pipe(cmd, scene, 0, 6);
      vkCmdEndRendering(cmd);
      for (unsigned i = 0; i < references; i++) {
         barrier(cmd);
         VkRenderingAttachmentInfo ref = at[i];
         ref.imageView = reference[i].view;
         ref.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
         VkRenderingInfo ref_info = ri;
         ref_info.colorAttachmentCount = 1;
         ref_info.pColorAttachments = &ref;
         vkCmdBeginRendering(cmd, &ref_info);
         draw_pipe(cmd, post_one[i], frame, 6);
         vkCmdEndRendering(cmd);
      }
      submit(cmd);

      uint64_t before[MAX_RT][MAX_MS], resolve_before[MAX_RT] = {0};
      for (unsigned i = 0; i < rts; i++) {
         for (int m = 0; m < samples; m++)
            before[i][m] = hash_image(rt[i].view, m);
         cmd = begin();
         VkImageResolve region = {
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
         vkCmdResolveImage(cmd, rt[i].image, VK_IMAGE_LAYOUT_GENERAL,
                           resolved[i].image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &region);
         submit(cmd);
         resolve_before[i] = hash_image(resolved[i].view, -1);
      }

      /* The visit. */
      const bool no_store = s == SHAPE_RESOLVE_NOSTORE ||
                            s == SHAPE_TRIANGLE_RESOLVE_NOSTORE;
      const bool resolve0 = s == SHAPE_RESOLVE || no_store ||
                            s == SHAPE_RESOLVE_ALL;
      for (unsigned i = 0; i < rts; i++) {
         at[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
         if (resolve0 && (i == 0 || s == SHAPE_RESOLVE_ALL)) {
            at[i].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            at[i].resolveImageView = resolved[i].view;
            at[i].resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
         }
      }
      if (no_store)
         at[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

      cmd = begin();
      if (s == SHAPE_NONE)
         vkCmdResetQueryPool(cmd, query_pool, 0, 1);
      vkCmdBeginRendering(cmd, &ri);
      switch (s) {
      case SHAPE_TRIANGLE:
      case SHAPE_TRIANGLE_RESOLVE_NOSTORE:
         draw_pipe(cmd, post_tri, frame, 3);
         break;
      case SHAPE_NONE:
         vkCmdBeginQuery(cmd, query_pool, 0, VK_QUERY_CONTROL_PRECISE_BIT);
         draw_pipe(cmd, hist, frame, 6);
         draw_pipe(cmd, hist, frame, 6);
         vkCmdEndQuery(cmd, query_pool, 0);
         break;
      case SHAPE_TWO_DRAWS:
         draw_pipe(cmd, post, frame, 6);
         draw_pipe(cmd, post1, frame, 6);
         break;
      default:
         draw_pipe(cmd, post, frame, 6);
         break;
      }
      vkCmdEndRendering(cmd);
      submit(cmd);

      /* Check. */
      bool ok = true;
      uint64_t expected[2] = {0};
      for (unsigned i = 0; i < references; i++)
         expected[i] = hash_image(reference[i].view, 0);
      for (unsigned i = 0; i < rts; i++) {
         /* A STORE-less RT0 has nothing to compare. */
         if (i == 0 && no_store)
            continue;
         for (int m = 0; m < samples; m++) {
            const bool written = (i == 0 && s != SHAPE_NONE) ||
                                 (i == 1 && s == SHAPE_TWO_DRAWS);
            if (hash_image(rt[i].view, m) !=
                (written ? expected[i] : before[i][m])) {
               printf("  %s: rt%u sample %d changed\n", shape_names[s], i, m);
               ok = false;
            }
         }
      }
      if (resolve0 && hash_image(resolved[0].view, -1) != expected[0]) {
         printf("  %s: resolved rt0 wrong\n", shape_names[s]);
         ok = false;
      }
      if (s == SHAPE_RESOLVE_ALL) {
         for (unsigned i = 1; i < rts; i++) {
            if (hash_image(resolved[i].view, -1) != resolve_before[i]) {
               printf("  %s: resolved rt%u wrong\n", shape_names[s], i);
               ok = false;
            }
         }
      }
      if (s == SHAPE_NONE) {
         /* 2430 of 3456 columns survive the discard, in both draws. */
         uint64_t passed = 0;
         CHECK(vkGetQueryPoolResults(dev, query_pool, 0, 1, sizeof(passed),
                                     &passed, 8,
                                     VK_QUERY_RESULT_64_BIT |
                                        VK_QUERY_RESULT_WAIT_BIT));
         if (passed != (uint64_t)2430 * H * samples * 2) {
            printf("  none: query counted %llu\n",
                   (unsigned long long)passed);
            ok = false;
         }
      }
      printf("%s %s\n", ok ? "PASS" : "FAIL", shape_names[s]);
      fails += !ok;
   }
   printf("%s\n", fails ? "FAIL" : "PASS");
   return fails != 0;
}
