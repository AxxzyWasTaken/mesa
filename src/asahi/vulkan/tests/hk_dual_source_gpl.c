/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Offline recording test for the pipeline state in VK-GL-CTS 1.4.6.2's
 * vktPipelineDualBlendTests.cpp (Apache-2.0, Khronos Group).
 * Requires the no-op DRM shim. Never submits commands to a queue.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "hk_cmd_buffer.h"
#include "hk_shader.h"
#include "vk_blend.h"

#define REQUIRE(x)                                                             \
   do {                                                                        \
      if (!(x)) {                                                              \
         fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);               \
         exit(1);                                                              \
      }                                                                        \
   } while (0)
#define VK_CHECK(x)                                                            \
   do {                                                                        \
      VkResult r_ = (x);                                                       \
      if (r_ != VK_SUCCESS) {                                                  \
         fprintf(stderr, "%s: %d\n", #x, r_);                                  \
         exit(1);                                                              \
      }                                                                        \
   } while (0)

static VkDevice dev;
static VkPipelineLayout layout;
static VkShaderModule vs, fs[2];
static VkRenderPass rp[2];
static VkFramebuffer fb[2];

static VkShaderModule
load_module(const char *dir, const char *name)
{
   char path[4096];
   snprintf(path, sizeof(path), "%s/%s.spv", dir, name);
   FILE *f = fopen(path, "rb");
   REQUIRE(f && fseek(f, 0, SEEK_END) == 0);
   long size = ftell(f);
   REQUIRE(size > 0 && size % 4 == 0);
   rewind(f);
   uint32_t *code = malloc(size);
   REQUIRE(code && fread(code, 1, size, f) == (size_t)size);
   fclose(f);
   VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = code,
   };
   VkShaderModule module;
   VK_CHECK(vkCreateShaderModule(dev, &ci, NULL, &module));
   free(code);
   return module;
}

static VkBlendFactor
generic_factor(VkBlendFactor f)
{
   switch (f) {
   case VK_BLEND_FACTOR_SRC1_COLOR:
      return VK_BLEND_FACTOR_SRC_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
   case VK_BLEND_FACTOR_SRC1_ALPHA:
      return VK_BLEND_FACTOR_SRC_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
   default:
      return f;
   }
}

/* mode 0 is monolithic, 1 is GPL fast linking, 2 is GPL with LTO. */
static VkPipeline
make_pipeline(unsigned mode, bool dual,
              VkPipelineColorBlendAttachmentState state)
{
   VkPipelineShaderStageCreateInfo stages[] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vs,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fs[dual],
       .pName = "main"},
   };
   const VkVertexInputBindingDescription binding = {
      0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
   const VkVertexInputAttributeDescription attr = {
      0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &attr,
   };
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport viewport = {0, 0, 4, 4, 0, 1};
   VkRect2D scissor = {{0, 0}, {4, 4}};
   VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .lineWidth = 1,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
   };
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      .minSampleShading = 1,
   };
   VkPipelineDepthStencilStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
      .minDepthBounds = 0,
      .maxDepthBounds = 1,
   };
   VkPipelineColorBlendAttachmentState attachments[4];
   for (unsigned i = 0; i < 4; ++i) {
      attachments[i] = state;
      if (!dual) {
         attachments[i].srcColorBlendFactor =
            generic_factor(state.srcColorBlendFactor);
         attachments[i].dstColorBlendFactor =
            generic_factor(state.dstColorBlendFactor);
         attachments[i].srcAlphaBlendFactor =
            generic_factor(state.srcAlphaBlendFactor);
         attachments[i].dstAlphaBlendFactor =
            generic_factor(state.dstAlphaBlendFactor);
      }
   }
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOp = VK_LOGIC_OP_COPY,
      .attachmentCount = dual ? 1 : 4,
      .pAttachments = attachments,
   };
   float constant = 0.333f;
   for (unsigned i = 0; i < 4; ++i, constant += 0.111f)
      cb.blendConstants[i] = constant;
   VkGraphicsPipelineCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pDepthStencilState = &ds,
      .pColorBlendState = &cb,
      .layout = layout,
      .renderPass = rp[dual],
   };
   VkPipeline pipeline;
   if (mode == 0) {
      VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL,
                                         &pipeline));
      return pipeline;
   }

   VkPipeline libs[4];
   for (unsigned i = 0; i < 4; ++i) {
      VkGraphicsPipelineLibraryCreateInfoEXT subset = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
         .flags = 1u << i,
      };
      VkGraphicsPipelineCreateInfo part = ci;
      part.pNext = &subset;
      part.flags =
         VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
         (mode == 2
             ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT
             : 0);
      part.stageCount = (i == 1 || i == 2) ? 1 : 0;
      part.pStages = i == 1 ? &stages[0] : i == 2 ? &stages[1] : NULL;
      VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &part, NULL,
                                         &libs[i]));
   }
   VkPipelineLibraryCreateInfoKHR link = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
      .libraryCount = 4,
      .pLibraries = libs,
   };
   VkGraphicsPipelineCreateInfo final = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &link,
      .layout = layout,
      .flags =
         mode == 2 ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0,
   };
   VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &final, NULL,
                                      &pipeline));
   for (unsigned i = 0; i < 4; ++i)
      vkDestroyPipeline(dev, libs[i], NULL);
   return pipeline;
}

struct capture {
   struct hk_fast_link_key_fs key;
   struct agx_fragment_control_packed control;
   struct agx_usc_registers_packed regs;
   size_t size;
   void *code;
};

static struct capture
capture_fs(VkCommandBuffer cmd, bool dual,
           VkPipelineColorBlendAttachmentState state)
{
   struct hk_cmd_buffer *c = hk_cmd_buffer_from_handle(cmd);
   struct hk_shader *shader =
      hk_only_variant(c->state.gfx.shaders[MESA_SHADER_FRAGMENT]);
   struct hk_linked_shader *linked = c->state.gfx.linked[MESA_SHADER_FRAGMENT];
   REQUIRE(c->current_cs.gfx && c->state.gfx.fast_link_fs.valid && linked);
   REQUIRE(shader->info.fs.epilog_key.loc_written == (dual ? 3 : 15));
   REQUIRE(c->state.gfx.render.color_att_count == (dual ? 1 : 4));
   REQUIRE(c->vk.dynamic_graphics_state.cb.attachment_count == (dual ? 1 : 4));
   struct capture out = {
      .key = c->state.gfx.fast_link_fs.key,
      .control = linked->b.fragment_control,
      .regs = linked->b.regs,
      .size = linked->b.size,
   };
   out.code = malloc(out.size);
   REQUIRE(out.code && linked->b.bo->_map);
   memcpy(out.code, (uint8_t *)linked->b.bo->_map + linked->b.entry_offset,
          out.size);
   for (unsigned i = dual ? 1 : 4; i < 8; ++i)
      REQUIRE(out.key.epilog.rt_formats[i] == PIPE_FORMAT_NONE);
   if (!dual)
      REQUIRE(out.key.epilog.rt_formats[0] == PIPE_FORMAT_NONE);
   for (unsigned i = 0; i < (dual ? 1 : 4); ++i) {
      const struct agx_blend_rt_key rt = out.key.epilog.blend.rt[i];
      const struct agx_blend_standard blend =
         agx_unpack_blend_standard(rt.mode);
      REQUIRE(!rt.advanced_blend && rt.colormask == state.colorWriteMask);
      REQUIRE(blend.rgb_func == vk_blend_op_to_pipe(state.colorBlendOp));
      REQUIRE(blend.alpha_func == vk_blend_op_to_pipe(state.alphaBlendOp));
      REQUIRE(blend.rgb_src_factor ==
              vk_blend_factor_to_pipe(
                 dual ? state.srcColorBlendFactor
                      : generic_factor(state.srcColorBlendFactor)));
      REQUIRE(blend.rgb_dst_factor ==
              vk_blend_factor_to_pipe(
                 dual ? state.dstColorBlendFactor
                      : generic_factor(state.dstColorBlendFactor)));
      REQUIRE(blend.alpha_src_factor ==
              vk_blend_factor_to_pipe(
                 dual ? state.srcAlphaBlendFactor
                      : generic_factor(state.srcAlphaBlendFactor)));
      REQUIRE(blend.alpha_dst_factor ==
              vk_blend_factor_to_pipe(
                 dual ? state.dstAlphaBlendFactor
                      : generic_factor(state.dstAlphaBlendFactor)));
   }
   return out;
}

static void
same_capture(const struct capture *a, const struct capture *b)
{
   REQUIRE(!memcmp(&a->key, &b->key, sizeof(a->key)));
   REQUIRE(!memcmp(&a->control, &b->control, sizeof(a->control)));
   REQUIRE(!memcmp(&a->regs, &b->regs, sizeof(a->regs)));
   REQUIRE(a->size == b->size && !memcmp(a->code, b->code, a->size));
}

int
main(int argc, char **argv)
{
   setbuf(stdout, NULL);
   REQUIRE(argc == 2 && getenv("ASAHI_SHIM_CHIP") && getenv("LD_PRELOAD"));
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&ici, NULL, &instance));
   uint32_t n = 1;
   VkPhysicalDevice physical;
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
   REQUIRE(n == 1);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(physical, &props);
   printf("device=%s limit=%u\n", props.deviceName,
          props.limits.maxFragmentDualSrcAttachments);
   REQUIRE(props.limits.maxFragmentDualSrcAttachments == 1);
   REQUIRE(props.limits.maxFragmentOutputAttachments >= 4);
   float priority = 1;
   VkDeviceQueueCreateInfo qi = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &priority};
   VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = {
      .sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
      .graphicsPipelineLibrary = VK_TRUE,
   };
   VkPhysicalDeviceFeatures features = {.dualSrcBlend = VK_TRUE,
                                        .independentBlend = VK_TRUE};
   const char *extensions[] = {"VK_KHR_pipeline_library",
                               "VK_EXT_graphics_pipeline_library"};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                            .pNext = &gpl,
                            .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qi,
                            .pEnabledFeatures = &features,
                            .enabledExtensionCount = 2,
                            .ppEnabledExtensionNames = extensions};
   VK_CHECK(vkCreateDevice(physical, &di, NULL, &dev));
   VkPushConstantRange pc = {
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
   VkPipelineLayoutCreateInfo li = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pc};
   VK_CHECK(vkCreatePipelineLayout(dev, &li, NULL, &layout));
   vs = load_module(argv[1], "common.vert");
   fs[0] = load_module(argv[1], "generic.frag");
   fs[1] = load_module(argv[1], "dual.frag");

   VkCommandPoolCreateInfo pi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool pool;
   VK_CHECK(vkCreateCommandPool(dev, &pi, NULL, &pool));
   VkCommandBufferAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
   VkCommandBuffer cmd;
   VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));

   VkBufferCreateInfo vbci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = 6 * 16,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   VkBuffer vertex;
   VK_CHECK(vkCreateBuffer(dev, &vbci, NULL, &vertex));
   VkMemoryRequirements vbmr;
   vkGetBufferMemoryRequirements(dev, vertex, &vbmr);
   VkMemoryAllocateInfo vbmi = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = vbmr.size,
      .memoryTypeIndex = __builtin_ctz(vbmr.memoryTypeBits)};
   VkDeviceMemory vertex_mem;
   VK_CHECK(vkAllocateMemory(dev, &vbmi, NULL, &vertex_mem));
   VK_CHECK(vkBindBufferMemory(dev, vertex, vertex_mem, 0));
   void *mapped;
   VK_CHECK(vkMapMemory(dev, vertex_mem, 0, VK_WHOLE_SIZE, 0, &mapped));
   const float vertices[6][4] = {
      {-1, -1, 0, 0}, {-1, 1, 0, 0}, {1, 1, 0, 0},
      {1, 1, 0, 0},   {1, -1, 0, 0}, {-1, -1, 0, 0},
   };
   memcpy(mapped, vertices, sizeof(vertices));
   VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                .memory = vertex_mem,
                                .size = VK_WHOLE_SIZE};
   VK_CHECK(vkFlushMappedMemoryRanges(dev, 1, &range));
   vkUnmapMemory(dev, vertex_mem);

   /* The 41 formats in the reported list; unsupported blend formats skip. */
   static const struct {
      VkFormat format;
      unsigned mask;
   } formats[] = {
#include "hk_dual_source_formats.h"
   };
   unsigned passed = 0, skipped = 0;
   for (unsigned fmt = 0; fmt < ARRAY_SIZE(formats); ++fmt) {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(physical, formats[fmt].format, &fp);
      VkFormatFeatureFlags required =
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if ((fp.optimalTilingFeatures & required) != required) {
         printf("SKIP format=%u: missing CTS blend/transfer format support\n",
                formats[fmt].format);
         skipped++;
         continue;
      }
      VkImage images[4];
      VkDeviceMemory memory[4];
      VkImageView views[4];
      VkAttachmentDescription desc[4];
      VkAttachmentReference refs[4];
      for (unsigned i = 0; i < 4; ++i) {
         VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = formats[fmt].format,
                                 .extent = {4, 4, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
         VK_CHECK(vkCreateImage(dev, &ii, NULL, &images[i]));
         VkMemoryRequirements mr;
         vkGetImageMemoryRequirements(dev, images[i], &mr);
         VkMemoryAllocateInfo mi = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = __builtin_ctz(mr.memoryTypeBits)};
         VK_CHECK(vkAllocateMemory(dev, &mi, NULL, &memory[i]));
         VK_CHECK(vkBindImageMemory(dev, images[i], memory[i], 0));
         VkImageViewCreateInfo iv = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = formats[fmt].format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
         VK_CHECK(vkCreateImageView(dev, &iv, NULL, &views[i]));
         desc[i] = (VkAttachmentDescription){
            .format = formats[fmt].format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
         refs[i] = (VkAttachmentReference){
            i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      }
      for (unsigned dual = 0; dual < 2; ++dual) {
         refs[0].attachment = dual ? 0 : VK_ATTACHMENT_UNUSED;
         VkSubpassDescription subpass = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = dual ? 1 : 4,
            .pColorAttachments = refs};
         VkRenderPassCreateInfo ri = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 4,
            .pAttachments = desc,
            .subpassCount = 1,
            .pSubpasses = &subpass};
         VK_CHECK(vkCreateRenderPass(dev, &ri, NULL, &rp[dual]));
         VkFramebufferCreateInfo fi = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rp[dual],
            .attachmentCount = 4,
            .pAttachments = views,
            .width = 4,
            .height = 4,
            .layers = 1};
         VK_CHECK(vkCreateFramebuffer(dev, &fi, NULL, &fb[dual]));
      }

      const VkBlendFactor secondary[] = {
         VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
         VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA};
      bool alpha = formats[fmt].mask & 8;
      for (unsigned factor = 0; factor < (alpha ? 4 : 2); ++factor) {
         VkPipelineColorBlendAttachmentState state = {
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR,
            .dstColorBlendFactor = secondary[factor],
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor =
               alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ZERO,
            .dstAlphaBlendFactor = !alpha ? VK_BLEND_FACTOR_ZERO
                                   : factor & 1
                                      ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA
                                      : VK_BLEND_FACTOR_SRC1_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = formats[fmt].mask};
         struct capture baseline[2];
         for (unsigned mode = 0; mode < 3; ++mode) {
            VkPipeline pipelines[2] = {make_pipeline(mode, false, state),
                                       make_pipeline(mode, true, state)};
            VK_CHECK(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo bi = {
               .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            VkImageLayout image_layout[4] = {VK_IMAGE_LAYOUT_UNDEFINED};
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertex, &offset);
            for (unsigned pass = 0; pass < 4; ++pass) {
               bool dual = pass & 1;
               VkImageMemoryBarrier barriers[4];
               for (unsigned i = 0; i < 4; ++i) {
                  barriers[i] = (VkImageMemoryBarrier){
                     .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                     .srcAccessMask =
                        pass ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0,
                     .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     .oldLayout = image_layout[i],
                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                     .image = images[i],
                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                          1},
                  };
                  image_layout[i] = VK_IMAGE_LAYOUT_GENERAL;
               }
               vkCmdPipelineBarrier(
                  cmd,
                  pass ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0,
                  NULL, 4, barriers);
               VkRenderPassBeginInfo begin = {
                  .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                  .renderPass = rp[dual],
                  .framebuffer = fb[dual],
                  .renderArea = {{0, 0}, {4, 4}}};
               vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
               vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 pipelines[dual]);
               float constants[16] = {
                  0.1f, 0, 0.5f, 0.25f, 0.6f, 0.5f, 0,    0.75f,
                  0.2f, 0, 0,    0.25f, 0.8f, 0,    0.5f, 0.75f,
               };
               if (dual)
                  for (unsigned i = 0; i < 4; ++i)
                     memcpy(&constants[4 * i], (float[]){0.2f, 0, 0, 0.25f},
                            4 * sizeof(float));
               vkCmdPushConstants(
                  cmd, layout,
                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                  sizeof(constants), constants);
               vkCmdDraw(cmd, 6, 1, 0, 0);
               struct capture cap = capture_fs(cmd, dual, state);
               if (mode == 0 && pass < 2)
                  baseline[dual] = cap;
               else {
                  same_capture(&baseline[dual], &cap);
                  free(cap.code);
               }
               vkCmdEndRenderPass(cmd);
               for (unsigned i = 0; i < 4; ++i)
                  if (dual ? i == 0 : i > 0)
                     image_layout[i] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            VK_CHECK(vkEndCommandBuffer(cmd));
            /* Discard executable commands before destroying their pipelines. */
            VK_CHECK(vkResetCommandBuffer(cmd, 0));
            vkDestroyPipeline(dev, pipelines[0], NULL);
            vkDestroyPipeline(dev, pipelines[1], NULL);
         }
         free(baseline[0].code);
         free(baseline[1].code);
      }
      for (unsigned i = 0; i < 2; ++i) {
         vkDestroyFramebuffer(dev, fb[i], NULL);
         vkDestroyRenderPass(dev, rp[i], NULL);
      }
      for (unsigned i = 0; i < 4; ++i) {
         vkDestroyImageView(dev, views[i], NULL);
         vkDestroyImage(dev, images[i], NULL);
         vkFreeMemory(dev, memory[i], NULL);
      }
      printf(
         "PASS format=%u: %u factors, monolithic/GPL/LTO, generic/dual/generic/dual\n",
         formats[fmt].format, alpha ? 4 : 2);
      passed++;
   }
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyBuffer(dev, vertex, NULL);
   vkFreeMemory(dev, vertex_mem, NULL);
   vkDestroyShaderModule(dev, vs, NULL);
   vkDestroyShaderModule(dev, fs[0], NULL);
   vkDestroyShaderModule(dev, fs[1], NULL);
   vkDestroyPipelineLayout(dev, layout, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(instance, NULL);
   printf("PASS %u formats; SKIP %u formats; no queue submission\n", passed,
          skipped);
   return 0;
}
