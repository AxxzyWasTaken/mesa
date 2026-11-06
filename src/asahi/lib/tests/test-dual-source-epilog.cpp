/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include "compiler/nir/nir_builder.h"
#include <gtest/gtest.h>
#include "agx_abi.h"
#include "agx_linker.h"
extern "C" {
#include "asahi/compiler/agx_nir.h"
}

class DualSourceEpilog : public ::testing::Test {
 protected:
   DualSourceEpilog()
   {
      glsl_type_singleton_init_or_ref();
   }
   ~DualSourceEpilog()
   {
      glsl_type_singleton_decref();
   }
};

/* Deliberately distinct primary, secondary and destination values: confusing
 * the two export slots must change the result. All are exactly representable.
 */
static const float colours[4][4] = {
   {0.125f, 0.25f, 0.375f, 0.5f},
   {0.75f, 0.625f, 0.5f, 0.375f},
   {0.25f, 0.5f, 0.125f, 0.75f},
   {0.5f, 0.125f, 0.75f, 0.25f},
};
static const float destination[4] = {0.25f, 0.125f, 0.5f, 0.25f};

static bool
substitute_inputs(nir_builder *b, nir_intrinsic_instr *intr, void *)
{
   const float *v;
   if (intr->intrinsic == nir_intrinsic_load_exported_agx) {
      unsigned base = nir_intrinsic_base(intr);
      EXPECT_GE(base, AGX_ABI_FOUT_COLOUR(0));
      unsigned loc = (base - AGX_ABI_FOUT_COLOUR(0)) / 8;
      EXPECT_LT(loc, 4u);
      if (loc >= 4)
         return false;
      v = colours[loc];
   } else if (intr->intrinsic == nir_intrinsic_load_local_pixel_agx) {
      v = destination;
   } else {
      return false;
   }
   EXPECT_EQ(intr->def.bit_size, 32u);
   EXPECT_EQ(intr->def.num_components, 4u);
   b->cursor = nir_instr_remove(&intr->instr);
   nir_def_rewrite_uses(&intr->def, nir_imm_vec4(b, v[0], v[1], v[2], v[3]));
   return true;
}

static float
blend(enum pipe_blend_func op, float src, float dst, float src_factor,
      float dst_factor)
{
   switch (op) {
   case PIPE_BLEND_ADD:
      return src * src_factor + dst * dst_factor;
   case PIPE_BLEND_SUBTRACT:
      return src * src_factor - dst * dst_factor;
   case PIPE_BLEND_REVERSE_SUBTRACT:
      return dst * dst_factor - src * src_factor;
   case PIPE_BLEND_MIN:
      return std::min(src, dst);
   case PIPE_BLEND_MAX:
      return std::max(src, dst);
   default:
      ADD_FAILURE();
      return 0;
   }
}

TEST_F(DualSourceEpilog, CTSGenericAndDualState)
{
   const pipe_blendfactor factors[] = {
      PIPE_BLENDFACTOR_SRC1_COLOR,
      PIPE_BLENDFACTOR_INV_SRC1_COLOR,
      PIPE_BLENDFACTOR_SRC1_ALPHA,
      PIPE_BLENDFACTOR_INV_SRC1_ALPHA,
   };
   const pipe_blend_func ops[] = {PIPE_BLEND_ADD, PIPE_BLEND_SUBTRACT,
                                  PIPE_BLEND_REVERSE_SUBTRACT, PIPE_BLEND_MIN,
                                  PIPE_BLEND_MAX};
   for (bool dual : {false, true}) {
      for (unsigned f = 0; f < ARRAY_SIZE(factors); ++f) {
         for (pipe_blend_func op : ops) {
            SCOPED_TRACE(::testing::Message()
                         << "dual=" << dual << " factor=" << f << " op=" << op);
            agx_fs_epilog_key key = {};
            key.nr_samples = 1;
            key.link.loc_written = dual ? 3 : 15;
            key.link.size_32 = key.link.loc_written;
            for (unsigned i = 0; i < 8; ++i)
               key.remap[i] = i;
            for (unsigned rt = dual ? 0 : 1; rt < (dual ? 1 : 4); ++rt) {
               key.rt_formats[rt] = PIPE_FORMAT_R32G32B32A32_FLOAT;
               /* CTS maps secondary-source factors to primary-source factors
                * for its generic pipeline. */
               const pipe_blendfactor generic[] = {
                  PIPE_BLENDFACTOR_SRC_COLOR,
                  PIPE_BLENDFACTOR_INV_SRC_COLOR,
                  PIPE_BLENDFACTOR_SRC_ALPHA,
                  PIPE_BLENDFACTOR_INV_SRC_ALPHA,
               };
               key.blend.rt[rt].colormask = 15;
               key.blend.rt[rt].mode = agx_pack_blend_standard(
                  op, PIPE_BLENDFACTOR_SRC_COLOR,
                  dual ? factors[f] : generic[f], op,
                  PIPE_BLENDFACTOR_SRC_ALPHA,
                  dual ? (f & 1 ? PIPE_BLENDFACTOR_INV_SRC1_ALPHA
                                : PIPE_BLENDFACTOR_SRC1_ALPHA)
                       : (f & 1 ? PIPE_BLENDFACTOR_INV_SRC_ALPHA
                                : PIPE_BLENDFACTOR_SRC_ALPHA));
            }
            nir_builder b = nir_builder_init_simple_shader(
               MESA_SHADER_FRAGMENT, &agx_nir_options,
               "CTS dual-source epilog");
            agx_nir_fs_epilog_agx2(&b, &key);
            nir_validate_shader(b.shader, "dual-source epilog");
            nir_shader_intrinsics_pass(b.shader, substitute_inputs,
                                       nir_metadata_none, NULL);
            bool progress;
            do {
               progress = false;
               NIR_PASS(progress, b.shader, nir_opt_constant_folding);
               NIR_PASS(progress, b.shader, nir_opt_algebraic);
               NIR_PASS(progress, b.shader, nir_opt_dce);
            } while (progress);
            unsigned stores = 0;
            nir_foreach_function_impl(impl, b.shader) {
               nir_foreach_block(block, impl) {
                  nir_foreach_instr(instr, block) {
                     if (instr->type != nir_instr_type_intrinsic)
                        continue;
                     auto *intr = nir_instr_as_intrinsic(instr);
                     if (intr->intrinsic != nir_intrinsic_store_local_pixel_agx)
                        continue;
                     unsigned rt = nir_intrinsic_target(intr);
                     ASSERT_TRUE(dual ? rt == 0 : rt >= 1 && rt < 4);
                     const float *src = colours[rt],
                                 *src1 = dual ? colours[1] : src;
                     for (unsigned c = 0; c < 4; ++c) {
                        nir_scalar scalar =
                           nir_scalar_resolved(intr->src[0].ssa, c);
                        ASSERT_TRUE(nir_scalar_is_const(scalar));
                        float df = src1[c == 3 || f >= 2 ? 3 : c];
                        if (f & 1)
                           df = 1 - df;
                        float expected = blend(op, src[c], destination[c],
                                               src[c == 3 ? 3 : c], df);
                        EXPECT_NEAR(nir_scalar_as_float(scalar), expected,
                                    0.000001);
                     }
                     ++stores;
                  }
               }
            }
            EXPECT_EQ(stores, dual ? 1u : 3u);
            ralloc_free(b.shader);
         }
      }
   }
}
