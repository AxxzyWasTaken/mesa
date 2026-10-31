/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

/* Every check in this test is an assert(); keep them in NDEBUG builds. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>

#include "agx_chip.h"

static struct drm_asahi_params_global
g15_params(char variant, uint32_t chip_id, uint32_t clusters)
{
   struct drm_asahi_params_global params = {
      .gpu_generation = 15,
      .gpu_variant = variant,
      .chip_id = chip_id,
      .num_dies = 1,
      .num_clusters_total = clusters,
      .num_cores_per_cluster = 10,
      .usc_generation = 3,
      .gpu_hal_generation = DRM_ASAHI_GPU_HAL_LEGACY,
   };

   for (unsigned i = 0; i < clusters; ++i)
      params.core_masks[i] = 0x3ff;
   return params;
}

static void
check_native_g15(enum agx_chip chip)
{
   assert(agx_chip_native_agx3(chip));
   assert(agx_chip_native_g15(chip));
   assert(agx_chip_native_imageblock(chip));
   assert(agx_chip_native_indirect_compute(chip));
   assert(agx_chip_driver_supported(chip));
   assert(agx_chip_usc_helper_table_slots(chip, 0) == 10);
}

int
main(void)
{
   check_native_g15(AGX_CHIP_G15S);
   check_native_g15(AGX_CHIP_G15C);

   /* The other M3 parts stay off the native path. */
   assert(!agx_chip_native_agx3(AGX_CHIP_G15G));
   assert(!agx_chip_native_agx3(AGX_CHIP_G15D));
   assert(!agx_chip_driver_supported(AGX_CHIP_G15G));
   assert(!agx_chip_driver_supported(AGX_CHIP_G15D));

   /* T6030 and T6031 are each admitted with their own identity only. */
   struct drm_asahi_params_global g15s = g15_params('S', 0x6030, 2);
   struct drm_asahi_params_global g15c = g15_params('C', 0x6031, 4);
   assert(agx_chip_from_params(&g15s) == AGX_CHIP_G15S);
   assert(agx_chip_from_params(&g15c) == AGX_CHIP_G15C);
   assert(agx_params_driver_supported(&g15s));
   assert(agx_params_driver_supported(&g15c));

   /* A binned part reports fewer cores in its masks and is still admitted. */
   struct drm_asahi_params_global binned = g15c;
   binned.core_masks[0] = binned.core_masks[3] = 0x1ff;
   assert(agx_params_driver_supported(&binned));

   struct drm_asahi_params_global bad = g15c;
   bad.chip_id = 0x6030;
   assert(!agx_params_driver_supported(&bad));

   bad = g15c;
   bad.num_clusters_total = 2;
   assert(!agx_params_driver_supported(&bad));

   bad = g15c;
   bad.num_dies = 2;
   assert(!agx_params_driver_supported(&bad));

   bad = g15c;
   bad.gpu_hal_generation = DRM_ASAHI_GPU_HAL_200;
   assert(!agx_params_driver_supported(&bad));

   bad = g15c;
   bad.usc_generation = 2;
   assert(!agx_params_driver_supported(&bad));

   bad = g15s;
   bad.chip_id = 0x6031;
   assert(!agx_params_driver_supported(&bad));

   bad = g15s;
   bad.num_clusters_total = 4;
   assert(!agx_params_driver_supported(&bad));

   /* The M4 Pro (T6040, G16S, AGX3, HAL200) runs as G17P with its two
    * clusters; other G16S identities stay G16S and unsupported. */
   struct drm_asahi_params_global t6040 = g15_params('S', 0x6040, 2);
   t6040.gpu_generation = 16;
   t6040.gpu_hal_generation = DRM_ASAHI_GPU_HAL_200;
   assert(agx_chip_from_params(&t6040) == AGX_CHIP_G17P);
   assert(agx_params_driver_supported(&t6040));

   bad = t6040;
   bad.num_clusters_total = 3;
   assert(!agx_params_driver_supported(&bad));

   bad = t6040;
   bad.chip_id = 0x6041;
   assert(agx_chip_from_params(&bad) == AGX_CHIP_G16S);
   assert(!agx_params_driver_supported(&bad));

   bad = t6040;
   bad.gpu_hal_generation = DRM_ASAHI_GPU_HAL_LEGACY;
   assert(agx_chip_from_params(&bad) == AGX_CHIP_G16S);

   /* G17P helper tables have a slot per core of a cluster, never fewer than
    * the A18 Pro's six: six there, ten on the M4 Pro. */
   assert(agx_chip_usc_helper_table_slots(AGX_CHIP_G17P, 0) == 6);
   assert(agx_chip_usc_helper_table_slots(AGX_CHIP_G17P, 6) == 6);
   assert(agx_chip_usc_helper_table_slots(AGX_CHIP_G17P, 10) == 10);

   /* The M5 Max (T6050, G17C) is admitted on the M5 bring-up path with its
    * four clusters only. */
   struct drm_asahi_params_global t6050c = g15_params('C', 0x6050, 4);
   t6050c.gpu_generation = 17;
   t6050c.gpu_hal_generation = DRM_ASAHI_GPU_HAL_300;
   assert(agx_chip_from_params(&t6050c) == AGX_CHIP_G17C);
   assert(agx_chip_g17_m5(AGX_CHIP_G17C));
   assert(agx_params_m5_bringup_supported(&t6050c));
   assert(!agx_params_driver_supported(&t6050c));

   bad = t6050c;
   bad.num_clusters_total = 2;
   assert(!agx_params_m5_bringup_supported(&bad));

   bad = t6050c;
   bad.chip_id = 0x6051;
   assert(!agx_params_m5_bringup_supported(&bad));

   bad = t6050c;
   bad.num_dies = 2;
   assert(!agx_params_m5_bringup_supported(&bad));

   printf("chip admission: ok\n");
   return 0;
}
