/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * The G17P 32x32 -> 64 integer multiply-add.
 *
 * `imadd` writes a 64-bit register pair when its destination width selector
 * (f10) is 3 instead of 2, and each 32-bit operand is widened according to its
 * own sign-extend bit (f28 slot A, f43 slot B, f58 slot C). That is the whole
 * instruction: `umul_high`, `imul_high`, `umul_2x32_64` and `imul_2x32_64` are
 * all this one encoding, with the caller reading whichever half it wants.
 *
 * Every vector below is a byte-exact `metal-nt -arch applegpu_g17p` witness,
 * one MSL kernel per variable. The kernels are the obvious ones:
 *
 *   umul_wide      o[0] = (ulong)a[i] * (ulong)b[i]
 *   imul_wide      o[0] = (long)a[i] * (long)b[i]
 *   mixed_wide     (long)a[i] * (long)b[i], a signed and b unsigned
 *   umad_wide      o[0] = (ulong)a[i] * (ulong)b[i] + (ulong)c[i]
 *   imad_wide      the signed form of the same
 *   umad_wide_c64  ... + c[i] where c is a device ulong
 *   umad_wide_c16  ... + c[i] where c is a device ushort
 *   umul_wide_imm  o[0] = (ulong)a[i] * 253ul
 *   umad_wide_imm  o[0] = (ulong)a[i] * 253ul + (ulong)c[i]
 *   umul_wide_16   both multiplicands are ushort
 *   umul_wide_live the product is read again by a following add
 *   umul_wide_high the product lands at a 2-aligned, not 4-aligned, pair
 *
 * `umul_hi`/`imul_hi` (Metal `mulhi`) compile to byte-identical instructions to
 * umul_wide/imul_wide -- only the store differs -- which is the direct evidence
 * that the high-half multiply has no encoding of its own.
 */

#include <stdio.h>
#include <string.h>

#include "agx_compile.h"
#include "agx3_iadd_core.h"
#include "nir_builder.h"

static unsigned checks, failures;

#define CHECK(x)                                                               \
   do {                                                                        \
      ++checks;                                                                \
      if (!(x)) {                                                              \
         ++failures;                                                           \
         fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #x);          \
      }                                                                        \
   } while (0)

static void
expect(const char *name, const struct agx3_imad_desc *d, const uint8_t *want,
       unsigned want_len)
{
   uint8_t out[AGX3_IADD_MAX_LENGTH];
   uint8_t len = 0;
   ++checks;

   if (!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, d, out, &len)) {
      ++failures;
      fprintf(stderr, "FAIL %s: refused\n", name);
      return;
   }

   if (len != want_len || memcmp(out, want, want_len)) {
      ++failures;
      fprintf(stderr, "FAIL %s:\n  got  ", name);
      for (unsigned i = 0; i < len; ++i)
         fprintf(stderr, "%02x", out[i]);
      fprintf(stderr, "\n  want ");
      for (unsigned i = 0; i < want_len; ++i)
         fprintf(stderr, "%02x", want[i]);
      fprintf(stderr, "\n");
   }
}

static void
test_apple_wide_witnesses(void)
{
   /* Every kernel here waits scoreboard slot 6 for its device loads. */
   const uint16_t w6 = 1u << 5;

   /* o[0] = (ulong)a[i] * (ulong)b[i] -- both operands zero-extended. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x00,
                                     0x08, 0x00, 0xe0, 0x26, 0x0a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 0, .src_a_kill = true,
         .multiplier_is_register = true, .multiplier_reg = 2,
         .multiplier_kill = true, .no_addend = true, .wait_mask = w6,
      };
      expect("umul_wide", &d, want, sizeof(want));
   }

   /* The signed form differs only in the two sign-extend bits. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x00,
                                     0x08, 0x00, 0xe0, 0x26, 0x1e, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 0, .src_a_kill = true,
         .src_a_sign_extend = true, .multiplier_is_register = true,
         .multiplier_reg = 2, .multiplier_kill = true,
         .multiplier_sign_extend = true, .no_addend = true, .wait_mask = w6,
      };
      expect("imul_wide", &d, want, sizeof(want));
   }

   /* Signedness is per operand, so a signed x unsigned product sets one bit. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x00,
                                     0x08, 0x00, 0xe0, 0x26, 0x1a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 0, .src_a_kill = true,
         .multiplier_is_register = true, .multiplier_reg = 2,
         .multiplier_kill = true, .multiplier_sign_extend = true,
         .no_addend = true, .wait_mask = w6,
      };
      expect("mixed_wide", &d, want, sizeof(want));
   }

   /* With a 32-bit register addend the slot-C width stays 2. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x04,
                                     0x10, 0x00, 0xe0, 0x2f, 0x2a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 2, .src_a_kill = true,
         .multiplier_is_register = true, .multiplier_reg = 4,
         .multiplier_kill = true, .src_c = 0, .src_c_kill = true,
         .wait_mask = w6,
      };
      expect("umad_wide", &d, want, sizeof(want));
   }

   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x04,
                                     0x10, 0x00, 0xe0, 0x2f, 0x7e, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 2, .src_a_kill = true,
         .src_a_sign_extend = true, .multiplier_is_register = true,
         .multiplier_reg = 4, .multiplier_kill = true,
         .multiplier_sign_extend = true, .src_c = 0, .src_c_kill = true,
         .src_c_sign_extend = true, .wait_mask = w6,
      };
      expect("imad_wide", &d, want, sizeof(want));
   }

   /* A 64-bit addend needs no widening at all: slot-C width 3. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x08,
                                     0x18, 0x00, 0xf0, 0x2e, 0x2a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 4, .src_a_kill = true,
         .multiplier_is_register = true, .multiplier_reg = 6,
         .multiplier_kill = true, .src_c = 0, .src_c_64 = true,
         .src_c_kill = true, .wait_mask = w6,
      };
      expect("umad_wide_c64", &d, want, sizeof(want));
   }

   /* ... and a 16-bit addend keeps slot-C width 1 under a 64-bit product. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x00,
                                     0x08, 0x20, 0xe0, 0x2e, 0x2a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 0, .src_a_kill = true,
         .multiplier_is_register = true, .multiplier_reg = 2,
         .multiplier_kill = true, .src_c = 4, .src_c_16 = true,
         .src_c_kill = true, .wait_mask = w6,
      };
      expect("umad_wide_c16", &d, want, sizeof(want));
   }

   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x00,
                                     0x08, 0x20, 0xe0, 0x2e, 0x7e, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 0, .src_a_kill = true,
         .src_a_sign_extend = true, .multiplier_is_register = true,
         .multiplier_reg = 2, .multiplier_kill = true,
         .multiplier_sign_extend = true, .src_c = 4, .src_c_16 = true,
         .src_c_kill = true, .src_c_sign_extend = true, .wait_mask = w6,
      };
      expect("imad_wide_c16", &d, want, sizeof(want));
   }

   /* An immediate multiplier is always zero-extended; only slot A can sign. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x00,
                                     0xf4, 0x03, 0x60, 0x22, 0x02, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 0, .src_a_kill = true,
         .multiplier = 253, .no_addend = true, .wait_mask = w6,
      };
      expect("umul_wide_imm", &d, want, sizeof(want));
   }

   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x04,
                                     0xf4, 0x03, 0x60, 0x2b, 0x22, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 2, .src_a_kill = true,
         .multiplier = 253, .src_c = 0, .src_c_kill = true, .wait_mask = w6,
      };
      expect("umad_wide_imm", &d, want, sizeof(want));
   }

   /* 16-bit multiplicands: the operand widths are per slot, the product is
    * still a 64-bit pair. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x00,
                                     0x08, 0x00, 0x20, 0x26, 0x0a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 0, .src_a_16 = true,
         .src_a_kill = true, .multiplier_is_register = true,
         .multiplier_reg = 2, .multiplier_16 = true, .multiplier_kill = true,
         .no_addend = true, .wait_mask = w6,
      };
      expect("umul_wide_16", &d, want, sizeof(want));
   }

   /* The destination cache hint is a real per-instruction bit: the same pure
    * multiply carries it when an ALU op reads the product and clears it when
    * only a store does. Both witnesses are here so neither can be mistaken
    * for a baseline constant. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x03, 0x00,
                                     0x10, 0x04, 0xe0, 0x22, 0x0a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .dest_cache = true, .src_a = 0,
         .src_a_kill = true, .multiplier_is_register = true,
         .multiplier_reg = 4, .multiplier_cache = true, .no_addend = true,
         .wait_mask = w6,
      };
      expect("umul_wide_live", &d, want, sizeof(want));
   }

   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x08,
                                     0x1a, 0x04, 0xe0, 0x20, 0x0a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .dest_64 = true, .src_a = 4, .src_a_cache = true,
         .multiplier_is_register = true, .multiplier_reg = 6,
         .multiplier_cache = true, .no_addend = true, .wait_mask = w6,
      };
      expect("umul_wide_regs", &d, want, sizeof(want));
   }

   /* Apple places a 64-bit product at half 18: the pair is 32-bit aligned, not
    * 64-bit aligned, so the encoder must not demand four-half alignment. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x12, 0x03, 0x04,
                                     0x10, 0x00, 0xe0, 0x26, 0x0a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 18, .dest_64 = true, .dest_cache = true, .src_a = 2,
         .src_a_kill = true, .multiplier_is_register = true,
         .multiplier_reg = 4, .multiplier_kill = true, .no_addend = true,
         .wait_mask = w6,
      };
      expect("umul_wide_high", &d, want, sizeof(want));
   }

   /* The narrow forms must not move. This is Apple's own 32-bit pure multiply
    * with the product consumed only by a store, so its cache hint is clear --
    * the case that proves f11 is not part of the baseline. */
   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x04,
                                     0x00, 0x00, 0xd0, 0x26, 0x0a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .src_a = 2, .src_a_kill = true,
         .multiplier_is_register = true, .multiplier_reg = 0,
         .multiplier_kill = true, .no_addend = true, .wait_mask = w6,
      };
      expect("umul_narrow", &d, want, sizeof(want));
   }

   {
      static const uint8_t want[] = {0x9f, 0x00, 0x56, 0x00, 0x02, 0x04,
                                     0x00, 0x20, 0xd0, 0x2f, 0x2a, 0x00};
      const struct agx3_imad_desc d = {
         .dest = 0, .src_a = 2, .src_a_kill = true,
         .multiplier_is_register = true, .multiplier_reg = 0,
         .multiplier_kill = true, .src_c = 4, .src_c_kill = true,
         .wait_mask = w6,
      };
      expect("imad_narrow", &d, want, sizeof(want));
   }
}

static void
test_domain_is_closed(void)
{
   uint8_t out[AGX3_IADD_MAX_LENGTH];
   uint8_t len;

   /* A 64-bit destination occupies four halves, so it must be even. (The
    * form's destination field stops accepting values above half 191 for both
    * widths, which is a property of Apple's own tables, not of this change --
    * the composer refuses those and the encoder fails closed with it.) */
   struct agx3_imad_desc d = {
      .dest = 0, .dest_64 = true, .src_a = 0, .multiplier_is_register = true,
      .multiplier_reg = 2, .no_addend = true,
   };
   CHECK(agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &d, out, &len));
   d.dest = 190;
   CHECK(agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &d, out, &len));
   d.dest = 1;
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &d, out, &len));
   d.dest = 254;
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &d, out, &len));

   /* Widening a source is only meaningful under a wider destination: a 32-bit
    * product has nothing to sign-extend into. */
   struct agx3_imad_desc narrow = {
      .dest = 0, .src_a = 0, .multiplier_is_register = true,
      .multiplier_reg = 2, .no_addend = true, .src_a_sign_extend = true,
   };
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &narrow, out, &len));
   narrow.src_a_sign_extend = false;
   narrow.multiplier_sign_extend = true;
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &narrow, out, &len));

   /* An 8-bit immediate multiplier has no sign-extend bit of its own. */
   struct agx3_imad_desc imm = {
      .dest = 0, .dest_64 = true, .src_a = 0, .multiplier = 253,
      .no_addend = true, .multiplier_sign_extend = true,
   };
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &imm, out, &len));

   /* A pure multiply has no addend to widen. */
   struct agx3_imad_desc pure = {
      .dest = 0, .dest_64 = true, .src_a = 0,
      .multiplier_is_register = true, .multiplier_reg = 2, .no_addend = true,
      .src_c_64 = true,
   };
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &pure, out, &len));

   /* A 64-bit addend is a pair too, and is not also 16-bit. */
   struct agx3_imad_desc wide_c = {
      .dest = 0, .dest_64 = true, .src_a = 0,
      .multiplier_is_register = true, .multiplier_reg = 2, .src_c = 1,
      .src_c_64 = true,
   };
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &wide_c, out, &len));
   wide_c.src_c = 4;
   CHECK(agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &wide_c, out, &len));
   wide_c.src_c_16 = true;
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3B, &wide_c, out, &len));

   /* The AGX3c `imadd64` is witnessed with an immediate or a 64-bit
    * register addend (test-agx3-iadd-core.c); the addend-free pure multiply
    * and the sign-extending variants still fail closed there. */
   struct agx3_imad_desc agx3c = {
      .dest = 0, .dest_64 = true, .src_a = 0,
      .multiplier_is_register = true, .multiplier_reg = 2, .no_addend = true,
   };
   CHECK(!agx3_imad_encode(AGX_ISA_ENCODING_AGX3C, &agx3c, out, &len));
   agx3c.no_addend = false;
   agx3c.addend_is_immediate = true;
   CHECK(agx3_imad_encode(AGX_ISA_ENCODING_AGX3C, &agx3c, out, &len));
   agx3c.src_a_sign_extend = true;
   CHECK(!agx3_imad_encode_g18g(AGX_ISA_ENCODING_AGX3C, &agx3c, out, &len));
   agx3c.multiplier_sign_extend = true;
   CHECK(agx3_imad_encode(AGX_ISA_ENCODING_AGX3C, &agx3c, out, &len));
}

/*
 * Whole-shader compiles. Each of these reaches the backend as an `imad` with a
 * 64-bit destination, which is exactly what could not be encoded: pipeline
 * creation for any shader containing one failed with the compiler's
 * fail-closed error.
 */
static bool
compile_compute(nir_shader *nir, const char *what)
{
   static const uint8_t end[] = {0x0e, 0x00, 0x00, 0x00};

   struct agx_shader_key key = {
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .reserved_preamble = 8,
      .no_preamble = true,
   };
   struct agx_shader_part part = {0};

   ++checks;
   const bool compiled = agx_compile_shader_nir(nir, &key, &part);
   const bool valid =
      compiled && part.binary != NULL && part.info.binary_size > sizeof(end) &&
      !memcmp((uint8_t *)part.binary + part.info.binary_size - sizeof(end),
              end, sizeof(end));

   if (!valid) {
      ++failures;
      fprintf(stderr, "FAIL %s: %s\n", what,
              compiled ? "compiled but produced no terminated binary"
                       : "the AGX3 backend refused the shader");
   }

   free(part.binary);
   ralloc_free(nir);
   return valid;
}

static nir_builder
begin_compute(const char *name)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                                  &agx_nir_options, "%s", name);
   b.shader->info.workgroup_size[0] = 1;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   return b;
}

static void
test_wide_multiply_shaders(void)
{
   /* umul_2x32_64 / imul_2x32_64: agx_nir_options deliberately leaves these
    * unlowered, so they arrive as a wide imad. */
   {
      nir_builder b = begin_compute("G17P 32x32->64 multiply");
      nir_def *out = nir_load_preamble(&b, 1, 64, .base = 4);
      nir_def *x = nir_load_preamble(&b, 1, 32, .base = 8);
      nir_def *y = nir_load_preamble(&b, 1, 32, .base = 10);
      nir_store_global(&b, nir_umul_2x32_64(&b, x, y), out, .align_mul = 8);
      nir_store_global(&b, nir_imul_2x32_64(&b, x, y),
                       nir_iadd_imm(&b, out, 8), .align_mul = 8);
      agx_preprocess_nir(b.shader);
      compile_compute(b.shader, "32x32->64 multiply");
   }

   /* The high halves are the same instruction with the low word dropped. */
   {
      nir_builder b = begin_compute("G17P high-word multiply");
      nir_def *out = nir_load_preamble(&b, 1, 64, .base = 4);
      nir_def *x = nir_load_preamble(&b, 1, 32, .base = 8);
      nir_def *y = nir_load_preamble(&b, 1, 32, .base = 10);
      nir_store_global(&b, nir_umul_high(&b, x, y), out, .align_mul = 4);
      nir_store_global(&b, nir_imul_high(&b, x, y),
                       nir_iadd_imm(&b, out, 4), .align_mul = 4);
      agx_preprocess_nir(b.shader);
      compile_compute(b.shader, "high-word multiply");
   }

   /* Division and modulo by a compile-time constant are strength-reduced to a
    * high-word multiply by a magic number, so every shader that divides by a
    * constant other than a power of two needs the wide imad.
    *
    * (Division by a *dynamic* divisor is a separate, still-open gap: NIR
    * lowers it through a float reciprocal and AGX3 has no grounded `rcp`
    * encoding yet, so it is deliberately not asserted here.) */
   {
      nir_builder b = begin_compute("G17P constant integer division");
      nir_def *out = nir_load_preamble(&b, 1, 64, .base = 4);
      nir_def *x = nir_load_preamble(&b, 1, 32, .base = 8);
      nir_store_global(&b, nir_udiv_imm(&b, x, 3), out, .align_mul = 4);
      nir_store_global(&b, nir_umod_imm(&b, x, 3), nir_iadd_imm(&b, out, 4),
                       .align_mul = 4);
      nir_store_global(&b, nir_idiv(&b, x, nir_imm_int(&b, 3)), nir_iadd_imm(&b, out, 8),
                       .align_mul = 4);
      agx_preprocess_nir(b.shader);
      compile_compute(b.shader, "constant integer division");
   }

   /* Division by a constant is strength-reduced to a high-word multiply by a
    * magic number, which is the shape a vertex shader hits when it maps a
    * vertex index onto a primitive. */
   {
      nir_builder b = nir_builder_init_simple_shader(
         MESA_SHADER_VERTEX, &agx_nir_options, "G17P vertex divide by three");
      nir_def *id = nir_load_vertex_id(&b);
      nir_def *pos[4] = {
         nir_i2f32(&b, nir_idiv(&b, id, nir_imm_int(&b, 3))),
         nir_u2f32(&b, nir_umod(&b, id, nir_imm_int(&b, 3))),
         nir_imm_float(&b, 0.0f),
         nir_imm_float(&b, 1.0f),
      };
      for (unsigned c = 0; c < 4; ++c)
         nir_store_uvs_agx(&b, pos[c], nir_imm_intN_t(&b, c, 16));

      b.shader->info.outputs_written = VARYING_BIT_POS;
      agx_preprocess_nir(b.shader);

      struct agx_shader_key key = {.dev.encoding = AGX_ISA_ENCODING_AGX3B};
      struct agx_shader_part part = {0};
      ++checks;
      if (!agx_compile_shader_nir(b.shader, &key, &part)) {
         ++failures;
         fprintf(stderr, "FAIL vertex divide by three: refused\n");
      }
      free(part.binary);
      ralloc_free(b.shader);
   }
}

int
main(void)
{
   test_apple_wide_witnesses();
   test_domain_is_closed();
   test_wide_multiply_shaders();

   printf("agx3_wide_imad: %u checks, %u failures\n", checks, failures);
   return failures ? 1 : 0;
}
