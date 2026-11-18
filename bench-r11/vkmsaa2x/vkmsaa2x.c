// vkmsaa2x: off-screen Vulkan frames on 2x MSAA colour + 2x D32_SFLOAT attachments, timed.
// (bench-r11 version: the bench-17nov program plus the r11g11b10f format, the VKM_EOT=1 mode and the resolve
// check below; with VKM_EOT unset, rgba8 and rgba16f record exactly the bench-17nov frames.)
// Per frame (dynamic rendering, no multiview): pass 0 clears and draws opaque geometry, passes 1..P-2 load the
// 2x attachments and draw blended geometry, pass P-1 loads them, draws, and resolves colour (average) and depth
// (sample 0) to 1x images; a post pass samples the resolved colour into a 1x RGBA8 image. Every pass stores
// both 2x attachments, so each frame moves the 2x surfaces through memory P times. Three frames in flight.
// After the timed frames, one verify pass reads BOTH samples of the 2x colour and depth bit-exactly; the
// hashes printed cover those samples and the resolved and post images, so two drivers can be compared.
// usage: vkmsaa2x <w> <h> <frames> <rgba8|rgba16f|r11g11b10f> <draws> [passes=4] [probe]
//   r11g11b10f: VK_FORMAT_B10G11R11_UFLOAT_PACK32 colour (the post pass still writes RGBA8).
//   VKM_EOT=1: the last pass's 2x colour has storeOp DONT_CARE, so its average resolve is the end-of-tile one
//   (the driver resolves while it writes the tile out) instead of a resolve after the pass (storeOp STORE).
//   Resolve check, after the timed frames: one more frame with the last pass's colour stored and resolved into
//   a second 1x image (a resolve after the pass). Prints how many pixels of the timed frames' resolve differ
//   from it, split into edge pixels (the two samples differ) and interior ones (equal samples), and the largest
//   channel difference. Without VKM_EOT both resolves take the same path.
//   probe: instead of timing, render a clear-only frame and then a full frame, and report for each 2x
//   attachment how many bytes past VKM_PLAIN_C / VKM_PLAIN_Z (the plain image sizes, from a run with
//   ASAHI_MESA_DEBUG=nocompress) the GPU changed between the two (needs host-visible image memory).
// Shaders: geo.vert geo.frag quad.vert post.frag verify.frag, compiled to <name>.spv with glslc, in the cwd.
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)
static VkDevice d; static VkPhysicalDeviceMemoryProperties mp;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
static VkShaderModule mod(const char *p) {
  FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); } fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  uint32_t *b = malloc(n); fread(b, 1, n, f); fclose(f);
  VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = b}; VkShaderModule m; CK(vkCreateShaderModule(d, &si, 0, &m)); return m; }
static uint32_t mtype(uint32_t bits, uint32_t want) {
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  fprintf(stderr, "no memory type 0x%x in 0x%x\n", want, bits); exit(2); }

// Decode one resolved texel to floats for the resolve check.
static float uf(uint32_t m, uint32_t e, int mb) { // unsigned small float: 5-bit exponent, mb-bit mantissa
  if (e == 31) return m ? NAN : INFINITY;
  return e ? ldexpf(1.0f + (float)m / (1 << mb), (int)e - 15) : ldexpf((float)m / (1 << mb), -14); }
static float hf(uint16_t h) { uint32_t s = h >> 15, e = h >> 10 & 31, m = h & 1023; float v = uf(m, e, 10); return s ? -v : v; }
static void unpack(VkFormat f, const uint8_t *p, float o[4]) {
  if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { uint32_t w; memcpy(&w, p, 4);
    o[0] = uf(w & 63, w >> 6 & 31, 6); o[1] = uf(w >> 11 & 63, w >> 17 & 31, 6); o[2] = uf(w >> 22 & 31, w >> 27 & 31, 5); o[3] = 1; }
  else if (f == VK_FORMAT_R16G16B16A16_SFLOAT) { uint16_t h[4]; memcpy(h, p, 8); for (int i = 0; i < 4; i++) o[i] = hf(h[i]); }
  else for (int i = 0; i < 4; i++) o[i] = p[i] / 255.0f; }

struct img { VkImage i; VkImageView v; VkDeviceMemory m; VkDeviceSize size; uint32_t type; const char *name; };
static struct img mkimg(const char *name, uint32_t w, uint32_t h, VkFormat f, VkSampleCountFlagBits s, VkImageUsageFlags u, VkImageAspectFlags a, uint32_t memflags) {
  struct img r = {.name = name};
  VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                          .samples = s, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u};
  fprintf(stderr, "vkmsaa2x: creating %s\n", name);
  CK(vkCreateImage(d, &ii, 0, &r.i)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(d, r.i, &mr);
  r.size = mr.size; r.type = mtype(mr.memoryTypeBits, memflags);
  VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = r.i};
  VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded, .allocationSize = mr.size, .memoryTypeIndex = r.type};
  CK(vkAllocateMemory(d, &ma, 0, &r.m)); CK(vkBindImageMemory(d, r.i, r.m, 0));
  VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r.i, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = {a, 0, 1, 0, 1}};
  CK(vkCreateImageView(d, &vi, 0, &r.v));
  printf("image %-10s %ux%u %ux size=%llu memtype=%u flags=0x%x\n", name, w, h, s, (unsigned long long)mr.size, r.type, mp.memoryTypes[r.type].propertyFlags);
  return r; }
static void ibar(VkCommandBuffer c, VkImage i, VkImageAspectFlags a, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss, VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = i, .subresourceRange = {a, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(c, ss, ds, 0, 0, 0, 0, 0, 1, &b); }
static void mbar(VkCommandBuffer c, VkPipelineStageFlags ss, VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
  VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da}; vkCmdPipelineBarrier(c, ss, ds, 0, 1, &b, 0, 0, 0, 0); }

static VkPipeline pipe(VkPipelineLayout pl, const char *vs, const char *fs, uint32_t ncol, const VkFormat *cf, VkFormat df, VkSampleCountFlagBits samples,
                       int depth_test, int depth_write, VkCompareOp cmp, int blend) {
  VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = mod(vs), .pName = "main"},
                                           {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = mod(fs), .pName = "main"}};
  VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = samples};
  VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = depth_test, .depthWriteEnable = depth_write, .depthCompareOp = cmp};
  VkPipelineColorBlendAttachmentState cba[2] = {{.blendEnable = blend, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                 .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                 .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf}, {.colorWriteMask = 0xf}};
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = ncol, .pAttachments = cba};
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
  VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = ncol, .pColorAttachmentFormats = cf, .depthAttachmentFormat = df};
  VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pri, .stageCount = 2, .pStages = st, .pVertexInputState = &vin,
                                      .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                      .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pl};
  VkPipeline p; CK(vkCreateGraphicsPipelines(d, 0, 1, &gpi, 0, &p)); return p; }

struct push { int pass; float alpha; float zbias; };
static uint32_t W, H; static int P, D;
static struct img msc, msz, rc, rz, post, vc, vz, rc2; static int EOT;
static VkPipeline p_opaque, p_blend, p_post, p_verify; static VkPipelineLayout pl; static VkDescriptorSet set;
static VkQueryPool qp;

static VkRenderingAttachmentInfo att(VkImageView v, VkImageLayout l, VkAttachmentLoadOp lo, VkClearValue cv) {
  return (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v, .imageLayout = l, .loadOp = lo, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = cv}; }
static void begin(VkCommandBuffer c, uint32_t ncol, VkRenderingAttachmentInfo *col, VkRenderingAttachmentInfo *dep) {
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1, .colorAttachmentCount = ncol, .pColorAttachments = col, .pDepthAttachment = dep};
  vkCmdBeginRendering(c, &ri);
  VkViewport vp = {0, 0, W, H, 0, 1}; VkRect2D sc = {{0, 0}, {W, H}}; vkCmdSetViewport(c, 0, 1, &vp); vkCmdSetScissor(c, 0, 1, &sc); }
#define ATT_STAGES (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
#define ATT_ACCESS (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)

// One frame. draws=0 records a clear-only frame (pass 0 without geometry, nothing else).
static void record_frame(VkCommandBuffer c, int slot, int draws, VkClearColorValue clear, int eot, struct img *res) {
  if (slot >= 0) { vkCmdResetQueryPool(c, qp, slot * 2, 2); vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, slot * 2); }
  mbar(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  ibar(c, res->i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  VkClearValue cc = {.color = clear}, cz = {.depthStencil = {1.0f, 0}};
  int passes = draws ? P : 1;
  for (int p = 0; p < passes; p++) {
    VkRenderingAttachmentInfo ca = att(msc.v, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, p ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, cc);
    VkRenderingAttachmentInfo za = att(msz.v, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, p ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, cz);
    if (p == P - 1) {
      ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT; ca.resolveImageView = res->v; ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      if (eot) ca.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      za.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; za.resolveImageView = rz.v; za.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    begin(c, 1, &ca, &za);
    if (draws) {
      struct push pc = {p, p ? 0.5f : 1.0f, p ? -0.02f * p : 0.0f};
      vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p ? p_blend : p_opaque);
      vkCmdPushConstants(c, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
      vkCmdDraw(c, 3 * (p == 0 ? draws : p == P - 1 ? (draws + 3) / 4 : (draws + 1) / 2), 1, 0, 0);
    }
    vkCmdEndRendering(c);
    mbar(c, ATT_STAGES, ATT_ACCESS, ATT_STAGES, ATT_ACCESS);
  }
  if (draws) {
    ibar(c, res->i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkRenderingAttachmentInfo pa = att(post.v, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE, cc);
    begin(c, 1, &pa, 0);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p_post);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, 0);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRendering(c);
  } else {
    ibar(c, res->i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0);
  }
  if (slot >= 0) vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, slot * 2 + 1);
}

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage: vkmsaa2x w h frames rgba8|rgba16f|r11g11b10f draws [passes] [probe]\n"); return 1; }
  W = atoi(argv[1]); H = atoi(argv[2]); int F = atoi(argv[3]); D = atoi(argv[5]); P = argc > 6 ? atoi(argv[6]) : 4;
  int probe = argc > 7 && !strcmp(argv[7], "probe");
  VkFormat CF = !strcmp(argv[4], "rgba16f") ? VK_FORMAT_R16G16B16A16_SFLOAT : !strcmp(argv[4], "r11g11b10f") ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : VK_FORMAT_R8G8B8A8_UNORM, ZF = VK_FORMAT_D32_SFLOAT;
  const uint32_t cbpp = CF == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
  EOT = getenv("VKM_EOT") && atoi(getenv("VKM_EOT"));
  if (P < 2) P = 2;

  VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai}; VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
  uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv}; vkGetPhysicalDeviceProperties2(pd, &props2);
  VkPhysicalDeviceProperties props = props2.properties;
  VkQueueFamilyProperties qf[8]; uint32_t nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
  printf("driver %s | %s %s | timestamp bits %u period %.3f ns\n", props.deviceName, drv.driverName, drv.driverInfo, qf[0].timestampValidBits, props.limits.timestampPeriod);
  float pr = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &pr};
  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
  CK(vkCreateDevice(pd, &dci, 0, &d));
  VkQueue q; vkGetDeviceQueue(d, 0, 0, &q);

  uint32_t want = probe ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  msc = mkimg("ms-colour", W, H, CF, VK_SAMPLE_COUNT_2_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, want);
  msz = mkimg("ms-depth", W, H, ZF, VK_SAMPLE_COUNT_2_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, want);
  rc = mkimg("resolved", W, H, CF, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!probe) rc2 = mkimg("resolved-2", W, H, CF, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  rz = mkimg("res-depth", W, H, ZF, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  post = mkimg("post", W, H, VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vc = mkimg("verify-c", W, H, VK_FORMAT_R32G32B32A32_UINT, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vz = mkimg("verify-z", W, H, VK_FORMAT_R32G32B32A32_UINT, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
  VkSampler lin, nea; CK(vkCreateSampler(d, &sci, 0, &lin)); sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; CK(vkCreateSampler(d, &sci, 0, &nea));
  VkDescriptorSetLayoutBinding b[3]; for (int i = 0; i < 3; i++) b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
  VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b}; VkDescriptorSetLayout dl; CK(vkCreateDescriptorSetLayout(d, &dli, 0, &dl));
  VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(struct push)};
  VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
  CK(vkCreatePipelineLayout(d, &pli, 0, &pl));
  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps}; VkDescriptorPool dp; CK(vkCreateDescriptorPool(d, &dpi, 0, &dp));
  VkDescriptorSetAllocateInfo dsa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl}; CK(vkAllocateDescriptorSets(d, &dsa, &set));
  VkDescriptorImageInfo dii[3] = {{nea, msc.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {nea, msz.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {lin, rc.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkWriteDescriptorSet wr[3]; for (int i = 0; i < 3; i++) wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
                                                                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii[i]};
  vkUpdateDescriptorSets(d, 3, wr, 0, 0);
  VkFormat rgba8 = VK_FORMAT_R8G8B8A8_UNORM, vf[2] = {VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT};
  p_opaque = pipe(pl, "geo.vert.spv", "geo.frag.spv", 1, &CF, ZF, VK_SAMPLE_COUNT_2_BIT, 1, 1, VK_COMPARE_OP_LESS, 0);
  p_blend = pipe(pl, "geo.vert.spv", "geo.frag.spv", 1, &CF, ZF, VK_SAMPLE_COUNT_2_BIT, 1, 1, VK_COMPARE_OP_LESS_OR_EQUAL, 1);
  p_post = pipe(pl, "quad.vert.spv", "post.frag.spv", 1, &rgba8, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0, 0, VK_COMPARE_OP_ALWAYS, 0);
  p_verify = pipe(pl, "quad.vert.spv", "verify.frag.spv", 2, vf, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0, 0, VK_COMPARE_OP_ALWAYS, 0);
  VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8}; CK(vkCreateQueryPool(d, &qpi, 0, &qp));

  VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT}; VkCommandPool cp; CK(vkCreateCommandPool(d, &cpi, 0, &cp));
  VkCommandBuffer cb[4]; VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 4};
  CK(vkAllocateCommandBuffers(d, &cai, cb));
  VkFence fe[4]; VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; for (int i = 0; i < 4; i++) CK(vkCreateFence(d, &fci, 0, &fe[i]));
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
#define SUBMIT(c, f) do { VkSubmitInfo si_ = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &(c)}; CK(vkQueueSubmit(q, 1, &si_, (f))); } while (0)
#define WAIT(f) do { CK(vkWaitForFences(d, 1, &(f), 1, UINT64_MAX)); CK(vkResetFences(d, 1, &(f))); } while (0)

  // Initial layouts.
  CK(vkBeginCommandBuffer(cb[3], &bi));
  ibar(cb[3], msc.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, ATT_STAGES, ATT_ACCESS);
  ibar(cb[3], msz.i, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, ATT_STAGES, ATT_ACCESS);
  ibar(cb[3], rc.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0);
  if (!probe) ibar(cb[3], rc2.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0);
  ibar(cb[3], rz.i, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, ATT_STAGES, ATT_ACCESS);
  ibar(cb[3], post.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, ATT_STAGES, ATT_ACCESS);
  ibar(cb[3], vc.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, ATT_STAGES, ATT_ACCESS);
  ibar(cb[3], vz.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, ATT_STAGES, ATT_ACCESS);
  CK(vkEndCommandBuffer(cb[3])); SUBMIT(cb[3], fe[3]); WAIT(fe[3]);

  VkClearColorValue clear0 = {.float32 = {0.1f, 0.2f, 0.3f, 1.0f}};
  if (probe) {
    // Clear-only frame, snapshot, full frame, snapshot: count changed bytes past the plain image size.
    const char *pc = getenv("VKM_PLAIN_C"), *pz = getenv("VKM_PLAIN_Z");
    VkDeviceSize plain[2] = {pc ? strtoull(pc, 0, 10) : 0, pz ? strtoull(pz, 0, 10) : 0};
    struct img *ims[2] = {&msc, &msz}; uint8_t *snap[2]; void *map[2];
    for (int k = 0; k < 2; k++) { CK(vkMapMemory(d, ims[k]->m, 0, VK_WHOLE_SIZE, 0, &map[k])); snap[k] = malloc(ims[k]->size); }
    CK(vkBeginCommandBuffer(cb[0], &bi)); record_frame(cb[0], -1, 0, clear0, EOT, &rc); CK(vkEndCommandBuffer(cb[0])); SUBMIT(cb[0], fe[0]); WAIT(fe[0]);
    for (int k = 0; k < 2; k++) memcpy(snap[k], map[k], ims[k]->size);
    CK(vkBeginCommandBuffer(cb[0], &bi)); record_frame(cb[0], -1, D, clear0, EOT, &rc); CK(vkEndCommandBuffer(cb[0])); SUBMIT(cb[0], fe[0]); WAIT(fe[0]);
    for (int k = 0; k < 2; k++) {
      const uint8_t *a = snap[k], *bb = map[k]; VkDeviceSize sz = ims[k]->size, p0 = plain[k] && plain[k] < sz ? plain[k] : sz;
      size_t body = 0, tail = 0, tail2 = 0, nz_clear = 0, nz_draw = 0;
      for (VkDeviceSize i = 0; i < p0; i++) body += a[i] != bb[i];
      for (VkDeviceSize i = p0; i < sz; i++) { tail += a[i] != bb[i]; nz_clear += a[i] != 0; nz_draw += bb[i] != 0; if (i >= p0 + (sz - p0) / 2) tail2 += a[i] != bb[i]; }
      printf("probe %-10s size=%llu plain=%llu: bytes changed clear->draw: first %llu bytes %zu, past them %zu of %llu (last half %zu); nonzero past them: clear %zu, draw %zu\n",
             ims[k]->name, (unsigned long long)sz, (unsigned long long)plain[k], (unsigned long long)p0, body, tail, (unsigned long long)(sz - p0), tail2, nz_clear, nz_draw);
    }
    return 0;
  }

  // Timed frames: 3 in flight, round robin.
  for (int i = 0; i < 3; i++) { CK(vkBeginCommandBuffer(cb[i], &bi)); record_frame(cb[i], i, D, clear0, EOT, &rc); CK(vkEndCommandBuffer(cb[i])); }
  int warm = 10; double t0 = 0, gpu = 0; int ngpu = 0; int pending[3] = {0, 0, 0};
  for (int fr = 0; fr < F + warm; fr++) {
    int s = fr % 3;
    if (pending[s]) {
      WAIT(fe[s]); pending[s] = 0;
      uint64_t ts[2];
      if (fr - 3 >= warm && vkGetQueryPoolResults(d, qp, s * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) { gpu += (ts[1] - ts[0]) * props.limits.timestampPeriod * 1e-6; ngpu++; }
    }
    if (fr == warm) { for (int k = 0; k < 3; k++) if (pending[k]) { WAIT(fe[k]); pending[k] = 0; } t0 = now(); }
    SUBMIT(cb[s], fe[s]); pending[s] = 1;
  }
  for (int k = 0; k < 3; k++) if (pending[k]) WAIT(fe[k]);
  double t = now() - t0;

  // Resolve check frame: the same frame with the last pass's colour stored, resolved into rc2 after the pass.
  // The verify pass below then reads this frame's samples.
  CK(vkBeginCommandBuffer(cb[3], &bi)); record_frame(cb[3], -1, D, clear0, 0, &rc2); CK(vkEndCommandBuffer(cb[3])); SUBMIT(cb[3], fe[3]); WAIT(fe[3]);

  // Verify: both samples of colour and depth, plus the resolved, depth-resolve and post images.
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  struct { struct img *im; VkImageAspectFlags a; VkImageLayout l; uint32_t bpp; VkBuffer b; VkDeviceMemory m; } out[6] = {
    {&vc, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 16}, {&vz, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 16},
    {&rc, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cbpp},
    {&rz, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 4}, {&post, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 4},
    {&rc2, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cbpp}};
  for (int k = 0; k < 6; k++) {
    bci.size = (VkDeviceSize)W * H * out[k].bpp; CK(vkCreateBuffer(d, &bci, 0, &out[k].b)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d, out[k].b, &mr);
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CK(vkAllocateMemory(d, &ma, 0, &out[k].m)); CK(vkBindBufferMemory(d, out[k].b, out[k].m, 0));
  }
  VkCommandBuffer c = cb[3]; CK(vkBeginCommandBuffer(c, &bi));
  mbar(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  ibar(c, msc.i, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ATT_STAGES, ATT_ACCESS, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  ibar(c, msz.i, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ATT_STAGES, ATT_ACCESS, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  VkClearValue z0 = {0};
  VkRenderingAttachmentInfo va[2] = {att(vc.v, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE, z0), att(vz.v, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE, z0)};
  begin(c, 2, va, 0); vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p_verify);
  vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, 0); vkCmdDraw(c, 3, 1, 0, 0); vkCmdEndRendering(c);
  for (int k = 0; k < 6; k++) {
    ibar(c, out[k].im->i, out[k].a, out[k].l, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy r = {.imageSubresource = {out[k].a, 0, 0, 1}, .imageExtent = {W, H, 1}};
    vkCmdCopyImageToBuffer(c, out[k].im->i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out[k].b, 1, &r);
  }
  mbar(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  CK(vkEndCommandBuffer(c)); SUBMIT(c, fe[3]); WAIT(fe[3]);
  uint64_t hh[6]; size_t edge = 0, nz[6] = {0}; void *pm[6];
  for (int k = 0; k < 6; k++) {
    void *p; CK(vkMapMemory(d, out[k].m, 0, VK_WHOLE_SIZE, 0, &p)); pm[k] = p; hh[k] = fnv(p, (size_t)W * H * out[k].bpp);
    for (size_t i = 0; i < (size_t)W * H * out[k].bpp; i++) nz[k] += ((const uint8_t *)p)[i] != 0;
    if (k == 0) for (size_t i = 0; i < (size_t)W * H; i++) { const uint32_t *v = (const uint32_t *)p + i * 4; edge += v[0] != v[2] || v[1] != v[3]; }
  }
  // Resolve check: the timed frames' resolve (rc) against the stored frame's resolve after the pass (rc2).
  size_t diff_edge = 0, diff_interior = 0; double maxd = 0; uint32_t first_a = 0, first_b = 0; int have_first = 0;
  for (size_t i = 0; i < (size_t)W * H; i++) {
    const uint8_t *a = (const uint8_t *)pm[2] + i * cbpp, *b = (const uint8_t *)pm[5] + i * cbpp;
    if (!memcmp(a, b, cbpp)) continue;
    const uint32_t *v = (const uint32_t *)pm[0] + i * 4;
    if (v[0] != v[2] || v[1] != v[3]) diff_edge++; else diff_interior++;
    float fa[4], fb[4]; unpack(CF, a, fa); unpack(CF, b, fb);
    for (int ch = 0; ch < 4; ch++) { double dd = fabs((double)fa[ch] - fb[ch]); if (!(dd <= maxd)) maxd = dd; }
    if (!have_first) { memcpy(&first_a, a, 4); memcpy(&first_b, b, 4); have_first = 1; }
  }
  for (int k = 0; k < 6; k++) vkUnmapMemory(d, out[k].m);
  printf("vkmsaa2x %ux%u %s 2x passes=%d draws=%d frames=%d%s: %.3f ms/frame wall, gpu %s%.3f ms/frame (%d frames) | samples=%016llx depth-samples=%016llx resolved=%016llx depth-resolve=%016llx post=%016llx edge-pixels=%zu nonzero-bytes=%zu/%zu/%zu/%zu/%zu\n",
         W, H, argv[4], P, D, F, EOT ? " eot" : "", t * 1000 / F, ngpu ? "" : "n/a ", ngpu ? gpu / ngpu : 0.0, ngpu, (unsigned long long)hh[0], (unsigned long long)hh[1], (unsigned long long)hh[2],
         (unsigned long long)hh[3], (unsigned long long)hh[4], edge, nz[0], nz[1], nz[2], nz[3], nz[4]);
  printf("resolve-check %s%s: resolved-after-pass=%016llx; timed resolve differs on %zu of %zu pixels (%zu edge, %zu interior), max channel difference %g",
         argv[4], EOT ? " eot" : "", (unsigned long long)hh[5], diff_edge + diff_interior, (size_t)W * H, diff_edge, diff_interior, maxd);
  if (have_first) printf(", first differing word %08x vs %08x", first_a, first_b);
  printf("\n");
  return 0;
}
