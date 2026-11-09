/* SPDX-License-Identifier: MIT
 * CPU-side loader/device/mapping regression checks for drm-shim.
 * No command buffers are submitted and no GPU execution is tested.
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "vulkan-smoke-vert.h"

#define REQUIRE(x) do { if (!(x)) { \
   fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) REQUIRE((x) == VK_SUCCESS)

int main(int argc, char **argv)
{
   REQUIRE(dlsym(RTLD_DEFAULT, "asahi_noop_is_registered_fd") != NULL);
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "siliconforest-host-smoke",
                            .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
   VkInstance instance;
   OK(vkCreateInstance(&ici, NULL, &instance));
   uint32_t n = 0;
   OK(vkEnumeratePhysicalDevices(instance, &n, NULL));
   if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
      REQUIRE(n == 0);
      vkDestroyInstance(instance, NULL);
      puts("device admission refused as expected");
      return 0;
   }
   REQUIRE(n == 1);
   VkPhysicalDevice physical;
   OK(vkEnumeratePhysicalDevices(instance, &n, &physical));
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(physical, &props);
   REQUIRE(strstr(props.deviceName, "Apple") != NULL);
   printf("device: %s\n", props.deviceName);

   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
      .pNext = &f13,
   };
   VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
      .pNext = &xfb,
   };
   VkPhysicalDeviceFeatures2 features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &placed,
   };
   vkGetPhysicalDeviceFeatures2(physical, &features);
   REQUIRE(placed.memoryMapPlaced && placed.memoryUnmapReserve && xfb.transformFeedback);
   REQUIRE(f13.dynamicRendering);
   f13 = (VkPhysicalDeviceVulkan13Features){
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };
   placed.memoryMapRangePlaced = VK_FALSE;
   xfb.geometryStreams = VK_FALSE;

   uint32_t queue_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
   REQUIRE(queue_count > 0);
   VkQueueFamilyProperties *families = calloc(queue_count, sizeof(*families));
   REQUIRE(families);
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, families);
   uint32_t family = 0;
   while (family < queue_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      ++family;
   REQUIRE(family < queue_count);
   free(families);
   float priority = 1;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
   };
   const char *extensions[] = {VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME,
                               VK_KHR_MAP_MEMORY_2_EXTENSION_NAME,
                               VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                            .pNext = &placed, .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qci,
                            .enabledExtensionCount = 3,
                            .ppEnabledExtensionNames = extensions};
   VkDevice device;
   OK(vkCreateDevice(physical, &dci, NULL, &device));
   PFN_vkMapMemory2KHR map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(device, "vkMapMemory2KHR");
   PFN_vkUnmapMemory2KHR unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(device, "vkUnmapMemory2KHR");
   REQUIRE(map2 && unmap2);

   VkPhysicalDeviceMemoryProperties memory;
   vkGetPhysicalDeviceMemoryProperties(physical, &memory);
   uint32_t type = 0;
   while (type < memory.memoryTypeCount &&
          !(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) ++type;
   REQUIRE(type < memory.memoryTypeCount);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = 0x104000, .memoryTypeIndex = type};
   VkDeviceMemory mem;
   OK(vkAllocateMemory(device, &ai, NULL, &mem));
   vkFreeMemory(device, mem, NULL); /* prime the oversized BO cache */
   ai.allocationSize = 0x100000;
   OK(vkAllocateMemory(device, &ai, NULL, &mem));
   const size_t alignment = 0x10000, total = ai.allocationSize + 3 * alignment;
   unsigned char *reservation = mmap(NULL, total, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   REQUIRE(reservation != MAP_FAILED);
   unsigned char *address = (void *)(((uintptr_t)reservation + alignment - 1) & ~(alignment - 1));
   unsigned char *guard = address + ai.allocationSize;
   memset(guard, 0x5a, 4096);
   VkMemoryMapPlacedInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,
                                  .pPlacedAddress = address};
   VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR,
                            .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                            .memory = mem, .size = VK_WHOLE_SIZE};
   void *mapped = NULL;
   OK(map2(device, &mi, &mapped));
   REQUIRE(mapped == address);
   memset(mapped, 0x3c, ai.allocationSize);
   for (unsigned i = 0; i < 4096; ++i) REQUIRE(guard[i] == 0x5a);
   VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
                              .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT, .memory = mem};
   OK(unmap2(device, &ui));
   for (unsigned i = 0; i < 4096; ++i) REQUIRE(guard[i] == 0x5a);
   OK(map2(device, &mi, &mapped));
   REQUIRE(mapped == address);
   vkFreeMemory(device, mem, NULL); /* freeing mapped placed memory must unmap */
   void *again = mmap(address, ai.allocationSize, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
   REQUIRE(again == address);
   for (unsigned i = 0; i < 4096; ++i) REQUIRE(guard[i] == 0x5a);
   REQUIRE(munmap(reservation, total) == 0);

   PFN_vkCmdBeginTransformFeedbackEXT begin = (PFN_vkCmdBeginTransformFeedbackEXT)
      vkGetDeviceProcAddr(device, "vkCmdBeginTransformFeedbackEXT");
   PFN_vkCmdEndTransformFeedbackEXT end = (PFN_vkCmdEndTransformFeedbackEXT)
      vkGetDeviceProcAddr(device, "vkCmdEndTransformFeedbackEXT");
   REQUIRE(begin && end);
   VkShaderModuleCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof(sf_smoke_vert_spv),
                                   .pCode = sf_smoke_vert_spv};
   VkShaderModule shader;
   OK(vkCreateShaderModule(device, &sci, NULL, &shader));
   VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout layout;
   OK(vkCreatePipelineLayout(device, &lci, NULL, &layout));
   VkPipelineShaderStageCreateInfo stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader, .pName = "main",
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .rasterizerDiscardEnable = VK_TRUE, .polygonMode = VK_POLYGON_MODE_FILL,
      .lineWidth = 1};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineRenderingCreateInfo render_pipeline = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
   VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &render_pipeline, .stageCount = 1, .pStages = &stage,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia,
      .pRasterizationState = &raster, .pMultisampleState = &ms, .layout = layout,
   };
   VkPipeline pipeline;
   OK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = 64, .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT};
   VkBuffer xfb_buffer;
   OK(vkCreateBuffer(device, &bci, NULL, &xfb_buffer));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(device, xfb_buffer, &req);
   uint32_t xfb_type = 0;
   while (xfb_type < memory.memoryTypeCount && !(req.memoryTypeBits & (1u << xfb_type))) ++xfb_type;
   REQUIRE(xfb_type < memory.memoryTypeCount);
   VkMemoryAllocateInfo xai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size, .memoryTypeIndex = xfb_type};
   VkDeviceMemory xfb_memory;
   OK(vkAllocateMemory(device, &xai, NULL, &xfb_memory));
   OK(vkBindBufferMemory(device, xfb_buffer, xfb_memory, 0));
   PFN_vkCmdBindTransformFeedbackBuffersEXT bind_xfb = (PFN_vkCmdBindTransformFeedbackBuffersEXT)
      vkGetDeviceProcAddr(device, "vkCmdBindTransformFeedbackBuffersEXT");
   REQUIRE(bind_xfb);
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .queueFamilyIndex = family};
   VkCommandPool pool;
   OK(vkCreateCommandPool(device, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                     .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                     .commandBufferCount = 1};
   VkCommandBuffer cmd;
   OK(vkAllocateCommandBuffers(device, &cai, &cmd));
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   OK(vkBeginCommandBuffer(cmd, &cbi));
   VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                 .renderArea = {.extent = {1, 1}}, .layerCount = 1};
   vkCmdBeginRendering(cmd, &rendering);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   const VkDeviceSize xfb_offset = 0;
   bind_xfb(cmd, 0, 1, &xfb_buffer, &xfb_offset, NULL);
   begin(cmd, 0, 1, NULL, NULL);
   end(cmd, 0, 1, NULL, NULL);
   vkCmdEndRendering(cmd);
   OK(vkEndCommandBuffer(cmd));
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyShaderModule(device, shader, NULL);
   vkDestroyBuffer(device, xfb_buffer, NULL);
   vkFreeMemory(device, xfb_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   puts("PASS host device setup, placed mapping guards/free and NULL XFB counters (no submissions)");
   return 0;
}
