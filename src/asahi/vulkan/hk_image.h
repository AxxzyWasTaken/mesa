/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "asahi/layout/layout.h"
#include "vulkan/vulkan_core.h"

#include "hk_private.h"

#include "vk_format.h"
#include "vk_image.h"

/* Because small images can end up with an array_stride_B that is less than
 * the sparse block size (in bytes), we have to set SINGLE_MIPTAIL_BIT when
 * advertising sparse properties to the client.  This means that we get one
 * single memory range for the miptail of the image.  For large images with
 * mipTailStartLod > 0, we have to deal with the array stride ourselves.
 *
 * We do this by returning HK_MIP_TAIL_START_OFFSET as the image's
 * imageMipTailOffset.  We can then detect anything with that address as
 * being part of the miptail and re-map it accordingly.  The Vulkan spec
 * explicitly allows for this.
 *
 * From the Vulkan 1.3.279 spec:
 *
 *    "When VK_SPARSE_MEMORY_BIND_METADATA_BIT is present, the resourceOffset
 *    must have been derived explicitly from the imageMipTailOffset in the
 *    sparse resource properties returned for the metadata aspect. By
 *    manipulating the value returned for imageMipTailOffset, the
 *    resourceOffset does not have to correlate directly to a device virtual
 *    address offset, and may instead be whatever value makes it easiest for
 *    the implementation to derive the correct device virtual address."
 */
#define HK_MIP_TAIL_START_OFFSET 0x6d74000000000000UL

struct hk_device_memory;
struct hk_physical_device;

static VkFormatFeatureFlags2
hk_get_image_plane_format_features(struct hk_physical_device *pdev,
                                   VkFormat vk_format, VkImageTiling tiling);

VkFormatFeatureFlags2
hk_get_image_format_features(struct hk_physical_device *pdevice,
                             VkFormat format, VkImageTiling tiling);

struct hk_image_plane {
   struct ail_layout layout;

   /* Byte offset of this DRM memory plane from the image memory binding. */
   uint64_t offset_B;
   uint64_t addr;
   struct agx_va *va;

   /* Bytes of that VA counted in hk_device::sparse_scratch_B. */
   uint64_t sparse_scratch_B;

   /* For host image copy */
   void *map;
   uint32_t rem;

   /* If the image has sparse residency, its residency is tracked in this
    * secondary page table. Otherwise, this map is NULL.
    */
   struct agx_bo *sparse_map;
};

struct hk_image {
   struct vk_image vk;

   /** True if the planes are bound separately
    *
    * This is set based on VK_IMAGE_CREATE_DISJOINT_BIT
    */
   bool disjoint;

   uint8_t plane_count;
   struct hk_image_plane planes[3];

   /** True for a swapchain image and for an image bound to swapchain memory.
    *
    * Such memory can be aliased by other images only, never by a buffer, so
    * work reads what it holds only through an image.
    */
   bool swapchain_memory;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(hk_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)

static inline uint64_t
hk_image_plane_base_address(const struct hk_image_plane *plane)
{
   return plane->addr;
}

static inline uint64_t
hk_image_base_address(const struct hk_image *image, uint8_t plane)
{
   return hk_image_plane_base_address(&image->planes[plane]);
}

/* The GPU VA range [*lo, *hi) covering every bound plane of the image. Empty
 * (*lo >= *hi) while no plane is bound. */
static inline void
hk_image_va_range(const struct hk_image *image, uint64_t *lo, uint64_t *hi)
{
   *lo = UINT64_MAX;
   *hi = 0;
   for (unsigned p = 0; p < image->plane_count; ++p) {
      const struct hk_image_plane *plane = &image->planes[p];
      if (!plane->addr)
         continue;
      *lo = MIN2(*lo, plane->addr);
      *hi = MAX2(*hi, plane->addr + plane->layout.size_B);
   }
}

/* Packed 4:2:2 contains two luma samples and one pair of chroma samples
 * per four-component word. The shader reconstructs the full-resolution texels. */
static inline unsigned
hk_packed_ycbcr_bits(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_G8B8G8R8_422_UNORM:
   case VK_FORMAT_B8G8R8G8_422_UNORM: return 8;
   case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
   case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16: return 10;
   case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
   case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16: return 12;
   case VK_FORMAT_G16B16G16R16_422_UNORM:
   case VK_FORMAT_B16G16R16G16_422_UNORM: return 16;
   default: return 0;
   }
}

static inline bool
hk_packed_ycbcr_luma_first(VkFormat format)
{
   return format == VK_FORMAT_G8B8G8R8_422_UNORM ||
          format == VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16 ||
          format == VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16 ||
          format == VK_FORMAT_G16B16G16R16_422_UNORM;
}

static inline enum pipe_format
hk_format_to_pipe_format(VkFormat vkformat)
{
   if (hk_packed_ycbcr_bits(vkformat) > 8)
      return PIPE_FORMAT_R16G16_R16B16_422_UNORM;
   switch (vkformat) {
   case VK_FORMAT_R10X6_UNORM_PACK16:
   case VK_FORMAT_R12X4_UNORM_PACK16:
      return PIPE_FORMAT_R16_UNORM;
   case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
   case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
      return PIPE_FORMAT_R16G16_UNORM;
   default:
      return vk_format_to_pipe_format(vkformat);
   }
}

/* Scaled image samples use integer texels plus shader conversion to float.
 * Byte layout and component swizzles are identical to these integer formats. */
static inline enum pipe_format
hk_scaled_texture_format(enum pipe_format format)
{
   switch (format) {
#define SCALED(base) \
   case PIPE_FORMAT_##base##_SSCALED: return PIPE_FORMAT_##base##_SINT; \
   case PIPE_FORMAT_##base##_USCALED: return PIPE_FORMAT_##base##_UINT;
   SCALED(R8) SCALED(R8G8) SCALED(R8G8B8) SCALED(R8G8B8A8)
   SCALED(B8G8R8) SCALED(B8G8R8A8)
   SCALED(R16) SCALED(R16G16) SCALED(R16G16B16) SCALED(R16G16B16A16)
   SCALED(R10G10B10A2)
   SCALED(B10G10R10A2)
#undef SCALED
   default: return PIPE_FORMAT_NONE;
   }
}

/* Unsigned scaled filtering can use the hardware UNORM filter: conversion
 * back to the integer-valued float domain is affine and commutes with filtering. */
static inline enum pipe_format
hk_uscaled_normalized_format(enum pipe_format format)
{
   switch (format) {
#define USCALED(base) \
   case PIPE_FORMAT_##base##_USCALED: return PIPE_FORMAT_##base##_UNORM;
   USCALED(R8) USCALED(R8G8) USCALED(R8G8B8) USCALED(R8G8B8A8)
   USCALED(B8G8R8) USCALED(B8G8R8A8)
   USCALED(R16) USCALED(R16G16) USCALED(R16G16B16) USCALED(R16G16B16A16)
   USCALED(R10G10B10A2) USCALED(B10G10R10A2)
#undef USCALED
   default: return PIPE_FORMAT_NONE;
   }
}

/* Optimal RGB images use four-component storage. Transfers pack/unpack the
 * three API components on the GPU; descriptors force alpha to one. */
static inline enum pipe_format
hk_padded_image_format(enum pipe_format format)
{
   switch (format) {
#define RGB(bits, type) \
   case PIPE_FORMAT_R##bits##G##bits##B##bits##_##type: \
      return PIPE_FORMAT_R##bits##G##bits##B##bits##A##bits##_##type;
   RGB(8, UNORM) RGB(8, SNORM) RGB(8, UINT) RGB(8, SINT) RGB(8, SRGB)
   RGB(16, UNORM) RGB(16, SNORM) RGB(16, UINT) RGB(16, SINT) RGB(16, FLOAT)
   RGB(8, USCALED) RGB(8, SSCALED)
   RGB(16, USCALED) RGB(16, SSCALED)
   RGB(32, UINT) RGB(32, SINT) RGB(32, FLOAT)
#undef RGB
#define BGR(type) \
   case PIPE_FORMAT_B8G8R8_##type: return PIPE_FORMAT_B8G8R8A8_##type;
   BGR(UNORM) BGR(SNORM) BGR(UINT) BGR(SINT) BGR(SRGB)
   BGR(USCALED) BGR(SSCALED)
#undef BGR
   default: return format;
   }
}

static inline bool
hk_split_depth_stencil(VkFormat format)
{
   return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
          format == VK_FORMAT_D16_UNORM_S8_UINT;
}

/* Neither packed float representation round-trips all storage bits through
 * float channels (NaN payloads in R11G11B10, shared exponents in RGB9E5).
 * AGX2 transfers use an R32_UINT view instead. Such a view requires an
 * uncompressed image: lossless compression is channel-class dependent.
 */
static inline bool
hk_agx2_packed_float_copy(enum agx_chip chip, VkFormat format)
{
   return agx_chip_isa_encoding(chip) == AGX_ISA_ENCODING_AGX2 &&
          (format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
           format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32);
}

static inline VkFormat
hk_depth_stencil_plane_format(VkFormat format, unsigned plane)
{
   if (!hk_split_depth_stencil(format))
      return format;
   return plane ? VK_FORMAT_S8_UINT :
      format == VK_FORMAT_D16_UNORM_S8_UINT ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
}

static inline uint8_t
hk_image_aspects_to_plane(const struct hk_image *image,
                          VkImageAspectFlags aspectMask)
{
   /* Must only be one aspect unless it's depth/stencil */
   assert(aspectMask ==
             (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ||
          util_bitcount(aspectMask) == 1);

   switch (aspectMask) {
   default:
      assert(aspectMask != VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT);
      return 0;

   case VK_IMAGE_ASPECT_STENCIL_BIT:
      return hk_split_depth_stencil(image->vk.format);

   case VK_IMAGE_ASPECT_PLANE_1_BIT:
   case VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT:
      return 1;

   case VK_IMAGE_ASPECT_PLANE_2_BIT:
   case VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT:
      return 2;
   }
}

struct agx_device;
bool hk_can_compress_format(const struct agx_device *dev, VkFormat format);

/* DRM_FORMAT_MOD_APPLE_INTERCHANGE: G17P-only, single-level,
 * single-layer, single-sample 2D colour images of the 32-bit RGBA formats
 * the display coprocessor scans out.  pNext may be NULL (format queries). */
bool hk_interchange_format(const struct agx_device *dev, VkFormat format);

struct hk_cmd_buffer;
void hk_clear_image(struct hk_cmd_buffer *cmd, struct hk_image *image,
                    enum pipe_format view_format, const uint32_t *clear_value,
                    const VkImageSubresourceRange *range, bool whole_3d);
