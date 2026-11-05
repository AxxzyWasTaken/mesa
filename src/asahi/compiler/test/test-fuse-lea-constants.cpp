/* Copyright 2026 Jonathan Goyvaerts
 * SPDX-License-Identifier: MIT
 */
#include <gtest/gtest.h>
#include "agx_test.h"
extern "C" {
#include "compiler/nir/nir_builder.h"
#include "agx_nir.h"
}

namespace {

/* agx_nir_fuse_lea on base + u2u64(b + amul(c, 16)), then evaluated with
 * base = 0x100000000 and c = 2 by constant folding the fused address. */
uint64_t
fused_address(uint32_t b_const)
{
   const uint64_t base_value = 0x100000000ull;
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "fuse lea");
   nir_def *base = nir_load_preamble(&b, 1, 64, .base = 0);
   nir_def *c = nir_load_preamble(&b, 1, 32, .base = 4);
   nir_def *offset =
      nir_iadd(&b, nir_imm_int(&b, b_const), nir_amul(&b, c, nir_imm_int(&b, 16)));
   nir_def *addr = nir_iadd(&b, base, nir_u2u64(&b, offset));
   nir_store_global(&b, nir_imm_int(&b, 0), addr);

   EXPECT_TRUE(agx_nir_fuse_lea(b.shader));

   b.cursor = nir_before_impl(nir_shader_get_entrypoint(b.shader));
   nir_def_rewrite_uses(base, nir_imm_int64(&b, base_value));
   nir_def_rewrite_uses(c, nir_imm_int(&b, 2));
   nir_opt_constant_folding(b.shader);

   nir_foreach_block(block, nir_shader_get_entrypoint(b.shader)) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic &&
             nir_instr_as_intrinsic(instr)->intrinsic ==
                nir_intrinsic_store_global) {
            nir_src *src = &nir_instr_as_intrinsic(instr)->src[1];
            EXPECT_TRUE(nir_src_is_const(*src));
            uint64_t v = nir_src_as_uint(*src);
            ralloc_free(b.shader);
            return v;
         }
      }
   }
   ADD_FAILURE() << "no store";
   ralloc_free(b.shader);
   return 0;
}

/* The fused address is base + the 32-bit offset, zero-extended. */
TEST(FuseLeaConstants, MatchesTheThirtyTwoBitOffset)
{
   for (uint32_t b_const : {16u, 0x7ffffff0u, 0xa0000000u, 0xfffffff0u,
                            0x80000000u}) {
      const uint64_t expected = 0x100000000ull + (uint32_t)(b_const + 2 * 16);
      EXPECT_EQ(fused_address(b_const), expected) << std::hex << b_const;
   }
}

} // namespace
