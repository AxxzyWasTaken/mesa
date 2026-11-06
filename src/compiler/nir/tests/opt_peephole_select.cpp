/*
 * Copyright (c) 2020 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include <gtest/gtest.h>
#include "nir.h"
#include "nir_builder.h"

class nir_opt_peephole_select_test : public ::testing::Test {
protected:
   nir_opt_peephole_select_test();
   ~nir_opt_peephole_select_test();

   nir_builder bld;
};

nir_opt_peephole_select_test::nir_opt_peephole_select_test()
{
   glsl_type_singleton_init_or_ref();

   static const nir_shader_compiler_options options = { };
   bld = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options, "peephole test");
}

nir_opt_peephole_select_test::~nir_opt_peephole_select_test()
{
   ralloc_free(bld.shader);
   glsl_type_singleton_decref();
}

TEST_F(nir_opt_peephole_select_test, opt_load_ubo_no_speculate)
{
   /* Tests that opt_peephole_select does not optimize ubo loads:
    *
    * impl main {
    *     block b0:  // preds:
    *     32    %0 = load_const (0x00000001)
    *     32    %1 = load_const (0x00000002)
    *     32    %2 = load_const (0x0000000a = 10)
    *     1     %3 = ieq %0 (0x1), %1 (0x2)
    *                // succs: b1 b2
    *     if %3 {
    *         block b1:  // preds: b0
    *         32    %4 = @load_ubo (%0 (0x1), %2 (0xa)) (access=0, align_mul=16, align_offset=0, range_base=16, range=16)
    *                    // succs: b3
    *     } else {
    *         block b2:  // preds: b0
    *         32    %5 = @load_ubo (%0 (0x1), %2 (0xa)) (access=0, align_mul=16, align_offset=0, range_base=16, range=16)
    *                    // succs: b3
    *     }
    *     block b3:  // preds: b1 b2, succs: b4
    *     block b4:
    * }
    */
   nir_function *main = nir_shader_get_function_for_name(bld.shader, "main");

   nir_def *one = nir_imm_int(&bld, 1);
   nir_def *two = nir_imm_int(&bld, 2);
   nir_def *ten = nir_imm_int(&bld, 10);

   nir_def *cmp_result = nir_ieq(&bld, one, two);
   nir_push_if(&bld, cmp_result);

   nir_load_ubo(&bld, 1, 32, one, ten, (gl_access_qualifier)0, 16, 0, 16, 16);

   nir_push_else(&bld, NULL);

   nir_load_ubo(&bld, 1, 32, one, ten, (gl_access_qualifier)0, 16, 0, 16, 16);

   nir_pop_if(&bld, NULL);

   nir_index_blocks(main->impl);
   EXPECT_EQ(main->impl->num_blocks, 4);

   nir_opt_peephole_select_options peephole_select_options = {
      .limit = 16,
      .indirect_load_ok = true,
      .expensive_alu_ok = true,
   };
   ASSERT_FALSE(nir_opt_peephole_select(bld.shader, &peephole_select_options));
   nir_validate_shader(bld.shader, NULL);

   nir_index_blocks(main->impl);
   EXPECT_EQ(main->impl->num_blocks, 4);
}

TEST_F(nir_opt_peephole_select_test, opt_load_ubo_speculate)
{
   /* Tests that opt_peephole_select correctly optimizes speculative ubo loads:
    *
    * impl main {
    *    block b0:  // preds:
    *    32    %0 = load_const (0x00000001)
    *    32    %1 = load_const (0x00000002)
    *    32    %2 = load_const (0x0000000a = 10)
    *    1     %3 = ieq %0 (0x1), %1 (0x2)
    *               // succs: b1 b2
    *    if %3 {
    *        block b1:  // preds: b0
    *        32    %4 = @load_ubo (%0 (0x1), %2 (0xa)) (access=4096, align_mul=16, align_offset=0, range_base=16, range=16)
    *                   // succs: b3
    *    } else {
    *        block b2:  // preds: b0
    *        32    %5 = @load_ubo (%0 (0x1), %2 (0xa)) (access=4096, align_mul=16, align_offset=0, range_base=16, range=16)
    *                   // succs: b3
    *    }
    *    block b3:  // preds: b1 b2, succs: b4
    *    block b4:
    *}
    */
   nir_function *main = nir_shader_get_function_for_name(bld.shader, "main");

   nir_def *one = nir_imm_int(&bld, 1);
   nir_def *two = nir_imm_int(&bld, 2);
   nir_def *ten = nir_imm_int(&bld, 10);

   nir_def *cmp_result = nir_ieq(&bld, one, two);
   nir_push_if(&bld, cmp_result);

   nir_load_ubo(&bld, 1, 32, one, ten, (gl_access_qualifier)ACCESS_CAN_SPECULATE, 16, 0, 16, 16);

   nir_push_else(&bld, NULL);

   nir_load_ubo(&bld, 1, 32, one, ten, (gl_access_qualifier)ACCESS_CAN_SPECULATE, 16, 0, 16, 16);

   nir_pop_if(&bld, NULL);

   nir_index_blocks(main->impl);
   EXPECT_EQ(main->impl->num_blocks, 4);

   nir_opt_peephole_select_options peephole_select_options = {
      .limit = 16,
      .indirect_load_ok = true,
      .expensive_alu_ok = true,
   };
   ASSERT_TRUE(nir_opt_peephole_select(bld.shader, &peephole_select_options));
   nir_validate_shader(bld.shader, NULL);

   nir_index_blocks(main->impl);
   EXPECT_EQ(main->impl->num_blocks, 1);
}

/* An if with rcp in one arm and a cheap arm of the same size in the other. */
static bool
flattens(nir_builder *b, bool rcp_first, unsigned expensive_limit,
         nir_selection_control control = nir_selection_control_none)
{
   nir_def *x = nir_load_var(b, nir_variable_create(b->shader, nir_var_shader_in, glsl_float_type(), "x"));
   nir_def *values[2];

   nir_if *nif = nir_push_if(b, nir_flt_imm(b, x, 1.0));
   nif->control = control;
   for (unsigned arm = 0; arm < 2; arm++) {
      if (arm)
         nir_push_else(b, NULL);
      const bool rcp = rcp_first == (arm == 0);
      values[arm] = nir_fadd_imm(b, rcp ? nir_frcp(b, x) : nir_fadd_imm(b, x, 2.0), 3.0);
   }
   nir_pop_if(b, NULL);
   nir_store_var(b, nir_variable_create(b->shader, nir_var_shader_out, glsl_float_type(), "y"),
                 nir_if_phi(b, values[0], values[1]), 1);

   nir_opt_peephole_select_options options = {
      .limit = 64,
      .expensive_alu_ok = true,
      .expensive_limit = expensive_limit,
   };
   bool progress = nir_opt_peephole_select(b->shader, &options);
   nir_validate_shader(b->shader, NULL);
   return progress;
}

TEST_F(nir_opt_peephole_select_test, expensive_limit)
{
   /* Both arms count, whichever holds the expensive ALU. */
   EXPECT_FALSE(flattens(&bld, true, 3));
}

TEST_F(nir_opt_peephole_select_test, expensive_limit_second_arm)
{
   EXPECT_FALSE(flattens(&bld, false, 3));
}

TEST_F(nir_opt_peephole_select_test, expensive_limit_within)
{
   EXPECT_TRUE(flattens(&bld, true, 4));
}

TEST_F(nir_opt_peephole_select_test, expensive_limit_explicit_flatten)
{
   EXPECT_TRUE(flattens(&bld, true, 3, nir_selection_control_flatten));
}
