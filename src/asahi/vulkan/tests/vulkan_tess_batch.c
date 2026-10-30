/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Many small tessellated draws in one render pass, as terrain renderers
 * record them. Software tessellation runs five compute stages per draw before
 * the render (hk_launch_tess()); each draw has its own rectangle, colour and
 * tessellation level in push constants, the draws alternate between a
 * triangle and a quad domain pipeline, and their rectangles overlap so that
 * the image depends on the draws being rasterised in API order.
 *
 * The first pass also mixes in indexed and indirect draws. The image of each
 * pass must match the rectangles painted in order on the CPU, exactly.
 *
 * It then times passes of 1024 direct and indexed draws (submit to fence
 * signal, GPU-bound) and prints the time per pass and per draw.
 *
 * Usage: vulkan-tess-batch [passes]   Exit status 0 = correct output.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define CHECK(call)                                                            \
  do {                                                                         \
    VkResult r = (call);                                                       \
    if (r != VK_SUCCESS) {                                                     \
      fprintf(stderr, "%s: %d\n", #call, r);                                   \
      exit(2);                                                                 \
    }                                                                          \
  } while (0)

#define W 1024
#define CELL 32
#define DRAWS ((W / CELL) * (W / CELL))
#define RUNS 5

struct pc {
  int32_t rect[4];
  uint32_t rgba;
  float level;
  uint32_t quad;
};

static VkDevice dev;
static VkPhysicalDevice pdev;
static VkQueue q;
static VkCommandBuffer cb;
static VkFence fence;
static VkImage img;
static VkImageView view;
static VkBuffer buf, ib, indirect;
static VkPipelineLayout pl;
static VkPipeline pipes[2];

#include "vulkan_tess_batch_shaders.h"

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties m;
  vkGetPhysicalDeviceMemoryProperties(pdev, &m);
  for (uint32_t i = 0; i < m.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  fprintf(stderr, "no memory type\n");
  exit(2);
}

static void *host_buffer(VkBuffer *b, VkDeviceSize size,
                         VkBufferUsageFlags usage) {
  CHECK(vkCreateBuffer(dev,
                       &(VkBufferCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                          .size = size, .usage = usage},
                       NULL, b));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, *b, &mr);
  VkDeviceMemory mem;
  CHECK(vkAllocateMemory(
     dev,
     &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex =
           memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)},
     NULL, &mem));
  CHECK(vkBindBufferMemory(dev, *b, mem, 0));
  void *map;
  CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map));
  return map;
}

static VkShaderModule module(const uint32_t *code, size_t size) {
  VkShaderModule m;
  CHECK(vkCreateShaderModule(
     dev,
     &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size, .pCode = code},
     NULL, &m));
  return m;
}

/* Draw i of a pass: a cell of the grid, grown into its right and lower
 * neighbours, which later draws partly cover again. */
static struct pc draw_params(unsigned i, unsigned pass) {
  int32_t x = (i % (W / CELL)) * CELL, y = (i / (W / CELL)) * CELL;
  uint32_t h = (i + 1) * 2654435761u ^ (pass * 0x9e3779b9u);
  return (struct pc){
     .rect = {x, y, x + CELL + CELL / 4, y + CELL + CELL / 4},
     .rgba = 0xff000000u | (h & 0xffffff),
     .level = 1 + i % 7,
     .quad = (i / 5) & 1,
  };
}

/* One pass. mixed: direct, indexed and indirect draws; otherwise direct and
 * indexed. */
static void record_pass(unsigned pass, bool mixed) {
  vkCmdBeginRendering(
     cb, &(VkRenderingInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, {W, W}}, .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &(VkRenderingAttachmentInfo){
               .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
               .imageView = view,
               .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
               .storeOp = VK_ATTACHMENT_STORE_OP_STORE}});
  vkCmdSetViewport(cb, 0, 1, &(VkViewport){0, 0, W, W, 0, 1});
  vkCmdSetScissor(cb, 0, 1, &(VkRect2D){{0, 0}, {W, W}});
  vkCmdBindIndexBuffer(cb, ib, 0, VK_INDEX_TYPE_UINT16);
  int bound = -1;
  for (unsigned i = 0; i < DRAWS; i++) {
    struct pc p = draw_params(i, pass);
    if (bound != (int)p.quad) {
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[p.quad]);
      bound = p.quad;
    }
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(p), &p);
    const uint32_t n = p.quad ? 4 : 6;
    switch (mixed ? i % 4 : i % 2) {
    case 0: vkCmdDraw(cb, n, 1, 0, 0); break;
    case 1: vkCmdDrawIndexed(cb, n, 1, 0, 0, 0); break;
    case 2: vkCmdDrawIndirect(cb, indirect, p.quad * 16, 1, 16); break;
    default:
      vkCmdDrawIndexedIndirect(cb, indirect, 32 + p.quad * 20, 1, 20);
      break;
    }
  }
  vkCmdEndRendering(cb);
}

static void paint(uint32_t *ref, unsigned pass) {
  memset(ref, 0, W * W * 4);
  for (unsigned i = 0; i < DRAWS; i++) {
    struct pc p = draw_params(i, pass);
    for (int y = p.rect[1]; y < p.rect[3] && y < W; y++)
      for (int x = p.rect[0]; x < p.rect[2] && x < W; x++)
        ref[y * W + x] = p.rgba;
  }
}

static void barrier(VkImageLayout from, VkImageLayout to) {
  vkCmdPipelineBarrier2(
     cb, &(VkDependencyInfo){
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &(VkImageMemoryBarrier2){
               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               .dstAccessMask =
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
               .oldLayout = from, .newLayout = to, .image = img,
               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}});
}

/* Record passes [first, first + count), read back the last one. Returns the
 * submit-to-signal time in ms and the recording time in *record_ms. */
static double run(unsigned first, unsigned count, bool mixed,
                  double *record_ms) {
  struct timespec t0, t1, t2;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  CHECK(vkBeginCommandBuffer(
     cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
  barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  for (unsigned p = first; p < first + count; p++)
    record_pass(p, mixed);
  barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vkCmdCopyImageToBuffer(
     cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
     &(VkBufferImageCopy){
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {W, W, 1}});
  CHECK(vkEndCommandBuffer(cb));
  clock_gettime(CLOCK_MONOTONIC, &t1);
  CHECK(vkQueueSubmit(q, 1,
                      &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                      .commandBufferCount = 1,
                                      .pCommandBuffers = &cb},
                      fence));
  CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
  clock_gettime(CLOCK_MONOTONIC, &t2);
  CHECK(vkResetFences(dev, 1, &fence));
  *record_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
  return (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6;
}

static unsigned compare(const uint32_t *px, const uint32_t *ref,
                        const char *what) {
  unsigned bad = 0;
  for (unsigned i = 0; i < W * W; i++) {
    if (px[i] != ref[i] && bad++ < 8)
      fprintf(stderr, "%s (%u, %u): %08x, expected %08x\n", what, i % W,
              i / W, px[i], ref[i]);
  }
  return bad;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
  const unsigned passes = argc > 1 ? atoi(argv[1]) : 4;
  VkInstance inst;
  CHECK(vkCreateInstance(
     &(VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &(VkApplicationInfo){
           .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
           .apiVersion = VK_API_VERSION_1_3}},
     NULL, &inst));
  uint32_t n = 1;
  CHECK(vkEnumeratePhysicalDevices(inst, &n, &pdev));
  if (!n) {
    fprintf(stderr, "no device\n");
    return 2;
  }
  float prio = 1;
  VkPhysicalDeviceVulkan13Features f13 = {
     .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
     .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE};
  CHECK(vkCreateDevice(
     pdev,
     &(VkDeviceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
           .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
           .queueCount = 1, .pQueuePriorities = &prio},
        .pEnabledFeatures = &(VkPhysicalDeviceFeatures){
           .tessellationShader = VK_TRUE}},
     NULL, &dev));
  vkGetDeviceQueue(dev, 0, 0, &q);

  const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
  CHECK(vkCreateImage(
     dev,
     &(VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {W, W, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT},
     NULL, &img));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, img, &mr);
  VkDeviceMemory im;
  CHECK(vkAllocateMemory(
     dev,
     &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex =
           memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)},
     NULL, &im));
  CHECK(vkBindImageMemory(dev, img, im, 0));
  CHECK(vkCreateImageView(
     dev,
     &(VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
     NULL, &view));

  uint32_t *px = host_buffer(&buf, (VkDeviceSize)W * W * 4,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  uint16_t *indices = host_buffer(&ib, 16, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  for (unsigned i = 0; i < 6; i++)
    indices[i] = i;
  /* Draw records for the triangle and the quad pipeline at 0 and 16, the
   * indexed ones at 32 and 52. */
  uint32_t *cmds = host_buffer(&indirect, 72, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
  const uint32_t records[18] = {6, 1, 0, 0, 4, 1, 0, 0, 6, 1, 0, 0, 0,
                                4, 1, 0, 0, 0};
  memcpy(cmds, records, sizeof(records));

  CHECK(vkCreatePipelineLayout(
     dev,
     &(VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &(VkPushConstantRange){
           VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(struct pc)}},
     NULL, &pl));
  VkShaderModule vs = module(tess_batch_vert, sizeof(tess_batch_vert));
  VkShaderModule fs = module(tess_batch_frag, sizeof(tess_batch_frag));
  VkShaderModule tcs[2] = {
     module(tess_batch_tri_tesc, sizeof(tess_batch_tri_tesc)),
     module(tess_batch_quad_tesc, sizeof(tess_batch_quad_tesc))};
  VkShaderModule tes[2] = {
     module(tess_batch_tri_tese, sizeof(tess_batch_tri_tese)),
     module(tess_batch_quad_tese, sizeof(tess_batch_quad_tese))};
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  for (unsigned quad = 0; quad < 2; quad++) {
    const VkPipelineShaderStageCreateInfo stages[] = {
       {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
       {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        .module = tcs[quad], .pName = "main"},
       {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
        .module = tes[quad], .pName = "main"},
       {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
    };
    CHECK(vkCreateGraphicsPipelines(
       dev, VK_NULL_HANDLE, 1,
       &(VkGraphicsPipelineCreateInfo){
          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
          .pNext = &(VkPipelineRenderingCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
             .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt},
          .stageCount = 4, .pStages = stages,
          .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
          .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
             .topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST},
          .pTessellationState = &(VkPipelineTessellationStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
             .patchControlPoints = quad ? 4 : 3},
          .pViewportState = &(VkPipelineViewportStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
             .viewportCount = 1, .scissorCount = 1},
          .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
             .lineWidth = 1},
          .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
             .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT},
          .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
             .attachmentCount = 1,
             .pAttachments = &(VkPipelineColorBlendAttachmentState){
                .colorWriteMask = 0xf}},
          .pDynamicState = &(VkPipelineDynamicStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
             .dynamicStateCount = 2, .pDynamicStates = dyn},
          .layout = pl},
       NULL, &pipes[quad]));
  }

  VkCommandPool pool;
  CHECK(vkCreateCommandPool(
     dev,
     &(VkCommandPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT},
     NULL, &pool));
  CHECK(vkAllocateCommandBuffers(
     dev,
     &(VkCommandBufferAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1},
     &cb));
  CHECK(vkCreateFence(
     dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO},
     NULL, &fence));

  uint32_t *ref = malloc(W * W * 4);
  unsigned bad = 0;
  double record_ms, gpu[RUNS], cpu[RUNS];

  run(0, 1, true, &record_ms);
  paint(ref, 0);
  bad += compare(px, ref, "mixed");

  for (unsigned r = 0; r < RUNS; r++) {
    const unsigned first = 1 + r * passes;
    gpu[r] = run(first, passes, false, &cpu[r]);
    paint(ref, first + passes - 1);
    bad += compare(px, ref, "timed");
  }
  qsort(gpu, RUNS, sizeof(double), cmp_double);
  qsort(cpu, RUNS, sizeof(double), cmp_double);
  printf("%u passes x %u tessellated draws: GPU %.2f ms per pass "
         "(%.1f us per draw, median of %u; min %.2f ms), "
         "recording %.1f us per draw\n",
         passes, DRAWS, gpu[RUNS / 2] / passes,
         gpu[RUNS / 2] * 1e3 / (passes * DRAWS), RUNS, gpu[0] / passes,
         cpu[RUNS / 2] * 1e3 / (passes * DRAWS));

  if (bad) {
    printf("FAIL: %u pixels wrong\n", bad);
    return 1;
  }
  printf("PASS\n");
  return 0;
}
