/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include "agx_compile.h"
#include "agx_debug.h"
#include "agx3_graphics_program.h"
#include "agx3_pixel_core.h"
#include "agx_linker.h"
#include "agx_nir.h"
#include "agx_nir_texture.h"
#include "agx_tilebuffer.h"
#include "nir_builder.h"
#include "util/format/u_formats.h"
#include "../../compiler/test/probes/g17p-sparse-mrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t
blend_mode(bool additive)
{
   return agx_pack_blend_standard(
      PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE,
      additive ? PIPE_BLENDFACTOR_ONE : PIPE_BLENDFACTOR_ZERO,
      PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE,
      additive ? PIPE_BLENDFACTOR_ONE : PIPE_BLENDFACTOR_ZERO);
}

static bool
test_r11g11b10_mrt_layout(void)
{
   enum pipe_format formats[AGX_MAX_RENDER_TARGETS] = {
      PIPE_FORMAT_R11G11B10_FLOAT,
      PIPE_FORMAT_R11G11B10_FLOAT,
   };
   struct agx_tilebuffer_layout tib =
      agx_build_tilebuffer_layout(formats, 2, 1, false);
   if (agx_g17p_classify_mrt(&tib) != AGX_G17P_MRT_R11G11B10 ||
       agx_tilebuffer_offset_B(&tib, 0) != 0 ||
       agx_tilebuffer_offset_B(&tib, 1) != 4 || tib.sample_size_B != 8)
      return false;

   struct agx_tilebuffer_layout bad = tib;
   bad._offset_B[1] = 8;
   if (agx_g17p_classify_mrt(&bad) != AGX_G17P_MRT_NONE)
      return false;

   bad = tib;
   bad.logical_format[1] = PIPE_FORMAT_R8G8B8A8_UNORM;
   if (agx_g17p_classify_mrt(&bad) != AGX_G17P_MRT_MIXED ||
       !agx_g17p_rt0_tile_layout_supported(&bad))
      return false;

   bad = tib;
   bad.logical_format[2] = PIPE_FORMAT_R11G11B10_FLOAT;
   return agx_g17p_classify_mrt(&bad) == AGX_G17P_MRT_NONE;
}

static bool
compile_epilog(const struct agx_fs_epilog_link_info *link,
               enum pipe_format rt1_format, bool rt0_additive,
               bool rt1_additive, struct agx_shader_part *part)
{
   struct agx_fs_epilog_key key = {
      .link = *link,
      .nr_samples = 1,
   };
   memset(key.remap, -1, sizeof(key.remap));
   key.remap[0] = 0;
   key.remap[1] = 1;
   key.rt_formats[0] = PIPE_FORMAT_R16G16B16A16_FLOAT;
   key.rt_formats[1] = rt1_format;
   key.blend.rt[0].colormask = 0xf;
   key.blend.rt[0].mode = blend_mode(rt0_additive);
   key.blend.rt[1].colormask = 0x1;
   key.blend.rt[1].mode = blend_mode(rt1_additive);

   nir_builder b =
      nir_builder_init_simple_shader(0, &agx_nir_options, "G17P MRT epilog");
   agx_nir_fs_epilog(&b, &key);
   agx_nir_lower_g17p_1x_tile_samples(b.shader);
   agx_preprocess_nir(b.shader);

   struct agx_shader_key backend_key = {
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .secondary = true,
   };
   const bool ok = agx_compile_shader_nir(b.shader, &backend_key, part);
   ralloc_free(b.shader);
   return ok;
}

static bool
compile_rgba8_mrt_epilog(const struct agx_fs_epilog_link_info *link,
                         enum pipe_format rt1_format, bool rt1_additive,
                         struct agx_shader_part *part)
{
   struct agx_fs_epilog_key key = {
      .link = *link,
      .nr_samples = 1,
   };
   memset(key.remap, -1, sizeof(key.remap));
   key.remap[0] = 0;
   key.remap[1] = 1;
   key.rt_formats[0] = PIPE_FORMAT_R8G8B8A8_UNORM;
   key.rt_formats[1] = rt1_format;
   key.blend.rt[0].colormask = 0xf;
   key.blend.rt[0].mode = blend_mode(false);
   key.blend.rt[1].colormask = 0xf;
   key.blend.rt[1].mode = blend_mode(rt1_additive);

   nir_builder b = nir_builder_init_simple_shader(
      0, &agx_nir_options, "G17P two-RGBA8 MRT epilog");
   agx_nir_fs_epilog(&b, &key);
   agx_nir_lower_g17p_1x_tile_samples(b.shader);
   agx_preprocess_nir(b.shader);

   struct agx_shader_key backend_key = {
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .secondary = true,
   };
   const bool ok = agx_compile_shader_nir(b.shader, &backend_key, part);
   ralloc_free(b.shader);
   return ok;
}

static bool
compile_vkquake_mboit_epilog(const struct agx_fs_epilog_link_info *link,
                             struct agx_shader_part *part)
{
   struct agx_fs_epilog_key key = {
      .link = *link,
      .nr_samples = 1,
      .g17p_tile_formats = true,
      .preserve_zero_mask = true,
   };
   memset(key.remap, -1, sizeof(key.remap));
   key.remap[0] = 0;
   key.remap[1] = 1;
   key.rt_formats[0] = PIPE_FORMAT_R16G16B16A16_FLOAT;
   key.rt_formats[1] = PIPE_FORMAT_R8_UNORM;
   key.blend.rt[0].colormask = 0xf;
   key.blend.rt[0].mode = agx_pack_blend_standard(
      PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ONE,
      PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ONE);
   key.blend.rt[1].colormask = 0x1;
   key.blend.rt[1].mode = agx_pack_blend_standard(
      PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ZERO,
      PIPE_BLENDFACTOR_INV_SRC_COLOR, PIPE_BLEND_ADD,
      PIPE_BLENDFACTOR_ZERO, PIPE_BLENDFACTOR_INV_SRC_ALPHA);

   nir_builder b = nir_builder_init_simple_shader(
      0, &agx_nir_options, "G17P vkQuake MBOIT epilog");
   agx_nir_fs_epilog(&b, &key);
   agx_nir_lower_g17p_1x_tile_samples(b.shader);
   agx_preprocess_nir(b.shader);

   struct agx_shader_key backend_key = {
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .secondary = true,
   };
   const bool ok = agx_compile_shader_nir(b.shader, &backend_key, part);
   ralloc_free(b.shader);
   return ok;
}

static bool
test_wboit_background_clear(void)
{
   enum pipe_format formats[AGX_MAX_RENDER_TARGETS] = {
      PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_R8_UNORM,
   };
   struct agx_tilebuffer_layout tib =
      agx_build_tilebuffer_layout(formats, 2, 1, false);
   if (agx_g17p_classify_mrt(&tib) != AGX_G17P_MRT_WBOIT ||
       agx_tilebuffer_offset_B(&tib, 0) != 0 ||
       agx_tilebuffer_offset_B(&tib, 1) != 8)
      return false;

   /* Follow the production background-clear path from logical output stores
    * through the real tilebuffer lowering, rather than constructing backend
    * intrinsics directly. RT1 has its own preamble value at halves12..13. */
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &agx_nir_options, "G17P WBOIT background clear");
   b.shader->info.fs.untyped_color_outputs = true;
   nir_store_output(
      &b, nir_load_preamble(&b, 4, 32, .base = 4), nir_imm_int(&b, 0),
      .write_mask = 0xf, .src_type = nir_type_uint32,
      .io_semantics.location = FRAG_RESULT_DATA0,
      .io_semantics.num_slots = 1);
   nir_store_output(
      &b, nir_load_preamble(&b, 1, 32, .base = 12), nir_imm_int(&b, 0),
      .write_mask = 0x1, .src_type = nir_type_uint32,
      .io_semantics.location = FRAG_RESULT_DATA1,
      .io_semantics.num_slots = 1);
   b.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0) |
                                     BITFIELD64_BIT(FRAG_RESULT_DATA1);

   agx_preprocess_nir(b.shader);
   unsigned bindless_base = 0;
   NIR_PASS(_, b.shader, agx_nir_lower_tilebuffer, &tib, NULL,
            &bindless_base, NULL, NULL, false, false);
   NIR_PASS(_, b.shader, agx_nir_lower_monolithic_msaa, tib.nr_samples);
   NIR_PASS(_, b.shader, agx_nir_lower_multisampled_image_store);
   NIR_PASS(_, b.shader, agx_nir_lower_g17p_tile_samples, tib.nr_samples);

   struct agx_shader_key key = {
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .fs.ignore_tib_dependencies = true,
      .reserved_preamble = AGX3_G17P_WBOIT_MRT_CLEAR_PUSH_HALFS,
      .no_preamble = true,
   };
   struct agx_shader_part part = {0};
   const bool compiled = agx_compile_shader_nir(b.shader, &key, &part);
   static const uint8_t expected[] = {
      0x0b,0x04,0x01,0x08,0x01,0x01,0x1c,0x81,0x00,0x02,
      0x1b,0x06,0x01,0x08,0x01,0x03,0x1c,0x81,0x04,0x02,
      0x1b,0x08,0x01,0x08,0x11,0x03,0x1c,0x81,0x00,0x02,
      0x2b,0x0a,0x01,0x08,0x11,0x05,0x1c,0x81,0x04,0x02,
      0x97,0x04,0x54,0x04,0x02,0x18,0x00,0x90,0x04,0xc0,
      0x87,0x02,0x54,0x00,0x06,0x00,
      0x87,0x02,0x54,0x30,0x08,0x00,
      0xe7,0x06,0x54,0x04,0x00,0x04,0x01,0x42,0x00,0x00,0x00,0x00,
      0x07,0x02,0x54,0x30,0x00,0x00,
      0x87,0x02,0x54,0x0c,0x08,0x00,
      0xe7,0x06,0x54,0x00,0x00,0x00,0x01,0x0e,0x00,0x00,0x00,0x00,
      0x07,0x02,0x54,0x0c,0x02,0x00,
      0x0e,0x00,0x00,0x00,
   };
   const bool ok = compiled && part.binary &&
                   part.info.binary_size == sizeof(expected) &&
                   part.info.main_offset == 0 &&
                   part.info.main_size == sizeof(expected) &&
                   part.info.push_count ==
                      AGX3_G17P_WBOIT_MRT_CLEAR_PUSH_HALFS &&
                   !memcmp(part.binary, expected, sizeof(expected));

   if (!ok) {
      fprintf(stderr, "G17P WBOIT background clear changed size=%u bytes=",
              part.info.main_size);
      if (part.binary) {
         for (unsigned i = 0; i < part.info.main_size; ++i)
            fprintf(stderr, "%02x", ((uint8_t *)part.binary +
                                      part.info.main_offset)[i]);
      }
      fputc('\n', stderr);
   }
   free(part.binary);
   ralloc_free(b.shader);
   return ok;
}

static bool
test_lower_reload_handle(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_texture_handle_agx)
      return false;
   b->cursor = nir_instr_remove(&intr->instr);
   nir_def_rewrite_uses(&intr->def, nir_bindless_image_agx(
      b, nir_imul_imm(b, intr->src[0].ssa, 32), .desc_set = 0));
   return true;
}

static bool
test_wide_rgba8_background(unsigned count, bool reload,
                           unsigned target_mask, unsigned partition_mask)
{
   enum pipe_format formats[AGX_MAX_RENDER_TARGETS] = {0};
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &agx_nir_options, "G17P wide RGBA8 background");
   b.shader->info.fs.untyped_color_outputs = true;
   for (unsigned rt = 0; rt < count; ++rt) {
      if (!(target_mask & BITFIELD_BIT(rt)))
         continue;
      formats[rt] = PIPE_FORMAT_R8G8B8A8_UNORM;
      nir_def *color = reload
         ? nir_txf(&b, nir_u2u32(&b, nir_load_pixel_coord(&b)),
                   .texture_index = rt * 2, .dim = GLSL_SAMPLER_DIM_2D,
                   .dest_type = nir_type_uint32,
                   .backend_flags = AGX_TEXTURE_FLAG_NO_CLAMP)
         : nir_load_preamble(&b, 4, 32, .base = 4 + 8 * rt);
      nir_store_output(&b, color,
                       nir_imm_int(&b, 0), .write_mask = 0xf,
                       .src_type = nir_type_uint32,
                       .io_semantics.location = FRAG_RESULT_DATA0 + rt,
                       .io_semantics.num_slots = 1);
      b.shader->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DATA0 + rt);
   }
   struct agx_tilebuffer_layout tib =
      agx_build_tilebuffer_layout(formats, count, 1, false);
   agx_nir_lower_texture(b.shader);
   agx_preprocess_nir(b.shader);
   unsigned bindless_base = 0;
   NIR_PASS(_, b.shader, agx_nir_lower_tilebuffer, &tib, NULL,
            &bindless_base, NULL, NULL, false, false);
   NIR_PASS(_, b.shader, agx_nir_lower_monolithic_msaa, 1);
   NIR_PASS(_, b.shader, agx_nir_lower_multisampled_image_store);
   NIR_PASS(_, b.shader, agx_nir_lower_texture);
   NIR_PASS(_, b.shader, nir_shader_intrinsics_pass, test_lower_reload_handle,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, b.shader, agx_nir_lower_g17p_tile_samples, 1);
   struct agx_shader_key key = {
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .fs.ignore_tib_dependencies = true,
      .reserved_preamble = reload ? 0 : 4 + 8 * util_last_bit(target_mask),
      .no_preamble = true,
   };
   struct agx_shader_part part = {0};
   const bool compiled = agx_compile_shader_nir(b.shader, &key, &part);
   bool ok = compiled && part.binary &&
             part.info.agx3_colour_link.colour_classes == partition_mask &&
             part.info.agx3_colour_link.entry_emitted &&
             part.info.push_count ==
                (reload ? 0 : 4 + 8 * util_last_bit(target_mask));
   const unsigned active = util_bitcount(target_mask);
   /* This traverses real NIR lowering and pre-RA store ordering. Inspect the
    * terminal transaction rather than accepting a successful compile alone:
    * all targets must survive RA at descending physical byte offsets. */
   if (ok) {
      const uint8_t *code = (uint8_t *)part.binary + part.info.main_offset;
      unsigned stores = 0;
      for (unsigned i = 0; i + 12 <= part.info.main_size; ++i) {
         if (code[i] == 0xe7 && code[i+1] == 0x06 && code[i+2] == 0x54) {
            ok &= stores < active && code[i+5] == 2 * (active - stores - 1);
            ++stores;
            i += 11;
         }
      }
      ok &= stores == active;
   }
   if (!ok)
      fprintf(stderr, "G17P %u-target background failed lowering/RA/transaction\n", count);
   free(part.binary);
   ralloc_free(b.shader);
   return ok;
}

static bool
test_wide_rgba8_epilog(unsigned count, unsigned target_mask,
                       unsigned partition_mask)
{
   struct agx_fs_epilog_key key = {
      .link = {.size_32 = target_mask, .loc_written = target_mask},
      .nr_samples = 1,
   };
   memset(key.remap, -1, sizeof(key.remap));
   for (unsigned rt = 0; rt < count; ++rt) {
      if (!(target_mask & BITFIELD_BIT(rt)))
         continue;
      key.remap[rt] = rt;
      key.rt_formats[rt] = PIPE_FORMAT_R8G8B8A8_UNORM;
      key.blend.rt[rt].colormask = 0xf;
      key.blend.rt[rt].mode = blend_mode(false);
   }
   nir_builder b = nir_builder_init_simple_shader(
      0, &agx_nir_options, "G17P wide RGBA8 epilog");
   agx_nir_fs_epilog(&b, &key);
   agx_nir_lower_g17p_1x_tile_samples(b.shader);
   agx_preprocess_nir(b.shader);
   struct agx_shader_key backend = {
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .secondary = true,
   };
   struct agx_shader_part part = {0};
   fprintf(stderr, "Compiling RGBA8 epilog target mask 0x%x\n", target_mask);
   bool ok = agx_compile_shader_nir(b.shader, &backend, &part) && part.binary &&
             part.info.agx3_colour_link.colour_classes == partition_mask;
   const unsigned active = util_bitcount(target_mask);
   if (ok) {
      const uint8_t *code = (uint8_t *)part.binary + part.info.main_offset;
      unsigned stores = 0;
      for (unsigned i = 0; i + 12 <= part.info.main_size; ++i) {
         if (code[i] == 0xe7 && code[i + 1] == 0x06 && code[i + 2] == 0x54) {
            ok &= stores < active && code[i + 5] == 2 * (active - stores - 1);
            ++stores;
            i += 11;
         }
      }
      ok &= stores == active;
   }
   free(part.binary);
   ralloc_free(b.shader);
   return ok;
}

int
main(void)
{
   if (!test_r11g11b10_mrt_layout())
      return 1;

   if (!test_wboit_background_clear())
      return 1;


   if (!test_wide_rgba8_epilog(4, 0xf, 7) ||
       !test_wide_rgba8_epilog(8, 0xff, 7))
      return 1;
   for (unsigned i = 0; i < ARRAY_SIZE(g17p_sparse_mrt_oracles); ++i) {
      if (!test_wide_rgba8_epilog(8, g17p_sparse_mrt_oracles[i].target_mask,
                                 g17p_sparse_mrt_oracles[i].partition_mask))
         return 1;
   }

   /* No background case here. The wide-MRT class protocol is witnessed on
    * Apple's FRAGMENT programs; its BG constructors keep the ascending,
    * transaction-free shape, and agx3_g17p_rt0_bg_program_generate()'s loader
    * descriptor is built for that. Compiling a clear with the transaction
    * aborts KHR-GL30.draw_buffers.draw_buffers_1 on hardware at "background
    * Ioto program". */

   nir_builder mb = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &agx_nir_options, "G17P MRT main");
   nir_store_output(&mb, nir_imm_vec4(&mb, 0.75f, 0.5f, 0.25f, 1.0f),
                    nir_imm_int(&mb, 0), .write_mask = 0xf,
                    .io_semantics.location = FRAG_RESULT_DATA0,
                    .src_type = nir_type_float32);
   nir_store_output(&mb, nir_imm_float(&mb, 0.5f), nir_imm_int(&mb, 0),
                    .write_mask = 0x1,
                    .io_semantics.location = FRAG_RESULT_DATA1,
                    .src_type = nir_type_float32);
   mb.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0) |
                                     BITFIELD64_BIT(FRAG_RESULT_DATA1);
   struct agx_fs_epilog_link_info actual_link = {0};
   agx_nir_lower_fs_output_to_epilog_g17p(mb.shader, &actual_link);
   agx_preprocess_nir(mb.shader);
   struct agx_shader_key main_key = {
      .dev.encoding = AGX_ISA_ENCODING_AGX3B,
      .no_stop = true,
      .no_preamble = true,
      .promote_constants = true,
      .promote_textures = true,
   };
   struct agx_shader_part main_part = {0};
   if (!agx_compile_shader_nir(mb.shader, &main_key, &main_part)) {
      fprintf(stderr, "G17P RGBA16F+R8 MRT main did not compile\n");
      return 1;
   }
   if (actual_link.loc_written != 3 || actual_link.size_32 != 3 ||
       actual_link.loc0_w_1) {
      fprintf(stderr, "G17P MRT main did not preserve the full export ABI\n");
      return 1;
   }

   struct agx_shader_part vkquake_part = {0};
   const bool vkquake_ok =
      compile_vkquake_mboit_epilog(&actual_link, &vkquake_part) &&
      vkquake_part.binary && vkquake_part.info.main_offset == 0 &&
      vkquake_part.info.main_size == vkquake_part.info.binary_size &&
      vkquake_part.info.agx3_colour_link.entry_emitted &&
      vkquake_part.info.agx3_colour_link.straight_line &&
      vkquake_part.info.agx3_colour_link.colour_classes == 3 &&
      vkquake_part.info.agx3_colour_link.entry_offset == 0;
   if (!vkquake_ok) {
      fprintf(stderr,
              "G17P vkQuake MBOIT epilog did not preserve one transaction\n");
      return 1;
   }
   /* Full post-RA oracle for the exact vkQuake blend state.  Its partition
    * fences and tile-op order match the clean-room native witness cited in
    * agx3_pack_binary.c, while the complete byte string also keeps both MRT
    * payloads live through the descending stores. The second payload is
    * produced directly in its store registers, so the second LSTCOL reads r6
    * without IADD payload copies. */
   static const uint8_t expected_vkquake_coalesced[] = {
      0x87,0x02,0x54,0x00,0x06,0x00,0x87,0x02,0x54,0x20,
      0x08,0x00,0x67,0x06,0x54,0x00,0x00,0x04,0x01,0xc2,
      0x02,0x00,0x00,0x00,0x87,0x02,0x54,0x08,0x08,0x00,
      0x67,0x06,0x54,0x04,0x00,0x00,0x01,0x8e,0x02,0x00,
      0x00,0x00,0x19,0x04,0x3c,0x81,0x00,0xc2,0x19,0x03,
      0x18,0x09,0x49,0x04,0x3c,0x81,0x08,0x02,0x29,0x09,
      0x18,0x0b,0x49,0x06,0x3c,0x81,0x00,0x02,0x49,0x09,
      0x18,0x0d,0x59,0x06,0x3c,0x81,0x08,0x02,0x39,0x0b,
      0x18,0x0f,0x61,0x03,0x1c,0x81,0x00,0x02,0x61,0x05,
      0x1c,0x81,0x04,0x02,0x71,0x09,0x1c,0x81,0x00,0x02,
      0x71,0x07,0x1c,0x81,0x04,0x02,0x17,0x04,0x54,0x00,
      0x01,0x00,0x14,0x6a,0x19,0xb1,0x14,0x11,0x80,0x08,
      0x19,0x03,0x1c,0x81,0x01,0x02,0x00,0x02,0x09,0x03,
      0x19,0x01,0x97,0x04,0x54,0x00,0x02,0x00,0x02,0x10,
      0x44,0xc0,0x87,0x02,0x54,0x10,0x08,0x00,0xe7,0x06,
      0x54,0x00,0x00,0x04,0x01,0x42,0x00,0x00,0x00,0x00,
      0x07,0x02,0x54,0x30,0x00,0x00,0x87,0x02,0x54,0x04,
      0x08,0x00,0xe7,0x06,0x54,0x0c,0x00,0x00,0x01,0x0e,
      0x00,0x00,0x00,0x00,0x07,0x02,0x54,0x0c,0x02,0x00,
      0x0e,0x00,0x00,0x00,
   };
   const uint8_t *vkquake_expected = expected_vkquake_coalesced;
   const size_t vkquake_expected_size = sizeof(expected_vkquake_coalesced);
   if (vkquake_part.info.main_size != vkquake_expected_size ||
       memcmp((uint8_t *)vkquake_part.binary + vkquake_part.info.main_offset,
              vkquake_expected, vkquake_expected_size)) {
      fprintf(stderr, "G17P vkQuake phased MRT bytes changed\n");
      return 1;
   }
   free(vkquake_part.binary);

   static const uint8_t expected_main[] = {
      0x4b,0x06,0x01,0x08,0x5b,0x00,0x01,0x08,0x6b,0x04,
      0x01,0x08,0x7b,0x02,0x01,0x08,0x8b,0x00,0x01,0x08,
   };
   if (main_part.info.main_size != sizeof(expected_main) ||
       memcmp((uint8_t *)main_part.binary + main_part.info.main_offset,
              expected_main, sizeof(expected_main)) != 0) {
      fprintf(stderr, "G17P MRT main did not keep distinct RT0/RT1 exports\n");
      return 1;
   }

   struct agx_shader_part part = {0};
   const bool ok = compile_epilog(&actual_link, PIPE_FORMAT_R8_UNORM, false,
                                  true, &part) &&
                   part.info.agx3_colour_link.entry_emitted &&
                   part.info.agx3_colour_link.straight_line &&
                   part.info.agx3_colour_link.colour_classes == 3 &&
                   part.info.agx3_colour_link.entry_offset == 0 &&
                   part.info.main_offset == 0 &&
                   part.info.main_size == part.info.binary_size;
   if (!ok)
      fprintf(stderr, "G17P RGBA16F+R8 MRT epilog did not compile\n");
   static const uint8_t expected_epilog[] = {
      0x87,0x02,0x54,0x00,0x06,0x00,0x87,0x02,0x54,0x20,
      0x08,0x00,0x67,0x06,0x54,0x00,0x00,0x04,0x01,0xc2,
      0x02,0x00,0x00,0x00,0x21,0x09,0x1c,0x81,0x00,0x02,
      0x21,0x0b,0x1c,0x81,0x04,0x02,0x31,0x0d,0x1c,0x81,
      0x00,0x02,0x31,0x0f,0x1c,0x81,0x04,0x02,0x17,0x04,
      0x56,0x00,0x01,0x00,0x14,0x6a,0x19,0x11,0x1c,0x81,
      0x01,0x02,0x00,0x02,0x09,0x01,0x18,0x03,0x97,0x04,
      0x54,0x00,0x02,0x00,0x02,0x10,0x44,0xc0,0x87,0x02,
      0x54,0x10,0x08,0x00,0xe7,0x06,0x54,0x00,0x00,0x04,
      0x01,0x42,0x00,0x00,0x00,0x00,0x07,0x02,0x54,0x30,
      0x00,0x00,0x87,0x02,0x54,0x0c,0x08,0x00,0xe7,0x06,
      0x54,0x04,0x00,0x00,0x01,0x0e,0x00,0x00,0x00,0x00,
      0x07,0x02,0x54,0x0c,0x02,0x00,0x0e,0x00,0x00,0x00,
   };
   uint8_t entry[AGX3_PIXEL_MAX_LENGTH] = {0};
   uint8_t entry_size = 0;
   static const uint8_t stop[] = {0x0e, 0x00, 0x00, 0x00};
   if (ok &&
       (!agx3_pixel_colour_entry(AGX_ISA_ENCODING_AGX3B, entry,
                                 &entry_size) ||
        entry_size != AGX3_PIXEL_MAX_LENGTH ||
        memcmp((uint8_t *)part.binary + part.info.main_offset, entry,
               entry_size) != 0 ||
        memcmp((uint8_t *)part.binary + part.info.main_offset +
                  part.info.main_size - sizeof(stop),
               stop, sizeof(stop)) != 0)) {
      fprintf(stderr, "G17P MRT epilog entry/stop ABI changed\n");
      return 1;
   }
   if (ok && (part.info.main_size != sizeof(expected_epilog) ||
              memcmp((uint8_t *)part.binary + part.info.main_offset,
                     expected_epilog, sizeof(expected_epilog)) != 0)) {
      fprintf(stderr, "G17P MRT epilog bytes changed size=%u bytes=",
              part.info.main_size);
      for (unsigned i = 0; i < part.info.main_size; ++i)
         fprintf(stderr, "%02x", ((uint8_t *)part.binary +
                                   part.info.main_offset)[i]);
      fputc('\n', stderr);
      return 1;
   }
   free(part.binary);

   memset(&part, 0, sizeof(part));
   if (!compile_rgba8_mrt_epilog(&actual_link,
                                 PIPE_FORMAT_R8G8B8A8_UNORM, false, &part)) {
      fprintf(stderr, "G17P two-RGBA8 MRT did not compile\n");
      return 1;
   }
   static const uint8_t expected_rgba8_mrt[] = {
      0x97,0x0c,0x54,0x00,0x02,0x10,0x2a,0x54,0x44,0xc2,
      0x97,0x04,0x54,0x01,0x02,0x18,0x3a,0x54,0x44,0xc2,
      0x97,0x04,0x54,0x02,0x02,0x20,0x4a,0x54,0x44,0xc2,
      0x97,0x04,0x54,0x03,0x02,0x28,0x5a,0x54,0x44,0xc2,
      0x87,0x02,0x54,0x00,0x06,0x00,
      0x87,0x02,0x54,0x30,0x08,0x00,
      0xe7,0x06,0x54,0x02,0x00,0x02,0x01,0x4e,0x00,0x00,0x00,0x00,
      0x07,0x02,0x54,0x30,0x00,0x00,
      0x87,0x02,0x54,0x0c,0x08,0x00,
      0xe7,0x06,0x54,0x00,0x00,0x00,0x01,0x4e,0x00,0x00,0x00,0x00,
      0x07,0x02,0x54,0x0c,0x02,0x00,
      0x0e,0x00,0x00,0x00,
   };
   static const uint8_t apple_transaction[] = {
      0x87,0x02,0x54,0x00,0x06,0x00,
      0x87,0x02,0x54,0x30,0x08,0x00,
      0xe7,0x06,0x54,0x02,0x00,0x02,0x01,0x4e,0x00,0x00,0x00,0x00,
      0x07,0x02,0x54,0x30,0x00,0x00,
      0x87,0x02,0x54,0x0c,0x08,0x00,
      0xe7,0x06,0x54,0x00,0x00,0x00,0x01,0x4e,0x00,0x00,0x00,0x00,
      0x07,0x02,0x54,0x0c,0x02,0x00,
      0x0e,0x00,0x00,0x00,
   };
   const uint8_t *rgba8_code =
      (uint8_t *)part.binary + part.info.main_offset;
   if (part.info.main_size != sizeof(expected_rgba8_mrt) ||
       memcmp(rgba8_code, expected_rgba8_mrt,
              sizeof(expected_rgba8_mrt)) ||
       !part.info.agx3_colour_link.entry_emitted ||
       !part.info.agx3_colour_link.straight_line ||
       part.info.agx3_colour_link.colour_classes != 3 ||
       part.info.agx3_colour_link.entry_offset != 40 ||
       memcmp(rgba8_code + 40, apple_transaction,
              sizeof(apple_transaction))) {
      fprintf(stderr, "G17P two-RGBA8 MRT transaction changed size=%u bytes=",
              part.info.main_size);
      for (unsigned i = 0; i < part.info.main_size; ++i)
         fprintf(stderr, "%02x", rgba8_code[i]);
      fputc('\n', stderr);
      return 1;
   }
   free(part.binary);

   free(main_part.binary);
   ralloc_free(mb.shader);
   return ok ? 0 : 1;
}
