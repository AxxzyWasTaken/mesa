/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx_compile.h"
#include "agx_nir.h"
#include "agx_state.h"
#include "nir_builder.h"
#include "nir_serialize.h"
#include "util/u_math.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "%d: %s\n", __LINE__, #x); abort(); \
} } while (0)

static nir_shader *
build(float ref, unsigned sampler, bool dynamic)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "shadow gather custom border");
   b.shader->info.workgroup_size[0] = 32;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, dynamic ? 3 : 2);
   tex->op = nir_texop_tg4;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->is_shadow = true;
   tex->dest_type = nir_type_float32;
   tex->sampler_index = dynamic ? 0 : sampler;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                    nir_imm_vec2(&b, -0.1, 0.6));
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_comparator,
                                    nir_imm_float(&b, ref));
   if (dynamic)
      tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_sampler_offset,
         nir_channel(&b, nir_load_local_invocation_id(&b), 0));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_store_global(&b, &tex->def, nir_imm_int64(&b, 0x200000), .align_mul = 16);
   BITSET_SET(b.shader->info.samplers_used, sampler);
   BITSET_SET(b.shader->info.textures_used, 0);
   b.shader->info.num_textures = 1;
   bool bindless = false;
   struct agx_lower_bindings_options options = {
      .uses_bindless_samplers = &bindless,
      .g17p_native_sampler_handles = true,
      .bindful_sampler_count = AGX_GALLIUM_G17P_IMMEDIATE_API_SAMPLERS,
   };
   agx_nir_lower_bindings(b.shader, &options);
   agx_nir_lower_texture_for_generation(b.shader, AGX_DESCRIPTOR_GENERATION_9);
   agx_nir_rebind_g17p_texture_crawls(b.shader);
   agx_preprocess_nir(b.shader);
   return b.shader;
}

static void
optimize(nir_shader *s)
{
   bool progress;
   do {
      progress = nir_opt_constant_folding(s);
      progress |= nir_opt_copy_prop(s);
      progress |= nir_opt_dead_cf(s);
      progress |= nir_opt_dce(s);
      progress |= nir_opt_algebraic(s);
   } while (progress);
}

/* Inject sampler sysvals and synthetic texture results into the driver's
 * actual lowered NIR. No GPU is required. Taps 0/1 are interior (including
 * an identical NaN payload); taps 2/3 hit the border. */
static void
evaluate(nir_shader *s, float border, unsigned func, bool clamp, bool custom,
         unsigned sampler, float expected)
{
   unsigned samples = 0, white = 0, zero = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            nir_builder b = nir_builder_at(nir_before_instr(instr));
            nir_def *value = NULL;
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               CHECK(tex->op == nir_texop_tg4);
               bool raw = !tex->is_shadow;
               unsigned flags = tex->backend_flags;
               white += !!(flags & AGX_TEXTURE_FLAG_BORDER_NONCOMPARE);
               zero += !!(flags & AGX_TEXTURE_FLAG_CLAMP_TO_0);
               ++samples;
               if (raw) {
                  CHECK(nir_tex_instr_src_index(tex, nir_tex_src_comparator) < 0);
                  float edge = (flags & AGX_TEXTURE_FLAG_CLAMP_TO_0) ? 0 : 1;
                  value = nir_vec4(&b, nir_imm_int(&b, 0x7fc12345),
                     nir_imm_float(&b, 0.4), nir_imm_float(&b, edge),
                     nir_imm_float(&b, edge));
               } else {
                  value = nir_imm_vec4(&b, 0, 1, 0, 1);
               }
               nir_def_replace(&tex->def, value);
            } else if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *i = nir_instr_as_intrinsic(instr);
               if (i->intrinsic == nir_intrinsic_load_local_invocation_id)
                  value = nir_imm_ivec3(&b, sampler, 0, 0);
               if (i->intrinsic == nir_intrinsic_load_sampler_handle_agx)
                  value = nir_imm_intN_t(&b, sampler + 1, 16);
               if (i->intrinsic == nir_intrinsic_load_sysval_agx &&
                   nir_intrinsic_desc_set(i) == AGX_SYSVAL_TABLE_ROOT)
                  value = nir_imm_int64(&b, 0x100000);
               if (value)
                  nir_def_replace(&i->def, value);
            }
         }
      }
   }
   CHECK(samples == 3 && white == 1 && zero == 1);
   optimize(s);
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *i = nir_instr_as_intrinsic(instr);
            unsigned offset;
            if (i->intrinsic == nir_intrinsic_load_sysval_agx) {
               CHECK(nir_intrinsic_desc_set(i) == AGX_SYSVAL_STAGE(MESA_SHADER_COMPUTE));
               offset = nir_intrinsic_binding(i);
            } else if (i->intrinsic == nir_intrinsic_load_global_constant) {
               CHECK(nir_src_is_const(i->src[0]));
               offset = nir_src_as_uint(i->src[0]) - 0x100000;
            } else {
               continue;
            }
            nir_builder b = nir_builder_at(nir_before_instr(instr));
            nir_def *value = NULL;
            if (offset == offsetof(struct agx_stage_uniforms,
                                   custom_border_sampler_handle) + 2 * sampler)
               value = nir_imm_intN_t(&b, custom ? 33 + sampler : -1, 16);
            if (offset == offsetof(struct agx_stage_uniforms,
                                   custom_border_compare_func) + 4 * sampler)
               value = nir_imm_intN_t(&b, func | (clamp ? AGX_BORDER_COMPARE_CLAMP_REF : 0), 16);
            if (offset == offsetof(struct agx_stage_uniforms,
                                   custom_border) + 16 * sampler)
               value = nir_imm_vec4(&b, border, 0, 0, 0);
            CHECK(value);
            nir_def_replace(&i->def, value);
         }
      }
   }
   optimize(s);
   nir_validate_shader(s, "evaluated custom shadow gather");
   unsigned stores = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *i = nir_instr_as_intrinsic(instr);
            if (i->intrinsic != nir_intrinsic_store_global)
               continue;
            CHECK(nir_src_is_const(i->src[0]));
            CHECK(nir_src_comp_as_float(i->src[0], 0) == 0);
            CHECK(nir_src_comp_as_float(i->src[0], 1) == 1);
            CHECK(nir_src_comp_as_float(i->src[0], 2) == (custom ? expected : 0));
            CHECK(nir_src_comp_as_float(i->src[0], 3) == (custom ? expected : 1));
            ++stores;
         }
      }
   }
   CHECK(stores == 1);
}

static bool
compare(unsigned func, float ref, float depth)
{
   switch (func) {
   case PIPE_FUNC_NEVER: return false;
   case PIPE_FUNC_LESS: return ref < depth;
   case PIPE_FUNC_EQUAL: return ref == depth;
   case PIPE_FUNC_LEQUAL: return ref <= depth;
   case PIPE_FUNC_GREATER: return ref > depth;
   case PIPE_FUNC_NOTEQUAL: return ref != depth;
   case PIPE_FUNC_GEQUAL: return ref >= depth;
   case PIPE_FUNC_ALWAYS: return true;
   }
   abort();
}

static void
check_compile(enum agx_chip chip, unsigned sampler, bool dynamic,
              bool fragment)
{
   nir_shader *s = build(-0.5, sampler, dynamic);
   if (fragment)
      s->info.stage = MESA_SHADER_FRAGMENT;
   CHECK(agx_nir_lower_custom_border_g17p(s, true));
   struct agx_compiled_shader compiled = {0};
   struct agx_shader_key key = {
      .dev = {.chip = chip, .encoding = AGX_ISA_ENCODING_AGX3B},
      .no_preamble = true,
   };
   key.fs.nr_samples = 1;
   agx_nir_lower_sysvals(s, s->info.stage, true, 32);
   agx_nir_layout_uniforms(s, &compiled, &key.reserved_preamble, 8, true);
   struct agx_shader_part part = {0};
   CHECK(agx_compile_shader_nir(s, &key, &part));
   CHECK(part.info.binary_size > 0);
   free(part.binary);
   ralloc_free(s);
}

static void
check_opt_out(void)
{
   for (unsigned mode = 0; mode < 3; ++mode) {
      nir_shader *s = build(1.25, mode == 0 ? 0 : mode == 1 ? 7 : 31, mode != 0);
      struct blob before, after;
      blob_init(&before);
      blob_init(&after);
      nir_serialize(&before, s, false);
      CHECK(!agx_nir_lower_custom_border_g17p(s, false));
      nir_serialize(&after, s, false);
      CHECK(before.size == after.size);
      CHECK(!memcmp(before.data, after.data, before.size));
      blob_finish(&before);
      blob_finish(&after);
      ralloc_free(s);
   }
}

static void
check_uniforms(void)
{
   const enum agx_chip chips[] = {AGX_CHIP_G13G, AGX_CHIP_G14G,
      AGX_CHIP_G15S, AGX_CHIP_G15C, AGX_CHIP_G16G, AGX_CHIP_G17P};
   const enum pipe_format formats[] = {PIPE_FORMAT_Z16_UNORM,
      PIPE_FORMAT_Z24_UNORM_S8_UINT, PIPE_FORMAT_Z32_FLOAT,
      PIPE_FORMAT_Z32_FLOAT_S8X24_UINT};
   struct agx_screen *screen = calloc(1, sizeof(*screen));
   struct agx_context *ctx = calloc(1, sizeof(*ctx));
   struct agx_batch *batch = calloc(1, sizeof(*batch));
   CHECK(screen && ctx && batch);
   ctx->base.screen = &screen->pscreen;
   batch->ctx = ctx;
   struct agx_sampler_state sampler = {
      .uses_custom_border = true,
      .base.compare_func = PIPE_FUNC_GREATER,
   };
   ctx->stage[MESA_SHADER_COMPUTE].samplers[31] = &sampler;
   ctx->stage[MESA_SHADER_COMPUTE].valid_samplers = BITFIELD_BIT(31);
   for (unsigned c = 0; c < ARRAY_SIZE(chips); ++c) {
      screen->dev.chip = chips[c];
      for (unsigned f = 0; f < ARRAY_SIZE(formats); ++f) {
         sampler.base.border_color_format = formats[f];
         batch->stage_uniforms[MESA_SHADER_COMPUTE].custom_border_compare_func[31] = 0xdeadbeef;
         agx_set_sampler_uniforms(batch, MESA_SHADER_COMPUTE);
         const bool emulates = chips[c] == AGX_CHIP_G16G ||
                               chips[c] == AGX_CHIP_G17P;
         unsigned expected = emulates
            ? PIPE_FUNC_GREATER | (f < 2 ? AGX_BORDER_COMPARE_CLAMP_REF : 0)
            : 0xdeadbeef;
         CHECK(batch->stage_uniforms[MESA_SHADER_COMPUTE].custom_border_compare_func[31] == expected);
      }
   }
   free(batch);
   free(ctx);
   free(screen);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   check_opt_out();
   check_uniforms();
   const float borders[] = {0.7, 1.5, -0.5, INFINITY, -INFINITY, 0, 1, NAN};
   const float refs[] = {-0.5, 0, 0.5, 1, 1.25};
   unsigned cases = 0;
   for (unsigned f = PIPE_FUNC_NEVER; f <= PIPE_FUNC_ALWAYS; ++f) {
      for (unsigned z = 0; z < ARRAY_SIZE(borders); ++z) {
         for (unsigned r = 0; r < ARRAY_SIZE(refs); ++r) {
            for (unsigned mode = 0; mode < 3; ++mode) {
               unsigned sampler = mode == 0 ? 0 : mode == 1 ? 7 : 31;
               nir_shader *s = build(refs[r], sampler, mode != 0);
               CHECK(agx_nir_lower_custom_border_g17p(s, true));
               evaluate(s, borders[z], f, false, true, sampler,
                        compare(f, refs[r], borders[z]));
               ralloc_free(s);
               ++cases;
            }
         }
      }
   }
   for (unsigned mode = 0; mode < 3; ++mode) {
      unsigned sampler = mode == 0 ? 0 : mode == 1 ? 7 : 31;
      for (unsigned custom = 0; custom < 2; ++custom) {
         nir_shader *s = build(-0.5, sampler, mode != 0);
         CHECK(agx_nir_lower_custom_border_g17p(s, true));
         evaluate(s, 0, PIPE_FUNC_LESS, true, custom, sampler, 0);
         ralloc_free(s);
         ++cases;
      }
      check_compile(AGX_CHIP_G16G, sampler, mode != 0, false);
      check_compile(AGX_CHIP_G17P, sampler, mode != 0, false);
   }
   check_compile(AGX_CHIP_G16G, 0, false, true);
   check_compile(AGX_CHIP_G17P, 0, false, true);
   printf("PASS: %u shadow border NIR evaluations; 8 G16G/G17P encodings; "
          "3 byte-identical opt-outs; 24 uniform generation/format controls\n", cases);
   glsl_type_singleton_decref();
   return 0;
}
