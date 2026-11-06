/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include "agx_m5_va.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t vm_end = UINT64_C(0x3ffffff8000);

static void
reject(uint64_t start, uint64_t end, uint64_t minimum, uint64_t shader)
{
   struct agx_m5_va_layout layout, saved;
   memset(&layout, 0xa5, sizeof(layout));
   saved = layout;
   assert(!agx_m5_va_layout(start, end, minimum, shader, &layout));
   assert(memcmp(&layout, &saved, sizeof(layout)) == 0);
}

int main(void)
{
   uint64_t compact = 0;
   assert(agx_m5_vdm_compact(UINT64_C(0x1000058000), &compact));
   assert(compact == 0x58000);
   /* Physical native-Gallium failure: dVDM_SSD fetched 0x6ffff8c000
    * from the wrongly absolute PPP pointer 0x5ffff8c010. The architectural
    * base must be removed exactly once before the hardware adds it back.
    */
   assert(agx_m5_vdm_compact(UINT64_C(0x5ffff8c010), &compact));
   assert(compact == UINT64_C(0x4ffff8c010));
   assert(compact + AGX_M5_DRIVER_BASE == UINT64_C(0x5ffff8c010));
   assert(agx_m5_vdm_compact(AGX_M5_DRIVER_BASE, &compact) && compact == 0);
   assert(agx_m5_vdm_compact(AGX_M5_DRIVER_BASE + (UINT64_C(1) << 39) - 4,
                             &compact));
   const uint64_t saved_compact = compact;
   assert(!agx_m5_vdm_compact(AGX_M5_DRIVER_BASE - 4, &compact));
   assert(!agx_m5_vdm_compact(AGX_M5_DRIVER_BASE + 1, &compact));
   assert(!agx_m5_vdm_compact(AGX_M5_DRIVER_BASE + (UINT64_C(1) << 39), &compact));
   assert(!agx_m5_vdm_compact(UINT64_MAX, &compact));
   assert(!agx_m5_vdm_compact(AGX_M5_DRIVER_BASE, NULL));
   assert(compact == saved_compact);

   struct agx_m5_va_layout layout;
   assert(agx_m5_va_layout(0x4000, vm_end, 0x20000000,
                           AGX_M5_SHADER_BASE, &layout));
   assert(layout.driver_start == UINT64_C(0x1000000000));
   assert(layout.driver_end == UINT64_C(0x1800000000));
   assert(layout.data_start == UINT64_C(0x4000000000));
   assert(layout.data_end == UINT64_C(0x8000000000));
   assert(layout.shader_start == UINT64_C(0x10000000000));
   assert(layout.shader_end == UINT64_C(0x10100000000));

   /* Both ends of the compact interval are representable in the actual
    * 32-bit TA descriptor field, and every interval is disjoint. */
   assert(AGX_M5_DRIVER_BASE + UINT32_MAX < layout.driver_end);
   assert(layout.driver_end <= layout.data_start);
   assert(layout.data_end <= layout.shader_start);
   assert(layout.shader_end <= vm_end);
   assert(layout.data_end <= UINT64_C(1) << 39);

   /* A larger advertised minimum must be honored and page rounded. */
   assert(agx_m5_va_layout(0x4000, vm_end, AGX_M5_DRIVER_SIZE + 1,
                           AGX_M5_SHADER_BASE, &layout));
   assert(layout.driver_end == UINT64_C(0x1800004000));

   reject(AGX_M5_DRIVER_BASE + 0x4000, vm_end, 0, AGX_M5_SHADER_BASE);
   reject(0x4000, AGX_M5_SHADER_BASE, 0, AGX_M5_SHADER_BASE);
   reject(0x4000, vm_end, UINT64_MAX, AGX_M5_SHADER_BASE);
   reject(0x4000, vm_end, AGX_M5_DATA_START, AGX_M5_SHADER_BASE);
   reject(0x4000, vm_end, 0, AGX_M5_DRIVER_BASE);
   reject(0x4000, vm_end, 0, AGX_M5_DATA_START);
   reject(0x4000, vm_end, 0, AGX_M5_SHADER_BASE + 1);
   reject(0x4000, UINT64_MAX, 0, UINT64_C(0xffffffff00000000));
   assert(!agx_m5_va_layout(0x4000, vm_end, 0, AGX_M5_SHADER_BASE, NULL));
   puts("M5 compact driver, data/sparse and shader address windows: PASS");
   return 0;
}
