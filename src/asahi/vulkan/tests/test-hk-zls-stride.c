/* Consumer-visible check of the ZLS stride word the Vulkan driver sends.
 * Includes the production header; cc -I.. test-hk-zls-stride.c && ./a.out */
#include <stdio.h>

#include "hk_zls_stride.h"

static int failures;

#define CHECK(cond, ...)                                                       \
   do {                                                                        \
      if (!(cond)) {                                                           \
         failures++;                                                           \
         printf("FAIL line %d: ", __LINE__);                                   \
         printf(__VA_ARGS__);                                                  \
         printf("\n");                                                         \
      }                                                                        \
   } while (0)

static uint32_t
word(bool g18g, bool single, uint64_t stride, bool *ok)
{
   uint32_t w = 0xdeadbeef;
   *ok = hk_zls_m5_layer_stride(g18g, single, stride, &w);
   return w;
}

int
main(void)
{
   bool ok;

   /* The M6 vkcube frame: one layer, D16 1280x720, layer stride 120 pages.
    * Before the fix G18G sent ((120 - 1) << 14) | 1 = 1949697 here. */
   CHECK(word(true, true, 120 * HK_ZLS_PAGE_B, &ok) == 0 && ok,
         "G18G single layer must be 0");
   CHECK(word(false, true, 120 * HK_ZLS_PAGE_B, &ok) == 0 && ok,
         "G17G single layer must be 0");
   /* Single layer never needs a valid stride, even a degenerate one. */
   CHECK(word(true, true, 0, &ok) == 0 && ok, "single layer, zero stride");
   CHECK(word(true, true, 64, &ok) == 0 && ok, "single layer, odd stride");

   /* Layered: page units, bit 0 set (Gallium witness shapes). */
   CHECK(word(true, false, 2 * HK_ZLS_PAGE_B, &ok) == ((1u << 14) | 1) && ok,
         "2 pages");
   CHECK(word(true, false, 8 * HK_ZLS_PAGE_B, &ok) == ((7u << 14) | 1) && ok,
         "8 pages");
   CHECK(word(false, false, 6 * HK_ZLS_PAGE_B, &ok) == ((5u << 14) | 1) && ok,
         "G17G 6 pages");

   /* G18G sub-page array: one 128-byte cacheline per layer encodes as 0,
    * 64 cachelines (8 KiB layers) as 63 << 14, bit 0 clear. */
   CHECK(word(true, false, HK_ZLS_CACHELINE_B, &ok) == 0 && ok,
         "G18G 128 B layers");
   CHECK(word(true, false, 64 * HK_ZLS_CACHELINE_B, &ok) == (63u << 14) && ok,
         "G18G 8 KiB layers");

   /* G17G cannot express a sub-page stride: refused, not mis-encoded. */
   (void)word(false, false, HK_ZLS_CACHELINE_B, &ok);
   CHECK(!ok, "G17G sub-page stride must be refused");
   (void)word(true, false, 0, &ok);
   CHECK(!ok, "layered zero stride must be refused");
   (void)word(true, false, 100, &ok);
   CHECK(!ok, "unaligned stride must be refused");

   if (failures) {
      printf("%d failure(s)\n", failures);
      return 1;
   }
   printf("HK_ZLS_STRIDE_OK\n");
   return 0;
}
