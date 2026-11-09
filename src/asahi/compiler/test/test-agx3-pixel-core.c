/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Byte-exact replay of Apple's own pixwait / pixrel encodings.
 *
 * Every expected byte string below was decoded out of Apple's offline
 * translator output for the fragment probe corpus
 * (research/gpu/isa-re/gen-x/tileops/corpus/tile{1,2,3,4}), AGX3b from the
 * g15g + g16g targets (identical) and AGX3c from g17s. If this core ever
 * stops reproducing them it is emitting something Apple never emitted.
 */

#include "agx3_pixel_core.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;
static unsigned checks;

static void
expect(const char *what, bool ok, const uint8_t *bytes, const uint8_t *len,
       const char *expected_hex)
{
   char got[2 * AGX3_PIXEL_MAX_LENGTH + 1] = {0};

   checks++;

   if (!ok) {
      fprintf(stderr, "%s: refused\n", what);
      failures++;
      return;
   }

   for (unsigned i = 0; i < *len; ++i)
      snprintf(got + 2 * i, 3, "%02x", bytes[i]);

   if (strcmp(got, expected_hex) != 0) {
      fprintf(stderr, "%s: got %s, expected %s\n", what, got, expected_hex);
      failures++;
   }
}

static void
expect_refused(const char *what, bool ok)
{
   checks++;
   if (ok) {
      fprintf(stderr, "%s: accepted, expected refusal\n", what);
      failures++;
   }
}

int
main(void)
{
   uint8_t b[AGX3_PIXEL_MAX_LENGTH];
   uint8_t len;

   expect("AGX3b colour entry",
          agx3_pixel_colour_entry(AGX_ISA_ENCODING_AGX3B, b, &len), b, &len,
          "870254000600");
   expect("M5 native colour entry",
          agx3_pixel_colour_entry(AGX_ISA_ENCODING_AGX3C, b, &len), b, &len,
          "073000050000");
   expect_refused("AGX2 colour entry",
                  agx3_pixel_colour_entry(AGX_ISA_ENCODING_AGX2, b, &len));

   /* metal-nt -arch applegpu_g17p fragment storing the result of
    * atomic_fetch_add_explicit(): class 4 without the colour selector
    * before the atomic, a barrier after its reload. */
   expect("AGX3b returning-atomic class",
          agx3_pixel_atomic_acquire(AGX_ISA_ENCODING_AGX3B, b, &len), b, &len,
          "870254040000");
   expect("AGX3b returning-atomic barrier",
          agx3_pixel_atomic_barrier(AGX_ISA_ENCODING_AGX3B, b, &len), b, &len,
          "070454c40800");
   expect_refused("AGX3c returning-atomic barrier",
                  agx3_pixel_atomic_barrier(AGX_ISA_ENCODING_AGX3C, b, &len));

   /* Native multi-RT shaders allocate two pixel classes per tile partition.
    * Prove the architectural mapping independently of any workload layout. */
   static const struct {
      uint32_t read, write, combined;
   } partitions[] = {
      {8, 4, 12}, {32, 16, 48}, {128, 64, 192},
   };
   for (unsigned i = 0; i < sizeof(partitions) / sizeof(partitions[0]); ++i) {
      uint32_t read = 0, write = 0, combined = 0;
      if (!agx3_pixel_colour_partition_classes(i, &read, &write, &combined) ||
          read != partitions[i].read || write != partitions[i].write ||
          combined != partitions[i].combined) {
         fprintf(stderr, "FAIL colour partition %u classes\n", i);
         failures++;
      }
   }
   {
      uint32_t read = 0, write = 0, combined = 0;
      if (agx3_pixel_colour_partition_classes(3, &read, &write, &combined)) {
         fprintf(stderr, "FAIL unwitnessed colour partition accepted\n");
         failures++;
      }
   }

   /* AGX3b pixwait: the writeout mask passes straight through, and f34
    * separates the colour classes from the depth/coverage class. */
   static const struct {
      uint32_t writeout;
      const char *hex;
   } b_wait[] = {
      {1, "870254010000"},   {4, "870254040800"},   {8, "870254080800"},
      {12, "8702540c0800"},  {16, "870254100800"},  {32, "870254200800"},
      {48, "870254300800"},  {60, "8702543c0800"},  {64, "870254400800"},
      {128, "870254800800"}, {192, "870254c00800"},
      /* Every class of partitions 0..2 at once: 2x/4x store-only fragments
       * of three to eight targets. */
      {252, "870254fc0800"},
   };

   for (unsigned i = 0; i < sizeof(b_wait) / sizeof(b_wait[0]); ++i) {
      const bool ok = agx3_pixel_wait(AGX_ISA_ENCODING_AGX3B,
                                      b_wait[i].writeout, 0, b, &len);
      expect("AGX3b pixwait", ok, b, &len, b_wait[i].hex);
   }

   /* AGX3b pixwait carrying scoreboard waits on slots 1 and 2. */
   expect("AGX3b pixwait wait{1,2}",
          agx3_pixel_wait(AGX_ISA_ENCODING_AGX3B, 1, 0x3, b, &len), b, &len,
          "873254010000");

   /* AGX3b pixrel: descriptor = (last ? 0x200 : 0) | writeout. */
   static const struct {
      uint32_t writeout;
      bool last;
      const char *hex;
   } b_rel[] = {
      {1, false, "070254010000"},  {48, false, "070254300000"},
      {192, false, "070254c00000"}, {0, true, "070254000200"},
      {1, true, "070254010200"},   {12, true, "0702540c0200"},
      {48, true, "070254300200"},  {60, true, "0702543c0200"},
      {192, true, "070254c00200"}, {252, true, "070254fc0200"},
   };

   for (unsigned i = 0; i < sizeof(b_rel) / sizeof(b_rel[0]); ++i) {
      const bool ok =
         agx3_pixel_release(AGX_ISA_ENCODING_AGX3B, b_rel[i].writeout,
                            b_rel[i].last, 0, b, &len);
      expect("AGX3b pixrel", ok, b, &len, b_rel[i].hex);
   }

   /* AGX3c pixwait / pixrel, same roles over the wider encoding. */
   static const struct {
      uint32_t writeout;
      const char *hex;
   } c_wait[] = {
      {4, "274000050000"},
      {12, "674000050000"},
      {16, "874000050000"},
      {48, "874100050000"},
   };

   for (unsigned i = 0; i < sizeof(c_wait) / sizeof(c_wait[0]); ++i) {
      const bool ok = agx3_pixel_wait(AGX_ISA_ENCODING_AGX3C,
                                      c_wait[i].writeout, 0, b, &len);
      expect("AGX3c pixwait", ok, b, &len, c_wait[i].hex);
   }

   static const struct {
      uint32_t writeout;
      bool last;
      const char *hex;
   } c_rel[] = {
      {12, true, "671000040000"},
      {1, false, "0f0000040000"},
      {48, false, "870100040000"},
      {192, false, "070600040000"},
   };

   for (unsigned i = 0; i < sizeof(c_rel) / sizeof(c_rel[0]); ++i) {
      const bool ok =
         agx3_pixel_release(AGX_ISA_ENCODING_AGX3C, c_rel[i].writeout,
                            c_rel[i].last, 0, b, &len);
      expect("AGX3c pixrel", ok, b, &len, c_rel[i].hex);
   }

   /* Fail-closed: unwitnessed masks, the f15-gated entry form, out-of-range
    * scoreboard slots, and AGX2 all stay refused. */
   expect_refused("pixwait writeout 2",
                  agx3_pixel_wait(AGX_ISA_ENCODING_AGX3B, 2, 0, b, &len));
   expect_refused("pixwait writeout 0",
                  agx3_pixel_wait(AGX_ISA_ENCODING_AGX3B, 0, 0, b, &len));
   expect_refused("pixwait entry gate 512",
                  agx3_pixel_wait(AGX_ISA_ENCODING_AGX3B, 512, 0, b, &len));
   expect_refused("pixwait slot 7 on AGX3b",
                  agx3_pixel_wait(AGX_ISA_ENCODING_AGX3B, 1, 0x40, b, &len));
   expect_refused("pixwait on AGX2",
                  agx3_pixel_wait(AGX_ISA_ENCODING_AGX2, 1, 0, b, &len));
   expect_refused("pixrel writeout 4",
                  agx3_pixel_release(AGX_ISA_ENCODING_AGX3B, 4, false, 0, b,
                                     &len));
   expect_refused("pixrel on AGX2", agx3_pixel_release(AGX_ISA_ENCODING_AGX2,
                                                       1, false, 0, b, &len));

   if (failures) {
      fprintf(stderr, "agx3_pixel_core: %u/%u checks failed\n", failures,
              checks);
      return 1;
   }

   printf("agx3_pixel_core: all %u checks passed\n", checks);
   return 0;
}
