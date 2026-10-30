/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * A post-processing chain as games record it: full-resolution passes that
 * each read the previous target (five texel fetches) and write the next one,
 * ping-ponging between two images. It measures what lossless compression of
 * those targets saves: run it once with images the driver compresses and
 * once with plain ones, and compare the per-job GPU time of the passes in
 * the asahi_g17_complete trace (each pass is one render job; chains are
 * submitted 20 ms apart so the trace separates them).
 *
 * Correctness: the final image of the chain must be bit-identical to the
 * same chain on plain images (STORAGE usage keeps an image plain on G17P),
 * which this program runs after the timed chains.
 *
 * Usage: aurora-post-chain FORMAT USAGE NOISE [PASSES [CHAINS [W H]]]
 *   FORMAT  rgba16f | r11g11b10f | rgba8
 *   USAGE   attach (colour attachment + sampled) | storage (adds STORAGE)
 *   NOISE   0 = smooth gradient (compressible) .. 1 = white noise
 * Exit status 0 = the final image matches the plain reference.
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

#include "vulkan_post_chain_shaders.h"

static VkDevice dev;
static VkPhysicalDevice pdev;
static VkQueue q;
static VkCommandBuffer cb;
static VkFence fence;
static VkPipelineLayout pl;
static VkPipeline pipe;
static VkDescriptorSetLayout dsl;
static VkDescriptorPool dpool;
static VkSampler sampler;
static VkFormat fmt;
static uint32_t W = 3456, H = 2234, bpp;

struct target {
  VkImage img;
  VkDeviceMemory mem;
  VkImageView view;
  VkDescriptorSet set; /* samples this image */
};

struct push {
  float noise;
  uint32_t pass_index, generate;
};

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties m;
  vkGetPhysicalDeviceMemoryProperties(pdev, &m);
  for (uint32_t i = 0; i < m.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  fprintf(stderr, "no memory type\n");
  exit(2);
}

static void make_target(struct target *t, VkImageUsageFlags usage) {
  CHECK(vkCreateImage(
     dev,
     &(VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {W, H, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage},
     NULL, &t->img));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, t->img, &mr);
  CHECK(vkAllocateMemory(
     dev,
     &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex =
           memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)},
     NULL, &t->mem));
  CHECK(vkBindImageMemory(dev, t->img, t->mem, 0));
  CHECK(vkCreateImageView(
     dev,
     &(VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
     NULL, &t->view));
  CHECK(vkAllocateDescriptorSets(
     dev,
     &(VkDescriptorSetAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl},
     &t->set));
  vkUpdateDescriptorSets(
     dev, 1,
     &(VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->set,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &(VkDescriptorImageInfo){
           sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}},
     0, NULL);
}

static void free_target(struct target *t) {
  vkDestroyImageView(dev, t->view, NULL);
  vkDestroyImage(dev, t->img, NULL);
  vkFreeMemory(dev, t->mem, NULL);
}

static void barrier(VkImage img, VkImageLayout from, VkImageLayout to) {
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

/* One pass: `dst` = f(`src`), or the generator when src is NULL. */
static void pass(struct target *dst, struct target *src, float noise,
                 uint32_t index) {
  barrier(dst->img, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  vkCmdBeginRendering(
     cb, &(VkRenderingInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &(VkRenderingAttachmentInfo){
               .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
               .imageView = dst->view,
               .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
               .storeOp = VK_ATTACHMENT_STORE_OP_STORE}});
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdSetViewport(cb, 0, 1, &(VkViewport){0, 0, W, H, 0, 1});
  vkCmdSetScissor(cb, 0, 1, &(VkRect2D){{0, 0}, {W, H}});
  /* The generator binds its own target only for textureSize(). */
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1,
                          src ? &src->set : &dst->set, 0, NULL);
  struct push pc = {noise, index, src == NULL};
  vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);
  barrier(dst->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

static void begin(void) {
  CHECK(vkBeginCommandBuffer(
     cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
}

/* Generator plus `passes` passes; returns the target holding the result. */
static struct target *chain(struct target t[2], unsigned passes, float noise) {
  begin();
  pass(&t[0], NULL, noise, 0);
  for (unsigned i = 0; i < passes; i++)
    pass(&t[(i + 1) & 1], &t[i & 1], noise * 0.05f, i + 1);
  submit();
  return &t[passes & 1];
}

static void readback(struct target *t, VkBuffer buf) {
  begin();
  barrier(t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vkCmdCopyImageToBuffer(
     cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
     &(VkBufferImageCopy){
        0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}});
  submit();
}

static void sleep_ms(long ms) {
  nanosleep(&(struct timespec){ms / 1000, (ms % 1000) * 1000000L}, NULL);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s rgba16f|r11g11b10f|rgba8 attach|storage NOISE "
                    "[PASSES [CHAINS [W H]]]\n", argv[0]);
    return 2;
  }
  if (!strcmp(argv[1], "rgba16f"))
    fmt = VK_FORMAT_R16G16B16A16_SFLOAT, bpp = 8;
  else if (!strcmp(argv[1], "r11g11b10f"))
    fmt = VK_FORMAT_B10G11R11_UFLOAT_PACK32, bpp = 4;
  else if (!strcmp(argv[1], "rgba8"))
    fmt = VK_FORMAT_R8G8B8A8_UNORM, bpp = 4;
  else
    return 2;
  const bool storage = !strcmp(argv[2], "storage");
  const float noise = atof(argv[3]);
  const unsigned passes = argc > 4 ? atoi(argv[4]) : 30;
  const unsigned chains = argc > 5 ? atoi(argv[5]) : 10;
  if (argc > 7)
    W = atoi(argv[6]), H = atoi(argv[7]);

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
  if (!n)
    return 2;
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

  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(pdev, fmt, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
    fprintf(stderr, "%s has no storage support for the plain reference\n",
            argv[1]);
    return 2;
  }

  CHECK(vkCreateSampler(
     dev, &(VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO},
     NULL, &sampler));
  CHECK(vkCreateDescriptorSetLayout(
     dev,
     &(VkDescriptorSetLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &(VkDescriptorSetLayoutBinding){
           0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
           VK_SHADER_STAGE_FRAGMENT_BIT, NULL}},
     NULL, &dsl));
  CHECK(vkCreateDescriptorPool(
     dev,
     &(VkDescriptorPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4,
        .poolSizeCount = 1,
        .pPoolSizes = &(VkDescriptorPoolSize){
           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}},
     NULL, &dpool));
  CHECK(vkCreatePipelineLayout(
     dev,
     &(VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &(VkPushConstantRange){
           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct push)}},
     NULL, &pl));

  VkShaderModule vs, fs;
  CHECK(vkCreateShaderModule(
     dev,
     &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(post_chain_vert), .pCode = post_chain_vert},
     NULL, &vs));
  CHECK(vkCreateShaderModule(
     dev,
     &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(post_chain_frag), .pCode = post_chain_frag},
     NULL, &fs));
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

  const VkDeviceSize size = (VkDeviceSize)W * H * bpp;
  VkBuffer buf[2];
  VkDeviceMemory bm[2];
  uint8_t *px[2];
  for (unsigned i = 0; i < 2; i++) {
    CHECK(vkCreateBuffer(dev,
                         &(VkBufferCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = size,
                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT},
                         NULL, &buf[i]));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf[i], &mr);
    CHECK(vkAllocateMemory(
       dev,
       &(VkMemoryAllocateInfo){
          .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          .allocationSize = mr.size,
          .memoryTypeIndex = memory_type(
             mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)},
       NULL, &bm[i]));
    CHECK(vkBindBufferMemory(dev, buf[i], bm[i], 0));
    CHECK(vkMapMemory(dev, bm[i], 0, VK_WHOLE_SIZE, 0, (void **)&px[i]));
  }

  const VkImageUsageFlags base = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  struct target t[2];
  for (unsigned i = 0; i < 2; i++)
    make_target(&t[i], base | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0));

  /* A warm-up chain, then the timed ones, 20 ms apart. */
  double best = 1e9, sum = 0;
  struct target *out = NULL;
  for (unsigned c = 0; c <= chains; c++) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    out = chain(t, passes, noise);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (c) {
      sum += ms;
      best = ms < best ? ms : best;
    }
    sleep_ms(20);
  }
  readback(out, buf[0]);
  printf("%s %s noise=%.2f %ux%u: %u passes, wall per chain min %.2f ms "
         "mean %.2f ms\n",
         argv[1], storage ? "storage" : "attach", noise, W, H, passes, best,
         sum / chains);

  /* The plain reference, after a gap the trace parser cuts at. */
  sleep_ms(200);
  struct target r[2];
  for (unsigned i = 0; i < 2; i++)
    make_target(&r[i], base | VK_IMAGE_USAGE_STORAGE_BIT);
  readback(chain(r, passes, noise), buf[1]);

  size_t bad = 0, first = 0, nonzero = 0;
  for (size_t i = 0; i < size; i++) {
    nonzero += px[1][i] != 0;
    if (px[0][i] != px[1][i] && !bad++)
      first = i;
  }
  /* A chain that wrote nothing would match trivially. */
  if (nonzero < size / 2) {
    printf("FAIL: the reference is mostly zero (%zu of %zu bytes set)\n",
           nonzero, (size_t)size);
    return 1;
  }
  if (bad) {
    printf("FAIL: %zu bytes differ from the plain reference, first at "
           "pixel %zu\n", bad, first / bpp);
    return 1;
  }
  uint64_t hash = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < size; i++)
    hash = (hash ^ px[0][i]) * 0x100000001b3ull;
  printf("PASS (result fnv1a %016llx)\n", (unsigned long long)hash);
  for (unsigned i = 0; i < 2; i++)
    free_target(&t[i]), free_target(&r[i]);
  return 0;
}
