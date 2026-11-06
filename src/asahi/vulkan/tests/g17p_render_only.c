/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Render-only Vulkan submit for T8140/G17P: clear, draw one full-screen
 * triangle into a one-tile RGBA8 image, store at end of tile. No readback.
 *
 * This exists to exercise the 3D/TA launch path on its own. The image->buffer
 * copy of g17p-render-only is a *compute* dispatch, which is blocked
 * on its own separate problem, so removing it isolates whether a render
 * submit reaches the GPU and completes.
 *
 * Every wait is bounded: a launch that faults never signals its fence, and a
 * hung wait would cost a reboot. VK_TIMEOUT is reported as its own outcome
 * rather than blocking.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(G17P_GRAPHICS_SIDE_EFFECTS) || defined(G17P_FRAGMENT_SIDE_EFFECTS)
#define G17P_ANY_SIDE_EFFECTS 1
#include "g17p_vulkan_graphics_side_effects_frag.h"
#ifdef G17P_GRAPHICS_SIDE_EFFECTS
#include "g17p_vulkan_graphics_side_effects_vert.h"
#define G17P_VERTEX_SPV g17p_vulkan_graphics_side_effects_vert_spv
#else
#include "g17p_wsi_triangle_vert.h"
#define G17P_VERTEX_SPV g17p_wsi_triangle_vert_spv
#endif
#define G17P_FRAGMENT_SPV g17p_vulkan_graphics_side_effects_frag_spv
#else
#include "g17p_wsi_triangle_frag.h"
#include "g17p_wsi_triangle_vert.h"
#define G17P_VERTEX_SPV g17p_wsi_triangle_vert_spv
#define G17P_FRAGMENT_SPV g17p_wsi_triangle_frag_spv
#endif

#ifndef AGX_TEST_DEVICE_ID
#define AGX_TEST_DEVICE_ID 0x8140
#endif
#ifndef IMAGE_WIDTH
#define IMAGE_WIDTH 32
#endif
#ifndef IMAGE_HEIGHT
#define IMAGE_HEIGHT 32
#endif
#define IMAGE_BYTES (IMAGE_WIDTH * IMAGE_HEIGHT * 4)
#ifdef G17P_ANY_SIDE_EFFECTS
#define SIDE_EFFECT_GUARD UINT32_C(0xc09b5eed)
struct side_effect_output {
   uint32_t prefix_guard[4];
   uint32_t vertex_atomic;
   uint32_t vertex_values[3];
   uint32_t fragment_atomic;
   uint32_t fragment_value;
   uint32_t suffix_guard[4];
};
#endif
#ifdef G17P_VDM_CAPACITY
#define COMMAND_BUFFER_COUNT 193
#else
#define COMMAND_BUFFER_COUNT 1
#endif

#define VK_FN(name) static PFN_##name name

VK_FN(vkGetInstanceProcAddr);
VK_FN(vkGetDeviceProcAddr);
VK_FN(vkCreateInstance);
VK_FN(vkCreateDebugUtilsMessengerEXT);
VK_FN(vkEnumeratePhysicalDevices);
VK_FN(vkGetPhysicalDeviceProperties);
#if defined(G17P_OCCLUSION_QUERY) || defined(G17P_ANY_SIDE_EFFECTS)
VK_FN(vkGetPhysicalDeviceFeatures);
#endif
#if defined(G17P_CONDITIONAL_RENDERING) || defined(G17P_REPEAT_DRAW)
VK_FN(vkGetPhysicalDeviceFeatures2);
#endif
VK_FN(vkGetPhysicalDeviceMemoryProperties);
VK_FN(vkGetPhysicalDeviceQueueFamilyProperties);
VK_FN(vkGetPhysicalDeviceFormatProperties);
VK_FN(vkCreateDevice);
VK_FN(vkGetDeviceQueue);
VK_FN(vkCreateBuffer);
VK_FN(vkCmdCopyImageToBuffer);
VK_FN(vkGetBufferMemoryRequirements);
VK_FN(vkCreateImage);
VK_FN(vkGetImageMemoryRequirements);
VK_FN(vkAllocateMemory);
VK_FN(vkBindBufferMemory);
VK_FN(vkBindImageMemory);
VK_FN(vkMapMemory);
VK_FN(vkCreateImageView);
VK_FN(vkCreateRenderPass);
VK_FN(vkCreateShaderModule);
VK_FN(vkCreatePipelineLayout);
#ifdef G17P_ANY_SIDE_EFFECTS
VK_FN(vkCreateDescriptorSetLayout);
VK_FN(vkCreateDescriptorPool);
VK_FN(vkAllocateDescriptorSets);
VK_FN(vkUpdateDescriptorSets);
#endif
VK_FN(vkCreateGraphicsPipelines);
VK_FN(vkCreateFramebuffer);
VK_FN(vkCreateCommandPool);
VK_FN(vkAllocateCommandBuffers);
VK_FN(vkBeginCommandBuffer);
VK_FN(vkCmdBeginRenderPass);
VK_FN(vkCmdBindPipeline);
VK_FN(vkCmdBindVertexBuffers);
VK_FN(vkCmdBindIndexBuffer);
#ifdef G17P_ANY_SIDE_EFFECTS
VK_FN(vkCmdBindDescriptorSets);
#endif
VK_FN(vkCmdDraw);
VK_FN(vkCmdDrawIndirect);
VK_FN(vkCmdDrawIndexed);
VK_FN(vkCmdDrawIndexedIndirect);
#ifdef G17P_REPEAT_DRAW
VK_FN(vkCmdSetStencilReference);
VK_FN(vkResetFences);
VK_FN(vkUnmapMemory);
#endif
#ifdef G17P_CONDITIONAL_RENDERING
VK_FN(vkCmdBeginConditionalRenderingEXT);
VK_FN(vkCmdClearAttachments);
VK_FN(vkCmdDrawIndirectCount);
VK_FN(vkCmdEndConditionalRenderingEXT);
#endif
VK_FN(vkCmdEndRenderPass);
VK_FN(vkCmdPipelineBarrier);
VK_FN(vkCmdBlitImage);
VK_FN(vkEndCommandBuffer);
VK_FN(vkCreateFence);
VK_FN(vkQueueSubmit);
VK_FN(vkWaitForFences);
VK_FN(vkUnmapMemory);
#ifdef G17P_OCCLUSION_QUERY
VK_FN(vkCreateQueryPool);
VK_FN(vkCmdResetQueryPool);
VK_FN(vkCmdBeginQuery);
VK_FN(vkCmdEndQuery);
VK_FN(vkCmdWriteTimestamp);
VK_FN(vkGetQueryPoolResults);
#endif

static void
die(const char *what)
{
   fprintf(stderr, "g17p-render-only: %s\n", what);
   exit(EXIT_FAILURE);
}

static void
check(VkResult result, const char *what)
{
   if (result != VK_SUCCESS) {
      fprintf(stderr, "g17p-render-only: %s returned %d\n", what,
              result);
      exit(EXIT_FAILURE);
   }
}

#define LOAD_GLOBAL(name)                                                      \
   do {                                                                        \
      name = (PFN_##name)vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);         \
      if (!name)                                                               \
         die("loader is missing " #name);                                     \
   } while (0)

#define LOAD_INSTANCE(instance, name)                                          \
   do {                                                                        \
      name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);               \
      if (!name)                                                               \
         die("instance is missing " #name);                                   \
   } while (0)

#define LOAD_DEVICE(device, name)                                              \
   do {                                                                        \
      name = (PFN_##name)vkGetDeviceProcAddr(device, #name);                   \
      if (!name)                                                               \
         die("device is missing " #name);                                     \
   } while (0)

/* Non-fatal variant: returns UINT32_MAX instead of dying, so an optional
 * memory class can be probed. */
static uint32_t
try_memory_type(const VkPhysicalDeviceMemoryProperties *properties,
                uint32_t allowed, VkMemoryPropertyFlags required)
{
   for (uint32_t i = 0; i < properties->memoryTypeCount; ++i) {
      if (getenv("AGX_RENDER_UNCACHED") &&
          (properties->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
         continue;
      if ((allowed & (1u << i)) &&
          (properties->memoryTypes[i].propertyFlags & required) == required)
         return i;
   }
   return UINT32_MAX;
}

static uint32_t
find_memory_type(const VkPhysicalDeviceMemoryProperties *properties,
                 uint32_t allowed, VkMemoryPropertyFlags required)
{
   uint32_t type = try_memory_type(properties, allowed, required);
   if (type != UINT32_MAX)
      return type;

   die("required Vulkan memory type is unavailable");
   return UINT32_MAX;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
render_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                     VkDebugUtilsMessageTypeFlagsEXT types,
                     const VkDebugUtilsMessengerCallbackDataEXT *data,
                     void *user_data)
{
   (void)severity;
   (void)types;
   (void)user_data;
   fprintf(stderr, "render debug: %s\n", data->pMessage);
   return VK_FALSE;
}

int
main(
#ifdef G17P_CONDITIONAL_RENDERING
   int argc, char **argv
#else
   void
#endif
)
{
#ifdef G17P_CONDITIONAL_RENDERING
   if (argc != 3 || (strcmp(argv[1], "--record-only") &&
                     strcmp(argv[1], "--submit-only") &&
                     strcmp(argv[1], "--execute")) ||
       (strcmp(argv[2], "execute") && strcmp(argv[2], "discard") &&
        strcmp(argv[2], "indexed-execute") &&
        strcmp(argv[2], "indexed-discard") &&
        strcmp(argv[2], "indirect-execute") &&
        strcmp(argv[2], "indirect-discard") &&
        strcmp(argv[2], "count-execute") &&
        strcmp(argv[2], "count-discard") &&
        strcmp(argv[2], "clear-execute") &&
        strcmp(argv[2], "clear-discard")))
      die("usage: g17p-vulkan-conditional-rendering-graphics "
          "<--record-only|--submit-only|--execute> "
          "<execute|discard|indexed-execute|indexed-discard|"
          "indirect-execute|indirect-discard|count-execute|count-discard|"
          "clear-execute|clear-discard>");
   const bool record_only = !strcmp(argv[1], "--record-only");
   const bool submit_only = !strcmp(argv[1], "--submit-only");
   const bool clear_attachment = !strncmp(argv[2], "clear-", 6);
   const bool indexed_draw = !strncmp(argv[2], "indexed-", 8);
   const bool indirect_draw = !strncmp(argv[2], "indirect-", 9);
   const bool count_draw = !strncmp(argv[2], "count-", 6);
   const bool expect_execute =
      !strcmp(argv[2], "execute") || !strcmp(argv[2], "indexed-execute") ||
      !strcmp(argv[2], "indirect-execute") ||
      !strcmp(argv[2], "count-execute") ||
      !strcmp(argv[2], "clear-execute");
   if (!record_only && !submit_only) {
      const char *ack = getenv("G17P_CONDITIONAL_EXECUTION_ACK");
      if (!ack || strcmp(ack, "YES"))
         die("execution requires G17P_CONDITIONAL_EXECUTION_ACK=YES");
   }
   const char *shim_validate = getenv("ASAHI_SHIM_VALIDATE_G17P_STREAM");
   if (submit_only &&
       (!shim_validate || !strcmp(shim_validate, "0")))
      die("--submit-only requires the G17P DRM-shim stream validator");
   if (submit_only) {
      if (setenv("ASAHI_SHIM_G17P_CASE",
                 indexed_draw ? "indirect indexed" : "indirect nonindexed",
                 1))
         die("failed to select the strict G17P stream shape");
      if (setenv("ASAHI_SHIM_G17P_EXPECT_COMPUTE_COUNTS",
                 count_draw ? "8,8,1,3" : "7,7,1,2", 1))
         die("failed to select exact G17P compute-stream counts");
   }
#endif
#ifdef G17P_TINY_RENDER_RECORD
   const bool clear_only = getenv("G17P_TINY_CLEAR_ONLY") != NULL;
#endif
   void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!loader)
      die(dlerror());

   vkGetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
   if (!vkGetInstanceProcAddr)
      die("libvulkan.so.1 has no vkGetInstanceProcAddr");
   LOAD_GLOBAL(vkCreateInstance);

   const VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "g17p-render-only",
      .applicationVersion = 1,
#ifdef G17P_GRAPHICS_SIDE_EFFECTS
      /* Exercise the production profile that enables vertex pipeline stores
       * for DXVK/vkd3d, rather than silently testing only fragment stores. */
      .pEngineName = "DXVK",
#else
      .pEngineName = "none",
#endif
      .engineVersion = 1,
      .apiVersion = VK_API_VERSION_1_2,
   };
   const bool debug_messages = getenv("AGX_RENDER_DEBUG") != NULL;
   const char *debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
      .enabledExtensionCount = debug_messages ? 1 : 0,
      .ppEnabledExtensionNames = &debug_extension,
   };
   VkInstance instance;
   check(vkCreateInstance(&instance_info, NULL, &instance),
         "vkCreateInstance");

   if (debug_messages) {
      LOAD_INSTANCE(instance, vkCreateDebugUtilsMessengerEXT);
      const VkDebugUtilsMessengerCreateInfoEXT debug_info = {
         .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
         .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
         .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
         .pfnUserCallback = render_debug_message,
      };
      VkDebugUtilsMessengerEXT messenger;
      check(vkCreateDebugUtilsMessengerEXT(instance, &debug_info, NULL,
                                            &messenger),
            "vkCreateDebugUtilsMessengerEXT");
   }

   LOAD_INSTANCE(instance, vkGetDeviceProcAddr);
   LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties);
#if defined(G17P_OCCLUSION_QUERY) || defined(G17P_ANY_SIDE_EFFECTS)
   LOAD_INSTANCE(instance, vkGetPhysicalDeviceFeatures);
#endif
#if defined(G17P_CONDITIONAL_RENDERING) || defined(G17P_REPEAT_DRAW)
   LOAD_INSTANCE(instance, vkGetPhysicalDeviceFeatures2);
#endif
   LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties);
   LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties);
   LOAD_INSTANCE(instance, vkGetPhysicalDeviceFormatProperties);
   LOAD_INSTANCE(instance, vkCreateDevice);

   uint32_t physical_count = 0;
   check(vkEnumeratePhysicalDevices(instance, &physical_count, NULL),
         "vkEnumeratePhysicalDevices(count)");
   if (!physical_count)
      die("Vulkan loader found no physical device");

   VkPhysicalDevice *physical_devices =
      calloc(physical_count, sizeof(*physical_devices));
   if (!physical_devices)
      die("out of host memory");
   check(vkEnumeratePhysicalDevices(instance, &physical_count,
                                    physical_devices),
         "vkEnumeratePhysicalDevices(list)");

   VkPhysicalDevice physical = VK_NULL_HANDLE;
   uint32_t queue_family = UINT32_MAX;
   VkPhysicalDeviceProperties properties = {0};
   for (uint32_t p = 0; p < physical_count && !physical; ++p) {
      VkPhysicalDeviceProperties candidate;
      vkGetPhysicalDeviceProperties(physical_devices[p], &candidate);
      if (candidate.deviceID != AGX_TEST_DEVICE_ID)
         continue;

      uint32_t queue_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[p],
                                                &queue_count, NULL);
      VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
      if (!queues)
         die("out of host memory");
      vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[p],
                                                &queue_count, queues);
      for (uint32_t q = 0; q < queue_count; ++q) {
         const VkQueueFlags required =
            VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
         if ((queues[q].queueFlags & required) == required) {
            physical = physical_devices[p];
            queue_family = q;
            properties = candidate;
            break;
         }
      }
      free(queues);
   }
   free(physical_devices);
   if (!physical)
      die("requested Apple device has no graphics-and-transfer queue");

   VkFormatProperties format_properties;
   vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                       &format_properties);
   const VkFormatFeatureFlags required_format =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
   if ((format_properties.optimalTilingFeatures & required_format) !=
       required_format)
      die("RGBA8 optimal images lack color-attachment or transfer-src support");

   fprintf(stderr, "g17p-render-only: device %s, queue %u\n",
           properties.deviceName, queue_family);

#ifdef G17P_OCCLUSION_QUERY
   VkPhysicalDeviceFeatures physical_features;
   vkGetPhysicalDeviceFeatures(physical, &physical_features);
   if (!physical_features.occlusionQueryPrecise)
      die("T8140 does not advertise precise occlusion queries");
   if (!physical_features.pipelineStatisticsQuery)
      die("T8140 does not advertise pipeline statistics");
   if (!properties.limits.timestampComputeAndGraphics ||
       properties.limits.timestampPeriod != 1.0f)
      die("T8140 timestamp properties differ from the bounded profile");

   uint32_t query_queue_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &query_queue_count,
                                             NULL);
   VkQueueFamilyProperties *query_queues =
      calloc(query_queue_count, sizeof(*query_queues));
   if (!query_queues)
      die("out of host memory");
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &query_queue_count,
                                             query_queues);
   if (queue_family >= query_queue_count ||
       query_queues[queue_family].timestampValidBits != 64)
      die("T8140 graphics queue does not expose exact 64-bit timestamps");
   free(query_queues);
#endif

#ifdef G17P_ANY_SIDE_EFFECTS
   VkPhysicalDeviceFeatures physical_features;
   vkGetPhysicalDeviceFeatures(physical, &physical_features);
#ifdef G17P_GRAPHICS_SIDE_EFFECTS
   if (!physical_features.vertexPipelineStoresAndAtomics ||
       !physical_features.fragmentStoresAndAtomics)
      die("G17P graphics shader side effects were not explicitly enabled");
#else
   if (!physical_features.fragmentStoresAndAtomics)
      die("G17P fragment shader side effects are not advertised");
   if (physical_features.vertexPipelineStoresAndAtomics)
      die("fragment capability unexpectedly enabled vertex side effects");
#endif
#endif

#ifdef G17P_CONDITIONAL_RENDERING
   VkPhysicalDeviceVulkan12Features available12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
   };
   VkPhysicalDeviceConditionalRenderingFeaturesEXT available_conditional = {
      .sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
      .pNext = &available12,
   };
   VkPhysicalDeviceFeatures2 available_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &available_conditional,
   };
   vkGetPhysicalDeviceFeatures2(physical, &available_features);
   if (!available_conditional.conditionalRendering ||
       !available_conditional.inheritedConditionalRendering)
      die("conditional rendering features are not exposed");
   if (count_draw && !available12.drawIndirectCount)
      die("drawIndirectCount is required by the conditional count oracle");
   if (!available_features.features.robustBufferAccess)
      die("robustBufferAccess is required by the indexed conditional oracle");
   VkPhysicalDeviceVulkan12Features enabled12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .drawIndirectCount = count_draw,
   };
   const VkPhysicalDeviceConditionalRenderingFeaturesEXT enabled_conditional = {
      .sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
      .pNext = &enabled12,
      .conditionalRendering = VK_TRUE,
      .inheritedConditionalRendering = VK_TRUE,
   };
   const char *const conditional_extension =
      VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;
#endif

#ifdef G17P_REPEAT_DRAW
   const bool repeat_integer_bias = getenv("G17P_REPEAT_INTEGER_DEPTH_BIAS");
   VkPhysicalDeviceDepthBiasControlFeaturesEXT bias_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT,
   };
   if (repeat_integer_bias) {
      VkPhysicalDeviceFeatures2 available = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
         .pNext = &bias_features,
      };
      vkGetPhysicalDeviceFeatures2(physical, &available);
      if (!bias_features.depthBiasControl ||
          !bias_features.leastRepresentableValueForceUnormRepresentation)
         die("forced UNORM depth bias is not advertised");
      bias_features.floatRepresentation = VK_FALSE;
      bias_features.depthBiasExact = VK_FALSE;
   }
   const char *const bias_extension = VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME;
#endif

   const float queue_priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
#ifdef G17P_CONDITIONAL_RENDERING
      .pNext = &enabled_conditional,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = &conditional_extension,
#endif
#ifdef G17P_REPEAT_DRAW
      .pNext = repeat_integer_bias ? &bias_features : NULL,
      .enabledExtensionCount = repeat_integer_bias ? 1 : 0,
      .ppEnabledExtensionNames = repeat_integer_bias ? &bias_extension : NULL,
#endif
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
#ifdef G17P_OCCLUSION_QUERY
      .pEnabledFeatures = &(const VkPhysicalDeviceFeatures){
         .occlusionQueryPrecise = VK_TRUE,
#ifdef G17P_CONDITIONAL_RENDERING
         .robustBufferAccess = VK_TRUE,
#endif
      },
#elif defined(G17P_ANY_SIDE_EFFECTS)
      .pEnabledFeatures = &(const VkPhysicalDeviceFeatures){
#ifdef G17P_GRAPHICS_SIDE_EFFECTS
         .vertexPipelineStoresAndAtomics = VK_TRUE,
#endif
         .fragmentStoresAndAtomics = VK_TRUE,
      },
#endif
   };
   VkDevice device;
   check(vkCreateDevice(physical, &device_info, NULL, &device),
         "vkCreateDevice");

   LOAD_DEVICE(device, vkGetDeviceQueue);
   LOAD_DEVICE(device, vkCreateBuffer);
   LOAD_DEVICE(device, vkCmdCopyImageToBuffer);
   LOAD_DEVICE(device, vkGetBufferMemoryRequirements);
   LOAD_DEVICE(device, vkCreateImage);
   LOAD_DEVICE(device, vkGetImageMemoryRequirements);
   LOAD_DEVICE(device, vkAllocateMemory);
   LOAD_DEVICE(device, vkBindBufferMemory);
   LOAD_DEVICE(device, vkBindImageMemory);
   LOAD_DEVICE(device, vkMapMemory);
   LOAD_DEVICE(device, vkCreateImageView);
   LOAD_DEVICE(device, vkCreateRenderPass);
   LOAD_DEVICE(device, vkCreateShaderModule);
   LOAD_DEVICE(device, vkCreatePipelineLayout);
#ifdef G17P_ANY_SIDE_EFFECTS
   LOAD_DEVICE(device, vkCreateDescriptorSetLayout);
   LOAD_DEVICE(device, vkCreateDescriptorPool);
   LOAD_DEVICE(device, vkAllocateDescriptorSets);
   LOAD_DEVICE(device, vkUpdateDescriptorSets);
#endif
   LOAD_DEVICE(device, vkCreateGraphicsPipelines);
   LOAD_DEVICE(device, vkCreateFramebuffer);
   LOAD_DEVICE(device, vkCreateCommandPool);
   LOAD_DEVICE(device, vkAllocateCommandBuffers);
   LOAD_DEVICE(device, vkBeginCommandBuffer);
   LOAD_DEVICE(device, vkCmdBeginRenderPass);
   LOAD_DEVICE(device, vkCmdBindPipeline);
   LOAD_DEVICE(device, vkCmdBindVertexBuffers);
   LOAD_DEVICE(device, vkCmdBindIndexBuffer);
#ifdef G17P_ANY_SIDE_EFFECTS
   LOAD_DEVICE(device, vkCmdBindDescriptorSets);
#endif
   LOAD_DEVICE(device, vkCmdDraw);
   LOAD_DEVICE(device, vkCmdDrawIndirect);
   LOAD_DEVICE(device, vkCmdDrawIndexed);
   LOAD_DEVICE(device, vkCmdDrawIndexedIndirect);
#ifdef G17P_REPEAT_DRAW
   LOAD_DEVICE(device, vkCmdSetStencilReference);
   LOAD_DEVICE(device, vkResetFences);
   LOAD_DEVICE(device, vkUnmapMemory);
#endif
#ifdef G17P_CONDITIONAL_RENDERING
   LOAD_DEVICE(device, vkCmdBeginConditionalRenderingEXT);
   LOAD_DEVICE(device, vkCmdClearAttachments);
   LOAD_DEVICE(device, vkCmdDrawIndirectCount);
   LOAD_DEVICE(device, vkCmdEndConditionalRenderingEXT);
#endif
    LOAD_DEVICE(device, vkCmdEndRenderPass);
    LOAD_DEVICE(device, vkCmdPipelineBarrier);
    LOAD_DEVICE(device, vkCmdBlitImage);
   LOAD_DEVICE(device, vkEndCommandBuffer);
   LOAD_DEVICE(device, vkCreateFence);
   LOAD_DEVICE(device, vkQueueSubmit);
   LOAD_DEVICE(device, vkWaitForFences);
   LOAD_DEVICE(device, vkUnmapMemory);
#ifdef G17P_OCCLUSION_QUERY
   LOAD_DEVICE(device, vkCreateQueryPool);
   LOAD_DEVICE(device, vkCmdResetQueryPool);
   LOAD_DEVICE(device, vkCmdBeginQuery);
   LOAD_DEVICE(device, vkCmdEndQuery);
   LOAD_DEVICE(device, vkCmdWriteTimestamp);
   LOAD_DEVICE(device, vkGetQueryPoolResults);
#endif

   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   VkPhysicalDeviceMemoryProperties memory_properties;
   vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

   static const float vertices[][2] = {
      {-1.0f, -1.0f},
      {3.0f, -1.0f},
      {-1.0f, 3.0f},
   };
   const bool alternate_geometry = getenv("AGX_RENDER_ALTERNATE") != NULL;
   static const float clipped_vertices[][2] = {
      {4.0f, 4.0f}, {6.0f, 4.0f}, {4.0f, 6.0f},
   };
   const size_t vertex_bytes = sizeof(vertices) * (alternate_geometry ? 2 : 1);
   /* The first four words also form an arrays-indirect draw of three
    * vertices. Keep the index data separate from both draw records. */
   const VkDrawIndexedIndirectCommand repeat_indirect = {3, 1, 0, 0, 0};
   const uint32_t repeat_indices[] = {0, 1, 2};
   const VkDeviceSize repeat_index_offset =
      vertex_bytes + sizeof(repeat_indirect);
   const VkDeviceSize vertex_allocation_size =
      repeat_index_offset + sizeof(repeat_indices);
   const VkBufferCreateInfo vertex_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = vertex_allocation_size,
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer vertex_buffer;
   check(vkCreateBuffer(device, &vertex_info, NULL, &vertex_buffer),
         "vkCreateBuffer(vertex)");
   VkMemoryRequirements vertex_requirements;
   vkGetBufferMemoryRequirements(device, vertex_buffer, &vertex_requirements);
   const uint32_t vertex_memory_type = find_memory_type(
      &memory_properties, vertex_requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   const VkMemoryAllocateInfo vertex_memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = vertex_requirements.size,
      .memoryTypeIndex = vertex_memory_type,
   };
   VkDeviceMemory vertex_memory;
   check(vkAllocateMemory(device, &vertex_memory_info, NULL, &vertex_memory),
         "vkAllocateMemory(vertex)");
   check(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0),
         "vkBindBufferMemory(vertex)");
   void *vertex_map = NULL;
   check(vkMapMemory(device, vertex_memory, 0, vertex_allocation_size, 0,
                     &vertex_map),
         "vkMapMemory(vertex)");
   memcpy(vertex_map, vertices, sizeof(vertices));
   memcpy((uint8_t *)vertex_map + vertex_bytes, &repeat_indirect,
          sizeof(repeat_indirect));
   memcpy((uint8_t *)vertex_map + repeat_index_offset, repeat_indices,
          sizeof(repeat_indices));
   if (alternate_geometry)
      memcpy((uint8_t *)vertex_map + sizeof(vertices), clipped_vertices,
             sizeof(clipped_vertices));

#ifdef G17P_CONDITIONAL_RENDERING
   const VkBufferCreateInfo conditional_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 128,
      .usage = VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer conditional_buffer;
   check(vkCreateBuffer(device, &conditional_info, NULL, &conditional_buffer),
         "vkCreateBuffer(conditional)");
   VkMemoryRequirements conditional_requirements;
   vkGetBufferMemoryRequirements(device, conditional_buffer,
                                 &conditional_requirements);
   const uint32_t conditional_memory_type = find_memory_type(
      &memory_properties, conditional_requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   const VkMemoryAllocateInfo conditional_memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = conditional_requirements.size,
      .memoryTypeIndex = conditional_memory_type,
   };
   VkDeviceMemory conditional_memory;
   check(vkAllocateMemory(device, &conditional_memory_info, NULL,
                          &conditional_memory),
         "vkAllocateMemory(conditional)");
   check(vkBindBufferMemory(device, conditional_buffer, conditional_memory,
                            0),
         "vkBindBufferMemory(conditional)");
   uint32_t *conditional_map;
   check(vkMapMemory(device, conditional_memory, 0, 128, 0,
                     (void **)&conditional_map),
         "vkMapMemory(conditional)");
   uint32_t expected_conditional[32];
   for (unsigned i = 0; i < 32; ++i)
      expected_conditional[i] = UINT32_C(0xbaad0000) + i;
   expected_conditional[0] = expect_execute ? 1 : 0;
   expected_conditional[4] = 0;
   expected_conditional[5] = 1;
   expected_conditional[6] = 2;
   expected_conditional[8] = 3;
   expected_conditional[9] = 1;
   expected_conditional[10] = 0;
   expected_conditional[11] = 0;
   expected_conditional[12] = 1;
   memcpy(conditional_map, expected_conditional,
          sizeof(expected_conditional));
#endif

#ifdef G17P_ANY_SIDE_EFFECTS
   const VkBufferCreateInfo side_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(struct side_effect_output),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer side_buffer;
   check(vkCreateBuffer(device, &side_info, NULL, &side_buffer),
         "vkCreateBuffer(side effects)");
   VkMemoryRequirements side_requirements;
   vkGetBufferMemoryRequirements(device, side_buffer, &side_requirements);
   const uint32_t side_memory_type = find_memory_type(
      &memory_properties, side_requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   const VkMemoryAllocateInfo side_memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = side_requirements.size,
      .memoryTypeIndex = side_memory_type,
   };
   VkDeviceMemory side_memory;
   check(vkAllocateMemory(device, &side_memory_info, NULL, &side_memory),
         "vkAllocateMemory(side effects)");
   check(vkBindBufferMemory(device, side_buffer, side_memory, 0),
         "vkBindBufferMemory(side effects)");
   struct side_effect_output *side_map = NULL;
   check(vkMapMemory(device, side_memory, 0, sizeof(*side_map), 0,
                     (void **)&side_map),
         "vkMapMemory(side effects)");
   memset(side_map, 0, sizeof(*side_map));
   for (unsigned i = 0; i < 4; ++i) {
      side_map->prefix_guard[i] = SIDE_EFFECT_GUARD;
      side_map->suffix_guard[i] = SIDE_EFFECT_GUARD;
   }
#endif


   const bool blit_test = getenv("AGX_RENDER_BLIT") != NULL;
   const bool readback_test = getenv("AGX_RENDER_READBACK") != NULL;
   if (blit_test && readback_test)
      die("select one of AGX_RENDER_BLIT and AGX_RENDER_READBACK");
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   check(vkCreateImage(device, &image_info, NULL, &image), "vkCreateImage");
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   /* Inspect the source independently before testing an optional transfer.
    * This prevents a successful copy of an undrawn image from passing. */
   uint32_t image_memory_type =
      try_memory_type(&memory_properties, image_requirements.memoryTypeBits,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   bool image_host_visible = image_memory_type != UINT32_MAX;
   if (!image_host_visible)
      image_memory_type =
         find_memory_type(&memory_properties, image_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   fprintf(stderr, "g17p-render-only: image memory host-visible=%d type=%u\n",
           (int)image_host_visible, image_memory_type);
   const VkMemoryAllocateInfo image_memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = image_memory_type,
   };
   VkDeviceMemory image_memory;
   check(vkAllocateMemory(device, &image_memory_info, NULL, &image_memory),
         "vkAllocateMemory(image)");
   check(vkBindImageMemory(device, image, image_memory, 0),
         "vkBindImageMemory");

   VkImage blit_image = VK_NULL_HANDLE;
   VkDeviceMemory blit_memory = VK_NULL_HANDLE;
   uint8_t *blit_pixels = NULL;
   VkBuffer readback_buffer = VK_NULL_HANDLE;
   VkDeviceMemory readback_memory = VK_NULL_HANDLE;
   uint8_t *readback_map = NULL;
   if (blit_test) {
      VkImageCreateInfo copy_info = image_info;
      copy_info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      check(vkCreateImage(device, &copy_info, NULL, &blit_image),
            "vkCreateImage(blit)");
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, blit_image, &requirements);
      const VkMemoryAllocateInfo allocation = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = find_memory_type(&memory_properties,
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
      };
      check(vkAllocateMemory(device, &allocation, NULL, &blit_memory),
            "vkAllocateMemory(blit)");
      check(vkBindImageMemory(device, blit_image, blit_memory, 0),
            "vkBindImageMemory(blit)");
      check(vkMapMemory(device, blit_memory, 0, IMAGE_BYTES, 0,
                        (void **)&blit_pixels), "vkMapMemory(blit)");
      memset(blit_pixels, 0xa5, IMAGE_BYTES);
   }
   if (readback_test) {
      const VkBufferCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = IMAGE_BYTES + 32,
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      check(vkCreateBuffer(device, &info, NULL, &readback_buffer), "vkCreateBuffer(readback)");
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(device, readback_buffer, &requirements);
      const VkMemoryAllocateInfo allocation = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = find_memory_type(&memory_properties,
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
      };
      check(vkAllocateMemory(device, &allocation, NULL, &readback_memory),
            "vkAllocateMemory(readback)");
      check(vkBindBufferMemory(device, readback_buffer, readback_memory, 0),
            "vkBindBufferMemory(readback)");
      check(vkMapMemory(device, readback_memory, 0, IMAGE_BYTES + 32, 0,
                        (void **)&readback_map), "vkMapMemory(readback)");
      memset(readback_map, 0xa5, IMAGE_BYTES + 32);
      blit_pixels = readback_map + 16;
   }

   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView view;
   check(vkCreateImageView(device, &view_info, NULL, &view),
         "vkCreateImageView");

   const VkAttachmentDescription attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   };
   const VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   const VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
   };
   const VkSubpassDependency dependencies[] = {
      {
         .srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      },
      {
         .srcSubpass = 0,
         .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      },
   };
   const VkRenderPassCreateInfo render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = dependencies,
   };
   VkRenderPass render_pass;
   check(vkCreateRenderPass(device, &render_pass_info, NULL, &render_pass),
         "vkCreateRenderPass");

   const VkShaderModuleCreateInfo vert_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(G17P_VERTEX_SPV),
      .pCode = G17P_VERTEX_SPV,
   };
   const VkShaderModuleCreateInfo frag_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(G17P_FRAGMENT_SPV),
      .pCode = G17P_FRAGMENT_SPV,
   };
   VkShaderModule vert_module, frag_module;
   check(vkCreateShaderModule(device, &vert_module_info, NULL, &vert_module),
         "vkCreateShaderModule(vertex)");
   check(vkCreateShaderModule(device, &frag_module_info, NULL, &frag_module),
         "vkCreateShaderModule(fragment)");

#ifdef G17P_ANY_SIDE_EFFECTS
   const VkDescriptorSetLayoutBinding side_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags =
#ifdef G17P_GRAPHICS_SIDE_EFFECTS
         VK_SHADER_STAGE_VERTEX_BIT |
#endif
         VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo side_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &side_binding,
   };
   VkDescriptorSetLayout side_layout;
   check(vkCreateDescriptorSetLayout(device, &side_layout_info, NULL,
                                     &side_layout),
         "vkCreateDescriptorSetLayout(side effects)");

   const VkDescriptorPoolSize side_pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
   };
   const VkDescriptorPoolCreateInfo side_pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &side_pool_size,
   };
   VkDescriptorPool side_pool;
   check(vkCreateDescriptorPool(device, &side_pool_info, NULL, &side_pool),
         "vkCreateDescriptorPool(side effects)");
   const VkDescriptorSetAllocateInfo side_allocate = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = side_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &side_layout,
   };
   VkDescriptorSet side_set;
   check(vkAllocateDescriptorSets(device, &side_allocate, &side_set),
         "vkAllocateDescriptorSets(side effects)");
   const VkDescriptorBufferInfo side_descriptor = {
      .buffer = side_buffer,
      .offset = offsetof(struct side_effect_output, vertex_atomic),
      .range = offsetof(struct side_effect_output, suffix_guard) -
               offsetof(struct side_effect_output, vertex_atomic),
   };
   const VkWriteDescriptorSet side_write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = side_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &side_descriptor,
   };
   vkUpdateDescriptorSets(device, 1, &side_write, 0, NULL);
#endif

   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
#ifdef G17P_ANY_SIDE_EFFECTS
      .setLayoutCount = 1,
      .pSetLayouts = &side_layout,
#endif
   };
   VkPipelineLayout pipeline_layout;
   check(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout),
         "vkCreatePipelineLayout");

   const VkPipelineShaderStageCreateInfo stages[] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert_module,
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag_module,
         .pName = "main",
      },
   };
   const VkVertexInputBindingDescription vertex_binding = {
      .binding = 0,
      .stride = sizeof(vertices[0]),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   const VkVertexInputAttributeDescription vertex_attribute = {
      .location = 0,
      .binding = 0,
      .format = VK_FORMAT_R32G32_SFLOAT,
   };
   const VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vertex_binding,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &vertex_attribute,
   };
   const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   const VkViewport viewport = {
      .width = IMAGE_WIDTH,
      .height = IMAGE_HEIGHT,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
   };
   const VkRect2D scissor = {.extent = {IMAGE_WIDTH, IMAGE_HEIGHT}};
   const VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };
#ifdef G17P_REPEAT_DRAW
   const VkDepthBiasRepresentationInfoEXT bias_representation = {
      .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,
      .depthBiasRepresentation =
         VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT,
   };
#endif
   const VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
#ifdef G17P_REPEAT_DRAW
      .pNext = repeat_integer_bias ? &bias_representation : NULL,
      .depthBiasEnable = repeat_integer_bias,
      .depthBiasConstantFactor = repeat_integer_bias ? 1.0f : 0.0f,
#endif
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   const VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   const VkPipelineColorBlendAttachmentState blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   const VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
   };
#ifdef G17P_REPEAT_DRAW
   const bool repeat_stencil = getenv("G17P_REPEAT_STENCIL_REFERENCE") != NULL;
   unsigned repeats = G17P_REPEAT_DRAW;
   const VkDynamicState repeat_dynamic = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
   const VkPipelineDynamicStateCreateInfo repeat_dynamic_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 1,
      .pDynamicStates = &repeat_dynamic,
   };
#endif
   const VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
#ifdef G17P_REPEAT_DRAW
      .pDynamicState = repeat_stencil ? &repeat_dynamic_info : NULL,
#endif
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   VkPipeline pipeline;
   check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                   NULL, &pipeline),
         "vkCreateGraphicsPipelines");
   fprintf(stderr, "g17p-render-only: graphics pipeline created\n");

   const VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &view,
      .width = IMAGE_WIDTH,
      .height = IMAGE_HEIGHT,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   check(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer),
         "vkCreateFramebuffer");

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool command_pools[COMMAND_BUFFER_COUNT];
#ifdef G17P_VDM_CAPACITY
   for (unsigned command = 0; command < COMMAND_BUFFER_COUNT; ++command)
      check(vkCreateCommandPool(device, &pool_info, NULL,
                                &command_pools[command]),
            "vkCreateCommandPool");
#else
   check(vkCreateCommandPool(device, &pool_info, NULL, &command_pools[0]),
         "vkCreateCommandPool");
#endif
   unsigned frame_index = 0;
   unsigned frame_count = 1;
   const char *repeat_env = getenv("AGX_RENDER_REPEAT");
   if (repeat_env) {
      char *end = NULL;
      const unsigned long requested = strtoul(repeat_env, &end, 10);
      if (!*repeat_env || *end || requested < 1 || requested > 64)
         die("AGX_RENDER_REPEAT must be between 1 and 64");
      frame_count = requested;
   }
   if ((blit_test || readback_test) && (frame_count != 1 || alternate_geometry))
      die("copy tests require one full-frame draw");
   /* Allocate and record a fresh command buffer on every iteration. Reusing
    * old pixels or a fixed native descriptor cannot satisfy the sentinel
    * check below after the application has erased the previous result.
    */
next_render_frame:;
   VkCommandBufferAllocateInfo command_allocate = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffers[COMMAND_BUFFER_COUNT];
   for (unsigned command = 0; command < COMMAND_BUFFER_COUNT; ++command) {
      command_allocate.commandPool = command_pools[command];
      check(vkAllocateCommandBuffers(device, &command_allocate,
                                     &command_buffers[command]),
            "vkAllocateCommandBuffers");
   }
   VkCommandBuffer command_buffer = command_buffers[0];
   VkCommandBuffer blit_command = VK_NULL_HANDLE;
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
#ifdef G17P_REPEAT_DRAW
      .flags = 0, /* Reusable after completion for retained-context tests. */
#else
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
#endif
   };
#ifdef G17P_VDM_CAPACITY
   for (unsigned command = 0; command < COMMAND_BUFFER_COUNT; ++command) {
      command_buffer = command_buffers[command];
#endif
   check(vkBeginCommandBuffer(command_buffer, &begin_info),
         "vkBeginCommandBuffer");
#ifdef G17P_OCCLUSION_QUERY
   const VkQueryPoolCreateInfo query_info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_OCCLUSION,
      .queryCount = 2,
   };
   VkQueryPool occlusion_pool;
   check(vkCreateQueryPool(device, &query_info, NULL, &occlusion_pool),
         "vkCreateQueryPool(occlusion)");
   const VkQueryPoolCreateInfo timestamp_info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = 2,
   };
   VkQueryPool timestamp_pool;
   check(vkCreateQueryPool(device, &timestamp_info, NULL, &timestamp_pool),
         "vkCreateQueryPool(timestamp)");
   vkCmdResetQueryPool(command_buffer, occlusion_pool, 0, 2);
   vkCmdResetQueryPool(command_buffer, timestamp_pool, 0, 2);
   vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       timestamp_pool, 0);
#endif
   const VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
   const VkRenderPassBeginInfo render_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {.extent = {IMAGE_WIDTH, IMAGE_HEIGHT}},
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   vkCmdBeginRenderPass(command_buffer, &render_begin,
                        VK_SUBPASS_CONTENTS_INLINE);
#if defined(G17P_TINY_RENDER_RECORD)
   if (!clear_only) {
#endif
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     pipeline);
#ifdef G17P_REPEAT_DRAW
   if (repeat_stencil)
      vkCmdSetStencilReference(command_buffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
#endif
#ifdef G17P_ANY_SIDE_EFFECTS
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           pipeline_layout, 0, 1, &side_set, 0, NULL);
#endif
   const VkDeviceSize vertex_offset =
      alternate_geometry && (frame_index & 1) ? sizeof(vertices) : 0;
   vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer,
                          &vertex_offset);
#ifdef G17P_REPEAT_DRAW
   vkCmdBindIndexBuffer(command_buffer, vertex_buffer, repeat_index_offset,
                        VK_INDEX_TYPE_UINT32);
#endif
#ifdef G17P_CONDITIONAL_RENDERING
   if (indexed_draw)
      vkCmdBindIndexBuffer(command_buffer, conditional_buffer, 16,
                           VK_INDEX_TYPE_UINT32);
#endif
#ifdef G17P_OCCLUSION_QUERY
   vkCmdBeginQuery(command_buffer, occlusion_pool, 0,
                   VK_QUERY_CONTROL_PRECISE_BIT);
#endif
#ifdef G17P_CONDITIONAL_RENDERING
   const VkConditionalRenderingBeginInfoEXT conditional_begin = {
      .sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,
      .buffer = conditional_buffer,
      .offset = 0,
   };
   vkCmdBeginConditionalRenderingEXT(command_buffer, &conditional_begin);
   if (clear_attachment) {
      const VkClearAttachment attachment = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .colorAttachment = 0,
         .clearValue.color = {
            .float32 = {1.0f, 71.0f / 255.0f, 13.0f / 255.0f, 1.0f},
         },
      };
      const VkClearRect rect = {
         .rect = {.extent = {IMAGE_WIDTH, IMAGE_HEIGHT}},
         .baseArrayLayer = 0,
         .layerCount = 1,
      };
      vkCmdClearAttachments(command_buffer, 1, &attachment, 1, &rect);
   } else if (indexed_draw) {
      vkCmdDrawIndexed(command_buffer, 3, 1, 0, 0, 0);
   } else if (indirect_draw) {
      vkCmdDrawIndirect(command_buffer, conditional_buffer, 32, 1, 16);
   } else if (count_draw) {
      vkCmdDrawIndirectCount(command_buffer, conditional_buffer, 32,
                             conditional_buffer, 48, 1, 16);
   } else {
      vkCmdDraw(command_buffer, 3, 1, 0, 0);
   }
   vkCmdEndConditionalRenderingEXT(command_buffer);
#else
   vkCmdDraw(command_buffer, 3, 1, 0, 0);
#if defined(G17P_REPEAT_DRAW)
   /* Identical draws with nothing recorded in between. Honeykrisp takes the
    * unchanged-state early return for every one after the first, so they are
    * body-only draws that reuse the render state the first draw latched --
    * the case that must NOT collect an inter-draw VDM barrier.
    *
    * G17P_REPEAT_DRAW_COUNT raises the count without a second binary, so a
    * host-side sweep can ask how a stream's shape and byte cost grow with the
    * number of draws it carries.  Combine it with ASAHI_MESA_DEBUG=dirty to
    * make every repeat republish render state, which is what a real Zink
    * workload does and what a body-only repeat deliberately does not. */
   {
      const char *requested = getenv("G17P_REPEAT_DRAW_COUNT");
      const char *indirect_mode = getenv("G17P_REPEAT_INDIRECT");
      const bool mixed_indexed = indirect_mode && !strcmp(indirect_mode, "2");

      if (requested) {
         const unsigned long parsed = strtoul(requested, NULL, 10);

         if (parsed >= 1 && parsed <= 4096)
            repeats = (unsigned)parsed;
      }

      for (unsigned repeat = 1; repeat < repeats; ++repeat) {
         if (repeat_stencil)
            vkCmdSetStencilReference(command_buffer,
                                      VK_STENCIL_FACE_FRONT_AND_BACK, repeat);
         if (mixed_indexed && repeat % 4 == 3)
            vkCmdDrawIndexedIndirect(command_buffer, vertex_buffer,
                                     vertex_bytes, 1, sizeof(repeat_indirect));
         else if (mixed_indexed && repeat % 4 == 2)
            vkCmdDrawIndexed(command_buffer, 3, 1, 0, 0, 0);
         else if (indirect_mode && (repeat & 1))
            vkCmdDrawIndirect(command_buffer, vertex_buffer, vertex_bytes,
                              1, sizeof(repeat_indirect));
         else
            vkCmdDraw(command_buffer, 3, 1, 0, 0);
      }
   }
#endif
#endif
#if defined(G17P_TINY_RENDER_RECORD)
   }
#endif
#ifdef G17P_OCCLUSION_QUERY
   vkCmdEndQuery(command_buffer, occlusion_pool, 0);
   /* A second, empty precise interval proves reset and post-graphics
    * availability independently of the nonzero hardware counter. */
   vkCmdBeginQuery(command_buffer, occlusion_pool, 1,
                   VK_QUERY_CONTROL_PRECISE_BIT);
   vkCmdEndQuery(command_buffer, occlusion_pool, 1);
#endif
   vkCmdEndRenderPass(command_buffer);

#ifdef G17P_OCCLUSION_QUERY
   vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       timestamp_pool, 1);
#endif


   check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");
   if (blit_test || readback_test) {
      command_allocate.commandPool = command_pools[0];
      check(vkAllocateCommandBuffers(device, &command_allocate, &blit_command),
            "vkAllocateCommandBuffers(blit)");
      check(vkBeginCommandBuffer(blit_command, &begin_info),
            "vkBeginCommandBuffer(blit)");
      const VkImageMemoryBarrier barriers[] = {
         {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         },
         {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = blit_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         },
      };
      const VkMemoryBarrier host_write = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      };
      vkCmdPipelineBarrier(blit_command,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_HOST_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &host_write, 0, NULL,
         blit_test ? 2 : 1, barriers);
      const VkImageBlit region = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .srcOffsets = {{0,0,0}, {IMAGE_WIDTH,IMAGE_HEIGHT,1}},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .dstOffsets = {{0,0,0}, {IMAGE_WIDTH,IMAGE_HEIGHT,1}},
      };
      if (blit_test) {
         vkCmdBlitImage(blit_command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        blit_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &region, VK_FILTER_NEAREST);
      } else {
         const VkBufferImageCopy copy = {
            .bufferOffset = 16,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
         };
         vkCmdCopyImageToBuffer(blit_command, image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1, &copy);
      }
      const VkMemoryBarrier host_read = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(blit_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0, NULL, 0, NULL);
      check(vkEndCommandBuffer(blit_command), "vkEndCommandBuffer(blit)");
      fprintf(stderr, "g17p-render-only: separate %s command recorded; no submission yet\n",
              blit_test ? "blit" : "readback");
   }
   if (getenv("AGX_RENDER_RECORD_ONLY")) {
      printf("Apple %#x: %ux%u Vulkan draw recorded; no submission\n",
             properties.deviceID, IMAGE_WIDTH, IMAGE_HEIGHT);
      return EXIT_SUCCESS;
   }
#ifdef G17P_TINY_RENDER_RECORD
   if (getenv("G17P_TINY_RENDER_SUBMIT") == NULL) {
      printf("g17p-tiny-render-record: 2x2 %s command buffer recorded\n",
             clear_only ? "clear-only" : "draw");
      return EXIT_SUCCESS;
   }
#endif
#ifdef G17P_VDM_CAPACITY
   }
   puts("g17p-vdm-capacity: 193 live command buffers across 193 pools recorded");
   return EXIT_SUCCESS;
#else
   fprintf(stderr, "g17p-render-only: command buffer recorded\n");
#endif

#ifdef G17P_CONDITIONAL_RENDERING
   if (record_only) {
      printf("g17p-vulkan-conditional-rendering-graphics: RECORD PASS — "
             "%s predicate=%u expected=%s\n",
             clear_attachment ? "clear attachments"
                              : indexed_draw ? "direct indexed draw"
                              : indirect_draw ? "indirect draw"
                              : count_draw ? "indirect-count draw"
                                             : "direct draw",
             conditional_map[0],
             expect_execute ? "execute" : "discard");
      return EXIT_SUCCESS;
   }
#endif
#ifdef G17P_OCCLUSION_QUERY
   if (getenv("G17P_QUERY_RECORD_ONLY")) {
      puts("g17p-occlusion-query: record-only PASS");
      return EXIT_SUCCESS;
   }
#endif
#ifdef G17P_ANY_SIDE_EFFECTS
#ifdef G17P_FRAGMENT_SIDE_EFFECTS
   if (getenv("G17P_FRAGMENT_SIDE_EFFECTS_RECORD_ONLY")) {
      puts("g17p-fragment-side-effects: record-only PASS");
      return EXIT_SUCCESS;
   }
#else
   if (getenv("G17P_GRAPHICS_SIDE_EFFECTS_RECORD_ONLY")) {
      puts("g17p-graphics-side-effects: record-only PASS");
      return EXIT_SUCCESS;
   }
#endif
#endif

   const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkFence fence;
   check(vkCreateFence(device, &fence_info, NULL, &fence), "vkCreateFence");
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");

   fprintf(stderr, "g17p-render-only: submitted\n");

   /* Five seconds: long enough for any real 3D kick, short enough that a
    * launch which never signals is reported rather than hanging the box. */
   const VkResult waited =
      vkWaitForFences(device, 1, &fence, true,
#ifdef G17P_CONDITIONAL_RENDERING
                      UINT64_C(2000000000));
#else
                      UINT64_C(5000000000));
#endif
   if (waited == VK_TIMEOUT) {
      fprintf(stderr, "g17p-render-only: TIMEOUT - fence never signalled\n");
      return EXIT_FAILURE;
   }
   if (waited != VK_SUCCESS) {
      fprintf(stderr, "g17p-render-only: vkWaitForFences returned %d\n",
              waited);
      return EXIT_FAILURE;
   }

#if defined(G17P_REPEAT_DRAW)
   unsigned submissions = 1;
   const char *requested_submits = getenv("G17P_REPEAT_SUBMITS");
   if (requested_submits) {
      char *end;
      const unsigned long n = strtoul(requested_submits, &end, 10);
      if (*end || n == 0 || n > 128)
         die("G17P_REPEAT_SUBMITS must be in 1..128");
      submissions = n;
   }
   uint8_t *repeat_pixels = NULL;
   if (getenv("G17P_REPEAT_READBACK")) {
      if (!image_host_visible)
         die("repeat pixel verification requires host-visible memory");
      check(vkMapMemory(device, image_memory, 0, IMAGE_BYTES, 0,
                        (void **)&repeat_pixels), "vkMapMemory(repeat image)");
   }
   const uint8_t repeat_expected[] = {0xff, 0x47, 0x0d, 0xff};
   for (unsigned iteration = 0; iteration < submissions; ++iteration) {
      if (iteration > 0) {
         /* Completed work no longer owns this image. Poisoning it prevents
          * an empty or dropped subsequent submit from passing on old pixels.
          * The render pass discards the prior layout/content on every use. */
         if (repeat_pixels)
            memset(repeat_pixels, 0xa5, IMAGE_BYTES);
         check(vkResetFences(device, 1, &fence), "vkResetFences(repeat)");
         check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit(repeat)");
         check(vkWaitForFences(device, 1, &fence, true, UINT64_C(5000000000)),
               "vkWaitForFences(repeat)");
      }
      if (repeat_pixels) {
         for (unsigned pixel = 0; pixel < IMAGE_WIDTH * IMAGE_HEIGHT; ++pixel) {
            if (memcmp(repeat_pixels + 4 * pixel, repeat_expected, 4)) {
               fprintf(stderr, "repeat submission %u: pixel %u differs\n",
                       iteration + 1, pixel);
               die("repeat submission failed exact pixel verification");
            }
         }
         fprintf(stderr, "repeat submission %u/%u: %u/%u pixels exact\n",
                 iteration + 1, submissions, IMAGE_WIDTH * IMAGE_HEIGHT,
                 IMAGE_WIDTH * IMAGE_HEIGHT);
      }
   }
   if (repeat_pixels) {
      sleep(3);
      for (unsigned pixel = 0; pixel < IMAGE_WIDTH * IMAGE_HEIGHT; ++pixel) {
         if (memcmp(repeat_pixels + 4 * pixel, repeat_expected, 4))
            die("repeat target changed after final completion");
      }
      vkUnmapMemory(device, image_memory);
   }
   printf("g17p-vulkan-repeat-draw: %u draws recorded and "
          "submitted %u times on one device and queue\n",
          repeats, submissions);
   /* The shim validates framing and stream counts but does not execute.
    * Hardware qualification must also use the exact pixel oracle below. */
   if (!getenv("G17P_REPEAT_READBACK"))
      return EXIT_SUCCESS;
#endif

#ifdef G17P_CONDITIONAL_RENDERING
   if (submit_only) {
      printf("g17p-vulkan-conditional-rendering-graphics: SUBMIT PASS — "
             "%s reached the G17P UAPI stream validator\n",
             argv[2]);
      return EXIT_SUCCESS;
   }
#endif

#ifdef G17P_CONDITIONAL_RENDERING
   if (memcmp(conditional_map, expected_conditional,
              sizeof(expected_conditional)) != 0)
      die("conditional predicate or guards changed");
#endif

#ifdef G17P_ANY_SIDE_EFFECTS
   for (unsigned i = 0; i < 4; ++i) {
      if (side_map->prefix_guard[i] != SIDE_EFFECT_GUARD ||
          side_map->suffix_guard[i] != SIDE_EFFECT_GUARD)
         die("graphics side effect crossed an SSBO guard");
   }
#ifdef G17P_GRAPHICS_SIDE_EFFECTS
   if (side_map->vertex_atomic != 3 ||
       side_map->vertex_values[0] != UINT32_C(0x17000000) ||
       side_map->vertex_values[1] != UINT32_C(0x17000001) ||
       side_map->vertex_values[2] != UINT32_C(0x17000002) ||
       side_map->fragment_atomic != IMAGE_WIDTH * IMAGE_HEIGHT ||
       side_map->fragment_value != UINT32_C(0xf17e0001))
      die("vertex/fragment SSBO stores or atomics are not exact");
#else
   if (side_map->vertex_atomic != 0 || side_map->vertex_values[0] != 0 ||
       side_map->vertex_values[1] != 0 || side_map->vertex_values[2] != 0 ||
       side_map->fragment_atomic != IMAGE_WIDTH * IMAGE_HEIGHT ||
       side_map->fragment_value != UINT32_C(0xf17e0001))
      die("fragment SSBO stores or atomics are not exact");
#endif

   const struct side_effect_output stable_side = *side_map;
   sleep(3);
   if (memcmp(side_map, &stable_side, sizeof(stable_side)) != 0)
      die("graphics side effects changed after three seconds");
#ifdef G17P_GRAPHICS_SIDE_EFFECTS
   puts("g17p-graphics-side-effects: vertex/fragment stores, atomics, guards "
        "and three-second stability exact");
#else
   puts("g17p-fragment-side-effects: fragment stores, atomics, untouched "
        "vertex fields, guards and three-second stability exact");
#endif
#endif

#ifdef G17P_OCCLUSION_QUERY
   struct query_result {
      uint64_t samples;
      uint64_t available;
   } query_results[2] = {{0}};
   check(vkGetQueryPoolResults(device, occlusion_pool, 0, 2,
                               sizeof(query_results), query_results,
                               sizeof(query_results[0]),
                               VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WAIT_BIT |
                                  VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),
         "vkGetQueryPoolResults(occlusion)");
#ifdef G17P_CONDITIONAL_RENDERING
   const uint64_t expected_samples =
      !clear_attachment && expect_execute ? IMAGE_WIDTH * IMAGE_HEIGHT : 0;
#else
   const uint64_t expected_samples = IMAGE_WIDTH * IMAGE_HEIGHT;
#endif
   fprintf(stderr,
           "g17p-render-only: occlusion samples=[%" PRIu64 ",%" PRIu64
           "] available=[%" PRIu64 ",%" PRIu64 "] expected=[%" PRIu64
           ",0]\n",
           query_results[0].samples, query_results[1].samples,
           query_results[0].available, query_results[1].available,
           expected_samples);
   if (query_results[0].available != 1 || query_results[1].available != 1 ||
       query_results[0].samples != expected_samples ||
       query_results[1].samples != 0)
      die("precise occlusion results differ from full and empty intervals");

   struct timestamp_result {
      uint64_t value;
      uint64_t available;
   } timestamp_results[2] = {{0}};
   check(vkGetQueryPoolResults(device, timestamp_pool, 0, 2,
                               sizeof(timestamp_results), timestamp_results,
                               sizeof(timestamp_results[0]),
                               VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WAIT_BIT |
                                  VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),
         "vkGetQueryPoolResults(timestamp)");
   fprintf(stderr,
           "g17p-render-only: timestamps=[%" PRIu64 ",%" PRIu64
           "] available=[%" PRIu64 ",%" PRIu64 "]\n",
           timestamp_results[0].value, timestamp_results[1].value,
           timestamp_results[0].available, timestamp_results[1].available);
   if (timestamp_results[0].available != 1 ||
       timestamp_results[1].available != 1 ||
       timestamp_results[0].value == 0 ||
       timestamp_results[1].value <= timestamp_results[0].value)
      die("timestamp interval is unavailable, zero, or non-increasing");
#endif

   /* Verify PIXELS, not just the fence. The clear is opaque black and the
    * vertex data is a full-viewport triangle, so a correct render leaves NO
    * cleared pixel behind. */
   if (!image_host_visible) {
      printf("g17p-render-only: INCONCLUSIVE - fence signalled but the render "
             "target is not host-visible, so no pixel was verified\n");
      return EXIT_FAILURE;
   }
   uint8_t *pixels = NULL;
   check(vkMapMemory(device, image_memory, 0, IMAGE_BYTES, 0,
                     (void **)&pixels),
         "vkMapMemory(image)");
   /* Classify every pixel into exactly three buckets. The expected colour is a
    * shader immediate and the clear is opaque black, so "still the clear
    * value" (never stored) and "some third value" (stored wrong) are different
    * failures with different causes, and a bare "is it non-black" test reports
    * the second one as a pass. */
   static const uint8_t cleared_pixel[4] = {0x00, 0x00, 0x00, 0xff};
   uint8_t expected_pixel[4] = {0xff, 0x47, 0x0d, 0xff};
#ifdef G17P_CONDITIONAL_RENDERING
   if (!expect_execute)
      memcpy(expected_pixel, cleared_pixel, sizeof(expected_pixel));
#else
   if (alternate_geometry && (frame_index & 1))
      memcpy(expected_pixel, cleared_pixel, sizeof(expected_pixel));
#endif

   char *classes = malloc(IMAGE_WIDTH * IMAGE_HEIGHT);
   if (!classes)
      die("out of memory classifying the render target");

   unsigned exact = 0, still_cleared = 0, wrong = 0;
   uint32_t first = 0;
   unsigned first_bad = UINT32_MAX;
   uint32_t first_bad_value = 0;
   for (unsigned i = 0; i < IMAGE_WIDTH * IMAGE_HEIGHT; i++) {
      const uint8_t *p = &pixels[i * 4];
      const uint32_t value = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                             ((uint32_t)p[2] << 8) | p[3];
      if (i == 0)
         first = value;

      if (memcmp(p, expected_pixel, sizeof(expected_pixel)) == 0) {
         classes[i] = '#';
         exact++;
         continue;
      }

      if (memcmp(p, cleared_pixel, sizeof(cleared_pixel)) == 0) {
         classes[i] = '.';
         still_cleared++;
      } else {
         classes[i] = '?';
         wrong++;
      }

      if (first_bad == UINT32_MAX) {
         first_bad = i;
         first_bad_value = value;
      }
   }

   const uint32_t expected_value = ((uint32_t)expected_pixel[0] << 24) |
                                   ((uint32_t)expected_pixel[1] << 16) |
                                   ((uint32_t)expected_pixel[2] << 8) |
                                   expected_pixel[3];
   fprintf(stderr,
           "g17p-render-only: pixels exact=%u/%u cleared=%u wrong=%u "
           "first=0x%08x expected=0x%08x\n",
           exact, IMAGE_WIDTH * IMAGE_HEIGHT, still_cleared, wrong, first,
           expected_value);
   if (first_bad != UINT32_MAX)
      fprintf(stderr,
              "g17p-render-only: first non-exact pixel (%u,%u)=0x%08x\n",
              first_bad % IMAGE_WIDTH, first_bad / IMAGE_WIDTH,
              first_bad_value);

   /* Report the granularity of any loss. A fragment SIMD group covers an 8x4
    * pixel footprint and a derivative quad covers 2x2, so counting how many of
    * each are uniform decides, from a single run, whether pixels are lost per
    * pixel, per quad, or per SIMD group. The previous map sampled one row in
    * four and one column in two, which cannot tell those apart. */
   struct {
      unsigned w, h;
      const char *name;
   } shapes[2] = {{2, 2, "quads(2x2)"}, {8, 4, "simd-groups(8x4)"}};
   for (unsigned s = 0; s < 2; ++s) {
      unsigned total = 0, mixed = 0;
      for (unsigned by = 0; by < IMAGE_HEIGHT; by += shapes[s].h) {
         for (unsigned bx = 0; bx < IMAGE_WIDTH; bx += shapes[s].w) {
            const char first_class = classes[by * IMAGE_WIDTH + bx];
            bool uniform = true;
            for (unsigned y = by; y < by + shapes[s].h && y < IMAGE_HEIGHT; ++y)
               for (unsigned x = bx; x < bx + shapes[s].w && x < IMAGE_WIDTH;
                    ++x)
                  uniform &= classes[y * IMAGE_WIDTH + x] == first_class;
            total++;
            mixed += !uniform;
         }
      }
      fprintf(stderr, "g17p-render-only: %s mixed=%u/%u\n", shapes[s].name,
              mixed, total);
   }

   /* Every row, every column: the map is the evidence, so it must not
    * subsample. '#' is exact, '.' is the untouched clear, '?' is a third
    * value. */
   if (IMAGE_WIDTH <= 128 && IMAGE_HEIGHT <= 128) {
      char *row = malloc(IMAGE_WIDTH + 1);
      if (!row)
         die("out of memory printing the render target");
      row[IMAGE_WIDTH] = 0;
      for (unsigned y = 0; y < IMAGE_HEIGHT; y++) {
         memcpy(row, classes + (size_t)y * IMAGE_WIDTH, IMAGE_WIDTH);
         fprintf(stderr, "g17p-render-only: %3u |%s|\n", y, row);
      }
      free(row);
   }
   free(classes);

   fprintf(stderr, "g17p-render-only: frame=%u/%u pixels_exact=%u/%u\n",
           frame_index + 1, frame_count, exact, IMAGE_WIDTH * IMAGE_HEIGHT);
   if ((blit_test || readback_test) && exact == IMAGE_WIDTH * IMAGE_HEIGHT) {
      VkFence blit_fence;
      check(vkCreateFence(device, &fence_info, NULL, &blit_fence),
            "vkCreateFence(blit)");
      const VkSubmitInfo copy_submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &blit_command,
      };
      check(vkQueueSubmit(queue, 1, &copy_submit, blit_fence), "vkQueueSubmit(blit)");
      check(vkWaitForFences(device, 1, &blit_fence, true, UINT64_C(5000000000)),
            "vkWaitForFences(blit)");
      unsigned copied = 0, sentinel = 0;
      for (unsigned i = 0; i < IMAGE_WIDTH * IMAGE_HEIGHT; ++i) {
         copied += !memcmp(blit_pixels + 4*i, expected_pixel, 4);
         sentinel += !memcmp(blit_pixels + 4*i, "\xa5\xa5\xa5\xa5", 4);
      }
      fprintf(stderr, "g17p-render-only: %s pixels exact=%u/%u sentinel=%u first=%02x%02x%02x%02x\n",
         blit_test ? "blit" : "readback", copied, IMAGE_WIDTH * IMAGE_HEIGHT, sentinel,
         blit_pixels[0], blit_pixels[1], blit_pixels[2], blit_pixels[3]);
      if (readback_test) {
         for (unsigned i = 0; i < 16; i++) {
            if (readback_map[i] != 0xa5 || readback_map[16 + IMAGE_BYTES + i] != 0xa5)
               die("readback changed guard bytes outside its destination");
         }
         fprintf(stderr, "g17p-render-only: readback guard bytes intact\n");
      }
      /* Retain and optionally export the actual destination for inspection. */
      pixels = blit_pixels;
      exact = copied;
   }

   const char *output_path = getenv("AGX_RENDER_OUTPUT");
   if (output_path) {
      FILE *output = fopen(output_path, "wb");
      if (!output)
         die("cannot open render output");
      const size_t written = fwrite(pixels, 1, IMAGE_BYTES, output);
      const int closed = fclose(output);
      if (written != IMAGE_BYTES || closed != 0)
         die("cannot write complete render output");
   }
   if (getenv("AGX_RENDER_HOLD") && frame_index + 1 == frame_count) {
      fprintf(stderr, "g17p-render-only: HOLD pid=%ld pixels_exact=%u/%u; "
                      "resources retained for inspection\n",
              (long)getpid(), exact, IMAGE_WIDTH * IMAGE_HEIGHT);
      fflush(stderr);
      for (;;)
         sleep(60);
   }
   if (exact != IMAGE_WIDTH * IMAGE_HEIGHT)
#ifdef G17P_CONDITIONAL_RENDERING
      die("render target differs from exact conditional result");
#else
      die("render target differs from the exact expected colour");
#endif
   if (memcmp(vertex_map, vertices, sizeof(vertices)) != 0)
      die("vertex source changed during query render");

   if (++frame_index < frame_count) {
      memset(pixels, 0xa5, IMAGE_BYTES);
      vkUnmapMemory(device, image_memory);
      goto next_render_frame;
   }

#ifdef G17P_OCCLUSION_QUERY
   sleep(3);
   struct query_result stable_queries[2] = {{0}};
   struct timestamp_result stable_timestamps[2] = {{0}};
   check(vkGetQueryPoolResults(device, occlusion_pool, 0, 2,
                               sizeof(stable_queries), stable_queries,
                               sizeof(stable_queries[0]),
                               VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),
         "vkGetQueryPoolResults(occlusion stable)");
   check(vkGetQueryPoolResults(device, timestamp_pool, 0, 2,
                               sizeof(stable_timestamps), stable_timestamps,
                               sizeof(stable_timestamps[0]),
                               VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),
         "vkGetQueryPoolResults(timestamp stable)");
   if (memcmp(stable_queries, query_results, sizeof(query_results)) != 0 ||
       memcmp(stable_timestamps, timestamp_results,
              sizeof(timestamp_results)) != 0 ||
       memcmp(vertex_map, vertices, sizeof(vertices)) != 0
#ifdef G17P_CONDITIONAL_RENDERING
       || memcmp(conditional_map, expected_conditional,
                 sizeof(expected_conditional)) != 0
#endif
   )
      die("query results or source changed after three seconds");
   for (unsigned i = 0; i < IMAGE_WIDTH * IMAGE_HEIGHT; ++i) {
      const uint8_t *p = &pixels[i * 4];
#ifdef G17P_CONDITIONAL_RENDERING
      const bool stable_pixel =
         expect_execute
            ? (p[0] == 0xff && p[1] == 0x47 && p[2] == 0x0d &&
               p[3] == 0xff)
            : (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0xff);
      if (!stable_pixel)
#else
      if (p[0] != 0xff || p[1] != 0x47 || p[2] != 0x0d || p[3] != 0xff)
#endif
         die("render target changed after three seconds");
   }
#endif

#ifdef G17P_CONDITIONAL_RENDERING
   printf("g17p-vulkan-conditional-rendering-graphics: PASS — %s %s, "
          "exact pixels=%u/%u, occlusion=%" PRIu64 "\n",
          clear_attachment ? "clear attachments"
                           : indexed_draw ? "direct indexed draw"
                           : indirect_draw ? "indirect draw"
                           : count_draw ? "indirect-count draw"
                                          : "direct draw",
          expect_execute ? "executed" : "discarded", exact,
          IMAGE_WIDTH * IMAGE_HEIGHT, expected_samples);
#else
   printf("g17p-render-only: PASS - %ux%u RGBA8, %u/%u exact pixels%s\n",
          IMAGE_WIDTH, IMAGE_HEIGHT, exact, IMAGE_WIDTH * IMAGE_HEIGHT,
#ifdef G17P_OCCLUSION_QUERY
          ", precise occlusion + timestamp interval stable"
#else
          ""
#endif
   );
#endif
   return EXIT_SUCCESS;
}
