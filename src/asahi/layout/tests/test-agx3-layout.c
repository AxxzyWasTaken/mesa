/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "layout_core.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned checks;

#define CHECK_EQ(actual, expected)                                             \
   do {                                                                        \
      unsigned long long a_ = (actual);                                         \
      unsigned long long e_ = (expected);                                       \
      ++checks;                                                                 \
      if (a_ != e_) {                                                           \
         fprintf(stderr, "%s:%d: got %llu, expected %llu\n", __FILE__,       \
                 __LINE__, a_, e_);                                             \
         exit(1);                                                               \
      }                                                                         \
   } while (0)

int
main(void)
{
   CHECK_EQ(ail_layout_policy_for_descriptor_generation(0),
            AIL_LAYOUT_POLICY_LEGACY);
   CHECK_EQ(ail_layout_policy_for_descriptor_generation(7),
            AIL_LAYOUT_POLICY_LEGACY);
   CHECK_EQ(ail_layout_policy_for_descriptor_generation(9),
            AIL_LAYOUT_POLICY_LEGACY);
   CHECK_EQ(ail_layout_policy_for_descriptor_generation(10),
            AIL_LAYOUT_POLICY_M5);

   CHECK_EQ(ail_effective_width_sa(37, 1), 37);
   CHECK_EQ(ail_effective_height_sa(19, 1), 19);
   CHECK_EQ(ail_effective_width_sa(37, 2), 37);
   CHECK_EQ(ail_effective_height_sa(19, 2), 38);
   CHECK_EQ(ail_effective_width_sa(37, 4), 74);
   CHECK_EQ(ail_effective_height_sa(19, 4), 38);
   CHECK_EQ(ail_effective_width_sa(37, 8), 74);
   CHECK_EQ(ail_effective_height_sa(19, 8), 76);

   struct ail_tile rgba16f = ail_get_max_tile_size(128);
   CHECK_EQ(rgba16f.width_el, 16);
   CHECK_EQ(rgba16f.height_el, 8);
   CHECK_EQ(rgba16f.width_el * rgba16f.height_el * 128, 0x4000);

   struct ail_tile rgba32f = ail_get_max_tile_size(256);
   CHECK_EQ(rgba32f.width_el, 8);
   CHECK_EQ(rgba32f.height_el, 8);
   CHECK_EQ(rgba32f.width_el * rgba32f.height_el * 256, 0x4000);

   /* M3+ single-layer images are never padded merely for their layer stride. */
   CHECK_EQ(ail_m5_layer_stride_B(0x4200, 1, true), 0x4200);

   /* Small arrays with compression metadata use the measured 2 KiB granule. */
   CHECK_EQ(ail_m5_layer_stride_B(0x401, 2, true), 0x800);
   CHECK_EQ(ail_m5_layer_stride_B(0x801, 7, true), 0x1000);
   CHECK_EQ(ail_m5_layer_stride_B(0x801, 7, false), 0x801);

   /* Large array layers continue to use the 16 KiB page granule. */
   CHECK_EQ(ail_m5_layer_stride_B(0x4000, 2, true), 0x4000);
   CHECK_EQ(ail_m5_layer_stride_B(0x4001, 2, false), 0x8000);

   const struct ail_tile metadata_rgba8 =
      ail_m5_compression_tile_size(4, 1);
   CHECK_EQ(metadata_rgba8.width_el, 16);
   CHECK_EQ(metadata_rgba8.height_el, 16);

   const struct ail_tile metadata_rgba32f =
      ail_m5_compression_tile_size(16, 1);
   CHECK_EQ(metadata_rgba32f.width_el, 16);
   CHECK_EQ(metadata_rgba32f.height_el, 8);

   printf("agx3 layout: all %u checks passed\n", checks);
   return 0;
}
