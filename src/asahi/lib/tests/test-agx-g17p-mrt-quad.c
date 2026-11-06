/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/format/u_formats.h"
#include "agx3_graphics_program.h"
#include "agx3_g17p_quad_eot.h"
#include "agx_compile.h"
#include "agx_linker.h"
#include "agx_nir.h"
#include "agx_tilebuffer.h"
#include "nir_builder.h"

static void
dump(const char *name, enum pipe_format format,
     const struct agx_shader_part *part)
{
   if (!getenv("AGX3_DUMP_BINARY"))
      return;
   printf("quad-%s format=%u gprs=%u staging=%u classes=%#x bytes=", name,
          format, part->info.nr_gprs, part->info.agx3_staging_halfs,
          part->info.agx3_colour_link.colour_classes);
   for (unsigned i = 0; i < part->info.main_size; ++i)
      printf("%02x", ((uint8_t *)part->binary)[part->info.main_offset + i]);
   puts("");
}

static void
check_format(enum pipe_format format, unsigned components, unsigned count)
{
   enum pipe_format formats[AGX_MAX_RENDER_TARGETS] = {0};
   for (unsigned i = 0; i < count; ++i)
      formats[i] = format;
   struct agx_tilebuffer_layout tib =
      agx_build_tilebuffer_layout(formats, count, 1, false);
   assert(agx_g17p_classify_mrt(&tib) ==
          (components == 4 ? AGX_G17P_MRT_RGBA32 : AGX_G17P_MRT_R32_QUAD));
   assert(agx_g17p_rt0_tile_layout_supported(&tib));
   assert(tib.sample_size_B == ALIGN_POT(count * 4 * components, 8));
   for (unsigned i = 0; i < count; ++i)
      assert(agx_tilebuffer_offset_B(&tib, i) == i * components * 4);
   struct agx_tilebuffer_layout invalid = tib;
   invalid.spilled[count - 1] = true;
   assert(agx_g17p_classify_mrt(&invalid) == AGX_G17P_MRT_NONE);
   invalid = tib;
   invalid._offset_B[count - 1]++;
   assert(agx_g17p_classify_mrt(&invalid) == AGX_G17P_MRT_NONE);
   invalid = tib;
   invalid.logical_format[count] = format;
   assert(agx_g17p_classify_mrt(&invalid) == AGX_G17P_MRT_NONE);
   invalid = tib;
   invalid.nr_samples = 3;
   assert(agx_g17p_classify_mrt(&invalid) == AGX_G17P_MRT_NONE);
   /* A layered layout is not a native quad/RGBA32 candidate -- those builtins
    * are 2D-only and the paths above screen it out -- but it is no longer
    * refused outright. It falls through to the general mixed-format programs,
    * which carry the layer wire and an array EOT. */
   invalid = tib;
   invalid.layered = true;
   assert(agx_g17p_classify_mrt(&invalid) == AGX_G17P_MRT_MIXED);

   struct agx_fs_epilog_key epilog = {.link.loc_written = (1u << count) - 1,
                                      .link.size_32 = (1u << count) - 1,
                                      .nr_samples = 1};
   memset(epilog.remap, -1, sizeof(epilog.remap));
   for (unsigned i = 0; i < count; ++i) {
      epilog.remap[i] = i;
      epilog.rt_formats[i] = format;
      epilog.blend.rt[i].colormask = (1u << components) - 1;
      epilog.blend.rt[i].mode = agx_pack_blend_standard(
         PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ZERO,
         PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ZERO);
   }
   agx_fs_epilog_apply_device_policy(AGX_CHIP_G17P, &epilog);
   nir_builder b =
      nir_builder_init_simple_shader(0, &agx_nir_options, "G17P quad epilog");
   agx_nir_fs_epilog(&b, &epilog);
   agx_nir_lower_g17p_1x_tile_samples(b.shader);
   agx_preprocess_nir(b.shader);
   struct agx_shader_key key = {.dev.encoding = AGX_ISA_ENCODING_AGX3B,
                                .secondary = true};
   struct agx_shader_part part = {0};
   assert(agx_compile_shader_nir(b.shader, &key, &part));
   assert(part.info.agx3_colour_link.colour_classes ==
          BITFIELD_MASK(MIN2(count, 3)));
   assert(part.info.agx3_colour_link.entry_emitted);
   assert(!part.info.scratch_size);
   dump("epilog", format, &part);
   free(part.binary);
   ralloc_free(b.shader);

   b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &agx_nir_options,
                                      "G17P quad BG clear");
   b.shader->info.fs.untyped_color_outputs = true;
   for (unsigned i = 0; i < count; ++i) {
      nir_store_output(&b,
                       nir_load_preamble(&b, components, 32, .base = 4 + i * 8),
                       nir_imm_int(&b, 0), .write_mask = (1u << components) - 1,
                       .src_type = nir_type_uint32,
                       .io_semantics.location = FRAG_RESULT_DATA0 + i,
                       .io_semantics.num_slots = 1);
      b.shader->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DATA0 + i);
   }
   agx_preprocess_nir(b.shader);
   unsigned bindless_base = 0;
   NIR_PASS(_, b.shader, agx_nir_lower_tilebuffer, &tib, NULL, &bindless_base,
            NULL, NULL, false, true);
   NIR_PASS(_, b.shader, agx_nir_lower_monolithic_msaa, tib.nr_samples);
   NIR_PASS(_, b.shader, agx_nir_lower_multisampled_image_store);
   NIR_PASS(_, b.shader, agx_nir_lower_g17p_tile_samples, tib.nr_samples);
   key = (struct agx_shader_key){
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .fs.ignore_tib_dependencies = true,
      .reserved_preamble = 4 + 8 * count,
      .no_preamble = true};
   part = (struct agx_shader_part){0};
   assert(agx_compile_shader_nir(b.shader, &key, &part));
   assert(part.info.push_count == 4 + 8 * count);
   assert(!part.info.scratch_size && !part.info.agx3_colour_link.entry_emitted);
   dump("clear", format, &part);
   free(part.binary);
   ralloc_free(b.shader);

   b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &agx_nir_options,
                                      "G17P raw32 MRT EOT");
   nir_def *coords =
      nir_trim_vector(&b, nir_load_global_invocation_id(&b, 32), 2);
   for (unsigned i = 0; i < count; ++i)
      nir_image_store_block_agx(
         &b, nir_imm_intN_t(&b, i, 16), nir_imm_int(&b, 4 * components * i),
         coords, .format = PIPE_FORMAT_R32_UINT,
         .image_dim = GLSL_SAMPLER_DIM_2D, .explicit_coord = true);
   agx_preprocess_nir(b.shader);
   key = (struct agx_shader_key){
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .g17p_rt0_eot_i32 = true,
      .no_preamble = true};
   part = (struct agx_shader_part){0};
   assert(agx_compile_shader_nir(b.shader, &key, &part));
   const uint8_t *expected = count == 2 ? agx3_g17p_dual_raw32_eot
                            : count == 3 ? agx3_g17p_triple_raw32_eot
                                         : agx3_g17p_quad_eot;
   assert(part.info.binary_size == 32 + 44 * count);
   assert(part.info.agx3_staging_known &&
          part.info.agx3_staging_halfs == 8 * count);
   assert(!memcmp(part.binary, expected, part.info.binary_size));
   dump("eot", format, &part);
   free(part.binary);
   ralloc_free(b.shader);
}

int
main(void)
{
   for (unsigned count = 2; count <= 4; ++count) {
      check_format(PIPE_FORMAT_R32G32B32A32_FLOAT, 4, count);
      check_format(PIPE_FORMAT_R32G32B32A32_UINT, 4, count);
      check_format(PIPE_FORMAT_R32G32B32A32_SINT, 4, count);
   }
   check_format(PIPE_FORMAT_R32_FLOAT, 1, 4);
   puts("two-to-four-target layouts, epilogs and clear shaders PASS");
   return 0;
}
