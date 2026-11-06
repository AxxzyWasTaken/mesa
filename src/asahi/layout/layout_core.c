/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "layout_core.h"
#include "util/macros.h"

/* Every maximum tile occupies one 16 KiB GPU page. */
struct ail_tile
ail_get_max_tile_size(unsigned blocksize_B)
{
   /* clang-format off */
   switch (blocksize_B) {
   case   1: return (struct ail_tile) { 128, 128 };
   case   2: return (struct ail_tile) { 128,  64 };
   case   4: return (struct ail_tile) {  64,  64 };
   case   8: return (struct ail_tile) {  64,  32 };
   case  16: return (struct ail_tile) {  32,  32 };
   case  32: return (struct ail_tile) {  32,  16 };
   case  64: return (struct ail_tile) {  16,  16 };
   case 128: return (struct ail_tile) {  16,   8 };
   case 256: return (struct ail_tile) {   8,   8 };
   default: UNREACHABLE("Invalid blocksize");
   }
   /* clang-format on */
}

uint64_t
ail_m5_layer_stride_B(uint64_t unaligned_size_B, unsigned layers,
                     bool has_compression_metadata)
{
   if (layers <= 1)
      return unaligned_size_B;

   if (unaligned_size_B >= 0x4000)
      return ALIGN_POT(unaligned_size_B, 0x4000);

   if (has_compression_metadata)
      return ALIGN_POT(unaligned_size_B, 0x800);

   return unaligned_size_B;
}

struct ail_tile
ail_m5_compression_tile_size(unsigned blocksize_B, unsigned sample_count_sa)
{
   switch (blocksize_B) {
   case 1:
   case 2:
   case 4:
   case 8:
      return (struct ail_tile){16, 16};
   case 16:
      /* HAL300/HAL400 getMacroBlockDimensions uses logical-pixel blocks
       * of 8x8 at 2x and 4x4 at 8x for RGBA32. Express those in the same
       * sample-element coordinates used by AIL's metadata addressing. */
      if (sample_count_sa == 2 || sample_count_sa == 8)
         return (struct ail_tile){8, 16};
      return (struct ail_tile){16, 8};
   default:
      UNREACHABLE("Unsupported M5 compression block size");
   }
}
