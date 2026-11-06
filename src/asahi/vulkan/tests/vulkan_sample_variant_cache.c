/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * A pipeline with a dynamic sample count and an alpha-tested fragment shader,
 * as games build them: the driver compiles the shader's multisample variant
 * at the first multisampled draw (hk_fs_build_sample_variant), in the middle
 * of recording a frame. That variant must go to the disk cache, so that the
 * next process that draws with the shader loads it instead of compiling it
 * again.
 *
 * The test starts from an empty cache directory and records the same 2x MSAA
 * draw from two Vulkan instances in turn, timing the command buffer
 * recording and reading back the resolved image. The second recording must
 * take a fraction of the first (it loads the variant) and render the same
 * image. On the M4 Pro: 3.9 ms against 0.56 ms; a driver that compiles the
 * variant again takes 2.7 ms against 1.5 ms.
 *
 * Usage: vulkan-sample-variant-cache   Exit status 0 = pass.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "vulkan_sample_variant_cache_shaders.h"

#define CHECK(call)                                                            \
  do {                                                                         \
    VkResult r = (call);                                                       \
    if (r != VK_SUCCESS) {                                                     \
      fprintf(stderr, "%s: %d\n", #call, r);                                   \
      exit(2);                                                                 \
    }                                                                          \
  } while (0)

#define W 256
#define H 256

static VkPhysicalDevice pdev;
static VkDevice dev;
static VkDeviceMemory image_mem[2];
static unsigned image_count;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties m;
  vkGetPhysicalDeviceMemoryProperties(pdev, &m);
  for (uint32_t i = 0; i < m.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  fprintf(stderr, "no memory type\n");
  exit(2);
}

static VkImageView image(VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                         VkImage *out) {
  CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1,
      .arrayLayers = 1, .samples = samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage}, NULL, out));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, *out, &mr);
  VkDeviceMemory mem;
  CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = memory_type(mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)},
      NULL, &mem));
  CHECK(vkBindImageMemory(dev, *out, mem, 0));
  image_mem[image_count++ % 2] = mem;
  VkImageView view;
  CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *out,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}, NULL, &view));
  return view;
}

static VkShaderModule module(const uint32_t *code, size_t size) {
  VkShaderModule m;
  CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size,
      .pCode = code}, NULL, &m));
  return m;
}

/* One instance: build the pipeline, record and run the draw, return the
 * recording time and a hash of the resolved image. */
static void run(double *record_ms, uint64_t *hash) {
  VkInstance inst;
  CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &(VkApplicationInfo){
          .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
          .apiVersion = VK_API_VERSION_1_3}}, NULL, &inst));
  uint32_t n = 1;
  vkEnumeratePhysicalDevices(inst, &n, &pdev);
  CHECK(vkCreateDevice(pdev, &(VkDeviceCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &(VkPhysicalDeviceVulkan13Features){
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
          .dynamicRendering = VK_TRUE,
          .pNext = &(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT){
              .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
              .extendedDynamicState3RasterizationSamples = VK_TRUE}},
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = (const char *[]){
          VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME},
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
          .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
          .pQueuePriorities = (float[]){1}}}, NULL, &dev));
  VkQueue q;
  vkGetDeviceQueue(dev, 0, 0, &q);
  PFN_vkCmdSetRasterizationSamplesEXT set_samples =
      (PFN_vkCmdSetRasterizationSamplesEXT)vkGetDeviceProcAddr(
          dev, "vkCmdSetRasterizationSamplesEXT");

  VkImage ms_img, rs_img;
  VkImageView ms_view = image(VK_SAMPLE_COUNT_2_BIT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &ms_img);
  VkImageView rs_view = image(VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &rs_img);

  VkPipelineLayout pl;
  CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &(VkPushConstantRange){
          VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4}}, NULL, &pl));
  VkShaderModule vs = module(sample_variant_cache_vert,
                             sizeof(sample_variant_cache_vert));
  VkShaderModule fs = module(sample_variant_cache_frag,
                             sizeof(sample_variant_cache_frag));
  const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipeline pipe;
  CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &(VkPipelineRenderingCreateInfo){
          .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
          .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt},
      .stageCount = 2,
      .pStages = (VkPipelineShaderStageCreateInfo[]){
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
           VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
           VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}},
      .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
      .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
          .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
          .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
      .pViewportState = &(VkPipelineViewportStateCreateInfo){
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
          .viewportCount = 1, .pViewports = &(VkViewport){0, 0, W, H, 0, 1},
          .scissorCount = 1, .pScissors = &(VkRect2D){{0, 0}, {W, H}}},
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
          .dynamicStateCount = 1,
          .pDynamicStates = (VkDynamicState[]){
              VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT}},
      .layout = pl}, NULL, &pipe));

  VkBuffer rb;
  CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT}, NULL, &rb));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, rb, &mr);
  VkDeviceMemory rmem;
  CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = memory_type(mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)},
      NULL, &rmem));
  CHECK(vkBindBufferMemory(dev, rb, rmem, 0));

  VkCommandPool cp;
  CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}, NULL, &cp));
  VkCommandBuffer cb;
  CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb));

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
  VkImageMemoryBarrier2 to_attachment[2];
  for (int i = 0; i < 2; i++)
    to_attachment[i] = (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = i ? rs_img : ms_img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier2(cb, &(VkDependencyInfo){
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2,
      .pImageMemoryBarriers = to_attachment});
  vkCmdBeginRendering(cb, &(VkRenderingInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}},
      .layerCount = 1, .colorAttachmentCount = 1,
      .pColorAttachments = &(VkRenderingAttachmentInfo){
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = ms_view,
          .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
          .resolveImageView = rs_view,
          .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .clearValue.color.float32 = {0.1f, 0.2f, 0.3f, 1}}});
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  set_samples(cb, VK_SAMPLE_COUNT_2_BIT);
  const float k = 0.37f;
  vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &k);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);
  vkCmdPipelineBarrier2(cb, &(VkDependencyInfo){
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &(VkImageMemoryBarrier2){
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
          .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .image = rs_img,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}});
  vkCmdCopyImageToBuffer(cb, rs_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1,
                         &(VkBufferImageCopy){
                             .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .imageExtent = {W, H, 1}});
  CHECK(vkEndCommandBuffer(cb));
  clock_gettime(CLOCK_MONOTONIC, &t1);
  *record_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

  VkFence fence;
  CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &fence));
  CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
      .pCommandBuffers = &cb}, fence));
  CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

  const uint8_t *px;
  CHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
  uint64_t h = 1469598103934665603ull;
  unsigned discarded = 0;
  for (unsigned i = 0; i < W * H * 4; i++)
    h = (h ^ px[i]) * 1099511628211ull;
  for (unsigned i = 0; i < W * H; i++)
    discarded += abs(px[4 * i] - 26) <= 1 && abs(px[4 * i + 1] - 51) <= 1 &&
                 abs(px[4 * i + 2] - 77) <= 1;
  *hash = h;
  if (discarded == 0 || discarded == W * H) {
    fprintf(stderr, "the draw does not mix kept and discarded pixels (%u)\n",
            discarded);
    exit(1);
  }
  vkUnmapMemory(dev, rmem);
  vkDestroyFence(dev, fence, NULL);
  vkDestroyCommandPool(dev, cp, NULL);
  vkDestroyBuffer(dev, rb, NULL);
  vkFreeMemory(dev, rmem, NULL);
  vkDestroyPipeline(dev, pipe, NULL);
  vkDestroyShaderModule(dev, vs, NULL);
  vkDestroyShaderModule(dev, fs, NULL);
  vkDestroyPipelineLayout(dev, pl, NULL);
  vkDestroyImageView(dev, ms_view, NULL);
  vkDestroyImageView(dev, rs_view, NULL);
  vkDestroyImage(dev, ms_img, NULL);
  vkDestroyImage(dev, rs_img, NULL);
  for (unsigned i = 0; i < 2; i++)
    vkFreeMemory(dev, image_mem[i], NULL);
  /* Destroying the device and instance flushes the disk cache writes. */
  vkDestroyDevice(dev, NULL);
  vkDestroyInstance(inst, NULL);
}

int main(void) {
  char dir[] = "/tmp/hk-sample-variant-cache-XXXXXX";
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    return 2;
  }
  setenv("MESA_SHADER_CACHE_DIR", dir, 1);
  unsetenv("MESA_SHADER_CACHE_DISABLE");

  double cold, warm;
  uint64_t h0, h1;
  run(&cold, &h0);
  run(&warm, &h1);
  printf("recording with the variant compiled: %.2f ms, from the cache: %.2f ms\n",
         cold, warm);

  char cmd[96];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
  if (system(cmd) != 0)
    fprintf(stderr, "could not remove %s\n", dir);

  if (h0 != h1) {
    fprintf(stderr, "FAIL: the cached variant renders differently\n");
    return 1;
  }
  if (warm * 4 > cold) {
    fprintf(stderr, "FAIL: the second recording compiled the variant again\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
