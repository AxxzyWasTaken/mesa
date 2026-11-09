/* SPDX-License-Identifier: MIT */
#include "hk_nir_sampler_bias.h"
#include "asahi/compiler/agx_nir_texture.h"
#include "compiler/glsl_types.h"
#include <stdio.h>

/* Keep this test's assert()s in NDEBUG builds. NIR's headers above must see
 * the build's NDEBUG, so that nir_validate_shader() matches what libnir
 * provides (an inline no-op in NDEBUG builds). */
#undef NDEBUG
#include <assert.h>

static const nir_shader_compiler_options options = {0};

static nir_tex_instr *
texture(nir_builder *b, nir_texop op, bool shadow)
{
   unsigned count = 3 + (op == nir_texop_txb || op == nir_texop_txl) +
                    2 * (op == nir_texop_txd) + shadow;
   unsigned n = 0;
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, count);
   tex->op = op;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->is_shadow = shadow;
   tex->is_new_style_shadow = shadow;
   tex->dest_type = nir_type_float32;
   tex->coord_components = 2;
   tex->src[n++] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_imm_vec2(b, .5, .5));
   /* Distinct dynamic handles must survive the query and custom-border copy. */
   tex->src[n++] = nir_tex_src_for_ssa(nir_tex_src_texture_handle,
                         nir_load_uniform(b, 1, 32, nir_imm_int(b, 0)));
   tex->src[n++] = nir_tex_src_for_ssa(nir_tex_src_sampler_handle,
                         nir_load_uniform(b, 1, 32, nir_imm_int(b, 4)));
   if (op == nir_texop_txb)
      tex->src[n++] = nir_tex_src_for_ssa(nir_tex_src_bias, nir_imm_float(b, .590551f));
   if (op == nir_texop_txl)
      tex->src[n++] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_float(b, 1));
   if (op == nir_texop_txd) {
      tex->src[n++] = nir_tex_src_for_ssa(nir_tex_src_ddx, nir_imm_vec2(b, 1, 0));
      tex->src[n++] = nir_tex_src_for_ssa(nir_tex_src_ddy, nir_imm_vec2(b, 0, 1));
   }
   if (shadow)
      tex->src[n++] = nir_tex_src_for_ssa(nir_tex_src_comparator, nir_imm_float(b, .5));
   nir_def_init(&tex->instr, &tex->def, nir_tex_instr_dest_size(tex), 32);
   nir_builder_instr_insert(b, &tex->instr);
   return tex;
}

int main(void)
{
   glsl_type_singleton_init_or_ref();
   const nir_texop ops[] = {nir_texop_tex, nir_texop_txb, nir_texop_txl,
                            nir_texop_txd, nir_texop_lod};
   unsigned checks = 0;
   for (unsigned shadow = 0; shadow < 2; ++shadow) {
      for (unsigned i = 0; i < ARRAY_SIZE(ops); ++i) {
         if (shadow && ops[i] == nir_texop_lod)
            continue;
         nir_builder b = nir_builder_init_simple_shader(
            MESA_SHADER_FRAGMENT, &options, "sampler bias");
         nir_tex_instr *tex = texture(&b, ops[i], shadow);
         nir_def *handle = nir_get_tex_src(tex, nir_tex_src_sampler_handle);
         /* An explicit LOD takes the sampler's bias too (D3D LOD rules). */
         bool expected =
            (ops[i] == nir_texop_txb || ops[i] == nir_texop_txl) && !shadow;
         bool changed = nir_shader_tex_pass(b.shader, hk_lower_g17p_sampler_bias,
                                            nir_metadata_control_flow, NULL);
         assert(changed == expected);
         assert(!nir_shader_tex_pass(b.shader, hk_lower_g17p_sampler_bias,
                                    nir_metadata_control_flow, NULL));
         unsigned queries = 0;
         nir_foreach_function_impl(impl, b.shader) {
            nir_foreach_block(block, impl) {
               nir_foreach_instr(instr, block) {
                  if (instr->type != nir_instr_type_tex)
                     continue;
                  nir_tex_instr *q = nir_instr_as_tex(instr);
                  if (q->op != nir_texop_lod_bias)
                     continue;
                  queries++;
                  assert(q->backend_flags & HK_TEXTURE_FLAG_COMBINED_BIAS);
                  assert(nir_get_tex_src(q, nir_tex_src_sampler_handle) == handle);
               }
            }
         }
         assert(queries == expected);
         if (expected) {
            b.cursor = nir_after_instr(&tex->instr);
            nir_tex_instr *copy = nir_instr_as_tex(nir_instr_clone(b.shader, &tex->instr));
            copy->backend_flags |= AGX_TEXTURE_FLAG_CLAMP_TO_0;
            nir_builder_instr_insert(&b, &copy->instr);
            assert(copy->backend_flags & HK_TEXTURE_FLAG_COMBINED_BIAS);
            assert(nir_get_tex_src(copy, nir_tex_src_sampler_handle) == handle);
            assert(!nir_shader_tex_pass(b.shader, hk_lower_g17p_sampler_bias,
                                       nir_metadata_control_flow, NULL));
         }
         /* nir_validate_shader is compiled out of this NDEBUG build. */
         ralloc_free(b.shader);
         checks++;
      }
   }
   glsl_type_singleton_decref();
   printf("%u sampler bias scope/idempotence/handle checks passed\n", checks);
   return 0;
}
