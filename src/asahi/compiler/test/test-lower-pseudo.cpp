/*
 * Copyright 2021 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "agx_test.h"

#include <gtest/gtest.h>

#define CASE(instr, expected)                                                  \
   INSTRUCTION_CASE(instr, expected, agx_lower_pseudo)
#define NEGCASE(instr) CASE(instr, instr)

class LowerPseudo : public testing::Test {
 protected:
   LowerPseudo()
   {
      mem_ctx = ralloc_context(NULL);

      wx = agx_register(0, AGX_SIZE_32);
      wy = agx_register(2, AGX_SIZE_32);
      wz = agx_register(4, AGX_SIZE_32);
   }

   ~LowerPseudo()
   {
      ralloc_free(mem_ctx);
   }

   void *mem_ctx;
   agx_index wx, wy, wz;
};

TEST_F(LowerPseudo, Move)
{
   CASE(agx_mov_to(b, wx, wy), agx_bitop_to(b, wx, wy, agx_zero(), 0xA));
}

TEST_F(LowerPseudo, Agx3ImmediateMove)
{
   agx_shader_key key = {};
   key.dev.encoding = AGX_ISA_ENCODING_AGX3B;

   agx_builder *actual = agx_test_builder(mem_ctx);
   actual->shader->key = &key;
   agx_mov_to(actual, wx, agx_immediate(7));

   agx_builder *expected = agx_test_builder(mem_ctx);
   expected->shader->key = &key;
   agx_mov_imm_to(expected, wx, 7);

   agx_lower_pseudo(actual->shader);
   ASSERT_SHADER_EQUAL(actual->shader, expected->shader);
}

TEST_F(LowerPseudo, Not)
{
   CASE(agx_not_to(b, wx, wy), agx_bitop_to(b, wx, wy, agx_zero(), 0x5));
}

TEST_F(LowerPseudo, G18GMaterializesImmediateFloatAdd)
{
   /* G18G only (agx_is_g18g): the G17G-family compile re-encoded as agx3d. */
   {
      agx_shader_key key = {};
      key.dev.encoding = AGX_ISA_ENCODING_AGX3C;
      key.dev.emit_agx3d = true;
      const uint8_t constants[] = {0x00, 0x80, 0x30};
      const uint32_t bits[] = {0x00000000, 0x80000000, 0x3f800000};
      for (unsigned i = 0; i < 3; ++i) {
         agx_builder *actual = agx_test_builder(mem_ctx);
         actual->shader->key = &key;
         agx_fadd_to(actual, wx, agx_immediate(constants[i]), agx_negzero());

         agx_builder *expected = agx_test_builder(mem_ctx);
         expected->shader->key = &key;
         agx_mov_imm_to(expected, wx, bits[i]);
         agx_fadd_to(expected, wx, wx, agx_negzero());

         agx_lower_pseudo(actual->shader);
         ASSERT_SHADER_EQUAL(actual->shader, expected->shader);
      }
   }
}

TEST_F(LowerPseudo, LowersAgx3UnconditionalBreak)
{
   agx_shader_key key = {};
   key.dev.encoding = AGX_ISA_ENCODING_AGX3B;

   agx_builder *actual = agx_test_builder(mem_ctx);
   actual->shader->key = &key;
   agx_begin_cf(actual);
   agx_break_if_icmp(actual, wx, wy, 1, false, AGX_ICOND_ULT, NULL);
   agx_break(actual, 2, NULL);

   agx_builder *expected = agx_test_builder(mem_ctx);
   expected->shader->key = &key;
   agx_begin_cf(expected);
   agx_break_if_icmp(expected, wx, wy, 1, false, AGX_ICOND_ULT, NULL);
   agx_index r0l = agx_register(0, AGX_SIZE_16);
   agx_break_if_icmp(expected, r0l, r0l, 2, false, AGX_ICOND_UEQ, NULL);

   agx_lower_pseudo(actual->shader);
   ASSERT_SHADER_EQUAL(actual->shader, expected->shader);
}

/* NEST carries through at any depth: the packer, not this pass, turns it into
 * the native exec-mask level count, and CNDCF encodes an arbitrary one. */
TEST_F(LowerPseudo, LowersAgx3UnconditionalBreakAtEveryDepth)
{
   agx_shader_key key = {};
   key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   agx_index r0l = agx_register(0, AGX_SIZE_16);

   for (unsigned nest = 1; nest <= 8; ++nest) {
      agx_builder *actual = agx_test_builder(mem_ctx);
      actual->shader->key = &key;
      agx_begin_cf(actual);
      agx_break(actual, nest, NULL);

      agx_builder *expected = agx_test_builder(mem_ctx);
      expected->shader->key = &key;
      agx_begin_cf(expected);
      agx_break_if_icmp(expected, r0l, r0l, nest, false, AGX_ICOND_UEQ, NULL);

      agx_lower_pseudo(actual->shader);
      ASSERT_SHADER_EQUAL(actual->shader, expected->shader);
   }
}

TEST_F(LowerPseudo, Agx3ConstantControlFlowComparesUseTheIdentity)
{
   agx_shader_key key = {};
   key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   agx_index r0l = agx_register(0, AGX_SIZE_16);

   agx_builder *actual = agx_test_builder(mem_ctx);
   actual->shader->key = &key;
   agx_begin_cf(actual);
   /* if (true), as emitted for a constant condition: taken. */
   agx_if_icmp(actual, agx_immediate(1), agx_zero(), 1, AGX_ICOND_UEQ, true,
               NULL);
   /* if (1 == 0): never taken. */
   agx_if_icmp(actual, agx_immediate(1), agx_zero(), 1, AGX_ICOND_UEQ, false,
               NULL);
   agx_break_if_icmp(actual, agx_immediate(2), agx_immediate(3), 1, false,
                     AGX_ICOND_ULT, NULL);
   agx_break_if_icmp(actual, agx_immediate(2), agx_immediate(3), 1, true,
                     AGX_ICOND_ULT, NULL);
   /* A sign-dependent compare of immediates is left alone. */
   agx_if_icmp(actual, agx_immediate(0x8000), agx_zero(), 1, AGX_ICOND_SLT,
               false, NULL);

   agx_builder *expected = agx_test_builder(mem_ctx);
   expected->shader->key = &key;
   agx_begin_cf(expected);
   agx_if_icmp(expected, r0l, r0l, 1, AGX_ICOND_UEQ, false, NULL);
   agx_if_icmp(expected, r0l, r0l, 1, AGX_ICOND_UEQ, true, NULL);
   agx_break_if_icmp(expected, r0l, r0l, 1, false, AGX_ICOND_UEQ, NULL);
   agx_break_if_icmp(expected, r0l, r0l, 1, true, AGX_ICOND_UEQ, NULL);
   agx_if_icmp(expected, agx_immediate(0x8000), agx_zero(), 1, AGX_ICOND_SLT,
               false, NULL);

   agx_lower_pseudo(actual->shader);
   ASSERT_SHADER_EQUAL(actual->shader, expected->shader);
}

TEST_F(LowerPseudo, Agx3DivergentShuffleUsesFullRegisterLane)
{
   agx_shader_key key = {};
   key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   agx_index result = agx_register(12, AGX_SIZE_32);
   agx_index value = agx_register(4, AGX_SIZE_32);
   agx_index lane = agx_register(8, AGX_SIZE_32);

   agx_builder *actual = agx_test_builder(mem_ctx);
   actual->shader->key = &key;
   actual->shader->any_cf = true;
   actual->shader->any_quad_divergent_shuffle = true;
   agx_shuffle_to(actual, result, value, lane);

   agx_builder *expected = agx_test_builder(mem_ctx);
   expected->shader->key = &key;
   expected->shader->any_cf = true;
   expected->shader->any_quad_divergent_shuffle = true;
   /* AGX3 reserves r1 so r0h remains the control-flow nesting counter. */
   agx_index scratch = agx_register(2, AGX_SIZE_32);
   agx_mov_imm_to(expected, scratch, 0);
   agx_mov_to(expected, scratch, lane);
   agx_shuffle_to(expected, result, value, scratch);
   agx_mov_imm_to(expected, scratch, 0);

   agx_lower_divergent_shuffle(actual->shader);
   ASSERT_SHADER_EQUAL(actual->shader, expected->shader);
}
