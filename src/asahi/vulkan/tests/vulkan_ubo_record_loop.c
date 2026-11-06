/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * A gather loop over a uniform buffer array of tap records, written two
 * ways: reading each record's members in separate expressions (as
 * Counter-Strike 2's depth-of-field shader does), and reading each record
 * once. The two must render the same image, and a compiler that merges the
 * reads of one record makes them cost about the same: the program prints
 * the cost of each and their ratio (on the M4 Pro: 1.31 before the reads were
 * merged, 1.18 after; what remains is the scale read, which stays in the loop
 * because it may not be read speculatively).
 *
 * Usage: vulkan-ubo-record-loop [taps=48]   Exit status 0 = same image.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "vulkan_ubo_record_loop_shaders.h"

#define CHECK(call)                                                            \
  do {                                                                         \
    VkResult r = (call);                                                       \
    if (r != VK_SUCCESS) {                                                     \
      fprintf(stderr, "%s: %d\n", #call, r);                                   \
      exit(2);                                                                 \
    }                                                                          \
  } while (0)

#define W 2048
#define H 2048
#define TEX 512
#define PASSES 40
#define ROUNDS 7

static VkDevice dev;
static VkPhysicalDevice pdev;
static VkQueue q;
static VkCommandBuffer cb;
static VkFence fence;
static VkImage target;
static VkImageView target_view;
static VkBuffer staging;
static VkPipeline pipe[2];
static VkPipelineLayout layout;
static VkDescriptorSet set;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties m;
  vkGetPhysicalDeviceMemoryProperties(pdev, &m);
  for (uint32_t i = 0; i < m.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  fprintf(stderr, "no memory type\n");
  exit(2);
}

static VkDeviceMemory bind_memory(VkMemoryRequirements mr,
                                  VkMemoryPropertyFlags flags) {
  VkDeviceMemory mem;
  CHECK(vkAllocateMemory(
     dev,
     &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = mr.size,
                             .memoryTypeIndex =
                                memory_type(mr.memoryTypeBits, flags)},
     NULL, &mem));
  return mem;
}

static VkImage image(uint32_t w, uint32_t h, VkImageUsageFlags usage,
                     VkImageView *view) {
  VkImage img;
  CHECK(vkCreateImage(
     dev,
     &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                          .imageType = VK_IMAGE_TYPE_2D,
                          .format = VK_FORMAT_R8G8B8A8_UNORM,
                          .extent = {w, h, 1}, .mipLevels = 1,
                          .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                          .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage},
     NULL, &img));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, img, &mr);
  CHECK(vkBindImageMemory(
     dev, img, bind_memory(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
  CHECK(vkCreateImageView(
     dev,
     &(VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
     NULL, view));
  return img;
}

static VkBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            void **map) {
  VkBuffer buf;
  CHECK(vkCreateBuffer(dev,
                       &(VkBufferCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                          .size = size, .usage = usage},
                       NULL, &buf));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, buf, &mr);
  VkDeviceMemory mem =
     bind_memory(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  CHECK(vkBindBufferMemory(dev, buf, mem, 0));
  CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, map));
  return buf;
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

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from,
                    VkImageLayout to) {
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

static const VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

static int compare_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* Record `passes` draws of shader `s` (each its own render) and, if
 * `readback`, copy the target to the staging buffer. Returns ms. */
static double run(unsigned s, unsigned passes, bool readback) {
  CHECK(vkBeginCommandBuffer(
     cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
  barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  for (unsigned p = 0; p < passes; p++) {
    vkCmdBeginRendering(
       cb, &(VkRenderingInfo){
              .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
              .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
              .colorAttachmentCount = 1,
              .pColorAttachments = &(VkRenderingAttachmentInfo){
                 .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                 .imageView = target_view,
                 .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE}});
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[s]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                            1, &set, 0, NULL);
    vkCmdSetViewport(cb, 0, 1, &(VkViewport){0, 0, W, H, 0, 1});
    vkCmdSetScissor(cb, 0, 1, &(VkRect2D){{0, 0}, {W, H}});
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
  }
  if (readback) {
    barrier(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(
       cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
       &(VkBufferImageCopy){0, 0, 0, sl, {0, 0, 0}, {W, H, 1}});
  }
  CHECK(vkEndCommandBuffer(cb));
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  CHECK(vkQueueSubmit(q, 1,
                      &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                      .commandBufferCount = 1,
                                      .pCommandBuffers = &cb},
                      fence));
  CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
  clock_gettime(CLOCK_MONOTONIC, &t1);
  CHECK(vkResetFences(dev, 1, &fence));
  return (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}

int main(int argc, char **argv) {
  const int taps = argc > 1 ? atoi(argv[1]) : 48;
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
  CHECK(vkCreateDevice(
     pdev,
     &(VkDeviceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &(VkPhysicalDeviceVulkan13Features){
           .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
           .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE},
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
           .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
           .queueCount = 1, .pQueuePriorities = &prio}},
     NULL, &dev));
  vkGetDeviceQueue(dev, 0, 0, &q);

  /* Target, a noise texture, the tap records and a readback buffer. */
  VkImageView tex_view;
  target = image(W, H,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         &target_view);
  VkImage tex = image(TEX, TEX,
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      &tex_view);
  uint32_t *pixels;
  staging = host_buffer((VkDeviceSize)W * H * 4,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 (void **)&pixels);
  for (uint32_t i = 0; i < TEX * TEX; i++)
    pixels[i] = (i * 2654435761u) ^ (i >> 7);
  struct {
    float pad0[5];
    float scale; /* 20 */
    float pad1[6];
    float tap[256][4]; /* 48 */
    int32_t count; /* 4144 */
  } *ubo_data;
  VkBuffer ubo = host_buffer(sizeof(*ubo_data),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             (void **)&ubo_data);
  for (int i = 0; i < 256; i++) {
    float a = i * 2.39996f, r = 12.0f * sqrtf((i + 0.5f) / 256.0f);
    ubo_data->tap[i][0] = r * cosf(a) / W;
    ubo_data->tap[i][1] = r * sinf(a) / H;
    ubo_data->tap[i][2] = 0.1f;
    ubo_data->tap[i][3] = 0.6f + i / 1024.0f;
  }
  ubo_data->count = taps;
  ubo_data->scale = 1.5f;

  VkSampler smp;
  CHECK(vkCreateSampler(
     dev,
     &(VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                            .magFilter = VK_FILTER_LINEAR,
                            .minFilter = VK_FILTER_LINEAR},
     NULL, &smp));
  VkDescriptorSetLayout dsl;
  CHECK(vkCreateDescriptorSetLayout(
     dev,
     &(VkDescriptorSetLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = (VkDescriptorSetLayoutBinding[]){
           {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT},
           {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT},
           {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT}}},
     NULL, &dsl));
  VkDescriptorPool pool;
  CHECK(vkCreateDescriptorPool(
     dev,
     &(VkDescriptorPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
        .poolSizeCount = 3,
        .pPoolSizes = (VkDescriptorPoolSize[]){
           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
           {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
           {VK_DESCRIPTOR_TYPE_SAMPLER, 1}}},
     NULL, &pool));
  CHECK(vkAllocateDescriptorSets(
     dev,
     &(VkDescriptorSetAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl},
     &set));
  vkUpdateDescriptorSets(
     dev, 3,
     (VkWriteDescriptorSet[]){
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
         .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo = &(VkDescriptorBufferInfo){ubo, 0, VK_WHOLE_SIZE}},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
         .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .pImageInfo = &(VkDescriptorImageInfo){
            VK_NULL_HANDLE, tex_view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
         .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
         .pImageInfo = &(VkDescriptorImageInfo){smp}}},
     0, NULL);
  CHECK(vkCreatePipelineLayout(
     dev,
     &(VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl},
     NULL, &layout));

  const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
  VkShaderModule vs = module(ubo_loop_vert, sizeof(ubo_loop_vert));
  VkShaderModule fs[2] = {module(ubo_loop_split, sizeof(ubo_loop_split)),
                          module(ubo_loop_whole, sizeof(ubo_loop_whole))};
  for (unsigned i = 0; i < 2; i++) {
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,
                            VK_DYNAMIC_STATE_SCISSOR};
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
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
              .pName = "main"},
             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs[i],
              .pName = "main"}},
          .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
          .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
             .sType =
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
             .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
          .pViewportState = &(VkPipelineViewportStateCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
             .viewportCount = 1, .scissorCount = 1},
          .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
             .sType =
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
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
          .layout = layout},
       NULL, &pipe[i]));
  }

  VkCommandPool cmd_pool;
  CHECK(vkCreateCommandPool(
     dev,
     &(VkCommandPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT},
     NULL, &cmd_pool));
  CHECK(vkAllocateCommandBuffers(
     dev,
     &(VkCommandBufferAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1},
     &cb));
  CHECK(vkCreateFence(
     dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO},
     NULL, &fence));

  /* Upload the texture. */
  CHECK(vkBeginCommandBuffer(
     cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
  barrier(cb, tex, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyBufferToImage(
     cb, staging, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
     &(VkBufferImageCopy){0, 0, 0, sl, {0, 0, 0}, {TEX, TEX, 1}});
  barrier(cb, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  CHECK(vkEndCommandBuffer(cb));
  CHECK(vkQueueSubmit(q, 1,
                      &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                      .commandBufferCount = 1,
                                      .pCommandBuffers = &cb},
                      fence));
  CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
  CHECK(vkResetFences(dev, 1, &fence));

  /* Both shaders must render the same image. */
  uint32_t *image0 = malloc((size_t)W * H * 4);
  run(0, 1, true);
  memcpy(image0, pixels, (size_t)W * H * 4);
  run(1, 1, true);
  unsigned bad = 0;
  for (size_t i = 0; i < (size_t)W * H; i++)
    bad += image0[i] != pixels[i];

  /* Alternate the two, so that both see the same clocks. */
  double t[2][ROUNDS];
  for (unsigned r = 0; r < ROUNDS; r++)
    for (unsigned s = 0; s < 2; s++)
      t[s][r] = run(s, PASSES, false) / PASSES;
  qsort(t[0], ROUNDS, sizeof(double), compare_double);
  qsort(t[1], ROUNDS, sizeof(double), compare_double);
  printf("%d taps, %ux%u: members read separately %.0f us/pass, record read "
         "once %.0f us/pass, ratio %.2f\n",
         taps, W, H, t[0][ROUNDS / 2] * 1e3, t[1][ROUNDS / 2] * 1e3,
         t[0][ROUNDS / 2] / t[1][ROUNDS / 2]);

  if (bad) {
    printf("FAIL: %u pixels differ between the two shaders\n", bad);
    return 1;
  }
  printf("PASS\n");
  return 0;
}
