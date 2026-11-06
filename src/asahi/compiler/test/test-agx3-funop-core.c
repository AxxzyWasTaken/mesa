/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Byte-exact replay of Apple's own funop encodings.
 *
 * Every expected byte string below was decoded out of Apple's offline
 * translator output: the transcendental probe corpus
 * (research/gpu/transcendental-int-re/corpus/{g15g,g16g,g17s}, kernels) and
 * the fragment gradient corpus (corpus_frag, render pipelines). AGX3b bytes
 * are from g16g and are byte-identical on g15g (verified in the research
 * pass); AGX3c bytes are from g17s. If this core ever stops reproducing them
 * it is emitting something Apple never emitted.
 */

#include "agx3_funop_core.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;
static unsigned checks;

static void
expect(const char *what, bool ok, const uint8_t *bytes, uint8_t len,
       const char *expected_hex)
{
   char got[2 * AGX3_FUNOP_MAX_LENGTH + 1] = {0};

   checks++;

   if (!ok) {
      fprintf(stderr, "%s: refused\n", what);
      failures++;
      return;
   }

   for (unsigned i = 0; i < len; ++i)
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

struct vec {
   const char *name;
   enum agx3_funop op;
   uint16_t dest, src;
   bool abs, neg, sat;
   const char *hex;
};

int
main(void)
{
   uint8_t b[AGX3_FUNOP_MAX_LENGTH];
   uint8_t len;

   /* Apple applegpu_g17g half/mixed-width arithmetic witnesses. These retain
    * the actual native precision and independently qualify odd half registers
    * and both widths of the square-root-step input. */
   static const struct {
      enum agx3_funop op;
      uint16_t dest, src, wait;
      bool dest_half, src_half, live, abs;
      const char *hex;
   } typed_vectors[] = {
      {AGX3_FUNOP_LOG2,0,0,128,1,1,0,0,"2f0004082230a4022800"},
      {AGX3_FUNOP_LOG2,1,1,0,1,1,0,0,"270104092230a4022800"},
      {AGX3_FUNOP_LOG2,9,0,128,1,1,1,0,"2f8004092a30a4120800"},
      {AGX3_FUNOP_LOG2,7,2,0,1,1,1,0,"378204092a30a40a0800"},
      {AGX3_FUNOP_EXP2,0,0,128,1,1,0,0,"2f0004082230a5022800"},
      {AGX3_FUNOP_EXP2,1,1,0,1,1,0,0,"270104092230a5022800"},
      {AGX3_FUNOP_EXP2,9,0,128,1,1,1,0,"2f8004092a30a5120800"},
      {AGX3_FUNOP_EXP2,7,2,0,1,1,1,0,"378204092a30a50a0800"},
      {AGX3_FUNOP_RSQRT,0,0,128,1,1,0,0,"2f0004122220a3022800"},
      {AGX3_FUNOP_RSQRT,1,1,0,1,1,0,0,"270104132220a3022800"},
      {AGX3_FUNOP_RSQRT,9,0,128,1,1,1,0,"2f8004132a20a3120800"},
      {AGX3_FUNOP_RSQRT,7,2,0,1,1,1,0,"378204132a20a30a0800"},
      {AGX3_FUNOP_RSQRT,0,0,128,0,0,0,0,"2f0004182220a3023000"},
      {AGX3_FUNOP_RSQRT,12,4,0,0,0,1,0,"278004382a20a31a1000"},
      {AGX3_FUNOP_SQRT_STEP,14,0,128,0,1,1,0,"3f8004182a20a21a0800"},
      {AGX3_FUNOP_SQRT_STEP,10,2,0,0,1,1,0,"378204182a20a2120800"},
      {AGX3_FUNOP_SQRT_STEP,14,0,128,0,0,1,0,"3f8004182a20a21a1000"},
      {AGX3_FUNOP_SQRT_STEP,12,4,0,0,0,1,0,"278004382a20a21a1000"},
      {AGX3_FUNOP_SQRT_STEP,14,0,128,0,0,1,1,"3f8004182a20a21a1040"},
      {AGX3_FUNOP_SQRT_STEP,10,2,0,0,1,1,1,"378204182a20a2120840"},
      {AGX3_FUNOP_LOG2,0,4,128,0,1,0,0,"2f00041a2220a4022800"},
      {AGX3_FUNOP_LOG2,2,5,0,0,1,0,0,"3701041a2220a4022800"},
      {AGX3_FUNOP_LOG2,0,0,128,1,0,0,0,"2f0004082230a4023000"},
      {AGX3_FUNOP_LOG2,1,2,0,1,0,0,0,"270204092230a4023000"},
      {AGX3_FUNOP_EXP2,0,4,128,0,1,0,0,"2f00041a2220a5022800"},
      {AGX3_FUNOP_EXP2,2,5,0,0,1,0,0,"3701041a2220a5022800"},
      {AGX3_FUNOP_EXP2,0,0,128,1,0,0,0,"2f0004082230a5023000"},
      {AGX3_FUNOP_EXP2,1,2,0,1,0,0,0,"270204092230a5023000"},
   };
   for (unsigned i = 0; i < sizeof(typed_vectors) / sizeof(typed_vectors[0]); ++i) {
      const struct agx3_funop_runtime runtime = {
         .dest_cache = typed_vectors[i].live,
         .src_cache = typed_vectors[i].live,
         .src_discard = !typed_vectors[i].live,
         .dest_16 = typed_vectors[i].dest_half,
         .src_16 = typed_vectors[i].src_half,
      };
      const bool ok = agx3_funop_encode_g17g(
         typed_vectors[i].op, typed_vectors[i].dest, typed_vectors[i].src,
         typed_vectors[i].abs, false, false, typed_vectors[i].wait,
         &runtime, b, &len);
      expect("G17G typed arithmetic", ok, b, len, typed_vectors[i].hex);
   }

   /* Independent applegpu_g17g Metal 27.1.5194.15 witnesses: the live
    * probes multiply the funop result by its input; the dead probes only
    * store the result. Registers are in half units. The second instruction
    * has no outstanding load, so it must not inherit the template's wait. */
   static const struct {
      enum agx3_funop op;
      uint16_t dest, src, wait;
      bool live;
      const char *hex;
   } runtime_vectors[] = {
      {AGX3_FUNOP_LOG2, 14, 0, 128, true, "3f80040a2a20a41a1000"},
      {AGX3_FUNOP_LOG2, 12, 4,   0, true, "2780041a2a20a41a1000"},
      {AGX3_FUNOP_LOG2,  0, 0, 128, false,"2f00040a2220a4023000"},
      {AGX3_FUNOP_LOG2,  2, 2,   0, false,"3702040a2220a4023000"},
      {AGX3_FUNOP_EXP2, 14, 0, 128, true, "3f80040a2a20a51a1000"},
      {AGX3_FUNOP_EXP2, 12, 4,   0, true, "2780041a2a20a51a1000"},
      {AGX3_FUNOP_EXP2,  0, 0, 128, false,"2f00040a2220a5023000"},
      {AGX3_FUNOP_EXP2,  2, 2,   0, false,"3702040a2220a5023000"},
      {AGX3_FUNOP_FLOOR,14,0,128, true, "3f8004182a20a01a5000"},
      {AGX3_FUNOP_FLOOR,12,4,  0, true, "278004382a20a01a5000"},
      {AGX3_FUNOP_CEIL, 14,0,128, true, "3f8004182a20a01a9000"},
      {AGX3_FUNOP_CEIL, 12,4,  0, true, "278004382a20a01a9000"},
      {AGX3_FUNOP_TRUNC,14,0,128, true, "3f8004182a20a01ad000"},
      {AGX3_FUNOP_TRUNC,12,4,  0, true, "278004382a20a01ad000"},
      {AGX3_FUNOP_RINT, 14,0,128, true, "3f8004182a20a01a1000"},
      {AGX3_FUNOP_RINT, 12,4,  0, true, "278004382a20a01a1000"},
   };
   for (unsigned i = 0; i < sizeof(runtime_vectors) / sizeof(runtime_vectors[0]); ++i) {
      const struct agx3_funop_runtime runtime = {
         .dest_cache = runtime_vectors[i].live,
         .src_cache = runtime_vectors[i].live,
         .src_discard = !runtime_vectors[i].live,
      };
      const bool ok = agx3_funop_encode_g17g(
         runtime_vectors[i].op, runtime_vectors[i].dest, runtime_vectors[i].src,
         false, false, false, runtime_vectors[i].wait, &runtime, b, &len);
      expect("G17G runtime lifetime", ok, b, len, runtime_vectors[i].hex);
   }

   const struct agx3_funop_runtime invalid_runtime = {
      .src_cache = true, .src_discard = true,
   };
   expect_refused("conflicting source lifetime", agx3_funop_encode_g17g(
      AGX3_FUNOP_LOG2, 0, 2, false, false, false, 0, &invalid_runtime, b, &len));

   /* AGX3b, corpus/g16g fu_* kernels and corpus_frag/g16g dfd_func1. */
   static const struct vec vb[] = {
      {"fu_floor", AGX3_FUNOP_FLOOR, 0, 0, false, false, false,
       "2f0056000200b0400200"},
      {"fu_floor_abs", AGX3_FUNOP_FLOOR, 0, 0, true, false, false,
       "2f0056000200b0c00200"},
      {"fu_floor_neg", AGX3_FUNOP_FLOOR, 0, 0, false, true, false,
       "2f0056000200b0400300"},
      {"fu_ceil", AGX3_FUNOP_CEIL, 0, 0, false, false, false,
       "2f0056000200b0400400"},
      {"fu_trunc", AGX3_FUNOP_TRUNC, 0, 0, false, false, false,
       "2f0056000200b0400600"},
      {"fu_rint", AGX3_FUNOP_RINT, 0, 0, false, false, false,
       "2f0056000200b0400000"},
      {"fu_log2", AGX3_FUNOP_LOG2, 0, 0, false, false, false,
       "2f0256000200b0400000"},
      {"fu_exp2", AGX3_FUNOP_EXP2, 0, 0, false, false, false,
       "af0256000200b0400000"},
      {"fu_sin sinc", AGX3_FUNOP_SINC, 4, 6, false, false, false,
       "2f035404030c92400000"},
      {"dfd_func1 dfdx r1<-r2", AGX3_FUNOP_DFDX, 2, 4, false, false, false,
       "37055402030892400000"},
      {"dfd_func1 dfdx r5<-r0", AGX3_FUNOP_DFDX, 10, 0, false, false, false,
       "3705540a030092400000"},
      {"dfd_func1 dfdy r2<-r2", AGX3_FUNOP_DFDY, 4, 4, false, false, false,
       "37075404030890400000"},
   };

   for (unsigned i = 0; i < sizeof(vb) / sizeof(vb[0]); ++i) {
      const bool ok =
         agx3_funop_encode(AGX_ISA_ENCODING_AGX3B, vb[i].op, vb[i].dest,
                           vb[i].src, vb[i].abs, vb[i].neg, vb[i].sat, 0, b,
                           &len);
      expect(vb[i].name, ok && len == 10, b, len, vb[i].hex);
   }

   /* AGX3c, corpus/g17s fu_* kernels and corpus_frag/g17s dfd_func1. */
   static const struct vec vc[] = {
      {"fu_floor", AGX3_FUNOP_FLOOR, 0, 0, false, false, false,
       "2f0004182220a0027000"},
      {"fu_floor_abs", AGX3_FUNOP_FLOOR, 0, 0, true, false, false,
       "2f0004182220a0027040"},
      {"fu_floor_neg", AGX3_FUNOP_FLOOR, 0, 0, false, true, false,
       "2f0004182228a0027000"},
      {"fu_floor_sat", AGX3_FUNOP_FLOOR, 0, 0, false, false, true,
       "2f0004182220a0027010"},
      {"fu_ceil", AGX3_FUNOP_CEIL, 0, 0, false, false, false,
       "2f0004182220a002b000"},
      {"fu_trunc", AGX3_FUNOP_TRUNC, 0, 0, false, false, false,
       "2f0004182220a002f000"},
      {"fu_rint", AGX3_FUNOP_RINT, 0, 0, false, false, false,
       "2f0004182220a0023000"},
      {"fu_log2", AGX3_FUNOP_LOG2, 0, 0, false, false, false,
       "2f00040a2220a4023000"},
      {"fu_exp2", AGX3_FUNOP_EXP2, 0, 0, false, false, false,
       "2f00040a2220a5023000"},
      {"fu_sin sinc", AGX3_FUNOP_SINC, 4, 6, false, false, false,
       "278204382a20a60a1000"},
      {"dfd_func1 dfdx r1<-r2", AGX3_FUNOP_DFDX, 2, 4, false, false, false,
       "3780001a2b20aa021000"},
      {"dfd_func1 dfdx r5<-r0", AGX3_FUNOP_DFDX, 10, 0, false, false, false,
       "3780000a2b20aa121000"},
      {"dfd_func1 dfdy r2<-r2", AGX3_FUNOP_DFDY, 4, 4, false, false, false,
       "2700001a2b20ae0a1000"},
   };

   for (unsigned i = 0; i < sizeof(vc) / sizeof(vc[0]); ++i) {
      const bool ok =
         agx3_funop_encode(AGX_ISA_ENCODING_AGX3C, vc[i].op, vc[i].dest,
                           vc[i].src, vc[i].abs, vc[i].neg, vc[i].sat, 0, b,
                           &len);
      expect(vc[i].name, ok && len == 10, b, len, vc[i].hex);
   }

   /* Depositing a wait bit must change the bytes but never the length, on
    * every op whose wait fields are grade A in the wait-field map. */
   static const enum agx3_funop waitable[] = {
      AGX3_FUNOP_FLOOR, AGX3_FUNOP_CEIL,  AGX3_FUNOP_TRUNC,
      AGX3_FUNOP_RINT,  AGX3_FUNOP_LOG2, AGX3_FUNOP_EXP2,
   };
   static const enum agx_isa_encoding encs[] = {AGX_ISA_ENCODING_AGX3B,
                                                AGX_ISA_ENCODING_AGX3C};
   for (unsigned e = 0; e < 2; ++e) {
      const unsigned nslots = e == 0 ? 6 : 8;
      for (unsigned i = 0; i < sizeof(waitable) / sizeof(waitable[0]); ++i) {
         uint8_t plain[AGX3_FUNOP_MAX_LENGTH], waited[AGX3_FUNOP_MAX_LENGTH];
         uint8_t plain_len, waited_len;
         checks++;
         if (!agx3_funop_encode(encs[e], waitable[i], 8, 6, false, false,
                                false, 0, plain, &plain_len) ||
             !agx3_funop_encode(encs[e], waitable[i], 8, 6, false, false,
                                false, 1u << 2, waited, &waited_len) ||
             plain_len != waited_len ||
             memcmp(plain, waited, plain_len) == 0) {
            fprintf(stderr, "wait deposit failed: enc %u op %u\n", e,
                    waitable[i]);
            failures++;
         }
         /* A wait bit past the encoding's slot count is refused. */
         expect_refused("wait past slot count",
                        agx3_funop_encode(encs[e], waitable[i], 8, 6, false,
                                          false, false, 1u << nslots, b,
                                          &len));
      }
   }

   /* Fail-closed domain edges. */
   struct agx3_funop_runtime typed = {.dest_16 = true, .src_16 = true};
   expect_refused("half rounding has no native witness",
                  agx3_funop_encode_g17g(AGX3_FUNOP_FLOOR, 0, 0, false,
                    false, false, 0, &typed, b, &len));
   expect_refused("sqrt step requires float result",
                  agx3_funop_encode_g17g(AGX3_FUNOP_SQRT_STEP, 0, 0, false,
                    false, false, 0, &typed, b, &len));
   typed.dest_16 = false;
   expect_refused("mixed-width rsqrt is not qualified",
                  agx3_funop_encode_g17g(AGX3_FUNOP_RSQRT, 0, 0, false,
                    false, false, 0, &typed, b, &len));
   expect_refused("odd float result beside half source",
                  agx3_funop_encode_g17g(AGX3_FUNOP_LOG2, 1, 3, false,
                    false, false, 0, &typed, b, &len));
   expect_refused("sqrt step negation is not qualified",
                  agx3_funop_encode_g17g(AGX3_FUNOP_SQRT_STEP, 0, 3, false,
                    true, false, 0, &typed, b, &len));
   typed.src_16 = false;
   typed.dest_16 = true;
   expect_refused("odd float source beside half result",
                  agx3_funop_encode_g17g(AGX3_FUNOP_LOG2, 1, 3, false,
                    false, false, 0, &typed, b, &len));
   expect_refused("b saturate (Apple lowers via f32add)",
                  agx3_funop_encode(AGX_ISA_ENCODING_AGX3B, AGX3_FUNOP_FLOOR,
                                    0, 0, false, false, true, 0, b, &len));
   expect_refused("sinc with wait (grade B wait fields)",
                  agx3_funop_encode(AGX_ISA_ENCODING_AGX3B, AGX3_FUNOP_SINC,
                                    0, 0, false, false, false, 1, b, &len));
   expect_refused("dfdx with wait (absent from wait map)",
                  agx3_funop_encode(AGX_ISA_ENCODING_AGX3C, AGX3_FUNOP_DFDX,
                                    0, 0, false, false, false, 1, b, &len));
   expect_refused("odd dest register",
                  agx3_funop_encode(AGX_ISA_ENCODING_AGX3B, AGX3_FUNOP_FLOOR,
                                    1, 0, false, false, false, 0, b, &len));
   expect_refused("odd src register",
                  agx3_funop_encode(AGX_ISA_ENCODING_AGX3B, AGX3_FUNOP_FLOOR,
                                    0, 3, false, false, false, 0, b, &len));
   expect_refused("register out of range",
                  agx3_funop_encode(AGX_ISA_ENCODING_AGX3B, AGX3_FUNOP_FLOOR,
                                    128, 0, false, false, false, 0, b, &len));

   if (failures) {
      fprintf(stderr, "agx3_funop_core: %u/%u checks failed\n", failures,
              checks);
      return 1;
   }

   printf("agx3_funop_core: %u checks passed\n", checks);
   return 0;
}
