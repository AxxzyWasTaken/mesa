/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Structural checks of the native G18G capture vectors with the decoders
 * Mesa already has. There is no G18G chip identity yet, so each record goes
 * through the existing HAL300 identity that accepts its form: the VDM
 * stream through G17S (the tailed non-indexed Index List G18G emits is the
 * G17S/G17C form), PPP payloads and the CDM stream through G17G, and linear
 * colour PBE descriptors through the Gen10 unpacker.
 */

#include "../agx3_cdm_decode_core.h"
#include "../agx_pbe_agx3_core.h"
#include "../agx_ppp_decode_core.h"
#include "../agx_vdm_decode_core.h"
#include "g18g_capture_vectors.h"

#include <stdio.h>
#include <string.h>

static unsigned checks;

#define CHECK(x)                                                               \
   do {                                                                        \
      checks++;                                                                \
      if (!(x)) {                                                              \
         fprintf(stderr, "FAIL %s line %d: %s\n", mode->name, __LINE__, #x);   \
         return 1;                                                             \
      }                                                                        \
   } while (0)

#define MAX_BYTES 256

static void
store_words(const uint32_t *words, unsigned count, uint8_t *bytes)
{
   for (unsigned i = 0; i < count; ++i) {
      bytes[4 * i + 0] = (uint8_t)words[i];
      bytes[4 * i + 1] = (uint8_t)(words[i] >> 8);
      bytes[4 * i + 2] = (uint8_t)(words[i] >> 16);
      bytes[4 * i + 3] = (uint8_t)(words[i] >> 24);
   }
}

static int
test_vdm(const struct g18g_capture_mode *mode)
{
   uint8_t bytes[MAX_BYTES];
   size_t size = mode->stream_words * 4;
   unsigned ppp = 0, draws = 0;
   size_t offset = 0;
   bool terminated = false;

   CHECK(size <= sizeof(bytes));
   store_words(mode->stream, mode->stream_words, bytes);

   while (offset < size && !terminated) {
      struct agx_vdm_decoded_record record;
      CHECK(agx_vdm_decode_one(AGX_CHIP_G17S, bytes + offset, size - offset,
                               &record));

      switch (record.kind) {
      case AGX_VDM_RECORD_PPP_STATE_UPDATE:
         CHECK(ppp < mode->ppp_count);
         CHECK(mode->ppp[ppp].vdm_offset == offset);
         CHECK(record.data.ppp_state_update.pointer == mode->ppp[ppp].address);
         CHECK(record.data.ppp_state_update.size_words ==
               mode->ppp[ppp].size_words);
         ppp++;
         break;
      case AGX_VDM_RECORD_STATE_UPDATE:
         break;
      case AGX_VDM_RECORD_INDEX_LIST_NONINDEXED:
         CHECK(record.length == 20);
         draws++;
         break;
      case AGX_VDM_RECORD_STREAM_TERMINATE:
         terminated = true;
         break;
      default:
         CHECK(!"unexpected VDM record");
      }

      offset += record.length;
   }

   CHECK(terminated && offset == size);
   CHECK(ppp == mode->ppp_count);
   CHECK(draws >= 1);
   return 0;
}

static int
test_ppp(const struct g18g_capture_mode *mode)
{
   unsigned counts[AGX_PPP_DECODED_RECORD_HAL300_DEPTH_BIAS_SCISSOR_BASES + 1] =
      {0};

   for (unsigned i = 0; i < mode->ppp_count; ++i) {
      uint8_t bytes[MAX_BYTES * 4];
      struct agx_ppp_decoded_record record;
      const size_t size = mode->ppp[i].size_words * 4;

      CHECK(size <= sizeof(bytes));
      store_words(mode->ppp[i].words, mode->ppp[i].size_words, bytes);
      if (agx_ppp_decode_one(AGX_CHIP_G17G, bytes, size, &record)) {
         CHECK(record.length == size);
         counts[record.kind]++;
      }
   }

   /* Every render mode carries one viewport, one W clamp/cull2 and one
    * depth-bias/scissor base record, and the same depth-stencil record
    * twice: once in the prefix Apple emits ahead of the VDM state update and
    * once in the draw state. The other records are graphics-core and prefix
    * payloads that agx_ppp_decode_one does not claim. */
   CHECK(counts[AGX_PPP_DECODED_RECORD_VIEWPORT] == 1);
   CHECK(counts[AGX_PPP_DECODED_RECORD_W_CLAMP_CULL2] == 1);
   CHECK(counts[AGX_PPP_DECODED_RECORD_HAL300_DEPTH_BIAS_SCISSOR_BASES] == 1);
   CHECK(counts[AGX_PPP_DECODED_RECORD_HAL300_DEPTH_STENCIL] == 2);
   return 0;
}

static int
test_pbe(const struct g18g_capture_mode *mode)
{
   unsigned linear = 0;

   for (unsigned i = 0; i < mode->pbe_count; ++i) {
      uint8_t bytes[AGX_PBE_AGX3_DESCRIPTOR_LENGTH];
      struct agx_pbe_agx3_2d_fields fields;
      struct agx_pbe_agx3_descriptor repacked;

      store_words(mode->pbe[i].words, 8, bytes);
      if (!agx_pbe_agx3_2d_unpack(AGX_PBE_AGX3_GENERATION_10, bytes,
                                  sizeof(bytes), &fields))
         continue;

      CHECK(fields.buffer == mode->pbe[i].attachment);
      CHECK(fields.width == 256 && fields.height == 256);
      CHECK(agx_pbe_agx3_2d_pack(AGX_PBE_AGX3_GENERATION_10, &fields,
                                 &repacked));
      CHECK(repacked.length == sizeof(bytes));
      CHECK(memcmp(repacked.bytes, bytes, sizeof(bytes)) == 0);
      if (fields.layout == 0)
         linear++;
   }

   /* The colour target is the buffer-backed linear 256x256 texture. */
   CHECK(linear == 1);
   return 0;
}

static int
test_cdm(const struct g18g_capture_mode *mode)
{
   uint8_t bytes[MAX_BYTES];
   size_t size = mode->stream_words * 4;
   size_t offset = 0;
   unsigned launches = 0;
   bool terminated = false;

   CHECK(size <= sizeof(bytes));
   store_words(mode->stream, mode->stream_words, bytes);

   while (offset < size && !terminated) {
      struct agx3_cdm_decoded_record record;
      CHECK(agx3_cdm_decode_one(AGX_CHIP_G17G, bytes + offset, size - offset,
                                &record));

      switch (record.kind) {
      case AGX3_CDM_RECORD_LAUNCH:
         /* compute-add: 4096 threads in groups of 64. */
         CHECK(record.data.launch.global_size[0] == 4096);
         CHECK(record.data.launch.local_size[0] == 64);
         launches++;
         break;
      case AGX3_CDM_RECORD_BARRIER:
         break;
      case AGX3_CDM_RECORD_STREAM_TERMINATE:
         terminated = true;
         break;
      default:
         CHECK(!"unexpected CDM record");
      }

      offset += record.length;
   }

   CHECK(terminated && offset == size);
   CHECK(launches == 1);
   return 0;
}

int
main(void)
{
   const unsigned count =
      sizeof(g18g_capture_modes) / sizeof(g18g_capture_modes[0]);

   for (unsigned i = 0; i < count; ++i) {
      const struct g18g_capture_mode *mode = &g18g_capture_modes[i];
      int ret = mode->compute ? test_cdm(mode)
                              : test_vdm(mode) || test_ppp(mode) ||
                                   test_pbe(mode);
      if (ret)
         return ret;
   }

   printf("PASS %u G18G capture checks over %u modes\n", checks, count);
   return 0;
}
