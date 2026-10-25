/* Offline check for hk_sampler_heap_index().
 * cc -Wall -Wextra -I.. test-hk-sampler-index.c && ./a.out */
#include <stdio.h>

#include "hk_sampler_index.h"

int
main(void)
{
   int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

   /* G17P: raw index published, raw index used. */
   CHECK(hk_sampler_heap_index(true, 0) == 0);
   CHECK(hk_sampler_heap_index(true, 28) == 28);
   /* Every other chip publishes heap + 28 (hk_sampler_descriptor_index). */
   for (uint16_t h = 0; h < 1024; ++h)
      CHECK(hk_sampler_heap_index(false, (uint16_t)(h + 28)) == h);
   /* The M6 vkcube set held 28 = first sampler = heap entry 0. */
   CHECK(hk_sampler_heap_index(false, 28) == 0);
   /* Below the bias cannot be a published index: refused by the bounds check. */
   CHECK(hk_sampler_heap_index(false, 27) == UINT16_MAX);
   CHECK(hk_sampler_heap_index(false, 0) == UINT16_MAX);

   if (failures)
      return 1;
   printf("HK_SAMPLER_INDEX_OK\n");
   return 0;
}
