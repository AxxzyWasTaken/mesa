/*
 * M5-policy ZLS layer-stride word for the render command (depth.stride and
 * stencil.stride), shared by hk_CmdBeginRendering and the offline test.
 *
 * Mirrors gallium agx_zls_layer_stride() (agx_pipe.c): an M5-policy layout
 * rendered as a single layer carries NO array stride (word 0), on G17G and
 * G18G alike. G18G arrays use 16 KiB pages (bit 0 set) or 128-byte
 * cachelines (bit 0 clear), the upper field holds units minus one.
 *
 * Pure C on purpose: no Mesa headers, so the test links the production code.
 */
#ifndef HK_ZLS_STRIDE_H
#define HK_ZLS_STRIDE_H

#include <stdbool.h>
#include <stdint.h>

#define HK_ZLS_PAGE_B      0x4000u
#define HK_ZLS_CACHELINE_B 0x80u

/*
 * single_layer: the render touches one layer (view_mask == 0 && layer_count == 1).
 * g18g: also accept sub-page (cacheline) array strides, as G18G natively does.
 * Returns false for a stride the hardware word cannot express; *out is then 0.
 */
static inline bool
hk_zls_m5_layer_stride(bool g18g, bool single_layer, uint64_t layer_stride_B,
                       uint32_t *out)
{
   *out = 0;
   if (single_layer)
      return true;

   if (layer_stride_B == 0)
      return false;

   const bool pages = (layer_stride_B % HK_ZLS_PAGE_B) == 0;
   if (!pages && !(g18g && (layer_stride_B % HK_ZLS_CACHELINE_B) == 0))
      return false;

   const uint64_t unit = pages ? HK_ZLS_PAGE_B : HK_ZLS_CACHELINE_B;
   const uint64_t units_minus_1 = layer_stride_B / unit - 1;
   if (units_minus_1 > (UINT32_MAX >> 14))
      return false;

   *out = (uint32_t)(units_minus_1 << 14) | (pages ? 1u : 0u);
   return true;
}

#endif
