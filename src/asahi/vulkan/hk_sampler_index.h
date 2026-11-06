/*
 * Sampler heap index from the index hk publishes in the descriptor set.
 *
 * hk_sampler_descriptor_index() (hk_descriptor_set_layout.h) stores
 * heap_index + 28 on every chip except G17P (the AGX2 bias).  The G17P/G18G
 * combined-sampler pointer path addresses the heap as
 * sampler_heap_va + index * 32 with NO bias, so it must be given the heap
 * index, not the published one.  Returns UINT16_MAX when the published index
 * is below the bias; every caller bounds-checks against sampler_count, which
 * then refuses it.
 *
 * Pure C on purpose: the offline test links the production code.
 */
#ifndef HK_SAMPLER_INDEX_H
#define HK_SAMPLER_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#define HK_SAMPLER_AGX2_BIAS 28u

static inline uint16_t
hk_sampler_heap_index(bool native_indices, uint16_t published)
{
   if (native_indices)
      return published;
   return published >= HK_SAMPLER_AGX2_BIAS
             ? (uint16_t)(published - HK_SAMPLER_AGX2_BIAS)
             : UINT16_MAX;
}

#endif
