/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

/* Every check in this test is an assert(); keep them in NDEBUG builds. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "agx_linker.h"

#define PAGE 0x4000u

static uint8_t page[PAGE];
static uint8_t lead[PAGE];

int
main(void)
{
   /* The return the helper tables end with: the slot right after the second
    * table's ten records. */
   uint8_t terminal[16];

   /* G15S and G17P: the page is the plain lead followed by zeroes. */
   const enum agx_chip plain[] = {AGX_CHIP_G15S, AGX_CHIP_G17P};
   for (unsigned c = 0; c < sizeof(plain) / sizeof(plain[0]); ++c) {
      memset(page, 0xa5, sizeof(page));
      memset(lead, 0, sizeof(lead));
      agx3_build_container_lead(lead, plain[c], 0);
      agx3_build_usc_base_page(page, PAGE, plain[c], 0, 2);
      assert(memcmp(page, lead, PAGE) == 0);
   }

   /* G15C: the same lead below +0x2c0, a return in every slot from +0x2c0 to
    * +0x340 and from +0x350 to the end, and the zero block-size slot at
    * +0x340. */
   memset(page, 0xa5, sizeof(page));
   memset(lead, 0, sizeof(lead));
   agx3_build_container_lead(lead, AGX_CHIP_G15C, 0);
   agx3_build_usc_base_page(page, PAGE, AGX_CHIP_G15C, 0, 4);
   memcpy(terminal, lead + 0x200 + 10 * 16, sizeof(terminal));

   assert(memcmp(page, lead, 0x2c0) == 0);
   for (unsigned offset = 0x2c0; offset < 0x340; offset += 16)
      assert(memcmp(page + offset, terminal, 16) == 0);
   for (unsigned offset = 0x340; offset < 0x350; ++offset)
      assert(page[offset] == 0);
   for (unsigned offset = 0x350; offset < PAGE; offset += 16)
      assert(memcmp(page + offset, terminal, 16) == 0);

   /* Both tables still end in two returns at the same places. */
   assert(memcmp(page + 0x100 + 10 * 16, terminal, 16) == 0);
   assert(memcmp(page + 0x100 + 11 * 16, terminal, 16) == 0);
   assert(memcmp(page + 0x200 + 11 * 16, terminal, 16) == 0);

   {
      static uint8_t g15c[PAGE];
      memcpy(g15c, page, PAGE);

      memset(page, 0xa5, sizeof(page));
      agx3_build_usc_base_page(page, PAGE, AGX_CHIP_G17P, 10, 4);
      assert(memcmp(page, g15c, PAGE) == 0);

      for (unsigned cluster = 0; cluster < 4; ++cluster) {
         for (unsigned core = 0; core < 10; ++core) {
            unsigned slot = 0x100 + 16 * (16 * cluster + core);
            if (slot == 0x340)
               assert(memcmp(page + 0x350, terminal, 16) == 0);
            else if (cluster >= 2)
               assert(memcmp(page + slot, terminal, 16) == 0);
            else
               assert(page[slot] == 0x0f && page[slot + 2] == 0x54);
         }
      }

      memset(page, 0xa5, sizeof(page));
      memset(lead, 0, sizeof(lead));
      agx3_build_container_lead(lead, AGX_CHIP_G17P, 10);
      agx3_build_usc_base_page(page, PAGE, AGX_CHIP_G17P, 10, 2);
      assert(memcmp(page, lead, PAGE) == 0);
   }

   printf("usc base page: ok\n");
   return 0;
}
