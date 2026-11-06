/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Render passes whose render area is a small part of a large attachment, as
 * user interfaces record them, including areas on the right and bottom edges:
 * every pixel outside the areas must keep its contents, every pixel inside
 * must take the pass's clear or draw. The driver does not walk the tiles past
 * an area's bottom-right corner (hk_queue.c), so those tiles must survive
 * untouched.
 *
 * It then times 1000 passes of a 128x64 area on an 8192x8192 attachment. A
 * driver that walks every tile of the attachment for such a pass takes
 * several times longer per pass than one that stops at the area (on the
 * M4 Pro: 125-180 us against 24-37 us).
 *
 * Usage: vulkan-render-area   Exit status 0 = correct output.
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

#define W 2560
#define H 1440

static VkDevice dev;
static VkPhysicalDevice pdev;
static VkQueue q;
static VkCommandBuffer cb;
static VkFence fence;
static VkImageView view;
static VkPipelineLayout pl;
static VkPipeline pipe;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties m;
  vkGetPhysicalDeviceMemoryProperties(pdev, &m);
  for (uint32_t i = 0; i < m.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  fprintf(stderr, "no memory type\n");
  exit(2);
}

/* Full-screen triangle, constant colour from a push constant. */
#include "vulkan_render_area_shaders.h"

/* The rectangles of one round: small areas, edges, corners and an offset. */
static const VkRect2D areas[] = {
  {{0, 0}, {128, 64}},          {{W - 96, 0}, {96, 96}},
  {{0, H - 32}, {64, 32}},      {{W - 200, H - 120}, {200, 120}},
  {{1000, 600}, {33, 17}},      {{512, 0}, {256, 1440}},
  {{0, 700}, {2560, 40}},       {{2000, 1000}, {1, 1}},
};
#define NA (sizeof(areas) / sizeof(areas[0]))

static uint32_t pattern(uint32_t x, uint32_t y) {
  return 0xff000000u | ((x * 7 + y * 13) & 0xffffff);
}

static uint32_t area_colour(unsigned i, bool clear) {
  return 0xff000000u | (clear ? 0x00a00000u : 0x0000a000u) | (i * 16 + 1);
}

/* One pass on `area`: a load-op clear, or a load followed by a draw. */
static void pass(VkRect2D area, uint32_t colour, bool clear) {
  VkClearValue cv;
  for (unsigned c = 0; c < 4; c++)
    cv.color.float32[c] = ((colour >> (8 * c)) & 0xff) / 255.0f;
  vkCmdBeginRendering(
     cb, &(VkRenderingInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area,
            .layerCount = 1, .colorAttachmentCount = 1,
            .pColorAttachments = &(VkRenderingAttachmentInfo){
               .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
               .imageView = view,
               .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                               : VK_ATTACHMENT_LOAD_OP_LOAD,
               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
               .clearValue = cv}});
  if (!clear) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &(VkViewport){0, 0, W, H, 0, 1});
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &colour);
    vkCmdDraw(cb, 3, 1, 0, 0);
  }
  vkCmdEndRendering(cb);
}

static void submit(void) {
  CHECK(vkEndCommandBuffer(cb));
  CHECK(vkQueueSubmit(q, 1,
                      &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                      .commandBufferCount = 1,
                                      .pCommandBuffers = &cb},
                      fence));
  CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
  CHECK(vkResetFences(dev, 1, &fence));
}

int main(void) {
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
           .queueCount = 1, .pQueuePriorities = &prio}},
     NULL, &dev));
  vkGetDeviceQueue(dev, 0, 0, &q);

  const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
  VkImage img;
  CHECK(vkCreateImage(
     dev,
     &(VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {W, H, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT},
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

  /* Host buffer for the initial pattern and the readback. */
  const VkDeviceSize size = (VkDeviceSize)W * H * 4;
  VkBuffer buf;
  CHECK(vkCreateBuffer(dev,
                       &(VkBufferCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                          .size = size,
                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT},
                       NULL, &buf));
  vkGetBufferMemoryRequirements(dev, buf, &mr);
  VkDeviceMemory bm;
  CHECK(vkAllocateMemory(
     dev,
     &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex =
           memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)},
     NULL, &bm));
  CHECK(vkBindBufferMemory(dev, buf, bm, 0));
  uint32_t *px;
  CHECK(vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&px));

  VkShaderModule vs, fs;
  CHECK(vkCreateShaderModule(
     dev,
     &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(render_area_vert), .pCode = render_area_vert},
     NULL, &vs));
  CHECK(vkCreateShaderModule(
     dev,
     &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(render_area_frag), .pCode = render_area_frag},
     NULL, &fs));
  CHECK(vkCreatePipelineLayout(
     dev,
     &(VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &(VkPushConstantRange){
           VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4}},
     NULL, &pl));
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  CHECK(vkCreateGraphicsPipelines(
     dev, VK_NULL_HANDLE, 1,
     &(VkGraphicsPipelineCreateInfo){
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &(VkPipelineRenderingCreateInfo){
           .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
           .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt},
        .stageCount = 2,
        .pStages = (VkPipelineShaderStageCreateInfo[]){
           {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
           {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
            .pName = "main"}},
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
     NULL, &pipe));

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

  const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  const VkBufferImageCopy whole = {0, 0, 0, layers, {0, 0, 0}, {W, H, 1}};

#define BARRIER(from, to)                                                      \
  vkCmdPipelineBarrier2(                                                       \
     cb, &(VkDependencyInfo){                                                  \
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,                        \
            .imageMemoryBarrierCount = 1,                                      \
            .pImageMemoryBarriers = &(VkImageMemoryBarrier2){                  \
               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,              \
               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,           \
               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,                  \
               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,           \
               .dstAccessMask =                                                \
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,  \
               .oldLayout = (from), .newLayout = (to), .image = img,           \
               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}})

  /* Correctness: pattern, then each area once with a draw and once (half of
   * them) with a clear, then read back. */
  for (uint32_t y = 0; y < H; y++)
    for (uint32_t x = 0; x < W; x++)
      px[y * W + x] = pattern(x, y);
  CHECK(vkBeginCommandBuffer(
     cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
  BARRIER(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &whole);
  BARRIER(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  for (unsigned i = 0; i < NA; i++)
    pass(areas[i], area_colour(i, i & 1), i & 1);
  BARRIER(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                         &whole);
  submit();

  unsigned bad = 0;
  for (uint32_t y = 0; y < H; y++) {
    for (uint32_t x = 0; x < W; x++) {
      uint32_t want = pattern(x, y);
      for (unsigned i = 0; i < NA; i++) {
        const VkRect2D *a = &areas[i];
        if (x >= (uint32_t)a->offset.x && x < a->offset.x + a->extent.width &&
            y >= (uint32_t)a->offset.y && y < a->offset.y + a->extent.height)
          want = area_colour(i, i & 1);
      }
      if (px[y * W + x] != want && bad++ < 8)
        fprintf(stderr, "(%u, %u): %08x, expected %08x\n", x, y,
                px[y * W + x], want);
    }
  }

  /* Cost of a small-area pass on a large attachment. */
  const uint32_t big = 8192, timed = 1000;
  VkImage big_img;
  CHECK(vkCreateImage(
     dev,
     &(VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {big, big, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
     NULL, &big_img));
  vkGetImageMemoryRequirements(dev, big_img, &mr);
  VkDeviceMemory big_mem;
  CHECK(vkAllocateMemory(
     dev,
     &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex =
           memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)},
     NULL, &big_mem));
  CHECK(vkBindImageMemory(dev, big_img, big_mem, 0));
  CHECK(vkCreateImageView(
     dev,
     &(VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = big_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
     NULL, &view));
  img = big_img;
  double ms[2];
  for (unsigned run = 0; run < 2; run++) {
    CHECK(vkBeginCommandBuffer(
       cb, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
    BARRIER(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pass((VkRect2D){{0, 0}, {big, big}}, area_colour(0, true), true);
    /* Alternate the height by a row so that no pass continues the previous
     * one: each must be its own render. Run 0 only clears, as reference. */
    for (unsigned i = 0; run && i < timed; i++)
      pass((VkRect2D){{0, 0}, {128, 64 - (i & 1)}}, area_colour(i & 7, false),
           false);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    submit();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms[run] = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
  }
  printf("%.1f us per 128x64 pass on a %ux%u attachment\n",
         (ms[1] - ms[0]) * 1e3 / timed, big, big);

  if (bad) {
    printf("FAIL: %u pixels wrong\n", bad);
    return 1;
  }
  printf("PASS\n");
  return 0;
}
