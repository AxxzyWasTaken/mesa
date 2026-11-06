/* SPDX-License-Identifier: MIT */
#include "agx_m5_va.h"
#include "agx3_code_container_core.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
   static const uint8_t prefixes[][14] = {
      {0x03, 0, 0x07, 0, 0x02, 0, 0, 0, 0x60, 0, 0x0e, 0, 0, 0},
      {0x0e, 0, 0, 0},
      {0x03, 0, 0x07, 0, 0x02, 0, 0, 0, 0x20, 0, 0x0e, 0, 0, 0},
   };
   for (unsigned variant = 0; variant < 4; ++variant) {
      uint8_t guarded[AGX3_CONTAINER_CONSTANT_PROGRAM_SIZE + 2];
      memset(guarded, 0xa5, sizeof(guarded));
      const bool fragment = variant & 1, sampled = variant & 2;
      const unsigned program = !fragment ? 0 : sampled ? 2 : 1;
      const unsigned size = program == 1 ? 4 : 14;
      agx3_build_container_constant_program(guarded + 1, fragment, sampled);
      CHECK(guarded[0] == 0xa5 && guarded[sizeof(guarded) - 1] == 0xa5);
      CHECK(!memcmp(guarded + 1, prefixes[program], size));
      for (unsigned offset = size; offset < AGX3_CONTAINER_CONSTANT_PROGRAM_SIZE;
           offset += 2) {
         CHECK(guarded[1 + offset] == 0x06);
         CHECK(guarded[2 + offset] == 0x00);
      }
   }
   uint8_t bytes[0x641], before[sizeof(bytes)];
   memset(bytes, 0xa5, sizeof(bytes));
   CHECK(agx3_build_m5_container_lead(bytes, sizeof(bytes), 2));
   uint64_t hash = UINT64_C(0xcbf29ce484222325);
   for (unsigned i = 0; i < 0x640; ++i)
      hash = (hash ^ bytes[i]) * UINT64_C(0x100000001b3);
   CHECK(hash == UINT64_C(0x6b9ff646a3a29f63));
   CHECK(bytes[0x640] == 0xa5);
   CHECK(agx3_build_m5_container_lead(bytes, sizeof(bytes), 1));
   CHECK(!memcmp(bytes, (uint8_t[]){0x40,3,0,0}, 4));
   CHECK(!memcmp(bytes + 0x100, bytes + 0x200, 0x100));
   CHECK(!memcmp(bytes + 0x100, (uint8_t[]){0x5e,0,0x33,0x0e,0,0,0,0,0,0,6,0,6,0,6,0}, 16));
   memcpy(before, bytes, sizeof(bytes));
   CHECK(!agx3_build_m5_container_lead(bytes, 0x63f, 2));
   CHECK(!agx3_build_m5_container_lead(bytes, sizeof(bytes), 0));
   CHECK(!agx3_build_m5_container_lead(bytes, sizeof(bytes), UINT_MAX));
   CHECK(!memcmp(before, bytes, sizeof(bytes)));
   /* Four clusters: 0xc40 bytes, one helper table per cluster for each of
    * the two stage groups, all identical. */
   uint8_t four[0xc41];
   memset(four, 0xa5, sizeof(four));
   CHECK(!agx3_build_m5_container_lead(four, 0xc3f, 4));
   CHECK(agx3_build_m5_container_lead(four, sizeof(four), 4));
   CHECK(!memcmp(four, (uint8_t[]){0x40,0x0c,0,0}, 4));
   for (unsigned table = 5; table < 12; ++table)
      CHECK(!memcmp(four + 0x400, four + table * 0x100, 0x100));
   CHECK(!memcmp(four + 0x400, bytes + 0x100, 0x100));
   CHECK(four[0xc40] == 0xa5);
   struct agx_m5_va_layout layout;
   CHECK(agx_m5_va_layout(UINT64_C(0x100000000), UINT64_C(0x20000000000),
                          0, AGX_M5_SHADER_BASE, &layout));
   CHECK(layout.driver_start == AGX_M5_DRIVER_BASE);
   CHECK(layout.driver_end <= layout.data_start);
   CHECK(layout.data_end <= layout.shader_start);
   CHECK(agx_m5_va_layout(UINT64_C(0x100000000), UINT64_C(0x20000000000),
                          UINT64_C(0x900000000), AGX_M5_SHADER_BASE, &layout));
   CHECK(layout.driver_end == UINT64_C(0x1900000000));
   CHECK(!agx_m5_va_layout(AGX_M5_DRIVER_BASE + 1, UINT64_C(0x20000000000),
                           0, AGX_M5_SHADER_BASE, &layout));
   CHECK(!agx_m5_va_layout(0, AGX_M5_SHADER_BASE, 0, AGX_M5_SHADER_BASE, &layout));
   CHECK(!agx_m5_va_layout(0, UINT64_C(0x20000000000), UINT64_MAX,
                           AGX_M5_SHADER_BASE, &layout));
   puts("M5_LAYOUT_PASS");
   return 0;
}
