/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx_bg_eot_key.h"
#include "util/format/u_format.h"

enum pipe_format
agx_g17p_rt0_pbe_format(enum pipe_format format)
{
   /* R11G11B10 and R9G9B9E5 are packed explicitly into one tile word on
    * G17P. Store that word without another format conversion. Other formats
    * retain the existing raw-sRGB rule. */
   return (format == PIPE_FORMAT_R11G11B10_FLOAT ||
           format == PIPE_FORMAT_R9G9B9E5_FLOAT)
             ? PIPE_FORMAT_R32_UINT
             : util_format_linear(format);
}

enum pipe_format
agx_rt0_pbe_format(enum agx_chip chip, enum pipe_format format)
{
   if (format == PIPE_FORMAT_R11G11B10_FLOAT &&
       agx_rg11b10_tile_is_packed_float(chip))
      return format;

   return agx_g17p_rt0_pbe_format(format);
}

uint8_t
agx_mrt_eot_selector(enum agx_chip chip, enum pipe_format format)
{
   /* A packed-float tile word is a packed 32-bit lane, like RGB10A2. */
   if (format == PIPE_FORMAT_R11G11B10_FLOAT &&
       agx_rg11b10_tile_is_packed_float(chip))
      return 0x75;

   return agx_g17p_mrt_eot_selector(format);
}

uint8_t
agx_g17p_mrt_eot_selector(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R8_UNORM:
   case PIPE_FORMAT_R8G8_UNORM:
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_R8_SNORM:
   case PIPE_FORMAT_R8G8_SNORM:
   case PIPE_FORMAT_R8G8B8A8_SNORM:
   case PIPE_FORMAT_R8G8B8X8_SNORM:
   case PIPE_FORMAT_B8G8R8A8_SNORM:
   case PIPE_FORMAT_R8_SRGB:
   case PIPE_FORMAT_R8G8_SRGB:
   case PIPE_FORMAT_R8G8B8A8_SRGB:
   case PIPE_FORMAT_R8G8B8X8_SRGB:
   case PIPE_FORMAT_B8G8R8A8_SRGB:
   case PIPE_FORMAT_B8G8R8X8_SRGB:
   case PIPE_FORMAT_R10G10B10A2_UNORM:
   case PIPE_FORMAT_B10G10R10A2_UNORM:
      return 0x75;

   case PIPE_FORMAT_R8_UINT:
   case PIPE_FORMAT_R8G8_UINT:
   case PIPE_FORMAT_R8G8B8A8_UINT:
   case PIPE_FORMAT_R8G8B8X8_UINT:
   case PIPE_FORMAT_B8G8R8A8_UINT:
   case PIPE_FORMAT_R8_SINT:
   case PIPE_FORMAT_R8G8_SINT:
   case PIPE_FORMAT_R8G8B8A8_SINT:
   case PIPE_FORMAT_R8G8B8X8_SINT:
   case PIPE_FORMAT_B8G8R8A8_SINT:
      return 0x55;

   case PIPE_FORMAT_R16_UNORM:
   case PIPE_FORMAT_R16G16_UNORM:
   case PIPE_FORMAT_R16G16B16A16_UNORM:
   case PIPE_FORMAT_R16_SNORM:
   case PIPE_FORMAT_R16G16_SNORM:
   case PIPE_FORMAT_R16G16B16A16_SNORM:
      return 0x95;

   case PIPE_FORMAT_R16_UINT:
   case PIPE_FORMAT_R16G16_UINT:
   case PIPE_FORMAT_R16G16B16A16_UINT:
   case PIPE_FORMAT_R16G16B16X16_UINT:
   case PIPE_FORMAT_R16_SINT:
   case PIPE_FORMAT_R16G16_SINT:
   case PIPE_FORMAT_R16G16B16A16_SINT:
   case PIPE_FORMAT_R16G16B16X16_SINT:
   case PIPE_FORMAT_R16_FLOAT:
   case PIPE_FORMAT_R16G16_FLOAT:
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
   case PIPE_FORMAT_R16G16B16X16_FLOAT:
   case PIPE_FORMAT_R10G10B10A2_UINT:
   case PIPE_FORMAT_B10G10R10A2_UINT:
   case PIPE_FORMAT_R5G6B5_UNORM:
   case PIPE_FORMAT_B5G6R5_UNORM:
   case PIPE_FORMAT_A1B5G5R5_UNORM:
   case PIPE_FORMAT_A1R5G5B5_UNORM:
   case PIPE_FORMAT_R5G5B5A1_UNORM:
   case PIPE_FORMAT_B5G5R5A1_UNORM:
   case PIPE_FORMAT_R5G5B5X1_UNORM:
   case PIPE_FORMAT_B5G5R5X1_UNORM:
   case PIPE_FORMAT_R4G4B4A4_UNORM:
   case PIPE_FORMAT_B4G4R4A4_UNORM:
   case PIPE_FORMAT_A4B4G4R4_UNORM:
   case PIPE_FORMAT_A4R4G4B4_UNORM:
      return 0x15;

   case PIPE_FORMAT_R32_UINT:
   case PIPE_FORMAT_R32G32_UINT:
   case PIPE_FORMAT_R32G32B32A32_UINT:
   case PIPE_FORMAT_R32G32B32X32_UINT:
   case PIPE_FORMAT_R32_SINT:
   case PIPE_FORMAT_R32G32_SINT:
   case PIPE_FORMAT_R32G32B32A32_SINT:
   case PIPE_FORMAT_R32G32B32X32_SINT:
   case PIPE_FORMAT_R32_FLOAT:
   case PIPE_FORMAT_R32G32_FLOAT:
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
   case PIPE_FORMAT_R32G32B32X32_FLOAT:
   /* RGB9E5 and R11G11B10 are packed into one raw R32 word before the tile
    * store, and their PBE is likewise R32_UINT. Multi-target EOT therefore
    * uses the grounded W32 integer selector rather than asking IMGWBLK to
    * convert it again. */
   case PIPE_FORMAT_R11G11B10_FLOAT:
   case PIPE_FORMAT_R9G9B9E5_FLOAT:
      return 0x35;

   default:
      return 0;
   }
}

/* The shape of the RT0 end-of-tile store follows from the logical format
 * alone -- how many imageblock words it occupies and which raw store the
 * tilebuffer lowering leaves behind -- so both AGX3 generations configure it
 * the same way. Only the native program text differs between them.
 */
void
agx_g17p_configure_rt0_eot_key(enum pipe_format format,
                               struct agx_shader_key *key)
{
   if (!key)
      return;

   key->g17p_rt0_eot_rgb565 = format == PIPE_FORMAT_R5G6B5_UNORM ||
                              format == PIPE_FORMAT_B5G6R5_UNORM;
   key->g17p_rt0_eot_norm16_selector_95 =
      format == PIPE_FORMAT_R16_UNORM || format == PIPE_FORMAT_R16_SNORM ||
      format == PIPE_FORMAT_R16G16_UNORM ||
      format == PIPE_FORMAT_R16G16_SNORM ||
      format == PIPE_FORMAT_R16G16B16A16_SNORM;
   key->g17p_rt0_eot_i16_signed_two_words =
      format == PIPE_FORMAT_R16G16B16A16_SINT ||
      format == PIPE_FORMAT_R16G16B16X16_SINT;
   key->g17p_rt0_eot_i32_selector_15 =
      format == PIPE_FORMAT_R32G32_SINT;
   key->g17p_rt0_eot_i32_selector_75 =
      format == PIPE_FORMAT_R32G32_UINT;

   key->g17p_rt0_eot_words = 0;
   /* R11G11B10 is a raw I32 word unless the tile keeps M5's packed-float
    * format. G18G gathers its key as G17G (agx_gather_device_key), so
    * key->dev.chip alone would pick the M5 rule; its end-of-tile lowering
    * (agx_bg_eot.c) tests the real chip and stores one raw R32_UINT word, so
    * the key has to follow agx_is_g18g() to match that store. */
   const bool rg11b10_raw_word =
      agx_is_g18g(&key->dev) ||
      !agx_rg11b10_tile_is_packed_float(key->dev.chip);
   key->g17p_rt0_eot_i32 =
      format == PIPE_FORMAT_R32_FLOAT ||
      format == PIPE_FORMAT_R32G32_FLOAT || format == PIPE_FORMAT_R32_UINT ||
      format == PIPE_FORMAT_R32_SINT || format == PIPE_FORMAT_R32G32_SINT ||
      format == PIPE_FORMAT_R32G32_UINT ||
      (format == PIPE_FORMAT_R11G11B10_FLOAT && rg11b10_raw_word) ||
      format == PIPE_FORMAT_R9G9B9E5_FLOAT ||
      format == PIPE_FORMAT_R32G32B32X32_FLOAT ||
      format == PIPE_FORMAT_R32G32B32A32_FLOAT ||
      format == PIPE_FORMAT_R32G32B32X32_UINT ||
      format == PIPE_FORMAT_R32G32B32A32_UINT ||
      format == PIPE_FORMAT_R32G32B32X32_SINT ||
      format == PIPE_FORMAT_R32G32B32A32_SINT;
   key->g17p_rt0_eot_i32_four_words =
      format == PIPE_FORMAT_R32G32B32X32_FLOAT ||
      format == PIPE_FORMAT_R32G32B32A32_FLOAT ||
      format == PIPE_FORMAT_R32G32B32X32_UINT ||
      format == PIPE_FORMAT_R32G32B32A32_UINT;
   key->g17p_rt0_eot_i32_signed_four_words =
      format == PIPE_FORMAT_R32G32B32A32_SINT ||
      format == PIPE_FORMAT_R32G32B32X32_SINT;

   switch (format) {
   case PIPE_FORMAT_R32_FLOAT:
   case PIPE_FORMAT_R32_UINT:
   case PIPE_FORMAT_R32_SINT:
   case PIPE_FORMAT_R11G11B10_FLOAT:
   case PIPE_FORMAT_R9G9B9E5_FLOAT:
   case PIPE_FORMAT_R8_UINT:
   case PIPE_FORMAT_R8_SINT:
   case PIPE_FORMAT_R8G8_UINT:
   case PIPE_FORMAT_R8G8_SINT:
   case PIPE_FORMAT_R8G8B8X8_UINT:
   case PIPE_FORMAT_R8G8B8A8_UINT:
   case PIPE_FORMAT_B8G8R8X8_UINT:
   case PIPE_FORMAT_B8G8R8A8_UINT:
   case PIPE_FORMAT_R8G8B8X8_SINT:
   case PIPE_FORMAT_R8G8B8A8_SINT:
   case PIPE_FORMAT_B8G8R8X8_SINT:
   case PIPE_FORMAT_B8G8R8A8_SINT:
   case PIPE_FORMAT_R16_FLOAT:
   case PIPE_FORMAT_R16_UNORM:
   case PIPE_FORMAT_R16_SNORM:
   case PIPE_FORMAT_R16_UINT:
   case PIPE_FORMAT_R16_SINT:
   case PIPE_FORMAT_R16G16_FLOAT:
   case PIPE_FORMAT_R16G16_UNORM:
   case PIPE_FORMAT_R16G16_SNORM:
   case PIPE_FORMAT_R16G16_UINT:
   case PIPE_FORMAT_R16G16_SINT:
   case PIPE_FORMAT_R10G10B10A2_UINT:
   case PIPE_FORMAT_B10G10R10A2_UINT:
      key->g17p_rt0_eot_words = 1;
      break;
   case PIPE_FORMAT_R5G6B5_UNORM:
   case PIPE_FORMAT_B5G6R5_UNORM:
   /* The packed 5551/4444 formats expand to four F16 tile lanes. The PBE
    * performs the final memory packing, so their EOT shape is two words just
    * like RGBA16F rather than the distinct three-lane RGB565 program. */
   case PIPE_FORMAT_A1B5G5R5_UNORM:
   case PIPE_FORMAT_A1R5G5B5_UNORM:
   case PIPE_FORMAT_R5G5B5A1_UNORM:
   case PIPE_FORMAT_B5G5R5A1_UNORM:
   case PIPE_FORMAT_R5G5B5X1_UNORM:
   case PIPE_FORMAT_B5G5R5X1_UNORM:
   case PIPE_FORMAT_R4G4B4A4_UNORM:
   case PIPE_FORMAT_B4G4R4A4_UNORM:
   case PIPE_FORMAT_A4B4G4R4_UNORM:
   case PIPE_FORMAT_A4R4G4B4_UNORM:
   case PIPE_FORMAT_R32G32_FLOAT:
   /* Gallium stores API RGB16 targets in the four-lane X16 physical family;
    * the unused lane still occupies the second imageblock word. */
   case PIPE_FORMAT_R16G16B16X16_FLOAT:
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
   case PIPE_FORMAT_R16G16B16A16_UNORM:
   case PIPE_FORMAT_R16G16B16A16_SNORM:
   case PIPE_FORMAT_R16G16B16A16_UINT:
   case PIPE_FORMAT_R16G16B16X16_UINT:
   case PIPE_FORMAT_R16G16B16A16_SINT:
   case PIPE_FORMAT_R16G16B16X16_SINT:
   case PIPE_FORMAT_R32G32_SINT:
   case PIPE_FORMAT_R32G32_UINT:
      key->g17p_rt0_eot_words = 2;
      break;
   case PIPE_FORMAT_R32G32B32A32_UINT:
   case PIPE_FORMAT_R32G32B32X32_UINT:
   case PIPE_FORMAT_R32G32B32A32_SINT:
   case PIPE_FORMAT_R32G32B32X32_SINT:
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
   case PIPE_FORMAT_R32G32B32X32_FLOAT:
      key->g17p_rt0_eot_words = 4;
      break;
   default:
      break;
   }
}
