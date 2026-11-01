/*
 * Copyright 2021 Alyssa Rosenzweig
 * Copyright 2019-2020 Collabora, Ltd.
 * Copyright 2014-2017 Broadcom
 * Copyright 2010 Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "agx_state.h"
#include <errno.h>
#include <stdio.h>
#include "asahi/compiler/agx_compile.h"
#include "asahi/compiler/agx_debug.h"
#include "asahi/compiler/agx3_compute_program.h"
#include "asahi/compiler/agx3_graphics_program.h"
#include "asahi/compiler/agx_nir.h"
#include "asahi/genxml/agx_pack.h"
#include "asahi/layout/layout.h"
#include "asahi/lib/agx_abi.h"
#include "asahi/lib/agx3_code_container_core.h"
#include "asahi/lib/agx3_g17p_grid_helper_core.h"
#include "asahi/lib/agx_g17p_draw_state_upload.h"
#include "asahi/lib/agx_helpers.h"
#include "asahi/lib/agx_ppp.h"
#include "asahi/lib/agx_ppp_hal200_graphics_core.h"
#include "asahi/lib/agx_ppp_hal200_raster_core.h"
#include "asahi/lib/agx_ppp_viewport_payload_core.h"
#include "asahi/lib/agx_ppp_w_clamp_cull2_core.h"
#include "asahi/lib/agx_usc.h"
#include "asahi/lib/agx_vdm_barrier_core.h"
#include "asahi/lib/agx_vdm_state_update_core.h"
#include "asahi/lib/agx_vdm_stream_link_core.h"
#include "asahi/lib/agx_va_runtime_hole.h"
#include "asahi/lib/agx_vdm_hal200_direct_draw_core.h"
#include "asahi/libagx/compression.h"
#include "asahi/libagx/query.h"
#include "asahi/libagx/tessellator.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_serialize.h"
#include "compiler/shader_enums.h"
#include "gallium/auxiliary/nir/pipe_nir.h"
#include "gallium/auxiliary/nir/tgsi_to_nir.h"
#include "gallium/auxiliary/tgsi/tgsi_from_mesa.h"
#include "gallium/auxiliary/util/u_draw.h"
#include "gallium/auxiliary/util/u_framebuffer.h"
#include "gallium/auxiliary/util/u_helpers.h"
#include "gallium/auxiliary/util/u_prim_restart.h"
#include "gallium/auxiliary/util/u_viewport.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "poly/geometry.h"
#include "util/bitscan.h"
#include "util/bitset.h"
#include "util/blend.h"
#include "util/blob.h"
#include "util/compiler.h"
#include "util/format/u_format.h"
#include "util/format/u_formats.h"
#include "util/half_float.h"
#include "util/hash_table.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"
#include "agx_bg_eot.h"
#include "agx_bo.h"
#include "agx_device.h"
#include "agx_disk_cache.h"
#include "agx_linker.h"
#include "agx_nir.h"
#include "agx_nir_lower_vbo.h"
#include "agx_tilebuffer.h"
#include "libagx.h"
#include "libagx_dgc.h"
#include "libagx_shaders.h"
#include "nir_builder.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "nir_xfb_info.h"
#include "pool.h"

/* G17P adds the render-context base when a VDM record follows a nested PPP
 * pointer. Gallium's batch pool already returns the full context VA, so make
 * only that nested pointer relative. The root VDM stream remains absolute. */
static bool
agx_gallium_vdm_compact(struct agx_batch *batch, uint64_t gpu,
                        uint64_t *compact)
{
   if (!batch || !compact)
      return false;

   struct agx_bo *payload_bo = NULL;
   util_dynarray_foreach(&batch->pool.bos, struct agx_bo *, bo) {
      if (gpu >= (*bo)->va->addr && gpu - (*bo)->va->addr < (*bo)->size) {
         payload_bo = *bo;
         break;
      }
   }
   if (!payload_bo)
      return false;

   const uint64_t alias =
      agx_batch_get_g17p_vdm_alias(batch, payload_bo, gpu);
   if (!agx_g17p_vdm_payload_compact(alias, compact))
      return false;

   return true;
}

static bool
agx_gallium_ppp_fini(struct agx_batch *batch, uint8_t **out,
                     struct agx_ppp_update *ppp)
{
   if (!batch || !ppp)
      return false;

   struct agx_device *dev = agx_device(batch->ctx->base.screen);
   if (!agx_native_graphics(dev))
      return agx_ppp_fini(dev->chip, out, ppp);

   struct agx_ppp_update compact_ppp = *ppp;
   if (!agx_gallium_vdm_compact(batch, ppp->gpu_base,
                                &compact_ppp.gpu_base))
      return false;

   return agx_ppp_fini(dev->chip, out, &compact_ppp);
}

void
agx_legalize_compression(struct agx_context *ctx, struct agx_resource *rsrc,
                         enum pipe_format format)
{
   if (rsrc && !ail_is_view_compatible(&rsrc->layout, format)) {
      agx_decompress(ctx, rsrc, "Incompatible formats");
   }
}

static void
agx_set_shader_images(struct pipe_context *pctx, mesa_shader_stage shader,
                      unsigned start_slot, unsigned count,
                      unsigned unbind_num_trailing_slots,
                      const struct pipe_image_view *iviews)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_IMAGE;

   /* Unbind start_slot...start_slot+count */
   if (!iviews) {
      for (int i = start_slot;
           i < start_slot + count + unbind_num_trailing_slots; i++) {
         pipe_resource_reference(&ctx->stage[shader].images[i].resource, NULL);
      }

      ctx->stage[shader].image_mask &=
         ~(BITFIELD64_MASK(count + unbind_num_trailing_slots) << start_slot);
      return;
   }

   /* Images writeable with pixel granularity are incompatible with
    * compression. Decompress if necessary.
    *
    * Driver-internal images are used by the compute blitter and are exempt
    * from these transitions, as it only uses compressed images when safe.
    *
    * We do this upfront because agx_decompress and agx_legalize_compression can
    * call set_shader_images internall.
    */
   for (int i = 0; i < count; i++) {
      const struct pipe_image_view *image = &iviews[i];
      struct agx_resource *rsrc = agx_resource(image->resource);

      if (rsrc && !(image->access & PIPE_IMAGE_ACCESS_DRIVER_INTERNAL)) {
         if (!rsrc->layout.writeable_image &&
             (image->shader_access & PIPE_IMAGE_ACCESS_WRITE)) {

            agx_decompress(ctx, rsrc, "Shader image");
         }

         /* Readable images may be compressed but are still subject to format
          * reinterpretation rules.
          */
         agx_legalize_compression(ctx, rsrc, image->format);

         if (image->shader_access & PIPE_IMAGE_ACCESS_WRITE)
            assert(rsrc->layout.writeable_image);
      }
   }

   /* Bind start_slot...start_slot+count */
   for (int i = 0; i < count; i++) {
      const struct pipe_image_view *image = &iviews[i];

      if (!image->resource) {
         util_copy_image_view(&ctx->stage[shader].images[start_slot + i], NULL);
         ctx->stage[shader].image_mask &= ~BITFIELD64_BIT(start_slot + i);
      } else {
         util_copy_image_view(&ctx->stage[shader].images[start_slot + i],
                              image);
         ctx->stage[shader].image_mask |= BITFIELD64_BIT(start_slot + i);
      }
   }

   /* Unbind start_slot+count...start_slot+count+unbind_num_trailing_slots */
   for (int i = 0; i < unbind_num_trailing_slots; i++) {
      ctx->stage[shader].image_mask &= ~BITFIELD64_BIT(start_slot + count + i);
      util_copy_image_view(&ctx->stage[shader].images[start_slot + count + i],
                           NULL);
   }
}

static void
agx_set_shader_buffers(struct pipe_context *pctx, mesa_shader_stage shader,
                       unsigned start, unsigned count,
                       const struct pipe_shader_buffer *buffers,
                       unsigned writable_bitmask)
{
   struct agx_context *ctx = agx_context(pctx);

   util_set_shader_buffers_mask(ctx->stage[shader].ssbo,
                                &ctx->stage[shader].ssbo_mask, buffers, start,
                                count);

   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_SSBO;
   ctx->stage[shader].ssbo_writable_mask &= ~(BITFIELD_MASK(count) << start);
   ctx->stage[shader].ssbo_writable_mask |= writable_bitmask << start;
}

static bool
agx_front_face_ccw(const struct agx_context *ctx)
{
   bool ccw = ctx->rast->base.front_ccw;
   if (ctx->in_tess && !ctx->gs)
      ccw ^= !ctx->stage[MESA_SHADER_TESS_EVAL].shader->tess.ccw;
   return ccw;
}

static void
agx_set_blend_color(struct pipe_context *pctx,
                    const struct pipe_blend_color *state)
{
   struct agx_context *ctx = agx_context(pctx);

   if (state)
      memcpy(&ctx->blend_color, state, sizeof(*state));

   ctx->dirty |= AGX_DIRTY_BLEND_COLOR;
}

static void
agx_set_patch_vertices(struct pipe_context *pctx, unsigned char n)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->patch_vertices = n;
}

static void
agx_set_tess_state(struct pipe_context *pctx,
                   const float default_outer_level[4],
                   const float default_inner_level[2])
{
   struct agx_context *ctx = agx_context(pctx);

   memcpy(ctx->default_outer_level, default_outer_level, 4 * sizeof(float));
   memcpy(ctx->default_inner_level, default_inner_level, 2 * sizeof(float));
}

static void *
agx_create_blend_state(struct pipe_context *ctx,
                       const struct pipe_blend_state *state)
{
   struct agx_blend *so = CALLOC_STRUCT(agx_blend);
   struct agx_blend_key *key = &so->key;

   key->alpha_to_coverage = state->alpha_to_coverage;
   key->alpha_to_one = state->alpha_to_one;

   key->logicop_enable = state->logicop_enable;
   key->logicop_func = state->logicop_func;

   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; ++i) {
      unsigned rti = state->independent_blend_enable ? i : 0;
      struct pipe_rt_blend_state rt = state->rt[rti];

      if (state->logicop_enable || !rt.blend_enable) {
         /* No blending, but we get the colour mask below */
         key->rt[i].mode = agx_pack_blend_standard(
            PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ZERO,
            PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ZERO);
      } else {
         key->rt[i].mode = agx_pack_blend_standard(
            rt.rgb_func, rt.rgb_src_factor, rt.rgb_dst_factor, rt.alpha_func,
            rt.alpha_src_factor, rt.alpha_dst_factor);
      }

      key->rt[i].colormask = rt.colormask;

      if (rt.colormask)
         so->store |= (PIPE_CLEAR_COLOR0 << i);
   }

   return so;
}

static void
agx_bind_blend_state(struct pipe_context *pctx, void *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->blend = cso;
   ctx->dirty |= AGX_DIRTY_BLEND;
}

static const enum agx_stencil_op agx_stencil_ops[PIPE_STENCIL_OP_INVERT + 1] = {
   [PIPE_STENCIL_OP_KEEP] = AGX_STENCIL_OP_KEEP,
   [PIPE_STENCIL_OP_ZERO] = AGX_STENCIL_OP_ZERO,
   [PIPE_STENCIL_OP_REPLACE] = AGX_STENCIL_OP_REPLACE,
   [PIPE_STENCIL_OP_INCR] = AGX_STENCIL_OP_INCR_SAT,
   [PIPE_STENCIL_OP_DECR] = AGX_STENCIL_OP_DECR_SAT,
   [PIPE_STENCIL_OP_INCR_WRAP] = AGX_STENCIL_OP_INCR_WRAP,
   [PIPE_STENCIL_OP_DECR_WRAP] = AGX_STENCIL_OP_DECR_WRAP,
   [PIPE_STENCIL_OP_INVERT] = AGX_STENCIL_OP_INVERT,
};

static void
agx_pack_stencil(struct agx_fragment_stencil_packed *out,
                 struct pipe_stencil_state st)
{
   if (st.enabled) {
      agx_pack(out, FRAGMENT_STENCIL, cfg) {
         cfg.compare = (enum agx_zs_func)st.func;
         cfg.write_mask = st.writemask;
         cfg.read_mask = st.valuemask;

         cfg.depth_pass = agx_stencil_ops[st.zpass_op];
         cfg.depth_fail = agx_stencil_ops[st.zfail_op];
         cfg.stencil_fail = agx_stencil_ops[st.fail_op];
      }
   } else {
      agx_pack(out, FRAGMENT_STENCIL, cfg) {
         cfg.compare = AGX_ZS_FUNC_ALWAYS;
         cfg.write_mask = 0xFF;
         cfg.read_mask = 0xFF;

         cfg.depth_pass = AGX_STENCIL_OP_KEEP;
         cfg.depth_fail = AGX_STENCIL_OP_KEEP;
         cfg.stencil_fail = AGX_STENCIL_OP_KEEP;
      }
   }
}

static void *
agx_create_zsa_state(struct pipe_context *ctx,
                     const struct pipe_depth_stencil_alpha_state *state)
{
   struct agx_zsa *so = CALLOC_STRUCT(agx_zsa);
   assert(!state->depth_bounds_test && "todo");

   so->base = *state;

   /* Handle the enable flag */
   enum pipe_compare_func depth_func =
      state->depth_enabled ? state->depth_func : PIPE_FUNC_ALWAYS;

   /* Z func can otherwise be used as-is */
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_NEVER == AGX_ZS_FUNC_NEVER);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_LESS == AGX_ZS_FUNC_LESS);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_EQUAL == AGX_ZS_FUNC_EQUAL);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_LEQUAL == AGX_ZS_FUNC_LEQUAL);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_GREATER == AGX_ZS_FUNC_GREATER);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_NOTEQUAL == AGX_ZS_FUNC_NOT_EQUAL);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_GEQUAL == AGX_ZS_FUNC_GEQUAL);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_ALWAYS == AGX_ZS_FUNC_ALWAYS);

   agx_pack(&so->depth, FRAGMENT_FACE, cfg) {
      cfg.depth_function = (enum agx_zs_func)depth_func;
      cfg.disable_depth_write = !state->depth_writemask;
   }

   agx_pack_stencil(&so->front_stencil, state->stencil[0]);

   if (state->stencil[1].enabled) {
      agx_pack_stencil(&so->back_stencil, state->stencil[1]);
   } else {
      /* One sided stencil */
      so->back_stencil = so->front_stencil;
   }

   if (depth_func != PIPE_FUNC_NEVER && depth_func != PIPE_FUNC_ALWAYS)
      so->load |= PIPE_CLEAR_DEPTH;

   if (state->depth_writemask) {
      so->load |= PIPE_CLEAR_DEPTH;
      so->store |= PIPE_CLEAR_DEPTH;
   }

   if (state->stencil[0].enabled) {
      so->load |= PIPE_CLEAR_STENCIL; /* TODO: Optimize */
      so->store |= PIPE_CLEAR_STENCIL;
   }

   return so;
}

static void
agx_bind_zsa_state(struct pipe_context *pctx, void *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->zs = cso;
   ctx->dirty |= AGX_DIRTY_ZS;
}

static enum agx_polygon_mode
agx_translate_polygon_mode(unsigned mode)
{
   switch (mode) {
   case PIPE_POLYGON_MODE_FILL:
      return AGX_POLYGON_MODE_FILL;
   case PIPE_POLYGON_MODE_POINT:
      return AGX_POLYGON_MODE_POINT;
   case PIPE_POLYGON_MODE_LINE:
      return AGX_POLYGON_MODE_LINE;
   default:
      UNREACHABLE("Unsupported polygon mode");
   }
}

static void *
agx_create_rs_state(struct pipe_context *ctx,
                    const struct pipe_rasterizer_state *cso)
{
   struct agx_rasterizer *so = CALLOC_STRUCT(agx_rasterizer);
   so->base = *cso;

   agx_pack(so->cull, CULL, cfg) {
      cfg.cull_front = cso->cull_face & PIPE_FACE_FRONT;
      cfg.cull_back = cso->cull_face & PIPE_FACE_BACK;
      cfg.depth_clip = cso->depth_clip_near;
      cfg.depth_clamp = !cso->depth_clip_near;
      cfg.flat_shading_vertex =
         cso->flatshade_first ? AGX_PPP_VERTEX_0 : AGX_PPP_VERTEX_2;
      cfg.rasterizer_discard = cso->rasterizer_discard;
   };

   /* Two-sided polygon mode doesn't seem to work on G13. Apple's OpenGL
    * implementation lowers to multiple draws with culling. Warn.
    */
   if (unlikely(cso->fill_front != cso->fill_back)) {
      agx_msg("Warning: Two-sided fill modes are unsupported, "
              "rendering may be incorrect.\n");
   }

   so->polygon_mode = agx_translate_polygon_mode(cso->fill_front);
   so->line_width = agx_pack_line_width(cso->line_width);
   so->depth_bias = util_get_offset(cso, cso->fill_front);

   return so;
}

static void
agx_bind_rasterizer_state(struct pipe_context *pctx, void *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_rasterizer *so = cso;

   bool base_cso_changed = (cso == NULL) || (ctx->rast == NULL);

   /* Check if scissor or depth bias state has changed, since scissor/depth bias
    * enable is part of the rasterizer state but everything else needed for
    * scissors and depth bias is part of the scissor/depth bias arrays */
   bool scissor_zbias_changed = base_cso_changed ||
                                (ctx->rast->base.scissor != so->base.scissor) ||
                                (ctx->rast->depth_bias != so->depth_bias);

   ctx->dirty |= AGX_DIRTY_RS;

   if (scissor_zbias_changed)
      ctx->dirty |= AGX_DIRTY_SCISSOR_ZBIAS;

   if (base_cso_changed ||
       (ctx->rast->base.sprite_coord_mode != so->base.sprite_coord_mode))
      ctx->dirty |= AGX_DIRTY_SPRITE_COORD_MODE;

   ctx->rast = so;
}

static bool
has_edgeflags(struct agx_context *ctx, enum mesa_prim mode)
{
   return ctx->stage[MESA_SHADER_VERTEX].shader->info.has_edgeflags &&
          mode == MESA_PRIM_TRIANGLES &&
          (ctx->rast->base.fill_front != PIPE_POLYGON_MODE_FILL);
}

static enum agx_wrap
agx_wrap_from_pipe(enum pipe_tex_wrap in)
{
   switch (in) {
   case PIPE_TEX_WRAP_REPEAT:
      return AGX_WRAP_REPEAT;
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
      return AGX_WRAP_CLAMP_TO_EDGE;
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      return AGX_WRAP_MIRRORED_REPEAT;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      return AGX_WRAP_CLAMP_TO_BORDER;
   case PIPE_TEX_WRAP_CLAMP:
      return AGX_WRAP_CLAMP_GL;
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
      return AGX_WRAP_MIRRORED_CLAMP_TO_EDGE;
   default:
      UNREACHABLE("Invalid wrap mode");
   }
}

static enum agx_mip_filter
agx_mip_filter_from_pipe(enum pipe_tex_mipfilter in)
{
   switch (in) {
   case PIPE_TEX_MIPFILTER_NEAREST:
      return AGX_MIP_FILTER_NEAREST;
   case PIPE_TEX_MIPFILTER_LINEAR:
      return AGX_MIP_FILTER_LINEAR;
   case PIPE_TEX_MIPFILTER_NONE:
      return AGX_MIP_FILTER_NONE;
   }

   UNREACHABLE("Invalid mip filter");
}

static const enum agx_compare_func agx_compare_funcs[PIPE_FUNC_ALWAYS + 1] = {
   [PIPE_FUNC_NEVER] = AGX_COMPARE_FUNC_NEVER,
   [PIPE_FUNC_LESS] = AGX_COMPARE_FUNC_LESS,
   [PIPE_FUNC_EQUAL] = AGX_COMPARE_FUNC_EQUAL,
   [PIPE_FUNC_LEQUAL] = AGX_COMPARE_FUNC_LEQUAL,
   [PIPE_FUNC_GREATER] = AGX_COMPARE_FUNC_GREATER,
   [PIPE_FUNC_NOTEQUAL] = AGX_COMPARE_FUNC_NOT_EQUAL,
   [PIPE_FUNC_GEQUAL] = AGX_COMPARE_FUNC_GEQUAL,
   [PIPE_FUNC_ALWAYS] = AGX_COMPARE_FUNC_ALWAYS,
};

static const enum agx_filter agx_filters[] = {
   [PIPE_TEX_FILTER_LINEAR] = AGX_FILTER_LINEAR,
   [PIPE_TEX_FILTER_NEAREST] = AGX_FILTER_NEAREST,
};

static enum pipe_format
fixup_border_zs(enum pipe_format orig, union pipe_color_union *c)
{
   switch (orig) {
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_Z24X8_UNORM:
      /* Z24 is internally promoted to Z32F via transfer_helper. These formats
       * are normalized so should get clamped, but Z32F does not get clamped, so
       * we clamp here.
       */
      c->f[0] = SATURATE(c->f[0]);
      return PIPE_FORMAT_Z32_FLOAT;

   case PIPE_FORMAT_X24S8_UINT:
   case PIPE_FORMAT_X32_S8X24_UINT:
      /* Separate stencil is internally promoted */
      return PIPE_FORMAT_S8_UINT;

   default:
      return orig;
   }
}

static void *
agx_create_sampler_state(struct pipe_context *pctx,
                         const struct pipe_sampler_state *state)
{
   struct agx_sampler_state *so = CALLOC_STRUCT(agx_sampler_state);
   so->base = *state;

   so->border_colour = AGX_BORDER_COLOUR_TRANSPARENT_BLACK;
   const bool uses_border =
      state->wrap_s == PIPE_TEX_WRAP_CLAMP_TO_BORDER ||
      state->wrap_t == PIPE_TEX_WRAP_CLAMP_TO_BORDER ||
      state->wrap_r == PIPE_TEX_WRAP_CLAMP_TO_BORDER ||
      state->wrap_s == PIPE_TEX_WRAP_CLAMP ||
      state->wrap_t == PIPE_TEX_WRAP_CLAMP ||
      state->wrap_r == PIPE_TEX_WRAP_CLAMP;

   if (uses_border && state->border_color_format != PIPE_FORMAT_NONE) {
      union pipe_color_union border = state->border_color;
      enum pipe_format format =
         fixup_border_zs(state->border_color_format, &border);
      const bool integer = util_format_is_pure_integer(format);

      /* A fixed-point depth comparison uses the border depth clamped to
       * [0, 1]; a floating-point one uses it unclamped. Z24 is clamped
       * above, before its promotion to Z32F. */
      const struct util_format_description *zdesc =
         util_format_description(format);
      if (state->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE &&
          util_format_has_depth(zdesc) &&
          zdesc->channel[0].type != UTIL_FORMAT_TYPE_FLOAT)
         border.f[0] = SATURATE(border.f[0]);

      if (!agx_gallium_classify_standard_border(
             border.ui, integer, &so->border_colour)) {
         so->uses_custom_border = true;
         so->border_colour = AGX_BORDER_COLOUR_CUSTOM;
         agx_pack_border(&so->border, border.ui, format);
         memcpy(so->custom_border, border.ui, sizeof(so->custom_border));
      }
   }

   /* We report a max texture LOD bias of 16, so clamp appropriately */
   float lod_bias = CLAMP(state->lod_bias, -16.0, 16.0);
   so->lod_bias_as_fp16 = _mesa_float_to_half(lod_bias);

   agx_pack(&so->desc, SAMPLER, cfg) {
      cfg.minimum_lod = state->min_lod;
      cfg.maximum_lod = state->max_lod;
      cfg.maximum_anisotropy =
         util_next_power_of_two(MAX2(state->max_anisotropy, 1));
      cfg.magnify = agx_filters[state->mag_img_filter];
      cfg.minify = agx_filters[state->min_img_filter];
      cfg.mip_filter = agx_mip_filter_from_pipe(state->min_mip_filter);
      cfg.wrap_s = agx_wrap_from_pipe(state->wrap_s);
      cfg.wrap_t = agx_wrap_from_pipe(state->wrap_t);
      cfg.wrap_r = agx_wrap_from_pipe(state->wrap_r);
      cfg.pixel_coordinates = state->unnormalized_coords;
      cfg.compare_func = agx_compare_funcs[state->compare_func];
      cfg.compare_enable = state->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE;
      cfg.seamful_cube_maps = !state->seamless_cube_map;
      cfg.border_colour = so->border_colour;
   }

   memcpy(&so->desc_without_custom_border, &so->desc, sizeof(so->desc));

   if (so->uses_custom_border) {
      /* Neutralize the bindless-safe descriptor. XXX: This is a hack. */
      so->desc_without_custom_border.opaque[1] &= ~(1u << 23);
   }

   return so;
}

static void
agx_delete_sampler_state(struct pipe_context *ctx, void *state)
{
   struct agx_sampler_state *so = state;
   FREE(so);
}

static void
agx_bind_sampler_states(struct pipe_context *pctx, mesa_shader_stage shader,
                        unsigned start, unsigned count, void **states)
{
   struct agx_context *ctx = agx_context(pctx);

   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_SAMPLER;
   if (shader == MESA_SHADER_VERTEX)
      ctx->dirty |= AGX_DIRTY_VS_PROG;
   else if (shader == MESA_SHADER_FRAGMENT)
      ctx->dirty |= AGX_DIRTY_FS_PROG;

   for (unsigned i = 0; i < count; i++) {
      unsigned p = start + i;
      ctx->stage[shader].samplers[p] = states ? states[i] : NULL;
      if (ctx->stage[shader].samplers[p])
         ctx->stage[shader].valid_samplers |= BITFIELD_BIT(p);
      else
         ctx->stage[shader].valid_samplers &= ~BITFIELD_BIT(p);
   }

   ctx->stage[shader].sampler_count =
      util_last_bit(ctx->stage[shader].valid_samplers);

   /* Recalculate whether we need custom borders */
   ctx->stage[shader].custom_borders = false;

   u_foreach_bit(i, ctx->stage[shader].valid_samplers) {
      if (ctx->stage[shader].samplers[i]->uses_custom_border)
         ctx->stage[shader].custom_borders = true;
   }
}

static enum agx_texture_dimension
agx_translate_tex_dim(enum pipe_texture_target dim, unsigned samples)
{
   assert(samples >= 1);

   switch (dim) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D:
      /* Lowered to 2D */
      assert(samples == 1);
      return AGX_TEXTURE_DIMENSION_2D;

   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_2D:
      return samples > 1 ? AGX_TEXTURE_DIMENSION_2D_MULTISAMPLED
                         : AGX_TEXTURE_DIMENSION_2D;

   case PIPE_TEXTURE_1D_ARRAY:
      assert(samples == 1);
      /* Lowered to 2D */
      FALLTHROUGH;
   case PIPE_TEXTURE_2D_ARRAY:
      return samples > 1 ? AGX_TEXTURE_DIMENSION_2D_ARRAY_MULTISAMPLED
                         : AGX_TEXTURE_DIMENSION_2D_ARRAY;

   case PIPE_TEXTURE_3D:
      assert(samples == 1);
      return AGX_TEXTURE_DIMENSION_3D;

   case PIPE_TEXTURE_CUBE:
      assert(samples == 1);
      return AGX_TEXTURE_DIMENSION_CUBE;

   case PIPE_TEXTURE_CUBE_ARRAY:
      assert(samples == 1);
      return AGX_TEXTURE_DIMENSION_CUBE_ARRAY;

   default:
      UNREACHABLE("Unsupported texture dimension");
   }
}

static bool
target_is_cube(enum pipe_texture_target target)
{
   return target == PIPE_TEXTURE_CUBE || target == PIPE_TEXTURE_CUBE_ARRAY;
}

static void NORETURN
agx_native_descriptor_rejected(struct agx_device *dev, const char *operation)
{
   mesa_loge("asahi: %s has no proven native descriptor encoding on %s",
             operation, agx_chip_name(dev->chip));
   /* Never submit a zero or partially initialized native descriptor. */
   abort();
}

static enum agx_channel
agx_rgb32_buffer_fallback_channel(enum agx_channel channel)
{
   if (channel == AGX_CHANNEL_G || channel == AGX_CHANNEL_B)
      return AGX_CHANNEL_0;
   if (channel == AGX_CHANNEL_A)
      return AGX_CHANNEL_1;
   return channel;
}

static bool
agx_pack_texture(enum agx_descriptor_generation descriptor_generation,
                 struct agx_gallium_texture_descriptor *out,
                 struct agx_resource *rsrc,
                 enum pipe_format format /* override */,
                 const struct pipe_sampler_view *state)
{
   const struct util_format_description *desc = util_format_description(format);

   assert(ail_is_valid_pixel_format(format));

   uint8_t format_swizzle[4] = {
      desc->swizzle[0],
      desc->swizzle[1],
      desc->swizzle[2],
      desc->swizzle[3],
   };

   if (util_format_is_depth_or_stencil(format)) {
      assert(!util_format_is_depth_and_stencil(format) &&
             "separate stencil always used");

      /* Broadcast depth and stencil */
      format_swizzle[0] = 0;
      format_swizzle[1] = 0;
      format_swizzle[2] = 0;
      format_swizzle[3] = 0;
   }

   /* We only have a single swizzle for the user swizzle and the format fixup,
    * so compose them now. */
   uint8_t out_swizzle[4];
   uint8_t view_swizzle[4] = {state->swizzle_r, state->swizzle_g,
                              state->swizzle_b, state->swizzle_a};

   util_format_compose_swizzles(format_swizzle, view_swizzle, out_swizzle);

   unsigned first_layer =
      (state->target == PIPE_BUFFER) ? 0 : state->u.tex.first_layer;

   /* First construct generation-independent semantics.  The native packer
    * selects Gen7/8, Gen9, or Gen10 without allowing a legacy C type to choose
    * the heap stride.
    */
   struct AGX_TEXTURE cfg = {AGX_TEXTURE_header};
   {
      cfg.dimension = agx_translate_tex_dim(state->target,
                                            util_res_sample_count(&rsrc->base));
      cfg.layout = agx_translate_layout(rsrc->layout.tiling);
      cfg.channels = ail_pixel_format[format].channels;
      cfg.type = ail_pixel_format[format].type;
      cfg.swizzle_r = agx_channel_from_pipe(out_swizzle[0]);
      cfg.swizzle_g = agx_channel_from_pipe(out_swizzle[1]);
      cfg.swizzle_b = agx_channel_from_pipe(out_swizzle[2]);
      cfg.swizzle_a = agx_channel_from_pipe(out_swizzle[3]);

      if (state->target == PIPE_BUFFER) {
         unsigned size_el =
            agx_texture_buffer_size_el(format, state->u.buf.size);

         /* Use a 2D texture to increase the maximum size */
         cfg.width = AGX_TEXTURE_BUFFER_WIDTH;
         cfg.height = DIV_ROUND_UP(size_el, cfg.width);
         cfg.first_level = cfg.last_level = 0;
         cfg.buffer_size_sw = size_el;
         cfg.buffer_offset_sw = 0;
      } else {
         cfg.width = rsrc->base.width0;
         cfg.height = rsrc->base.height0;
         cfg.first_level = state->u.tex.first_level;
         cfg.last_level = state->u.tex.last_level;
      }

      cfg.srgb = (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB);
      cfg.unk_mipmapped = rsrc->mipmapped;
      cfg.srgb_2_channel = cfg.srgb && util_format_colormask(desc) == 0x3;

      cfg.compressed = rsrc->layout.compressed;
      cfg.extended = cfg.compressed;

      cfg.address = agx_map_texture_gpu(rsrc, first_layer);

      if (state->target == PIPE_BUFFER)
         cfg.address += state->u.buf.offset;

      if (rsrc->layout.compressed) {
         cfg.acceleration_buffer =
            agx_map_gpu(rsrc) + rsrc->layout.metadata_offset_B +
            (first_layer * rsrc->layout.compression_layer_stride_B);
      }

      /* Native Gen9 cube descriptors require one complete six-face range.
       * The shared extended-range address is filled for all applicable
       * dimensions below, after the selected layer count is known. */
      if (agx_native_graphics(agx_device(rsrc->base.screen)) &&
          state->target == PIPE_TEXTURE_CUBE) {
         const unsigned faces =
            state->u.tex.last_layer - state->u.tex.first_layer + 1;

         /* The recovered descriptor has one uncompressed six-face range.
          * Compression metadata or a different range would alias the same
          * secondary word, so reject those shapes instead of guessing. */
         if (rsrc->layout.compressed || faces != 6)
            return false;
      }

      if (state->target == PIPE_TEXTURE_3D) {
         cfg.depth = rsrc->base.depth0;
      } else if (state->target == PIPE_BUFFER) {
         cfg.depth = 1;
      } else {
         unsigned layers =
            state->u.tex.last_layer - state->u.tex.first_layer + 1;

         if (target_is_cube(state->target))
            layers /= 6;

         if (rsrc->layout.tiling == AIL_TILING_LINEAR &&
             (state->target == PIPE_TEXTURE_1D_ARRAY ||
              state->target == PIPE_TEXTURE_2D_ARRAY)) {

            cfg.depth_linear = layers;
            cfg.layer_stride_linear = (rsrc->layout.layer_stride_B - 0x80);
            cfg.extended = true;
         } else {
            assert((rsrc->layout.tiling != AIL_TILING_LINEAR) || (layers == 1));
            cfg.depth = layers;
         }
      }

      if (rsrc->base.nr_samples > 1)
         cfg.samples = agx_translate_sample_count(rsrc->base.nr_samples);

      if (state->target == PIPE_BUFFER) {
         cfg.stride = (cfg.width * util_format_get_blocksize(format)) - 16;
      } else if (rsrc->layout.tiling == AIL_TILING_LINEAR) {
         cfg.stride = ail_get_linear_stride_B(&rsrc->layout, 0) - 16;
      } else {
         cfg.page_aligned_layers = rsrc->layout.page_aligned_layers;
      }

      /* A18 Gen9 publishes a nonlinear texture's exclusive end VA whenever
       * the descriptor is extended: one-level arrays/3D/cubes as well as
       * mipmapped 2D.  The exact captures advance by the complete selected
       * layer stride (or by the complete 3D miptree), independently of whether
       * the layers themselves are tightly or page aligned.  Compression keeps
       * using the same word for metadata instead. */
      const bool gen9_range =
         agx_native_graphics(agx_device(rsrc->base.screen)) &&
         !rsrc->layout.compressed &&
         rsrc->layout.tiling != AIL_TILING_LINEAR &&
         (state->target == PIPE_TEXTURE_2D_ARRAY ||
          state->target == PIPE_TEXTURE_3D ||
          state->target == PIPE_TEXTURE_CUBE ||
          rsrc->mipmapped ||
          state->u.tex.first_level != 0 || state->u.tex.last_level != 0);
      if (gen9_range) {
         unsigned layers =
            state->u.tex.last_layer - state->u.tex.first_layer + 1;
         const uint64_t range_B =
            state->target == PIPE_TEXTURE_3D
               ? rsrc->layout.size_B
               : rsrc->layout.layer_stride_B * layers;

         cfg.extended = true;
         cfg.acceleration_buffer = cfg.address + range_B;
      }
   }

   if (state->target == PIPE_BUFFER &&
       descriptor_generation == AGX_DESCRIPTOR_GENERATION_9 &&
       cfg.channels == AGX_CHANNELS_R32G32B32_EMULATED) {
      cfg.channels = AGX_CHANNELS_R32;
      cfg.stride = cfg.width * sizeof(uint32_t) - 16;
      cfg.swizzle_r = agx_rgb32_buffer_fallback_channel(cfg.swizzle_r);
      cfg.swizzle_g = agx_rgb32_buffer_fallback_channel(cfg.swizzle_g);
      cfg.swizzle_b = agx_rgb32_buffer_fallback_channel(cfg.swizzle_b);
      cfg.swizzle_a = agx_rgb32_buffer_fallback_channel(cfg.swizzle_a);
   }

   const bool valid =
      agx_gallium_pack_native_texture(descriptor_generation, out, &cfg);
   if (valid && descriptor_generation == AGX_DESCRIPTOR_GENERATION_9 &&
       (agx_device(rsrc->base.screen)->debug &
        (AGX_DBG_TRACE | AGX_DBG_ASYNCTRACE))) {
      fprintf(stderr,
              "asahi: Gen9 texture %s %ux%u levels=%u..%u mipmapped=%u "
              "address=0x%" PRIx64 " secondary=0x%" PRIx64
              " size=0x%" PRIx64 " layer-stride=0x%" PRIx64
              " page-aligned=%u extended=%u compressed=%u words:",
              util_format_short_name(format), cfg.width, cfg.height,
              cfg.first_level, cfg.last_level, cfg.unk_mipmapped, cfg.address,
              cfg.acceleration_buffer, (uint64_t)rsrc->layout.size_B,
              (uint64_t)rsrc->layout.layer_stride_B, cfg.page_aligned_layers,
              cfg.extended, cfg.compressed);
      for (unsigned i = 0; i <
           agx_gallium_texture_descriptor_size(descriptor_generation) / 4; ++i)
         fprintf(stderr, " %08x", out->opaque[i]);
      fprintf(stderr, "\n");
   }
   return valid;
}

static struct pipe_sampler_view *
agx_create_sampler_view(struct pipe_context *pctx,
                        struct pipe_resource *orig_texture,
                        const struct pipe_sampler_view *state)
{
   struct agx_resource *rsrc = agx_resource(orig_texture);
   struct agx_sampler_view *so = CALLOC_STRUCT(agx_sampler_view);

   if (!so)
      return NULL;

   struct pipe_resource *texture = orig_texture;
   enum pipe_format format = state->format;

   const struct util_format_description *desc = util_format_description(format);

   /* Separate stencil always used on G13, so we need to fix up for Z32S8 */
   if (util_format_has_stencil(desc) && rsrc->separate_stencil) {
      if (util_format_has_depth(desc)) {
         /* Reinterpret as the depth-only part */
         format = util_format_get_depth_only(format);
      } else {
         /* Use the stencil-only-part */
         rsrc = rsrc->separate_stencil;
         texture = &rsrc->base;
         format = texture->format;
      }
   }

   agx_legalize_compression(agx_context(pctx), rsrc, format);

   /* Save off the resource that we actually use, with the stencil fixed up */
   so->rsrc = rsrc;
   so->format = format;

   so->base = *state;
   so->base.texture = NULL;
   pipe_resource_reference(&so->base.texture, orig_texture);
   pipe_reference_init(&so->base.reference, 1);
   so->base.context = pctx;
   return &so->base;
}

static void
agx_set_sampler_views(struct pipe_context *pctx, mesa_shader_stage shader,
                      unsigned start, unsigned count,
                      unsigned unbind_num_trailing_slots,
                      struct pipe_sampler_view **views)
{
   struct agx_context *ctx = agx_context(pctx);
   unsigned new_nr = 0;
   unsigned i;

   assert(start == 0);

   if (!views)
      count = 0;

   for (i = 0; i < count; ++i) {
      pipe_sampler_view_reference(
         (struct pipe_sampler_view **)&ctx->stage[shader].textures[i],
         views[i]);
   }

   for (; i < count + unbind_num_trailing_slots; i++) {
      pipe_sampler_view_reference(
         (struct pipe_sampler_view **)&ctx->stage[shader].textures[i], NULL);
   }

   for (unsigned t = 0; t < MAX2(ctx->stage[shader].texture_count, count);
        ++t) {
      if (ctx->stage[shader].textures[t])
         new_nr = t + 1;
   }

   ctx->stage[shader].texture_count = new_nr;
   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_IMAGE;
}

static void
agx_sampler_view_destroy(struct pipe_context *ctx,
                         struct pipe_sampler_view *pview)
{
   struct agx_sampler_view *view = (struct agx_sampler_view *)pview;
   pipe_resource_reference(&view->base.texture, NULL);
   FREE(view);
}

static void
agx_set_polygon_stipple(struct pipe_context *pctx,
                        const struct pipe_poly_stipple *state)
{
   struct agx_context *ctx = agx_context(pctx);

   memcpy(ctx->poly_stipple, state->stipple, sizeof(ctx->poly_stipple));
   ctx->dirty |= AGX_DIRTY_POLY_STIPPLE;
}

static void
agx_set_sample_mask(struct pipe_context *pipe, unsigned sample_mask)
{
   struct agx_context *ctx = agx_context(pipe);

   /* Optimization: At most MSAA 4x supported, so normalize to avoid pointless
    * dirtying switching between e.g. 0xFFFF and 0xFFFFFFFF masks.
    */
   unsigned new_mask = sample_mask & BITFIELD_MASK(4);

   if (ctx->sample_mask != new_mask) {
      ctx->sample_mask = new_mask;
      ctx->dirty |= AGX_DIRTY_SAMPLE_MASK;
   }
}

static void
agx_set_scissor_states(struct pipe_context *pctx, unsigned start_slot,
                       unsigned num_scissors,
                       const struct pipe_scissor_state *scissor)
{
   struct agx_context *ctx = agx_context(pctx);

   STATIC_ASSERT(sizeof(ctx->scissor[0]) == sizeof(*scissor));
   assert(start_slot + num_scissors <= AGX_MAX_VIEWPORTS);

   memcpy(&ctx->scissor[start_slot], scissor, sizeof(*scissor) * num_scissors);
   ctx->dirty |= AGX_DIRTY_SCISSOR_ZBIAS;
}

static void
agx_set_stencil_ref(struct pipe_context *pctx,
                    const struct pipe_stencil_ref state)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->stencil_ref = state;
   ctx->dirty |= AGX_DIRTY_STENCIL_REF;
}

static void
agx_set_viewport_states(struct pipe_context *pctx, unsigned start_slot,
                        unsigned num_viewports,
                        const struct pipe_viewport_state *vp)
{
   struct agx_context *ctx = agx_context(pctx);

   STATIC_ASSERT(sizeof(ctx->viewport[0]) == sizeof(*vp));
   assert(start_slot + num_viewports <= AGX_MAX_VIEWPORTS);

   memcpy(&ctx->viewport[start_slot], vp, sizeof(*vp) * num_viewports);
   ctx->dirty |= AGX_DIRTY_VIEWPORT;
}

static void
agx_get_scissor_extents(const struct pipe_viewport_state *vp,
                        const struct pipe_scissor_state *ss,
                        const struct pipe_framebuffer_state *fb, unsigned *minx,
                        unsigned *miny, unsigned *maxx, unsigned *maxy)
{
   float trans_x = vp->translate[0], trans_y = vp->translate[1];
   float abs_scale_x = fabsf(vp->scale[0]), abs_scale_y = fabsf(vp->scale[1]);

   /* Calculate the extent of the viewport. Note if a particular dimension of
    * the viewport is an odd number of pixels, both the translate and the scale
    * will have a fractional part of 0.5, so adding and subtracting them yields
    * an integer. Therefore we don't need to round explicitly */
   *minx = CLAMP((int)(trans_x - abs_scale_x), 0, fb->width);
   *miny = CLAMP((int)(trans_y - abs_scale_y), 0, fb->height);
   *maxx = CLAMP((int)(trans_x + abs_scale_x), 0, fb->width);
   *maxy = CLAMP((int)(trans_y + abs_scale_y), 0, fb->height);

   if (ss) {
      *minx = MAX2(ss->minx, *minx);
      *miny = MAX2(ss->miny, *miny);
      *maxx = MIN2(ss->maxx, *maxx);
      *maxy = MIN2(ss->maxy, *maxy);
   }
}

static void
agx_upload_viewport_scissor(struct agx_pool *pool, struct agx_batch *batch,
                            uint8_t **out, const struct pipe_viewport_state *vp,
                            const struct pipe_scissor_state *ss,
                            bool clip_halfz, bool multi_viewport)
{
   /* Number of viewports/scissors isn't precisely determinable in Gallium, so
    * just key off whether we can write to anything other than viewport 0. This
    * could be tuned in the future.
    */
   unsigned count = multi_viewport ? AGX_MAX_VIEWPORTS : 1;

   /* Allocate scissor descriptors */
   unsigned index = batch->scissor.size / AGX_SCISSOR_LENGTH;
   struct agx_scissor_packed *scissors =
      util_dynarray_grow_bytes(&batch->scissor, count, AGX_SCISSOR_LENGTH);

   unsigned minx[AGX_MAX_VIEWPORTS], miny[AGX_MAX_VIEWPORTS];
   unsigned maxx[AGX_MAX_VIEWPORTS], maxy[AGX_MAX_VIEWPORTS];

   /* Upload each scissor */
   for (unsigned i = 0; i < count; ++i) {
      agx_get_scissor_extents(&vp[i], ss ? &ss[i] : NULL, &batch->key, &minx[i],
                              &miny[i], &maxx[i], &maxy[i]);

      float minz, maxz;
      util_viewport_zmin_zmax(vp, clip_halfz, &minz, &maxz);

      agx_pack(scissors + i, SCISSOR, cfg) {
         cfg.min_x = minx[i];
         cfg.min_y = miny[i];
         cfg.min_z = minz;
         cfg.max_x = maxx[i];
         cfg.max_y = maxy[i];
         cfg.max_z = maxz;
      }
   }

   /* Upload state */
   struct AGX_PPP_HEADER present = {
      .depth_bias_scissor = true,
      .region_clip = true,
      .viewport = true,
      .viewport_count = count,
   };

   size_t size = agx_ppp_update_size(&present);
   struct agx_ptr T =
      agx_pool_alloc_aligned(&batch->pool, size, AGX_PPP_HEADER_ALIGN);
   struct agx_ppp_update ppp = agx_new_ppp_update(T, size, &present);

   agx_ppp_push(&ppp, DEPTH_BIAS_SCISSOR, cfg) {
      cfg.scissor = index;

      /* Use the current depth bias, we allocate linearly */
      unsigned count = batch->depth_bias.size / AGX_DEPTH_BIAS_LENGTH;
      cfg.depth_bias = count ? count - 1 : 0;
   };

   for (unsigned i = 0; i < count; ++i) {
      agx_ppp_push(&ppp, REGION_CLIP, cfg) {
         cfg.enable = true;
         cfg.min_x = minx[i] / 32;
         cfg.min_y = miny[i] / 32;
         cfg.max_x = DIV_ROUND_UP(MAX2(maxx[i], 1), 32);
         cfg.max_y = DIV_ROUND_UP(MAX2(maxy[i], 1), 32);
      }
   }

   agx_ppp_push(&ppp, VIEWPORT_CONTROL, cfg)
      ;

   /* Upload viewports */
   for (unsigned i = 0; i < count; ++i) {
      agx_ppp_push(&ppp, VIEWPORT, cfg) {
         cfg.translate_x = vp[i].translate[0];
         cfg.translate_y = vp[i].translate[1];
         cfg.translate_z = vp[i].translate[2];
         cfg.scale_x = vp[i].scale[0];
         cfg.scale_y = vp[i].scale[1];
         cfg.scale_z = vp[i].scale[2];

         if (!clip_halfz) {
            cfg.translate_z -= cfg.scale_z;
            cfg.scale_z *= 2;
         }
      }
   }

   bool emitted = agx_gallium_ppp_fini(batch, out, &ppp);
   assert(emitted);
   (void)emitted;
}

static void
agx_upload_depth_bias(struct agx_batch *batch,
                      const struct pipe_rasterizer_state *rast)
{
   void *ptr =
      util_dynarray_grow_bytes(&batch->depth_bias, 1, AGX_DEPTH_BIAS_LENGTH);

   agx_pack(ptr, DEPTH_BIAS, cfg) {
      cfg.depth_bias = rast->offset_units * 2.0f;
      cfg.slope_scale = rast->offset_scale;
      cfg.clamp = rast->offset_clamp;
   }
}

/* A framebuffer state can be reused across batches, so it doesn't make sense
 * to add surfaces to the BO list here. Instead we added them when flushing.
 */

static void
agx_set_framebuffer_state(struct pipe_context *pctx,
                          const struct pipe_framebuffer_state *state)
{
   struct agx_context *ctx = agx_context(pctx);

   if (!state)
      return;

   util_copy_framebuffer_state(&ctx->framebuffer, state);

   for (unsigned i = 0; i < state->nr_cbufs; ++i) {
      agx_legalize_compression(ctx,
                               agx_resource(ctx->framebuffer.cbufs[i].texture),
                               ctx->framebuffer.cbufs[i].format);
   }

   if (state->resolve)
      agx_legalize_compression(ctx, agx_resource(state->resolve),
                               state->resolve->format);

   agx_legalize_compression(ctx, agx_resource(ctx->framebuffer.zsbuf.texture),
                            ctx->framebuffer.zsbuf.format);

   ctx->batch = NULL;
   agx_dirty_all(ctx);
}

/*
 * To write out render targets, each render target surface is bound as a
 * writable shader image, written with the end-of-tile program. This helper
 * constructs the internal pipe_image_view used.
 */
static struct pipe_image_view
image_view_for_surface(const struct pipe_surface *surf)
{
   return (struct pipe_image_view){
      .resource = surf->texture,
      .format = surf->format,
      .access = PIPE_IMAGE_ACCESS_READ_WRITE,
      .shader_access = PIPE_IMAGE_ACCESS_READ_WRITE,
      .u.tex.single_layer_view = surf->first_layer == surf->last_layer,
      .u.tex.first_layer = surf->first_layer,
      .u.tex.last_layer = surf->last_layer,
      .u.tex.level = surf->level,
   };
}

/* The Gen9 path proven by Honeykrisp resolves a 2x/4x colour tile directly
 * into an ordinary single-sample PBE target. Gallium exposes exactly that WSI
 * contract through pipe_framebuffer_state::resolve: the multisample colour
 * attachment is transient and resolve names the image presented by the
 * loader. Keep every other resolve shape on the existing fail-closed path.
 */
static bool
agx_g17p_direct_resolve_surface(struct agx_batch *batch,
                                struct pipe_surface *out)
{
   const struct pipe_framebuffer_state *fb = &batch->key;
   const struct pipe_surface *source = &fb->cbufs[0];
   struct pipe_resource *target = fb->resolve;
   if (!agx_native_graphics(agx_device(batch->ctx->base.screen)) ||
       fb->nr_cbufs != 1 || !source->texture || !target ||
       (util_res_sample_count(source->texture) != 2 &&
        util_res_sample_count(source->texture) != 4) ||
       util_res_sample_count(target) != 1 || source->level != 0 ||
       source->first_layer != 0 || source->last_layer != 0 ||
       (target->target != PIPE_TEXTURE_2D &&
        target->target != PIPE_TEXTURE_RECT) ||
       target->last_level != 0 || target->array_size != 1 ||
       target->depth0 != 1 || target->format != source->format ||
       target->width0 != fb->width || target->height0 != fb->height ||
       source->texture->width0 != fb->width ||
       source->texture->height0 != fb->height ||
       !agx_g17p_rt0_tile_layout_supported(&batch->tilebuffer_layout))
      return false;

   *out = (struct pipe_surface){
      .format = source->format,
      .first_layer = 0,
      .last_layer = 0,
      .level = 0,
      .texture = target,
   };
   return true;
}

/* Similarly, to read render targets, surfaces are bound as textures */
static struct pipe_sampler_view
sampler_view_for_surface(const struct pipe_surface *surf)
{
   bool layered = surf->last_layer > surf->first_layer;

   return (struct pipe_sampler_view){
      /* To reduce shader variants, we always use a 2D texture. For reloads of
       * arrays and cube maps, we map a single layer as a 2D image.
       */
      .target = layered ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D,
      .swizzle_r = PIPE_SWIZZLE_X,
      .swizzle_g = PIPE_SWIZZLE_Y,
      .swizzle_b = PIPE_SWIZZLE_Z,
      .swizzle_a = PIPE_SWIZZLE_W,
      .u.tex =
         {
            .first_layer = surf->first_layer,
            .last_layer = surf->last_layer,
            .first_level = surf->level,
            .last_level = surf->level,
         },
   };
}

static bool
target_is_array(enum pipe_texture_target target)
{
   switch (target) {
   case PIPE_TEXTURE_3D:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_1D_ARRAY:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_CUBE_ARRAY:
      return true;
   default:
      return false;
   }
}

static bool
agx_batch_upload_pbe(struct agx_batch *batch,
                     struct agx_gallium_pbe_descriptor *out,
                     struct pipe_image_view *view, bool block_access,
                     bool arrays_as_2d, bool force_2d_array, bool emrt)
{
   struct agx_device *dev = agx_device(batch->ctx->base.screen);
   const enum agx_descriptor_generation descriptor_generation =
      agx_gallium_descriptor_generation(dev->chip);
   const unsigned descriptor_size =
      agx_gallium_pbe_descriptor_size(descriptor_generation);

   if (!descriptor_size)
      return false;

   if (!agx_gallium_native_pbe_supported(descriptor_generation)) {
      memset(out, 0, descriptor_size);
      return false;
   }

   struct agx_resource *tex = agx_resource(view->resource);
   const struct util_format_description *desc =
      util_format_description(view->format);
   enum pipe_texture_target target = tex->base.target;
   bool is_buffer = (target == PIPE_BUFFER);

   if (!is_buffer && view->u.tex.single_layer_view)
      target = PIPE_TEXTURE_2D;

   arrays_as_2d |= (view->access & PIPE_IMAGE_ACCESS_DRIVER_INTERNAL);

   /* To reduce shader variants, spilled layered render targets are accessed as
    * 2D Arrays regardless of the actual target, so force in that case.
    *
    * Likewise, cubes are accessed as arrays for consistency with NIR.
    */
   if ((arrays_as_2d && target_is_array(target)) || target_is_cube(target) ||
       force_2d_array)
      target = PIPE_TEXTURE_2D_ARRAY;

   unsigned level = is_buffer ? 0 : view->u.tex.level;
   unsigned layer = is_buffer ? 0 : view->u.tex.first_layer;

   /* Build the SEMANTIC descriptor and let the native packer decide how to
    * encode it. The AGX2 packet is a union -- stride aliases levels/layers,
    * and the software addressing sideband aliases the extended word -- so
    * only this producer knows which arm is live, and packing here would
    * throw that away before the AGX3 encoder could check it.
    */
   struct AGX_PBE cfg = {AGX_PBE_header};
   {
      cfg.dimension =
         agx_translate_tex_dim(target, util_res_sample_count(&tex->base));
      cfg.layout = agx_translate_layout(tex->layout.tiling);
      cfg.channels = ail_pixel_format[view->format].channels;
      cfg.type = ail_pixel_format[view->format].type;
      cfg.srgb = util_format_is_srgb(view->format);

      assert(desc->nr_channels >= 1 && desc->nr_channels <= 4);

      for (unsigned i = 0; i < desc->nr_channels; ++i) {
         if (desc->swizzle[i] == 0)
            cfg.swizzle_r = i;
         else if (desc->swizzle[i] == 1)
            cfg.swizzle_g = i;
         else if (desc->swizzle[i] == 2)
            cfg.swizzle_b = i;
         else if (desc->swizzle[i] == 3)
            cfg.swizzle_a = i;
      }

      cfg.buffer = agx_map_texture_gpu(tex, layer);
      cfg.unk_mipmapped = tex->mipmapped;

      if (is_buffer) {
         unsigned size_el =
            agx_texture_buffer_size_el(view->format, view->u.buf.size);

         /* Buffers uniquely have offsets (in bytes, not texels) */
         cfg.buffer += view->u.buf.offset;

         /* Use a 2D texture to increase the maximum size */
         cfg.width = AGX_TEXTURE_BUFFER_WIDTH;
         cfg.height = DIV_ROUND_UP(size_el, cfg.width);
         cfg.level = 0;
         cfg.stride = (cfg.width * util_format_get_blocksize(view->format)) - 4;
         cfg.layers = 1;
         cfg.levels = 1;
      } else if (util_res_sample_count(&tex->base) > 1 && !block_access) {
         /* Multisampled images are bound like buffer textures, with
          * addressing arithmetic to determine the texel to write.
          *
          * Note that the end-of-tile program uses real multisample images with
          * image_write_block instructions.
          */
         unsigned blocksize_B = util_format_get_blocksize(view->format);
         unsigned size_px =
            (tex->layout.size_B - tex->layout.layer_stride_B * layer) /
            blocksize_B;

         cfg.dimension = AGX_TEXTURE_DIMENSION_2D;
         cfg.layout = AGX_LAYOUT_LINEAR;
         cfg.width = AGX_TEXTURE_BUFFER_WIDTH;
         cfg.height = DIV_ROUND_UP(size_px, cfg.width);
         cfg.stride = (cfg.width * blocksize_B) - 4;
         cfg.layers = 1;
         cfg.levels = 1;

         cfg.buffer += tex->layout.level_offsets_B[level];
         cfg.level = 0;
      } else {
         cfg.width = view->resource->width0;
         cfg.height = view->resource->height0;
         cfg.level = level;

         unsigned layers = view->u.tex.last_layer - layer + 1;

         if (tex->layout.tiling == AIL_TILING_LINEAR &&
             (target == PIPE_TEXTURE_1D_ARRAY ||
              target == PIPE_TEXTURE_2D_ARRAY)) {

            cfg.depth_linear = layers;
            cfg.layer_stride_linear = (tex->layout.layer_stride_B - 0x80);
            cfg.extended = true;
         } else {
            assert((tex->layout.tiling != AIL_TILING_LINEAR) || (layers == 1));
            cfg.layers = layers;
         }

         if (tex->layout.tiling == AIL_TILING_LINEAR) {
            cfg.stride = ail_get_linear_stride_B(&tex->layout, level) - 4;
            cfg.levels = 1;
         } else {
            cfg.page_aligned_layers = tex->layout.page_aligned_layers;
            cfg.levels = tex->base.last_level + 1;
         }

         if (tex->base.nr_samples > 1)
            cfg.samples = agx_translate_sample_count(tex->base.nr_samples);
      }

      if (tex->layout.compressed && !emrt) {
         cfg.compressed = true;
         cfg.extended = true;

         cfg.acceleration_buffer =
            agx_map_gpu(tex) + tex->layout.metadata_offset_B +
            (layer * tex->layout.compression_layer_stride_B);
      }

      /* When the descriptor isn't extended architecturally, we can use the last
       * 8 bytes as a sideband. We use it to provide metadata for image atomics.
       *
       * Only libagx_image_texel_address() and libagx_buffer_texel_address()
       * ever read it, and both agx_unpack() the 24-byte AGX2 packet, so
       * neither can read an AGX3 descriptor at all. A block-access image is
       * addressed by the hardware and never consults them, so on AGX3 the
       * sideband would be bits with no Gen9/Gen10 home. Leave it out there;
       * every other AGX3 descriptor that would carry one stays refused by
       * agx_gallium_pack_native_pbe(), which is the honest answer until that
       * ABI is recovered.
       */
      const bool sideband_has_no_reader =
         agx_gallium_descriptor_generation_is_agx3(descriptor_generation);

      if (!cfg.extended && (tex->layout.writeable_image || emrt) &&
          tex->base.target != PIPE_BUFFER && !sideband_has_no_reader) {

         if (util_res_sample_count(&tex->base) > 1) {
            cfg.aligned_width_msaa_sw =
               align(u_minify(view->resource->width0, level),
                     tex->layout.tilesize_el[level].width_el);

            cfg.sample_count_log2_sw = util_logbase2(tex->base.nr_samples);
         } else {
            cfg.level_offset_sw =
               ail_get_level_offset_B(&tex->layout, cfg.level);
         }

         if (tex->layout.tiling != AIL_TILING_LINEAR || emrt) {
            struct ail_tile tile_size = tex->layout.tilesize_el[level];
            cfg.tile_width_sw = tile_size.width_el;
            cfg.tile_height_sw = tile_size.height_el;

            cfg.layer_stride_sw = tex->layout.layer_stride_B;
         }
      }
   }

   const bool packed =
      agx_gallium_pack_native_pbe(descriptor_generation, out, &cfg);
   if (packed && (dev->debug & AGX_DBG_ASYNCTRACE) &&
       util_res_sample_count(&tex->base) > 1) {
      agx_msg("MSAA PBE: format=%s dimension=%u samples=%u address=0x%llx "
              "words: %08x %08x %08x %08x %08x %08x %08x %08x\n",
              util_format_short_name(view->format), cfg.dimension,
              util_res_sample_count(&tex->base), (unsigned long long)cfg.buffer,
              out->opaque[0], out->opaque[1], out->opaque[2], out->opaque[3],
              out->opaque[4], out->opaque[5], out->opaque[6], out->opaque[7]);
   }
   return packed;
}

/* Likewise constant buffers, textures, and samplers are handled in a common
 * per-draw path, with dirty tracking to reduce the costs involved.
 */

static void
agx_set_constant_buffer(struct pipe_context *pctx, mesa_shader_stage shader,
                        uint index, const struct pipe_constant_buffer *cb)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_stage *s = &ctx->stage[shader];
   struct pipe_constant_buffer *constants = &s->cb[index];

   util_copy_constant_buffer(&s->cb[index], cb);

   /* Upload user buffer immediately */
   if (constants->user_buffer && !constants->buffer) {
      u_upload_data_ref(ctx->base.const_uploader, 0, constants->buffer_size, 64,
                        constants->user_buffer, &constants->buffer_offset,
                        &constants->buffer);
   }

   unsigned mask = (1 << index);

   if (cb)
      s->cb_mask |= mask;
   else
      s->cb_mask &= ~mask;

   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_CONST;
}

static void
agx_delete_state(struct pipe_context *ctx, void *state)
{
   FREE(state);
}

/* BOs added to the batch in the uniform upload path */

static void
agx_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
                       const struct pipe_vertex_buffer *buffers)
{
   struct agx_context *ctx = agx_context(pctx);

   util_set_vertex_buffers_mask(ctx->vertex_buffers, &ctx->vb_mask, buffers,
                                count);

   ctx->dirty |= AGX_DIRTY_VERTEX;
}

static void *
agx_create_vertex_elements(struct pipe_context *ctx, unsigned count,
                           const struct pipe_vertex_element *state)
{
   assert(count <= AGX_MAX_ATTRIBS);

   struct agx_vertex_elements *so = calloc(1, sizeof(*so));

   for (unsigned i = 0; i < count; ++i) {
      const struct pipe_vertex_element ve = state[i];

      const struct util_format_description *desc =
         util_format_description(ve.src_format);
      unsigned chan_size = desc->channel[0].size / 8;
      assert((ve.src_offset & (chan_size - 1)) == 0);

      so->buffers[i] = ve.vertex_buffer_index;
      so->src_offsets[i] = ve.src_offset;

      so->key[i] = (struct agx_velem_key){
         .stride = ve.src_stride,
         .format = ve.src_format,
         .divisor = ve.instance_divisor,
         .instanced = ve.instance_divisor > 0,
      };
   }

   return so;
}

static void
agx_bind_vertex_elements_state(struct pipe_context *pctx, void *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->attributes = cso;
   ctx->dirty |= AGX_DIRTY_VERTEX;
}

DERIVE_HASH_TABLE(asahi_vs_shader_key);
DERIVE_HASH_TABLE(asahi_fs_shader_key);
DERIVE_HASH_TABLE(asahi_cs_shader_key);
DERIVE_HASH_TABLE(agx_fast_link_key);

/* Dynamic lowered I/O version of nir_lower_clip_halfz */
static bool
agx_nir_lower_clip_m1_1(nir_builder *b, nir_intrinsic_instr *intr,
                        UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   if (nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_POS)
      return false;

   assert(nir_intrinsic_component(intr) == 0 && "not yet scalarized");
   b->cursor = nir_before_instr(&intr->instr);

   nir_def *pos = intr->src[0].ssa;
   nir_def *z = nir_channel(b, pos, 2);
   nir_def *w = nir_channel(b, pos, 3);
   nir_def *c = nir_load_clip_z_coeff(b);

   /* Lerp. If c = 0, reduces to z. If c = 1/2, reduces to (z + w)/2 */
   nir_def *new_z = nir_ffma(b, z, nir_fsub(b, nir_imm_float(b, 1.0f), c),
                            nir_fmul(b, w, c));
   nir_src_rewrite(&intr->src[0], nir_vector_insert_imm(b, pos, new_z, 2));
   return true;
}

/*
 * To implement point sprites, we'll replace TEX0...7 with point coordinate
 * reads as required. However, the .zw needs to read back 0.0/1.0. This pass
 * fixes up TEX loads of Z and W according to a uniform passed in a sideband,
 * eliminating shader variants.
 */
static bool
agx_nir_lower_point_sprite_zw(nir_builder *b, nir_intrinsic_instr *intr,
                              UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_input &&
       intr->intrinsic != nir_intrinsic_load_interpolated_input)
      return false;

   gl_varying_slot loc = nir_intrinsic_io_semantics(intr).location;
   if (!(loc >= VARYING_SLOT_TEX0 && loc <= VARYING_SLOT_TEX7))
      return false;

   b->cursor = nir_after_instr(&intr->instr);
   unsigned component = nir_intrinsic_component(intr);

   nir_def *mask = nir_load_tex_sprite_mask_agx(b);
   nir_def *location = nir_iadd_imm(b, nir_get_io_offset_src(intr)->ssa,
                                    loc - VARYING_SLOT_TEX0);
   nir_def *bit = nir_ishl(b, nir_imm_intN_t(b, 1, 16), location);
   nir_def *replace = nir_i2b(b, nir_iand(b, mask, bit));

   nir_def *vec = nir_pad_vec4(b, &intr->def);
   nir_def *chans[4] = {NULL, NULL, nir_imm_floatN_t(b, 0.0, vec->bit_size),
                        nir_imm_floatN_t(b, 1.0, vec->bit_size)};

   for (unsigned i = 0; i < 4; ++i) {
      nir_def *chan = nir_channel_or_undef(b, vec, i - component);
      chans[i] = chans[i] ? nir_bcsel(b, replace, chans[i], chan) : chan;
   }

   nir_def *new_vec = nir_vec(b, &chans[component], intr->def.num_components);
   nir_def_rewrite_uses_after(&intr->def, new_vec);
   return true;
}

static struct agx_tilebuffer_layout
agx_gallium_tilebuffer_layout(const struct agx_device *dev,
                              const enum pipe_format *formats,
                              unsigned count, unsigned samples, bool layered)
{
   const bool imageblock = agx_chip_native_imageblock(dev->chip);
   return agx_build_tilebuffer_layout_with_limits(
      formats, count, samples, layered,
      imageblock ? AGX_G17P_MAX_BYTES_PER_SAMPLE : 0,
      imageblock ? 32768 : 0, agx_native_graphics(dev) ? 2048 : 0);
}

/*
 * Compile a NIR shader. The only lowering left at this point is sysvals. The
 * shader key should have already been applied. agx_compile_variant may call
 * this multiple times if there are auxiliary shaders.
 */
static struct agx_compiled_shader *
agx_compile_nir(struct agx_device *dev, nir_shader *nir,
                struct util_debug_callback *debug, mesa_shader_stage stage,
                bool internal_kernel, bool terminal, bool secondary,
                unsigned cf_base, unsigned nr_samples,
                const struct agx_fs_epilog_key *epilog,
                BITSET_WORD *attrib_components_read)
{
   struct agx_compiled_shader *compiled = CALLOC_STRUCT(agx_compiled_shader);
   compiled->stage = stage;
   if (attrib_components_read)
      BITSET_COPY(compiled->attrib_components_read, attrib_components_read);

   struct agx_shader_key key = {
      .dev = agx_gather_device_key(dev),
      .has_scratch = !secondary,
      /* Apple's G17P constant-colour fragment keeps literals in the main
       * program (the byte-exact fs-flat-colour compiler ratchet covers that
       * sequence). A promoted literal instead requires an Ioto direct-uniform
       * range before a fragment with no asynchronous staging; that unwitnessed
       * combination retires without shading on hardware. Keep ordinary UBO
       * uniforms and proven compute/vertex promotion, but materialize fragment
       * literals through the native immediate path. */
      .promote_constants =
         !(agx_native_graphics(dev) && stage == MESA_SHADER_FRAGMENT),
      .no_stop = !terminal,
      .secondary = secondary,
      .g17p_emrt = epilog && epilog->native_spills,
       /* G17P binds the linked executable through a native Ioto program with
        * no executable preshader contract. Keep all stages monolithic,
        * including geometry/tessellation lowered to compute: `stage` still
        * names their original API stage here. */
       .no_preamble = agx_native_graphics(dev),
   };

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* G17P fragment Layer is a flat coefficient, as in Honeykrisp. The
       * compute grid-Z register is not a fragment Layer system value. */
      NIR_PASS(_, nir, agx_nir_lower_interpolation,
               agx_native_graphics(dev));
   }

   /* We always use dynamic sample shading in the GL driver. Indicate that. */
   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       nir->info.fs.uses_sample_shading)
      key.fs.inside_sample_loop = true;

   if (internal_kernel) {
      key.reserved_preamble = 8;
   } else if (!secondary) {
      const enum agx_descriptor_generation descriptor_generation =
         agx_gallium_descriptor_generation(dev->chip);
      const unsigned texture_descriptor_size =
         agx_gallium_texture_descriptor_size(descriptor_generation);
      if (!texture_descriptor_size)
         agx_native_descriptor_rejected(dev, "shader descriptor geometry");

      NIR_PASS(_, nir, agx_nir_lower_sysvals, stage, true,
               texture_descriptor_size);
      /* Every stage but the fragment one reaches the GPU as a compute
       * dispatch or a hardware vertex program on G17P -- geometry and
       * tessellation control run as compute, tessellation evaluation as the
       * vertex shader (keeping its API stage for sysvals) -- and both carry
       * the texture and sampler table pointers in uniform halves 0..7. */
      compiled->g17p_texture_resources =
         agx_native_graphics(dev) && stage != MESA_SHADER_FRAGMENT &&
         (BITSET_LAST_BIT(nir->info.textures_used) != 0 ||
          BITSET_LAST_BIT(nir->info.images_used) != 0);
      const unsigned uniform_base =
         compiled->g17p_texture_resources
            ? AGX_ABI_G17P_TEXTURE_POINTER_HALFS
            : 0;
      /* G17P's Ioto loader moves uniform dwords: lay the ranges out so
       * that each starts at an even half and covers an even count. */
      NIR_PASS(_, nir, agx_nir_layout_uniforms, compiled,
               &key.reserved_preamble, uniform_base,
               agx_native_graphics(dev));
      if (agx_native_graphics(dev))
         NIR_PASS(_, nir, agx_nir_lower_g17p_storage_records,
                  texture_descriptor_size);
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      key.fs.cf_base = cf_base;
      key.fs.nr_samples = nr_samples;
   }

   if (epilog) {
      const struct agx_tilebuffer_layout tib = agx_build_tilebuffer_layout_with_limits(
         epilog->rt_formats, ARRAY_SIZE(epilog->rt_formats), nr_samples, true,
         epilog->max_sample_size_B, epilog->max_bytes_per_tile,
         epilog->max_samples_per_tile);
      for (unsigned rt = 0; rt < ARRAY_SIZE(epilog->rt_formats); ++rt) {
         if (tib.logical_format[rt] != PIPE_FORMAT_NONE && !tib.spilled[rt])
            BITSET_SET(key.g17p_tile_partitions, tib._offset_B[rt]);
      }
      if (BITSET_COUNT(key.g17p_tile_partitions) < 2)
         BITSET_ZERO(key.g17p_tile_partitions);
   }

   if (key.g17p_emrt) {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      nir_metadata_require(impl, nir_metadata_dominance);
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
            if ((op == nir_intrinsic_store_local_pixel_agx ||
                 op == nir_intrinsic_load_local_pixel_agx) &&
                !nir_block_dominates(block, nir_impl_last_block(impl))) {
               key.g17p_emrt = false;
               mesa_loge("asahi: G17P eMRT has conditional resident tile access");
            }
         }
      }
   }

   if (!agx_compile_shader_nir(nir, &key, &compiled->b)) {
      /* Fail closed and say so. agx_compile_shader_nir() zeroes its output on
       * refusal, so what comes back here is an empty shader part with no
       * binary and no BO. Record that on the variant: the draw and dispatch
       * paths must skip it rather than dereference the missing BO.
       *
       * Which instruction was refused is only printed under
       * AGX_MESA_DEBUG=shaders, because only the operand shape makes it
       * actionable; name the stage unconditionally so the gap is visible
       * without a rebuild.
       */
      compiled->failed = true;
      const char *refusal = agx_last_compile_refusal();
      mesa_loge("asahi: the AGX backend cannot encode this %s shader for %s; "
                "draws and dispatches using it will be skipped: %s "
                "(AGX_MESA_DEBUG=shaders names the instruction)",
                _mesa_shader_stage_to_abbrev(stage), agx_chip_name(dev->chip),
                refusal && refusal[0] ? refusal : "no reason recorded");
      return compiled;
   }

   agx2_stats_util_debug(debug, _mesa_shader_stage_to_abbrev(nir->info.stage),
                         &compiled->b.info.stats);

   if (compiled->b.info.binary_size && !secondary) {
      compiled->bo = agx_bo_create(dev, compiled->b.info.binary_size, 0,
                                   AGX_BO_EXEC | AGX_BO_LOW_VA, "Executable");

      memcpy(agx_bo_map(compiled->bo), compiled->b.binary,
             compiled->b.info.binary_size);
   }

   return compiled;
}

static struct agx_compiled_shader *
agx_build_meta_shader_internal(struct agx_context *ctx,
                               meta_shader_builder_t builder, void *data,
                               size_t data_size, bool prolog, bool epilog,
                               unsigned cf_base, bool internal_kernel);

/* Does not take ownership of key. Clones if necessary. */
static struct agx_compiled_shader *
agx_compile_variant(struct agx_device *dev, struct pipe_context *pctx,
                    struct agx_uncompiled_shader *so,
                    union asahi_shader_key *key_)
{
   struct blob_reader reader;
   blob_reader_init(&reader, so->serialized_nir.data, so->serialized_nir.size);
   nir_shader *nir = nir_deserialize(NULL, &agx_nir_options, &reader);

   /* Auxiliary programs */
   struct poly_gs_info gs_info = {0};
   uint64_t outputs = 0;
   struct agx_fs_epilog_link_info epilog_key = {false};
   nir_shader *gs_count = NULL;
   nir_shader *gs_copy = NULL;
   nir_shader *pre_gs = NULL;
   BITSET_DECLARE(attrib_components_read, VERT_ATTRIB_MAX * 4) = {0};

   /* This can happen at inopportune times and cause jank, log it */
   perf_debug(dev, "Compiling %s shader variant #%u",
              _mesa_shader_stage_to_abbrev(so->type),
              _mesa_hash_table_num_entries(so->variants));

   const bool custom_borders =
      agx_native_graphics(dev) &&
      ((so->type == MESA_SHADER_FRAGMENT && key_->fs.custom_borders) ||
       ((so->type == MESA_SHADER_VERTEX ||
         so->type == MESA_SHADER_TESS_EVAL) &&
        key_->vs.custom_borders) ||
       (so->type == MESA_SHADER_COMPUTE && key_->cs.custom_borders));
   if (custom_borders)
      NIR_PASS(_, nir, agx_nir_lower_custom_border_g17p,
               agx_shadow_gather_custom_border(dev->chip));

   struct agx_unlinked_uvs_layout uvs = {0};
   bool translucent = false;

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      struct asahi_vs_shader_key *key = &key_->vs;

      if (nir->info.vs.tes_poly) {
         NIR_PASS(_, nir, poly_nir_lower_tes, key->hw);
      } else {
         NIR_PASS(_, nir, agx_nir_gather_vs_inputs, attrib_components_read);
         NIR_PASS(_, nir, agx_nir_lower_vs_input_to_prolog);
      }

      if (key->hw) {
         NIR_PASS(_, nir, agx_nir_lower_point_size, true);
         NIR_PASS(_, nir, nir_shader_intrinsics_pass, agx_nir_lower_clip_m1_1,
                  nir_metadata_control_flow, NULL);

         NIR_PASS(_, nir, nir_lower_io_to_scalar, nir_var_shader_out, NULL,
                  NULL);
         NIR_PASS(_, nir, agx_nir_lower_cull_distance_vs);
         NIR_PASS(_, nir, agx_nir_lower_uvs, &uvs,
                  agx_native_graphics(dev));
      } else {
         NIR_PASS(_, nir, poly_nir_lower_vs_before_gs);

         /* Turn into a compute shader now that we're free of vertexisms */
         nir->info.stage = MESA_SHADER_COMPUTE;
         memset(&nir->info.cs, 0, sizeof(nir->info.cs));
         nir->xfb_info = NULL;
         outputs = nir->info.outputs_written;
      }
   } else if (nir->info.stage == MESA_SHADER_TESS_CTRL) {
      /* The dispatch uses one workgroup per patch and one invocation per
       * output vertex. In particular, nir_create_passthrough_tcs() inherits
       * the builder's default 1x1x1 size; leaving that in place lets poly
       * fold every invocation ID to zero, so only control point zero is
       * copied. Declare the same shape that agx_draw_patches() launches. */
      nir->info.workgroup_size[0] = nir->info.tess.tcs_vertices_out;
      nir->info.workgroup_size[1] = nir->info.workgroup_size[2] = 1;
      NIR_PASS(_, nir, poly_nir_lower_tcs, true);
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      NIR_PASS(_, nir, poly_nir_lower_gs, &gs_count, &gs_copy, &pre_gs,
               &gs_info);

      agx_preprocess_nir(gs_count);
      agx_preprocess_nir(gs_copy);
      agx_preprocess_nir(pre_gs);
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      struct asahi_fs_shader_key *key = &key_->fs;

      const bool native_msaa_fbfetch =
         agx_native_graphics(dev) && key->nr_samples > 1 &&
         nir->info.fs.uses_fbfetch_output;
      if (native_msaa_fbfetch)
         nir->info.fs.uses_sample_shading = true;

      /* Discards must be lowering before lowering MSAA to handle discards */
      NIR_PASS(_, nir, agx_nir_lower_discard_zs_emit);

      /* G17P has no recovered samples-log2 special register, and the variant
       * key already carries the raster sample count, so fold it into the
       * shader as hk_shader.c does. gl_SampleMaskIn, gl_SampleID and
       * interpolateAtSample() all read it. */
      if (agx_native_graphics(dev) && agx_nir_needs_sample_count(nir))
         NIR_PASS(_, nir, agx_nir_specialize_sample_count,
                  MAX2(key->nr_samples, 1));

      /* At one rasterization sample, per-sample and per-pixel invocation are
       * the same thing, so collapsing sample shading to per-pixel is exact --
       * and it is what has to happen here, because G17P has no sample loop
       * for the linker to splice and asking for one would drop the draw
       * outright. hk_shader.c does the same on the Vulkan side. Do it before
       * the epilog split, so the link key records the collapse too. */
      if (agx_native_graphics(dev) && key->nr_samples <= 1 &&
          nir->info.fs.uses_sample_shading) {
         NIR_PASS(_, nir, agx_nir_lower_g17p_1x_sample_shading);
      }

      /* AGX3 has no equivalent of the AGX2 machine code agx_fast_link()
       * splices around a linked fragment program to run it once per sample.
       * Apple's own compiler unrolls the body instead, once per sample, in one
       * program.
       *
       * An unrolled main cannot carry a separate epilog: the stores the split
       * moves out -- colour, and depth/stencil, which it defers past discards
       * -- would run once for a body that ran per sample. So keep the whole
       * fragment in one program. Only the shape with nothing but depth and
       * stencil to keep is taken here, which is what a multisample depth blit
       * needs; a per-sample colour shader would also need its tilebuffer and
       * blending built into the main, and keeps its existing named refusal.
       */
      const uint64_t zs_outputs = BITFIELD64_BIT(FRAG_RESULT_DEPTH) |
                                  BITFIELD64_BIT(FRAG_RESULT_STENCIL);
      const bool unrolled_samples =
         agx_native_graphics(dev) && key->nr_samples > 1 &&
         nir->info.fs.uses_sample_shading &&
         !(nir->info.outputs_written & ~zs_outputs);

      if (!unrolled_samples) {
         NIR_PASS(_, nir, agx_nir_lower_fs_output_to_epilog_for_device,
                  dev->chip, &epilog_key);
      } else {
         /* The stores stay here, but the draw-time state still has to know
          * the fragment writes them. */
         epilog_key.write_z =
            !!(nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH));
         epilog_key.write_s =
            !!(nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL));
      }

      if (nir->info.fs.uses_fbfetch_output) {
         struct agx_tilebuffer_layout tib = agx_gallium_tilebuffer_layout(
            dev, key->rt_formats, ARRAY_SIZE(key->rt_formats), key->nr_samples,
            true);

         if (dev->debug & AGX_DBG_SMALLTILE)
            tib.tile_size = 16 * 16;

         /* XXX: don't replicate this all over the driver */
         unsigned rt_spill_base = BITSET_LAST_BIT(nir->info.textures_used) +
                                  (2 * BITSET_LAST_BIT(nir->info.images_used));
         unsigned rt_spill = rt_spill_base;
         NIR_PASS(_, nir, agx_nir_lower_tilebuffer, &tib, NULL, &rt_spill, NULL,
                  &translucent, false, agx_native_graphics(dev));
      }

      if (unrolled_samples) {
         NIR_PASS(_, nir, agx_nir_lower_monolithic_msaa, key->nr_samples);

         /* Nothing is left for the linker to wrap. */
         nir->info.fs.uses_sample_shading = false;
         epilog_key.sample_shading = false;
      } else if (nir->info.fs.uses_sample_shading &&
                 agx_native_graphics(dev)) {
         /* Use the same export layout and static sample copies as Vulkan.
          * The epilog consumes each sample's exports in the matching order.
          */
         NIR_PASS(_, nir, agx_nir_lower_sample_mask_deferred);
         if (!agx_nir_lower_g17p_sample_shading(nir, &epilog_key,
                                                key->nr_samples)) {
            mesa_loge("asahi: G17P per-sample colour exports exceed the register file");
            struct agx_compiled_shader *failed = CALLOC_STRUCT(agx_compiled_shader);
            failed->stage = so->type;
            failed->so = so;
            failed->failed = true;
            ralloc_free(nir);
            return failed;
         }
      } else if (nir->info.fs.uses_sample_shading) {
         /* Ensure the sample ID is preserved in register */
         nir_builder b =
            nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(nir)));
         nir_export_agx(
            &b,
            nir_load_exported_agx(&b, 1, 16, .base = AGX_ABI_FIN_SAMPLE_MASK),
            .base = AGX_ABI_FOUT_SAMPLE_MASK);

         NIR_PASS(_, nir, agx_nir_lower_to_per_sample);
      }

      if (native_msaa_fbfetch) {
         NIR_PASS(_, nir, nir_opt_constant_folding);
         NIR_PASS(_, nir, agx_nir_lower_g17p_tile_samples, key->nr_samples);
      }

      NIR_PASS(_, nir, agx_nir_lower_sample_mask);
      NIR_PASS(_, nir, agx_nir_lower_fs_active_samples_to_register);
   }

   /* The Gen9 form addresses through the storage tail record that
    * agx_g17p_fill_storage_tail() writes. */
   NIR_PASS(_, nir, agx_nir_lower_multisampled_image_store_for_generation,
            agx_gallium_descriptor_generation(dev->chip));

   struct agx_compiled_shader *compiled = agx_compile_nir(
      dev, nir, &pctx->debug, so->type, false, so->type != MESA_SHADER_FRAGMENT,
      false, 0, so->type == MESA_SHADER_FRAGMENT ? key_->fs.nr_samples : 0,
      NULL, attrib_components_read);
   compiled->g17p_custom_border_emulation = custom_borders;

   if (so->type == MESA_SHADER_FRAGMENT) {
      /* XXX: don't replicate this all over the driver */
      epilog_key.rt_spill_base =
         BITSET_LAST_BIT(nir->info.textures_used) +
         (so->info.image_records * BITSET_LAST_BIT(nir->info.images_used));

      compiled->epilog_key = epilog_key;
      compiled->b.info.reads_tib |= translucent;
   }

   compiled->so = so;
   compiled->uvs = uvs;

   /* Compile auxiliary programs */
   if (gs_count) {
      compiled->gs_count = agx_compile_nir(
         dev, gs_count, &pctx->debug, so->type, false, true, false, 0, 0, NULL, NULL);
      compiled->gs_count->so = so;
   }

   if (pre_gs) {
      compiled->pre_gs =
         agx_compile_nir(dev, pre_gs, &pctx->debug, MESA_SHADER_COMPUTE, false,
                         true, false, 0, 0, NULL, NULL);
   }

   if (gs_copy) {
      /* Replace the point size write if present, but do not insert a write:
       * the GS rast program writes point size iff we have points.
       */
      NIR_PASS(_, gs_copy, agx_nir_lower_point_size, false);

      NIR_PASS(_, gs_copy, nir_shader_intrinsics_pass, agx_nir_lower_clip_m1_1,
               nir_metadata_control_flow, NULL);

      NIR_PASS(_, gs_copy, nir_lower_io_to_scalar, nir_var_shader_out, NULL,
               NULL);
      NIR_PASS(_, gs_copy, agx_nir_lower_cull_distance_vs);

      struct agx_unlinked_uvs_layout uvs = {0};
      NIR_PASS(_, gs_copy, agx_nir_lower_uvs, &uvs,
               agx_native_graphics(dev));

      compiled->gs_copy =
         agx_compile_nir(dev, gs_copy, &pctx->debug, MESA_SHADER_GEOMETRY,
                         false, true, false, 0, 0, NULL, NULL);
      compiled->gs_copy->so = so;
      compiled->gs_copy->stage = so->type;
      compiled->gs_copy->uvs = uvs;
   }

   compiled->gs = gs_info;
   compiled->b.info.outputs = outputs;

   /* An auxiliary program is as load-bearing as the main one. A geometry
    * variant whose count, pre-GS or copy program was refused cannot run, so
    * the variant as a whole is unusable.
    */
   if ((compiled->gs_count && compiled->gs_count->failed) ||
       (compiled->pre_gs && compiled->pre_gs->failed) ||
       (compiled->gs_copy && compiled->gs_copy->failed))
      compiled->failed = true;

   ralloc_free(nir);
   ralloc_free(pre_gs);
   ralloc_free(gs_count);
   return compiled;
}

static struct agx_compiled_shader *
agx_get_shader_variant(struct agx_screen *screen, struct pipe_context *pctx,
                       struct agx_uncompiled_shader *so,
                       union asahi_shader_key *key)
{
   struct agx_compiled_shader *compiled =
      agx_disk_cache_retrieve(screen, so, key);

   if (!compiled) {
      compiled = agx_compile_variant(&screen->dev, pctx, so, key);

      /* The serialized form carries only the binary and its info, so a
       * refused variant would come back from the cache indistinguishable
       * from a legitimately empty one -- and silently, on a later run. Keep
       * refusals out of it; they are cheap to re-derive and must stay
       * visible.
       */
      if (!compiled->failed)
         agx_disk_cache_store(screen->disk_cache, so, key, compiled);
   }

   /* key may be destroyed after we return, so clone it before using it as a
    * hash table key. The clone is logically owned by the hash table.
    */
   union asahi_shader_key *cloned_key =
      rzalloc(so->variants, union asahi_shader_key);

   if (so->type == MESA_SHADER_FRAGMENT) {
      memcpy(cloned_key, key, sizeof(struct asahi_fs_shader_key));
   } else if (so->type == MESA_SHADER_VERTEX ||
              so->type == MESA_SHADER_TESS_EVAL) {
      memcpy(cloned_key, key, sizeof(struct asahi_vs_shader_key));
   } else if (so->type == MESA_SHADER_COMPUTE) {
      memcpy(cloned_key, key, sizeof(struct asahi_cs_shader_key));
   } else {
      /* No key */
   }

   _mesa_hash_table_insert(so->variants, cloned_key, compiled);

   return compiled;
}

/* Forward declared for the precompile-error path below. */
static void agx_delete_uncompiled_shader(struct agx_device *dev,
                                         struct agx_uncompiled_shader *so);

/*
 * st/mesa asks drivers to report failures from the default variant while it
 * is linking a GLSL program. The AGX compiler is deliberately fail-closed,
 * but merely retaining a failed variant here makes glLinkProgram() succeed
 * and postpones the error until the first draw is dropped. WebGL treats a
 * successful link as an executable program, so surface the refusal through
 * pipe_shader_state::error_message and return no CSO when requested.
 *
 * Non-link-time users keep the failed CSO. The draw/dispatch guards still
 * consume it safely and this preserves the existing behaviour for internal
 * shaders and late key-specific variants.
 */
static bool
agx_precompile_variant(struct agx_device *dev, struct pipe_context *pctx,
                       struct agx_uncompiled_shader *so,
                       const struct pipe_shader_state *cso,
                       union asahi_shader_key *key)
{
   struct agx_compiled_shader *compiled =
      agx_get_shader_variant(agx_screen(pctx->screen), pctx, so, key);

   if (!cso->report_compile_error || (compiled && !compiled->failed))
      return true;

   char message[192];
   snprintf(message, sizeof(message),
            "asahi: the AGX backend cannot encode the default %s shader "
            "variant for %s",
            _mesa_shader_stage_to_string(so->type), agx_chip_name(dev->chip));
   ((struct pipe_shader_state *)cso)->error_message = strdup(message);

   agx_delete_uncompiled_shader(dev, so);
   return false;
}

static unsigned
glsl_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static bool
should_lower_robustness(const nir_intrinsic_instr *intr, const void *data)
{
   const bool *gl_robust = data;

   switch (intr->intrinsic) {
   /* The texture/PBE hardware is robust, but our buffer image implementation
    * is not. Lower robustness only for buffer images.
    */
   case nir_intrinsic_image_load:
   case nir_intrinsic_image_store:
      return nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF;

   /* Image atomics are lowered to raw memory access */
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
      return true;

   /* UBOs/SSBOs are lowered to raw pointers */
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
      return *gl_robust;

   default:
      return false;
   }
}

static nir_def *
agx_buffer_texture_metadata(nir_builder *b, nir_tex_instr *tex)
{
   const int src = nir_tex_instr_src_index(tex, nir_tex_src_texture_handle);
   assert(src >= 0);
   nir_intrinsic_instr *handle = nir_src_as_intrinsic(tex->src[src].src);
   assert(handle && handle->intrinsic == nir_intrinsic_load_texture_handle_agx);
   mesa_shader_stage stage = b->shader->info.stage;
   if (stage == MESA_SHADER_VERTEX && b->shader->info.vs.tes_poly)
      stage = MESA_SHADER_TESS_EVAL;
   nir_def *base = nir_load_sysval_agx(
      b, 1, 64, .desc_set = AGX_SYSVAL_STAGE(stage),
      .binding = offsetof(struct agx_stage_uniforms, buffer_texture_base));
   return nir_iadd(b, base, nir_u2u64(b, nir_imul_imm(
      b, nir_u2u32(b, handle->src[0].ssa), AGX_G17P_EMRT_DESCRIPTOR_SIZE)));
}

static void
agx_shader_initialize(struct agx_device *dev, struct agx_uncompiled_shader *so,
                      nir_shader *nir, bool support_lod_bias, bool robust)
{
   if (nir->info.stage == MESA_SHADER_KERNEL)
      nir->info.stage = MESA_SHADER_COMPUTE;

   blob_init(&so->early_serialized_nir);
   nir_serialize(&so->early_serialized_nir, nir, true);

   /* We need to lower robustness before bindings, since robustness lowering
    * affects the bindings used.
    */
   NIR_PASS(_, nir, nir_lower_robust_access, should_lower_robustness, &robust);

   /* Similarly, we need to do early texture lowering before bindings. */
   if (agx_native_graphics(dev))
      NIR_PASS(_, nir, agx_nir_lower_g17p_texture_early, support_lod_bias,
               agx_chip_sampler_has_lod_bias(dev->chip));
   else
      NIR_PASS(_, nir, agx_nir_lower_texture_early, support_lod_bias);

   /* We need to lower binding tables before calling agx_preprocess_nir, since
    * that does texture lowering that needs to know the binding model.
    */
   if (agx_native_graphics(dev)) {
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_tex)
                  continue;

               nir_tex_instr *tex = nir_instr_as_tex(instr);
               so->info.uses_buffer_textures |=
                  tex->sampler_dim == GLSL_SAMPLER_DIM_BUF;
               if (tex->op == nir_texop_lod) {
                  /* G17P's native LOD query takes implicit derivatives and
                   * the sampler's descriptor bias, not a shader bias operand.
                   * The early pass above added only the API sampler bias;
                   * remove it here and bind a biased companion below. Keep
                   * ordinary sampling's software bias to avoid accumulating
                   * descriptor and sample-level quantization errors. */
                  nir_steal_tex_src(tex, nir_tex_src_bias);
                  so->info.uses_query_lod = true;
               }
            }
         }
      }
   }

   struct agx_lower_bindings_options binding_options = {
      .uses_bindless_samplers = &so->uses_bindless_samplers,
      .g17p_plain_txf_crawl = agx_native_graphics(dev),
      .g17p_native_sampler_handles = agx_native_graphics(dev),
      .g17p_bindless_storage = agx_native_graphics(dev),
      .image_records = agx_native_graphics(dev) ? 3 : 2,
      .bindful_sampler_count =
         agx_native_graphics(dev)
            ? AGX_GALLIUM_G17P_IMMEDIATE_API_SAMPLERS
            : 16,
   };
   NIR_PASS(_, nir, agx_nir_lower_bindings, &binding_options);
   so->info.image_records = binding_options.image_records;

   /* We need to do some I/O lowering before lowering textures */
   so->info.nr_bindful_textures = BITSET_LAST_BIT(nir->info.textures_used);
   so->info.nr_bindful_images = BITSET_LAST_BIT(nir->info.images_used);

   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            glsl_type_size,
            nir_lower_io_lower_64bit_to_32 |
               nir_lower_io_use_interpolated_input_intrinsics);

   /* Regather shader info after nir_lower_io. This recalculates interpolation
    * qualifiers which got lost when mesa/st lowered I/O back to vars.
    */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      so->info.uses_fbfetch = nir->info.fs.uses_fbfetch_output;
      so->info.uses_sample_shading = nir->info.fs.uses_sample_shading;
      so->info.writes_memory = nir->info.writes_memory;
      so->info.inputs_linear_shaded = nir->info.linear_varyings;
      so->info.inputs_flat_shaded = nir->info.inputs_read &
                                    ~nir->info.linear_varyings &
                                    ~nir->info.perspective_varyings;

      /* Interpolate varyings at fp16 and write to the tilebuffer at fp16. As an
       * exception, interpolate flat shaded at fp32. This works around a
       * hardware limitation. The resulting code (with an extra f2f16 at the end
       * if needed) matches what Metal produces.
       */
      if (likely(!(dev->debug & AGX_DBG_NO16))) {
         uint64_t texcoord = agx_gather_texcoords(nir);

         NIR_PASS(_, nir, nir_lower_mediump_io,
                  nir_var_shader_in | nir_var_shader_out,
                  ~(so->info.inputs_flat_shaded | texcoord), false);
      }
   } else if (nir->info.stage == MESA_SHADER_VERTEX ||
              nir->info.stage == MESA_SHADER_TESS_EVAL) {
      so->info.has_edgeflags = nir->info.outputs_written & VARYING_BIT_EDGE;
      so->info.cull_distance_size = nir->info.cull_distance_array_size;
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      so->info.cull_distance_size = nir->info.cull_distance_array_size;
   }

   /* Shrink and vectorize SSBOs before lowering them, since it is harder to
    * optimize the lowered code.
    */
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, agx_nir_cleanup_amul);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_cse);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_opt_shrink_vectors, true);
   NIR_PASS(_, nir, nir_opt_copy_prop);

   NIR_PASS(
      _, nir, nir_opt_load_store_vectorize,
      &(const nir_load_store_vectorize_options){
         .modes = nir_var_mem_global | nir_var_mem_constant | nir_var_mem_ssbo,
         .callback = agx_mem_vectorize_cb,
      });

   const enum agx_descriptor_generation descriptor_generation =
      agx_gallium_descriptor_generation(dev->chip);
   if (agx_native_graphics(dev))
      NIR_PASS(_, nir, agx_nir_lower_texture_with_buffer_metadata_cb,
               descriptor_generation, agx_buffer_texture_metadata);
   else
      NIR_PASS(_, nir, agx_nir_lower_texture_for_generation,
               descriptor_generation);
   if (agx_native_graphics(dev))
      NIR_PASS(_, nir, agx_nir_rebind_g17p_texture_crawls);
   NIR_PASS(_, nir, nir_lower_ssbo, NULL);

   agx_preprocess_nir(nir);

   /* The native stack protocol only encodes whole words; Honeykrisp gets
    * this from agx_preprocess_nir_g17p(). */
   if (agx_native_graphics(dev))
      agx3_nir_lower_scratch_words(nir);

   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       (nir->info.inputs_read & VARYING_BITS_TEX_ANY)) {

      NIR_PASS(_, nir, nir_shader_intrinsics_pass,
               agx_nir_lower_point_sprite_zw, nir_metadata_control_flow, NULL);
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, agx_nir_lower_sample_intrinsics, true);
      so->info.uses_sample_shading = nir->info.fs.uses_sample_shading;
      so->info.needs_sample_count = agx_native_graphics(dev) &&
                                    agx_nir_needs_sample_count(nir);
   }

   so->type = nir->info.stage;

   if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      nir->info.stage = MESA_SHADER_VERTEX;
      nir->info.vs.tes_poly = true;
   }

   blob_init(&so->serialized_nir);
   nir_serialize(&so->serialized_nir, nir, true);
   _mesa_blake3_compute(so->serialized_nir.data, so->serialized_nir.size,
                      so->nir_blake3);

   so->has_xfb_info = (nir->xfb_info != NULL);

   static_assert(
      ARRAY_SIZE(so->xfb_strides) == ARRAY_SIZE(nir->info.xfb_stride),
      "known target count");

   if (so->has_xfb_info) {
      struct nir_xfb_info *xfb = nir->xfb_info;

      for (unsigned i = 0; i < ARRAY_SIZE(so->xfb_strides); ++i) {
         so->xfb_strides[i] = xfb->buffers[i].stride;
      }
   }
}

static void *
agx_create_shader_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_uncompiled_shader *so =
      rzalloc(NULL, struct agx_uncompiled_shader);
   struct agx_device *dev = agx_device(pctx->screen);

   if (!so)
      return NULL;

   so->base = *cso;

   nir_shader *nir = cso->type == PIPE_SHADER_IR_NIR
                        ? cso->ir.nir
                        : tgsi_to_nir(cso->tokens, pctx->screen, false);

   if (nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_TESS_EVAL) {
      so->variants = asahi_vs_shader_key_table_create(so);
      so->linked_shaders = agx_fast_link_key_table_create(so);
   } else if (nir->info.stage == MESA_SHADER_TESS_CTRL ||
              nir->info.stage == MESA_SHADER_GEOMETRY) {
      /* No variants */
      so->variants = _mesa_hash_table_create(NULL, asahi_cs_shader_key_hash,
                                             asahi_cs_shader_key_equal);

      /* The geometry copy program is a hardware vertex program and on G17P
       * it links behind a vertex prolog like one, see
       * agx_update_gs_copy_link().
       */
      if (nir->info.stage == MESA_SHADER_GEOMETRY)
         so->linked_shaders = agx_fast_link_key_table_create(so);
   } else {
      so->variants = asahi_fs_shader_key_table_create(so);
      so->linked_shaders = agx_fast_link_key_table_create(so);
   }

   if (nir->info.stage == MESA_SHADER_TESS_EVAL ||
       nir->info.stage == MESA_SHADER_TESS_CTRL) {

      so->tess.ccw = nir->info.tess.ccw;
      so->tess.point_mode = nir->info.tess.point_mode;
      so->tess.spacing = nir->info.tess.spacing;
      so->tess.output_patch_size = nir->info.tess.tcs_vertices_out;
      so->tess.primitive = nir->info.tess._primitive_mode;
      so->tess.per_vertex_outputs = poly_tcs_per_vertex_outputs(nir);
      so->tess.nr_patch_outputs =
         util_last_bit(nir->info.patch_outputs_written);
      if (nir->info.stage == MESA_SHADER_TESS_CTRL)
         so->tess.output_stride = poly_tcs_output_stride(nir);
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      so->gs_mode = nir->info.gs.output_primitive;
   }

   agx_shader_initialize(dev, so, nir, ctx->support_lod_bias, ctx->robust);
   mesa_shader_stage next_stage = nir->info.next_stage;

   /* We're done with the NIR, throw it away */
   ralloc_free(nir);
   nir = NULL;

   /* Precompile shaders that have a small key. For shader-db, precompile a
    * shader with a default key. This could be improved but hopefully this is
    * acceptable for now.
    */
   if ((so->type == MESA_SHADER_TESS_CTRL) ||
       (so->type == MESA_SHADER_GEOMETRY) ||
       (so->type == MESA_SHADER_FRAGMENT && !so->info.uses_fbfetch)) {
      union asahi_shader_key key = {0};
      if (!agx_precompile_variant(dev, pctx, so, cso, &key))
         return NULL;
   } else if (so->type == MESA_SHADER_VERTEX) {
      union asahi_shader_key key = {
         .vs.hw = next_stage == MESA_SHADER_FRAGMENT,
      };
      if (!agx_precompile_variant(dev, pctx, so, cso, &key))
         return NULL;

      if (next_stage == MESA_SHADER_NONE) {
         key.vs.hw = true;
         if (!agx_precompile_variant(dev, pctx, so, cso, &key))
            return NULL;
      }
   } else if (dev->debug & AGX_DBG_PRECOMPILE) {
      union asahi_shader_key key = {0};

      switch (so->type) {
      case MESA_SHADER_TESS_EVAL:
         /* TODO: Tessellation shaders with shader-db */
         return so;

      case MESA_SHADER_FRAGMENT:
         key.fs.nr_samples = 1;

         /* For fbfetch */
         for (unsigned i = 0; i < ARRAY_SIZE(key.fs.rt_formats); ++i) {
            key.fs.rt_formats[i] = PIPE_FORMAT_R8G8B8A8_UNORM;
         }
         break;
      default:
         UNREACHABLE("Unknown shader stage in shader-db precompile");
      }

      agx_compile_variant(dev, pctx, so, &key);
   }

   return so;
}

static void *
agx_create_compute_state(struct pipe_context *pctx,
                         const struct pipe_compute_state *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_device *dev = agx_device(pctx->screen);
   struct agx_uncompiled_shader *so =
      rzalloc(NULL, struct agx_uncompiled_shader);

   if (!so)
      return NULL;

   so->variants = _mesa_hash_table_create(so, asahi_cs_shader_key_hash,
                                          asahi_cs_shader_key_equal);

   union asahi_shader_key key = {0};

   assert(cso->ir_type == PIPE_SHADER_IR_NIR && "TGSI kernels unsupported");
   nir_shader *nir = (void *)cso->prog;

   agx_shader_initialize(dev, so, nir, ctx->support_lod_bias, ctx->robust);
   agx_get_shader_variant(agx_screen(pctx->screen), pctx, so, &key);

   /* We're done with the NIR, throw it away */
   ralloc_free(nir);
   return so;
}

static void
agx_get_compute_state_info(struct pipe_context *pctx, void *cso,
                           struct pipe_compute_state_object_info *info)
{
   union asahi_shader_key key = {0};
   struct agx_compiled_shader *so =
      agx_get_shader_variant(agx_screen(pctx->screen), pctx, cso, &key);

   info->max_threads =
      agx_occupancy_for_register_count(so->b.info.nr_gprs).max_threads;
   info->private_memory = 0;
   info->preferred_simd_size = 32;
   info->simd_sizes = 32;
}

/* Does not take ownership of key. Clones if necessary. */
static bool
agx_update_shader(struct agx_context *ctx, struct agx_compiled_shader **out,
                  mesa_shader_stage stage, union asahi_shader_key *key)
{
   struct agx_uncompiled_shader *so = ctx->stage[stage].shader;
   assert(so != NULL);

   struct hash_entry *he = _mesa_hash_table_search(so->variants, key);

   if (he) {
      if ((*out) == he->data)
         return false;

      *out = he->data;
      return true;
   }

   struct agx_screen *screen = agx_screen(ctx->base.screen);
   *out = agx_get_shader_variant(screen, &ctx->base, so, key);
   return true;
}

/*
 * Whether a bound program can actually be issued.
 *
 * agx_compile_shader_nir() is fail-closed: a shape it cannot encode comes
 * back as a zeroed agx_shader_part, so the variant has an empty binary and no
 * executable BO. Consuming that is a null dereference inside a draw, which
 * for a compositor loses the whole session rather than one frame. A refused
 * program must therefore drop the work that needs it.
 *
 * `bo == NULL` is deliberately not the test. A fragment main whose entire
 * body lives in the epilog has a legitimately empty binary, and prologs,
 * epilogs and every other secondary part never own a BO at all.
 */
static bool
agx_program_usable(const struct agx_compiled_shader *cs)
{
   return cs != NULL && !cs->failed;
}

/*
 * Drop a draw or dispatch whose program is unusable, and say so once per
 * program rather than once per draw -- a refused shader in a compositor is
 * hit every frame.
 */
static bool
agx_drop_for_unusable_program(struct agx_context *ctx,
                              struct agx_compiled_shader *cs, bool linked,
                              const char *stage)
{
   if (agx_program_usable(cs) && linked)
      return false;

   ctx->skipped_no_program++;

   if (cs == NULL || !cs->refusal_reported) {
      if (cs != NULL)
         cs->refusal_reported = true;

      mesa_loge("asahi: dropping work: the %s program is unusable (%s). This "
                "is a driver gap; run with AGX_MESA_DEBUG=shaders to see the "
                "instruction the backend refused.",
                stage,
                cs == NULL     ? "no compiled variant"
                : cs->failed   ? "the backend refused to encode it"
                               : "it could not be linked");
   }

   return true;
}

/*
 * Refuse a draw whose G17P program address came back null.
 *
 * agx_build_g17p_graphics_program() is fail-closed and returns 0 from a dozen
 * places -- an unlinked shader, a preamble it cannot place, a sampling stage
 * whose texture/sampler pointer pair is missing, a program that overflows its
 * pool.  Every one of those used to be written straight into
 * agx_vdm_state_update_info.pipeline and submitted, so the shape of the bug on
 * hardware was a GPU fault at address 0 with nothing in the log connecting it
 * to the shader that caused it.  One such gap (a compiled-shader property lost
 * across the disk cache) took a hardware cycle to find for exactly that reason.
 *
 * Drop the work instead, and say which stage once per program rather than once
 * per draw -- a refused shader in a compositor is hit every frame.
 */
static bool
agx_drop_for_null_g17p_program(struct agx_context *ctx,
                               struct agx_compiled_shader *cs,
                               uint64_t program, const char *stage)
{
   if (program)
      return false;

   ctx->skipped_no_program++;

   if (cs == NULL || !cs->refusal_reported) {
      if (cs != NULL)
         cs->refusal_reported = true;

      mesa_loge("asahi: dropping work: no G17P %s program could be built for "
                "this draw. This is a driver gap; submitting a null program "
                "address faults the GPU.",
                stage);
   }

   return true;
}

static enum mesa_prim
rast_prim(enum mesa_prim mode, unsigned fill_mode)
{
   if (u_reduced_prim(mode) == MESA_PRIM_TRIANGLES) {
      if (fill_mode == PIPE_POLYGON_MODE_POINT)
         return MESA_PRIM_POINTS;
      else if (fill_mode == PIPE_POLYGON_MODE_LINE)
         return MESA_PRIM_LINES;
   }

   return mode;
}

static bool
lower_fs_prolog_abi(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *_)
{
   if (intr->intrinsic != nir_intrinsic_load_polygon_stipple_agx &&
       intr->intrinsic != nir_intrinsic_load_stat_query_address_poly)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *root = nir_load_preamble(b, 1, 64, .base = AGX_ABI_FUNI_ROOT);
   nir_def *repl;

   if (intr->intrinsic == nir_intrinsic_load_polygon_stipple_agx) {
      off_t stipple_offs = offsetof(struct agx_draw_uniforms, polygon_stipple);
      nir_def *stipple_ptr_ptr = nir_iadd_imm(b, root, stipple_offs);
      nir_def *base =
         nir_load_global_constant(b, 1, 64, stipple_ptr_ptr, .align_mul = 4);

      nir_def *row = intr->src[0].ssa;
      nir_def *addr = nir_iadd(b, base, nir_u2u64(b, nir_imul_imm(b, row, 4)));

      repl = nir_load_global_constant(b, 1, 32, addr);
   } else {
      off_t offs = offsetof(struct agx_draw_uniforms,
                            pipeline_statistics[nir_intrinsic_base(intr)]);
      repl = nir_load_global_constant(b, 1, 64, nir_iadd_imm(b, root, offs),
                                      .align_mul = 4);
   }

   nir_def_replace(&intr->def, repl);
   return true;
}

static void
build_fs_prolog(nir_builder *b, const void *key)
{
   agx_nir_fs_prolog(b, key);

   NIR_PASS(_, b->shader, nir_shader_intrinsics_pass, lower_fs_prolog_abi,
            nir_metadata_control_flow, NULL);
}

/* G17P addresses each tilebuffer sample explicitly, so a multisample epilog
 * writes one sample and leaves the rest untouched unless its tile stores are
 * expanded. Without this a four-sample draw reaches only sample zero, and
 * resolving it yields a quarter of the drawn colour.
 */
static void
build_g17p_fs_epilog(nir_builder *b, const void *key)
{
   agx_nir_fs_epilog_g17p(b, key);
}

static struct agx_linked_shader *
asahi_fast_link(struct agx_context *ctx, struct agx_uncompiled_shader *so,
                struct agx_fast_link_key *key)
{
   /* Try the cache */
   struct hash_entry *ent = _mesa_hash_table_search(so->linked_shaders, key);
   if (ent)
      return ent->data;

   /* Never link on top of a refused main program: agx_fast_link() would
    * happily concatenate an empty binary and produce a linked shader that
    * runs no API code at all. Refusing here keeps ctx->linked.* NULL, which
    * the draw path already treats as "this draw cannot be issued".
    */
   if (key->main == NULL || key->main->failed)
      return NULL;

   struct agx_compiled_shader *prolog = NULL, *epilog = NULL;

   /* Build the prolog/epilog now */
   if (so->type == MESA_SHADER_FRAGMENT) {
      prolog = agx_build_meta_shader_internal(
         ctx, build_fs_prolog, &key->prolog.fs, sizeof(key->prolog.fs), true,
         false, key->prolog.fs.cf_base, false);

      epilog = agx_build_meta_shader_internal(
         ctx,
         agx_native_graphics(agx_device(ctx->base.screen))
            ? build_g17p_fs_epilog
            : agx_nir_fs_epilog,
         &key->epilog.fs, sizeof(key->epilog.fs), false, true, 0, false);

   } else if (so->type == MESA_SHADER_TESS_EVAL) {
      /* No prolog/epilog needed */
   } else {
      /* A vertex shader, or a geometry shader's copy program: poly builds
       * that as a vertex-stage main, and on G17P it runs behind the same
       * vertex prolog as any hardware vertex shader. */
      assert(so->type == MESA_SHADER_VERTEX ||
             (so->type == MESA_SHADER_GEOMETRY &&
              key->main->b.info.stage == MESA_SHADER_VERTEX));

      /* G17P device loads return packed raw lanes. Its vertex prolog must
       * convert them rather than expect AGX2's formatted widening loads. */
      prolog = agx_build_meta_shader_internal(
         ctx,
         agx_native_graphics(agx_device(ctx->base.screen))
            ? agx_nir_vs_prolog_raw
            : agx_nir_vs_prolog,
         &key->prolog.vs, sizeof(key->prolog.vs), true,
         false, 0, false);
   }

   /* A refused prolog or epilog is just as fatal as a refused main: the
    * linked program would be missing the code that feeds or drains it.
    */
   if ((prolog && prolog->failed) || (epilog && epilog->failed))
      return NULL;

   /* Fast-link it all together */
   struct agx_device *dev = agx_device(ctx->base.screen);

   struct agx_linked_shader *linked =
      rzalloc(so->linked_shaders, struct agx_linked_shader);
   if (!agx_fast_link(linked, dev, so->type == MESA_SHADER_FRAGMENT,
                      &key->main->b, &prolog->b, &epilog->b,
                      key->nr_samples_shaded)) {
      ralloc_free(linked);
      return NULL;
   }

   if (so->type == MESA_SHADER_FRAGMENT) {
      linked->g17p_emrt_heap_base = key->epilog.fs.emrt_heap_base;
      linked->g17p_blend_constant_base = key->epilog.fs.blend_constant_base;
   }

   /* Cache the fast linked program */
   union asahi_shader_key *cloned_key =
      ralloc_memdup(so->linked_shaders, key, sizeof(*key));
   _mesa_hash_table_insert(so->linked_shaders, cloned_key, linked);
   return linked;
}

static bool
agx_update_vs(struct agx_batch *batch, unsigned index_size_B)
{
   struct agx_context *ctx = batch->ctx;

   /* Only proceed if the shader or anything the key depends on changes
    *
    * vb_mask, attributes, vertex_buffers: VERTEX
    */
   if (!((ctx->dirty & (AGX_DIRTY_VS_PROG | AGX_DIRTY_VERTEX | AGX_DIRTY_XFB)) ||
         ctx->stage[MESA_SHADER_TESS_EVAL].dirty ||
         ctx->stage[MESA_SHADER_GEOMETRY].dirty ||
         ctx->stage[MESA_SHADER_TESS_EVAL].shader ||
         ctx->stage[MESA_SHADER_GEOMETRY].shader || ctx->in_tess))
      return false;

   struct asahi_vs_shader_key key = {
      .hw = !((ctx->stage[MESA_SHADER_TESS_EVAL].shader && !ctx->in_tess) ||
              ctx->stage[MESA_SHADER_GEOMETRY].shader),
      .custom_borders = ctx->stage[MESA_SHADER_VERTEX].custom_borders,
   };

   agx_update_shader(ctx, &ctx->vs, MESA_SHADER_VERTEX,
                     (union asahi_shader_key *)&key);

   struct agx_device *dev = agx_device(ctx->base.screen);
   struct agx_fast_link_key link_key = {
      .prolog.vs.hw = key.hw,
      .prolog.vs.uniform_base =
         ctx->vs->g17p_texture_resources
            ? AGX_ABI_G17P_TEXTURE_POINTER_HALFS
            : 0,
      .prolog.vs.sw_index_size_B = key.hw ? 0 : index_size_B,

      .prolog.vs.robustness.level =
         ctx->robust ? AGX_ROBUSTNESS_GL : AGX_ROBUSTNESS_DISABLED,

      .prolog.vs.robustness.soft_fault = agx_has_soft_fault(dev),
      .main = ctx->vs,
   };

   STATIC_ASSERT(sizeof(link_key.prolog.vs.component_mask) ==
                 sizeof(ctx->vs->attrib_components_read));
   BITSET_COPY(link_key.prolog.vs.component_mask,
               ctx->vs->attrib_components_read);

   memcpy(link_key.prolog.vs.attribs, &ctx->attributes->key,
          sizeof(link_key.prolog.vs.attribs));

   void *old = ctx->linked.vs;

   ctx->linked.vs =
      asahi_fast_link(ctx, ctx->stage[MESA_SHADER_VERTEX].shader, &link_key);

   /* A refused variant owns no executable BO. Residency is decided per draw
    * in agx_draw_vbo(), which skips the draw entirely, so there is nothing
    * to make resident here and nothing to dereference.
    */
   if (ctx->vs->bo)
      agx_batch_add_bo(batch, ctx->vs->bo);
   if (ctx->linked.vs)
      agx_batch_add_bo(batch, ctx->linked.vs->bo);

   return old != ctx->linked.vs;
}

static bool
agx_update_tcs(struct agx_context *ctx, const struct pipe_draw_info *info)
{
   assert(info->mode == MESA_PRIM_PATCHES);

   ctx->tcs = _mesa_hash_table_next_entry(
                 ctx->stage[MESA_SHADER_TESS_CTRL].shader->variants, NULL)
                 ->data;
   return true;
}

/*
 * On G17P every hardware vertex program is a linked prolog plus main:
 * agx_build_g17p_graphics_program() has nothing to publish for a bare main.
 * The geometry copy program is exactly such a vertex program for a
 * geometry-assisted draw (transform feedback included), so link it like
 * agx_update_vs() links the vertex shader: hardware vertex ABI, no vertex
 * attributes, the usual vertex/instance ID exports. Other chips execute the
 * copy program's main directly and keep ctx->linked.gs_copy NULL.
 */
static void
agx_update_gs_copy_link(struct agx_context *ctx, struct agx_batch *batch)
{
   struct agx_device *dev = agx_device(ctx->base.screen);

   ctx->linked.gs_copy = NULL;
   if (!ctx->gs || !ctx->gs->gs_copy || !agx_native_graphics(dev))
      return;

   struct agx_compiled_shader *copy = ctx->gs->gs_copy;
   struct agx_fast_link_key link_key = {
      .prolog.vs.hw = true,
      .prolog.vs.uniform_base = copy->g17p_texture_resources
                                   ? AGX_ABI_G17P_TEXTURE_POINTER_HALFS
                                   : 0,
      .prolog.vs.robustness.level =
         ctx->robust ? AGX_ROBUSTNESS_GL : AGX_ROBUSTNESS_DISABLED,
      .prolog.vs.robustness.soft_fault = agx_has_soft_fault(dev),
      .main = copy,
   };

   ctx->linked.gs_copy =
      asahi_fast_link(ctx, ctx->stage[MESA_SHADER_GEOMETRY].shader, &link_key);
   if (ctx->linked.gs_copy)
      agx_batch_add_bo(batch, ctx->linked.gs_copy->bo);
}

static bool
agx_update_gs(struct agx_context *ctx, const struct pipe_draw_info *info,
              const struct pipe_draw_indirect_info *indirect)
{
   /* Only proceed if there is a geometry shader. Due to input assembly
    * dependence, we don't bother to dirty track right now.
    */
   if (!ctx->stage[MESA_SHADER_GEOMETRY].shader) {
      ctx->gs = NULL;
      return false;
   }

   /* Transform feedback always happens via the geometry shader, so look there
    * to get the XFB strides.
    */
   struct agx_uncompiled_shader *gs = ctx->stage[MESA_SHADER_GEOMETRY].shader;

   for (unsigned i = 0; i < ctx->streamout.num_targets; ++i) {
      struct agx_streamout_target *tgt =
         agx_so_target(ctx->streamout.targets[i]);

      if (tgt != NULL)
         tgt->stride = gs->xfb_strides[i];
   }

   ctx->gs = _mesa_hash_table_next_entry(
                ctx->stage[MESA_SHADER_GEOMETRY].shader->variants, NULL)
                ->data;
   return true;
}

static enum pipe_blendfactor
optimize_blend_factor_w_1(enum pipe_blendfactor f)
{
   if (f == PIPE_BLENDFACTOR_SRC_ALPHA)
      return PIPE_BLENDFACTOR_ONE;
   else if (f == PIPE_BLENDFACTOR_INV_SRC_ALPHA)
      return PIPE_BLENDFACTOR_ZERO;
   else
      return f;
}

static uint32_t
agx_nr_tex_descriptors(struct agx_batch *batch,
                       struct agx_compiled_shader *cs);

/* Select a uniform range which cannot overlap either the native texture /
 * sampler pointer prefix or any main-shader uniform/constant range.  The
 * fragment epilog is linked against this same absolute half index. */
static bool
agx_g17p_fragment_blend_constant_base(enum agx_chip chip,
                                       const struct agx_compiled_shader *cs,
                                       uint16_t *base)
{
   if (!cs || !base)
      return false;

   unsigned occupied_halfs = cs->b.info.push_count;
   if (cs->b.info.rodata.size_16) {
      const unsigned rodata_end = (unsigned)cs->b.info.rodata.base_uniform +
                                  cs->b.info.rodata.size_16;
      occupied_halfs = MAX2(occupied_halfs, rodata_end);
   }

   return agx3_g17p_fragment_blend_constant_base(chip, occupied_halfs, base);
}

/* Tessellation ignored here due to the hard rebinding we do atm */
static struct agx_uncompiled_shader *
agx_last_uncompiled_vgt(struct agx_context *ctx)
{
   if (ctx->stage[MESA_SHADER_GEOMETRY].shader)
      return ctx->stage[MESA_SHADER_GEOMETRY].shader;
   else
      return ctx->stage[MESA_SHADER_VERTEX].shader;
}

static bool
agx_update_fs(struct agx_batch *batch)
{
   struct agx_context *ctx = batch->ctx;

   /* Only proceed if the shader or anything the key depends on changes
    *
    * batch->key: implicitly dirties everything, no explicit check
    * rast: RS
    * blend: BLEND
    * sample_mask: SAMPLE_MASK
    * reduced_prim: PRIM
    */
   if (!(ctx->dirty & (AGX_DIRTY_VS_PROG | AGX_DIRTY_FS_PROG | AGX_DIRTY_RS |
                       AGX_DIRTY_BLEND | AGX_DIRTY_SAMPLE_MASK |
                       AGX_DIRTY_PRIM | AGX_DIRTY_QUERY)) &&
       !ctx->stage[MESA_SHADER_GEOMETRY].dirty)
      return false;

   struct agx_device *dev = agx_device(ctx->base.screen);
   unsigned nr_samples = util_framebuffer_get_num_samples(&batch->key);

   /* Get main shader */
   struct asahi_fs_shader_key key = {0};
   key.custom_borders = ctx->stage[MESA_SHADER_FRAGMENT].custom_borders;

   /* G17P coverage updates use different native words at one and several
    * samples, including discard-only main shaders. Keep their raster sample
    * count in the main key, matching Honeykrisp and the linked shader parts.
    * Other generations need variants for sample-dependent shader operations.
    */
   if (agx_native_graphics(dev) ||
       ctx->stage[MESA_SHADER_FRAGMENT].shader->info.needs_sample_count ||
       ctx->stage[MESA_SHADER_FRAGMENT].shader->info.uses_fbfetch ||
       ctx->stage[MESA_SHADER_FRAGMENT].shader->info.uses_sample_shading) {
      key.nr_samples = nr_samples;
   }

   if (ctx->stage[MESA_SHADER_FRAGMENT].shader->info.uses_fbfetch) {
      for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
         key.rt_formats[i] = batch->key.cbufs[i].format;
      }
   }

   agx_update_shader(ctx, &ctx->fs, MESA_SHADER_FRAGMENT,
                     (union asahi_shader_key *)&key);

   /* Fast link with prolog/epilog */
   bool msaa = ctx->rast->base.multisample;
   unsigned sample_mask = ctx->sample_mask & BITFIELD_MASK(nr_samples);
   const bool nontrivial_force_early =
      ctx->fs->b.info.early_fragment_tests &&
      (ctx->fs->b.info.writes_sample_mask ||
       (ctx->fs->so && ctx->fs->so->info.writes_memory));
   const bool defer_api_sample_mask = agx_native_graphics(dev) && msaa &&
      sample_mask != BITFIELD_MASK(nr_samples) && !nontrivial_force_early;

   struct agx_fast_link_key link_key = {
      .prolog.fs.statistics =
         ctx->pipeline_statistics[PIPE_STAT_QUERY_PS_INVOCATIONS],

      .prolog.fs.cull_distance_size =
         agx_last_uncompiled_vgt(ctx)->info.cull_distance_size,

      .prolog.fs.polygon_stipple =
         ctx->rast->base.poly_stipple_enable &&
         rast_prim(batch->reduced_prim, ctx->rast->base.fill_front) ==
            MESA_PRIM_TRIANGLES,

      .prolog.fs.api_sample_mask =
         (msaa && nr_samples > 1 && sample_mask != BITFIELD_MASK(nr_samples) &&
          !defer_api_sample_mask)
            ? sample_mask
            : 0xff,

      .prolog.fs.nr_samples = agx_native_graphics(dev) ? nr_samples : 0,
      .epilog.fs.nr_samples = nr_samples,
      .epilog.fs.max_sample_size_B = batch->tilebuffer_layout.max_sample_size_B,
      .epilog.fs.max_bytes_per_tile = batch->tilebuffer_layout.max_bytes_per_tile,
      .epilog.fs.max_samples_per_tile = batch->tilebuffer_layout.max_samples_per_tile,
      .epilog.fs.defer_api_sample_mask = defer_api_sample_mask,
      .epilog.fs.api_sample_mask = sample_mask,
      .epilog.fs.link = ctx->fs->epilog_key,
      .epilog.fs.force_small_tile = dev->debug & AGX_DBG_SMALLTILE,

      .main = ctx->fs,
      .nr_samples_shaded = !agx_native_graphics(dev) &&
                            ctx->fs->epilog_key.sample_shading ? nr_samples : 0,
   };
   agx_fs_epilog_apply_device_policy(dev->chip, &link_key.epilog.fs);

   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; ++i) {
      link_key.epilog.fs.rt_formats[i] = batch->key.cbufs[i].format;
      link_key.epilog.fs.remap[i] =
         link_key.epilog.fs.link.broadcast_rt0 ? 0 : i;
   }

   memcpy(&link_key.epilog.fs.blend, &ctx->blend->key,
          sizeof(link_key.epilog.fs.blend));

   /* HAL200 texture/sampler table pointers consume fragment uniform halves
    * 0..7, overlapping the legacy dynamic blend constants at halves4..11.
    * Link the epilog against a range after the main shader's complete uniform
    * footprint. agx_build_g17p_graphics_program() publishes that exact range.
    */
   if (agx_native_graphics(dev) &&
       agx_nr_tex_descriptors(batch, ctx->fs) != 0 &&
       !agx_g17p_fragment_blend_constant_base(
          dev->chip, ctx->fs, &link_key.epilog.fs.blend_constant_base)) {
      mesa_loge("asahi: G17P fragment blend constants exceed the native "
                "uniform file");
      ctx->linked.fs = NULL;
      return true;
   }

   link_key.epilog.fs.native_spills = agx_native_graphics(dev) &&
                                      agx_tilebuffer_spills(&batch->tilebuffer_layout);
   if (link_key.epilog.fs.native_spills) {
      uint16_t base;
      if (!agx_g17p_fragment_blend_constant_base(dev->chip, ctx->fs, &base) ||
          ALIGN_POT(base, 4) > 512 - 12) {
         mesa_loge("asahi: G17P spill table exceeds the native uniform file");
         ctx->linked.fs = NULL;
         return true;
      }
      link_key.epilog.fs.emrt_heap_base = ALIGN_POT(base, 4);
      link_key.epilog.fs.blend_constant_base =
         link_key.epilog.fs.emrt_heap_base + 4;
      link_key.epilog.fs.link.rt_spill_base = 0;
   }

   /* Normalize */
   if (!agx_tilebuffer_spills(&batch->tilebuffer_layout))
      link_key.epilog.fs.link.rt_spill_base = 0;

   /* Try to disable blending to get rid of some fsats */
   if (link_key.epilog.fs.link.loc0_w_1) {
      struct agx_blend_rt_key *k = &link_key.epilog.fs.blend.rt[0];
      struct agx_blend_standard b = agx_unpack_blend_standard(k->mode);

      b.rgb_src_factor = optimize_blend_factor_w_1(b.rgb_src_factor);
      b.rgb_dst_factor = optimize_blend_factor_w_1(b.rgb_dst_factor);

      b.alpha_src_factor = optimize_blend_factor_w_1(b.alpha_src_factor);
      b.alpha_dst_factor = optimize_blend_factor_w_1(b.alpha_dst_factor);

      k->mode = agx_pack_blend_standard(b.rgb_func, b.rgb_src_factor,
                                        b.rgb_dst_factor, b.alpha_func,
                                        b.alpha_src_factor, b.alpha_dst_factor);
   }

   link_key.epilog.fs.blend.alpha_to_coverage &= msaa;

   /* The main shader must not run tests if the epilog will */
   bool epilog_discards = agx_fs_epilog_may_discard(
      &link_key.epilog.fs.link,
      link_key.epilog.fs.blend.alpha_to_coverage, nontrivial_force_early) ||
      defer_api_sample_mask;
   batch->uniforms.no_epilog_discard = !epilog_discards ? ~0 : 0;

   bool prolog_discards = (link_key.prolog.fs.api_sample_mask != 0xff ||
                           link_key.prolog.fs.cull_distance_size ||
                           link_key.prolog.fs.polygon_stipple);

   /* The prolog runs tests if neither the main shader nor epilog will */
   link_key.prolog.fs.run_zs_tests = !nontrivial_force_early &&
                                     !ctx->fs->b.info.writes_sample_mask &&
                                     !epilog_discards && prolog_discards;

   link_key.epilog.fs.link.already_ran_zs |= nontrivial_force_early;

   if (link_key.prolog.fs.cull_distance_size)
      link_key.prolog.fs.cf_base = ctx->fs->b.info.varyings.fs.nr_cf;

   void *old = ctx->linked.fs;

   ctx->linked.fs =
      asahi_fast_link(ctx, ctx->stage[MESA_SHADER_FRAGMENT].shader, &link_key);

   if (ctx->fs->bo)
      agx_batch_add_bo(batch, ctx->fs->bo);

   if (ctx->linked.fs)
      agx_batch_add_bo(batch, ctx->linked.fs->bo);

   return old != ctx->linked.fs;
}

static void
agx_bind_shader_state(struct pipe_context *pctx, void *cso,
                      mesa_shader_stage stage)
{
   struct agx_context *ctx = agx_context(pctx);

   if (stage == MESA_SHADER_VERTEX)
      ctx->dirty |= AGX_DIRTY_VS_PROG;
   else if (stage == MESA_SHADER_FRAGMENT)
      ctx->dirty |= AGX_DIRTY_FS_PROG;
   else
      ctx->stage[stage].dirty = ~0;

   struct agx_uncompiled_shader *old = ctx->stage[stage].shader;
   struct agx_uncompiled_shader *next = cso;
   if ((old && old->info.uses_query_lod) !=
       (next && next->info.uses_query_lod))
      ctx->stage[stage].dirty |= AGX_STAGE_DIRTY_SAMPLER;

   ctx->stage[stage].shader = cso;
}

static void
agx_bind_vs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, MESA_SHADER_VERTEX);
}

static void
agx_bind_fs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, MESA_SHADER_FRAGMENT);
}

static void
agx_bind_gs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, MESA_SHADER_GEOMETRY);
}

static void
agx_bind_tcs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, MESA_SHADER_TESS_CTRL);
}

static void
agx_bind_tes_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, MESA_SHADER_TESS_EVAL);
}

static void
agx_bind_cs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, MESA_SHADER_COMPUTE);
}

static void
agx_delete_compiled_shader(struct agx_device *dev,
                           struct agx_compiled_shader *so)
{
   if (so->gs_count)
      agx_delete_compiled_shader(dev, so->gs_count);

   if (so->pre_gs)
      agx_delete_compiled_shader(dev, so->pre_gs);

   if (so->gs_copy)
      agx_delete_compiled_shader(dev, so->gs_copy);

   free(so->b.binary);
   agx_bo_unreference(dev, so->bo);
   FREE(so);
}

static void
agx_delete_uncompiled_shader(struct agx_device *dev,
                             struct agx_uncompiled_shader *so)
{
   hash_table_foreach(so->variants, ent) {
      agx_delete_compiled_shader(dev, ent->data);
   }

   _mesa_hash_table_destroy(so->variants, NULL);

   if (so->linked_shaders) {
      hash_table_foreach(so->linked_shaders, ent) {
         struct agx_linked_shader *link = ent->data;
         agx_bo_unreference(dev, link->bo);
      }

      _mesa_hash_table_destroy(so->linked_shaders, NULL);
   }

   blob_finish(&so->serialized_nir);
   blob_finish(&so->early_serialized_nir);

   for (unsigned i = 0; i < MESA_PRIM_COUNT; ++i) {
      for (unsigned j = 0; j < 3; ++j) {
         for (unsigned k = 0; k < 2; ++k) {
            if (so->passthrough_progs[i][j][k])
               agx_delete_uncompiled_shader(dev,
                                            so->passthrough_progs[i][j][k]);
         }
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(so->passthrough_tcs); ++i) {
      if (so->passthrough_tcs[i])
         agx_delete_uncompiled_shader(dev, so->passthrough_tcs[i]);
   }

   ralloc_free(so);
}

static void
agx_delete_shader_state(struct pipe_context *ctx, void *cso)
{
   struct agx_device *dev = agx_device(ctx->screen);
   agx_delete_uncompiled_shader(dev, cso);
}

struct agx_generic_meta_key {
   meta_shader_builder_t builder;
   size_t key_size;
   uint8_t key[];
};

static uint32_t
meta_key_hash(const void *key_)
{
   const struct agx_generic_meta_key *key = key_;

   return _mesa_hash_data(key,
                          sizeof(struct agx_generic_meta_key) + key->key_size);
}

static bool
meta_key_equal(const void *a_, const void *b_)
{
   const struct agx_generic_meta_key *a = a_;
   const struct agx_generic_meta_key *b = b_;

   return a->builder == b->builder && a->key_size == b->key_size &&
          memcmp(a->key, b->key, a->key_size) == 0;
}

void
agx_init_meta_shaders(struct agx_context *ctx)
{
   ctx->generic_meta =
      _mesa_hash_table_create(ctx, meta_key_hash, meta_key_equal);
}

static void
agx_destroy_compute_blitter(struct pipe_context *ctx, struct asahi_blitter *bl)
{
   hash_table_foreach(bl->blit_cs, ent) {
      ctx->delete_compute_state(ctx, ent->data);
   }

   ctx->delete_sampler_state(ctx, bl->sampler[0]);
   ctx->delete_sampler_state(ctx, bl->sampler[1]);

   _mesa_hash_table_destroy(bl->blit_cs, NULL);
}

void
agx_destroy_meta_shaders(struct agx_context *ctx)
{
   struct agx_device *dev = agx_device(ctx->base.screen);
   hash_table_foreach(ctx->generic_meta, ent) {
      agx_delete_compiled_shader(dev, ent->data);
   }

   agx_destroy_compute_blitter(&ctx->base, &ctx->compute_blitter);
   _mesa_hash_table_destroy(ctx->generic_meta, NULL);
}

static struct agx_compiled_shader *
agx_build_meta_shader_internal(struct agx_context *ctx,
                               meta_shader_builder_t builder, void *data,
                               size_t data_size, bool prolog, bool epilog,
                               unsigned cf_base, bool internal_kernel)
{
   /* Build the meta shader key */
   size_t total_key_size = sizeof(struct agx_generic_meta_key) + data_size;
   struct agx_generic_meta_key *key = alloca(total_key_size);

   *key = (struct agx_generic_meta_key){
      .builder = builder,
      .key_size = data_size,
   };

   if (data_size)
      memcpy(key->key, data, data_size);

   /* Try to get the cached shader */
   struct hash_entry *ent = _mesa_hash_table_search(ctx->generic_meta, key);
   if (ent)
      return ent->data;

   /* Otherwise, compile the shader fresh */
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "AGX meta shader");

   builder(&b, data);

   struct agx_device *dev = agx_device(ctx->base.screen);
   if (!prolog) {
      agx_preprocess_nir(b.shader);
      NIR_PASS(_, b.shader, agx_nir_lower_texture);
      NIR_PASS(_, b.shader, agx_nir_lower_multisampled_image_store);
   }

   /* Match the compiler's sample protocol to the epilog's tile operands.
    * Preprocessing may expose literal masks while unrolling a sample loop;
    * lower them after that transformation, as the Vulkan path does.
    */
   unsigned nr_samples = 0;
   if (builder == build_g17p_fs_epilog) {
      const struct agx_fs_epilog_key *fs = data;
      nr_samples = fs->nr_samples;
      if (!fs->link.sample_shading)
         NIR_PASS(_, b.shader, agx_nir_hoist_g17p_zs_writes);
      NIR_PASS(_, b.shader, agx_nir_lower_g17p_tile_samples, nr_samples);
   } else if (builder == build_fs_prolog && agx_native_graphics(dev)) {
      nr_samples = ((const struct agx_fs_prolog_key *)data)->nr_samples;
   }

   struct agx_compiled_shader *shader = agx_compile_nir(
      dev, b.shader, NULL, b.shader->info.stage, internal_kernel,
      !prolog && !(!agx_native_graphics(dev) &&
                   b.shader->info.stage == MESA_SHADER_FRAGMENT &&
                   b.shader->info.fs.uses_sample_shading),
      prolog || epilog, cf_base, nr_samples,
      builder == build_g17p_fs_epilog ? data : NULL, NULL);

   ralloc_free(b.shader);

   /* ..and cache it before we return. The key is on the stack right now, so
    * clone it before using it as a hash table key. The clone is logically owned
    * by the hash table.
    */
   void *cloned_key = rzalloc_size(ctx->generic_meta, total_key_size);
   memcpy(cloned_key, key, total_key_size);

   _mesa_hash_table_insert(ctx->generic_meta, cloned_key, shader);
   return shader;
}

struct agx_compiled_shader *
agx_build_meta_shader(struct agx_context *ctx, meta_shader_builder_t builder,
                      void *data, size_t data_size)
{
   return agx_build_meta_shader_internal(ctx, builder, data, data_size, false,
                                         false, 0, false);
}

static unsigned
sampler_count(struct agx_context *ctx, mesa_shader_stage stage)
{
   /* We reserve sampler #0 for txf so add 1 to the API count */
   unsigned count = ctx->stage[stage].sampler_count + 1;
   /* Native staged MS reads name sampler one. Even an image-only shader
    * therefore needs that entry inside the declared sampler table. */
   return agx_native_graphics(agx_device(ctx->base.screen))
             ? MAX2(count, 2u) : count;
}

static inline enum agx_sampler_states
translate_sampler_state_count(struct agx_context *ctx, mesa_shader_stage stage)
{
   struct agx_device *dev = agx_device(ctx->base.screen);

   /* Clamp to binding table maximum, anything larger will be bindless */
   return agx_translate_sampler_state_count(MIN2(sampler_count(ctx, stage), 16),
                                            !agx_native_graphics(dev) &&
                                               ctx->stage[stage].custom_borders);
}

static uint32_t
agx_nr_tex_descriptors_without_spilled_rts(const struct agx_compiled_shader *cs)
{
   if (!cs || !cs->so)
      return 0;

   /* image_records descriptors per image, 1 descriptor per texture */
   return cs->so->info.nr_bindful_textures +
          (cs->so->info.image_records * cs->so->info.nr_bindful_images);
}

static uint32_t
agx_nr_tex_descriptors(struct agx_batch *batch, struct agx_compiled_shader *cs)
{
   unsigned n = agx_nr_tex_descriptors_without_spilled_rts(cs);

   /* We add on texture/PBE descriptors for spilled render targets */
   bool spilled_rt = cs->stage == MESA_SHADER_FRAGMENT &&
                     agx_tilebuffer_spills(&batch->tilebuffer_layout);
   if (spilled_rt)
      n += (batch->key.nr_cbufs * 2);

   return n;
}

/*
 * For spilled render targets, upload a texture/PBE pair for each surface to
 * allow loading/storing to the render target from the shader.
 */
static struct agx_gallium_texture_descriptor *
agx_texture_descriptor_at(void *base, unsigned stride, unsigned index)
{
   return (struct agx_gallium_texture_descriptor *)((uint8_t *)base +
                                                     (stride * index));
}

static bool
agx_gallium_g17p_tile_layout_supported(struct agx_tilebuffer_layout *tib)
{
   if (!tib)
      return false;

   /* Layer selection lives in the proven array BG/EOT programs and PBE
    * descriptor. It does not change the physical tile allocation, so validate
    * that allocation through the same bounded RT0 contract Honeykrisp uses. */
   struct agx_tilebuffer_layout physical = *tib;
   physical.layered = false;
   return agx_g17p_rt0_tile_layout_supported(&physical);
}

static struct agx_gallium_pbe_descriptor *
agx_pbe_descriptor_at(void *base, unsigned stride, unsigned index)
{
   return (struct agx_gallium_pbe_descriptor *)((uint8_t *)base +
                                                 (stride * index));
}

static void
agx_upload_spilled_rt_descriptors(void *out, struct agx_batch *batch,
                                  enum agx_descriptor_generation generation)
{
   struct agx_device *dev = agx_device(batch->ctx->base.screen);
   const unsigned stride = agx_gallium_texture_descriptor_size(generation);

   for (unsigned rt = 0; rt < batch->key.nr_cbufs; ++rt) {
      struct agx_gallium_texture_descriptor *texture =
         agx_texture_descriptor_at(out, stride, 2 * rt);
      struct agx_gallium_pbe_descriptor *pbe =
         agx_pbe_descriptor_at(out, stride, (2 * rt) + 1);

      const struct pipe_surface *surf = &batch->key.cbufs[rt];
      if (!surf->texture)
         continue;

      struct agx_resource *rsrc = agx_resource(surf->texture);
      struct pipe_image_view view = image_view_for_surface(surf);
      struct pipe_sampler_view sampler_view = sampler_view_for_surface(surf);
      sampler_view.target = PIPE_TEXTURE_2D_ARRAY;

      if (!agx_pack_texture(generation, texture, rsrc, surf->format,
                            &sampler_view))
         agx_native_descriptor_rejected(dev, "spilled-RT texture");

      if (!agx_batch_upload_pbe(batch, pbe, &view, false, false, true, true))
         agx_native_descriptor_rejected(dev, "spilled-RT PBE");
   }
}

/*
 * The record after a G17P storage image's texture/PBE pair carries the
 * software words Honeykrisp's Gen9 storage descriptor keeps there, at the same
 * offsets from the texture descriptor: the native texture-table index of the
 * image's texture, the packed addressing word image atomics read through
 * libagx_image_texel_address_gen9(), and for multisampled images the aligned
 * width libagx_emrt_ms_index_gen9() needs. Gen9's PBE has no free bits to
 * carry these, which is why AGX2's in-descriptor sideband is left out of AGX3
 * PBEs. A view whose addressing does not fit the packed word keeps it zero.
 */
static void
agx_g17p_fill_storage_tail(uint8_t *tail, unsigned record_size_B,
                           const struct pipe_image_view *view,
                           unsigned texture_index, bool emrt)
{
   STATIC_ASSERT(AGX_G17P_IMAGE_ATOMIC_SW_OFFSET + sizeof(uint64_t) <=
                 3 * AGX_GALLIUM_DESCRIPTOR_AGX3_SIZE);
   STATIC_ASSERT(AGX_G17P_EMRT_ALIGNED_WIDTH_OFFSET + sizeof(uint32_t) <=
                 3 * AGX_GALLIUM_DESCRIPTOR_AGX3_SIZE);
   assert(record_size_B == AGX_GALLIUM_DESCRIPTOR_AGX3_SIZE);
   const unsigned base = 2 * record_size_B;

   uint32_t heap_index = texture_index;
   memcpy(tail + AGX_G17P_STORAGE_HEAP_INDEX_OFFSET - base, &heap_index,
          sizeof(heap_index));

   struct agx_resource *rsrc = agx_resource(view->resource);
   if (rsrc->base.target == PIPE_BUFFER) {
      /* Same software texel-buffer metadata as Honeykrisp. The native
       * descriptors already include the view byte offset, so the extra
       * coordinate offset is zero; atomics use the exact bound byte address. */
      const uint32_t size =
         agx_texture_buffer_size_el(view->format, view->u.buf.size);
      const uint32_t offset = 0;
      const uint64_t address = agx_map_gpu(rsrc) + view->u.buf.offset;
      memcpy(tail + AGX_G17P_BUFFER_TEXTURE_SIZE_OFFSET - base, &size,
             sizeof(size));
      memcpy(tail + AGX_G17P_BUFFER_TEXTURE_TAIL_OFFSET - base, &offset,
             sizeof(offset));
      memcpy(tail + AGX_G17P_BUFFER_TEXTURE_ADDRESS_OFFSET - base, &address,
             sizeof(address));
      return;
   }
   if (!rsrc->layout.writeable_image && !emrt)
      return;

   const struct ail_layout *layout = &rsrc->layout;
   const unsigned samples = util_res_sample_count(&rsrc->base);
   const bool linear = layout->tiling == AIL_TILING_LINEAR;
   /* Multisampled storage is bound as a linear buffer starting at its level,
    * so its level offset is zero, as in hk's pack_pbe(). */
   const unsigned level = samples > 1 ? 0 : view->u.tex.level;

   if (samples > 1) {
      const uint32_t aligned_width =
         align(u_minify(rsrc->base.width0, view->u.tex.level),
               layout->tilesize_el[view->u.tex.level].width_el);
      memcpy(tail + AGX_G17P_EMRT_ALIGNED_WIDTH_OFFSET - base, &aligned_width,
             sizeof(aligned_width));
   }

   uint64_t sw = 0;
   if (agx_g17p_pack_image_atomic_sw(
          ail_get_level_offset_B(layout, level),
          linear ? 1 : layout->tilesize_el[view->u.tex.level].width_el,
          linear ? 1 : layout->tilesize_el[view->u.tex.level].height_el,
          layout->layer_stride_B, samples, &sw))
      memcpy(tail + AGX_G17P_IMAGE_ATOMIC_SW_OFFSET - base, &sw, sizeof(sw));
}

static uint64_t
agx_upload_g17p_spilled_rt_table(struct agx_batch *batch, const void *pairs,
                                 unsigned texture_base)
{
   uint8_t entries[PIPE_MAX_COLOR_BUFS][AGX_G17P_EMRT_DESCRIPTOR_SIZE] = {0};
   for (unsigned rt = 0; rt < batch->key.nr_cbufs; ++rt) {
      if (!batch->tilebuffer_layout.spilled[rt])
         continue;
      const struct pipe_surface *surf = &batch->key.cbufs[rt];
      if (!surf->texture)
         return 0;
      struct pipe_image_view view = image_view_for_surface(surf);
      const uint32_t texture_index = texture_base + 2 * rt;
      memcpy(entries[rt], (const uint8_t *)pairs + 64 * rt, 64);
      agx_g17p_fill_storage_tail(entries[rt] + 64, 32, &view, texture_index, true);
      memcpy(entries[rt] + AGX_G17P_SAMPLED_HEAP_INDEX_OFFSET,
             &texture_index, sizeof(texture_index));
   }
   const uint64_t address = agx_pool_upload_aligned(
      &batch->pool, entries, batch->key.nr_cbufs * sizeof(entries[0]), 32);
   return address ? agx_pool_upload_aligned(&batch->pool, &address, 8, 8) : 0;
}

static void
agx_upload_textures(struct agx_batch *batch, struct agx_compiled_shader *cs,
                    mesa_shader_stage stage)
{
   struct agx_context *ctx = batch->ctx;

   /* This can occur for meta shaders */
   if (!cs->so) {
      batch->texture_count[stage] = 0;
      batch->stage_uniforms[stage].texture_base = 0;
      return;
   }

   unsigned nr_textures = cs->so->info.nr_bindful_textures;

   unsigned nr_active_textures = ctx->stage[stage].texture_count;
   unsigned nr_tex_descriptors = agx_nr_tex_descriptors(batch, cs);
   unsigned nr_images = cs->so->info.nr_bindful_images;

   struct agx_device *dev = agx_device(ctx->base.screen);
   const enum agx_descriptor_generation descriptor_generation =
      agx_gallium_descriptor_generation(dev->chip);
   const unsigned descriptor_size =
      agx_gallium_texture_descriptor_size(descriptor_generation);
   if (!descriptor_size)
      agx_native_descriptor_rejected(dev, "texture heap geometry");

   struct agx_ptr T_tex = agx_pool_alloc_aligned(
      &batch->pool, descriptor_size * nr_tex_descriptors, 64);

   struct agx_ptr buffer_metadata = {0};
   if (cs->so->info.uses_buffer_textures) {
      buffer_metadata = agx_pool_alloc_aligned(
         &batch->pool, nr_textures * AGX_G17P_EMRT_DESCRIPTOR_SIZE, 32);
      memset(buffer_metadata.cpu, 0, nr_textures * AGX_G17P_EMRT_DESCRIPTOR_SIZE);
   }
   batch->stage_uniforms[stage].buffer_texture_base = buffer_metadata.gpu;

   for (unsigned i = 0; i < MIN2(nr_textures, nr_active_textures); ++i) {
      struct agx_sampler_view *tex = ctx->stage[stage].textures[i];
      struct agx_gallium_texture_descriptor *texture =
         agx_texture_descriptor_at(T_tex.cpu, descriptor_size, i);

      if (tex == NULL) {
         if (!agx_gallium_set_null_native_texture(descriptor_generation,
                                                   texture))
            agx_native_descriptor_rejected(dev, "null texture");
         continue;
      }

      struct agx_resource *rsrc = tex->rsrc;
      agx_batch_reads(batch, tex->rsrc);

      /* Re-emit state because the layout might have changed from under us.
       * TODO: optimize this somehow?
       */
      if (!agx_pack_texture(descriptor_generation, &tex->desc, rsrc,
                            tex->format, &tex->base))
         agx_native_descriptor_rejected(dev, "sampled texture");

      memcpy(texture, &tex->desc, descriptor_size);
      if (buffer_metadata.cpu && tex->base.target == PIPE_BUFFER) {
         uint8_t *entry = (uint8_t *)buffer_metadata.cpu +
                          i * AGX_G17P_EMRT_DESCRIPTOR_SIZE;
         const uint32_t elements =
            agx_texture_buffer_size_el(tex->format, tex->base.u.buf.size);
         const uint64_t address = agx_map_gpu(rsrc) + tex->base.u.buf.offset;
         const uint32_t rgb32 = tex->format == PIPE_FORMAT_R32G32B32_FLOAT ||
                                tex->format == PIPE_FORMAT_R32G32B32_UINT ||
                                tex->format == PIPE_FORMAT_R32G32B32_SINT;
         memcpy(entry, &tex->desc, descriptor_size);
         memcpy(entry + AGX_G17P_BUFFER_TEXTURE_SIZE_OFFSET, &elements, 4);
         memcpy(entry + AGX_G17P_BUFFER_TEXTURE_ADDRESS_OFFSET, &address, 8);
         memcpy(entry + AGX_G17P_BUFFER_TEXTURE_RGB32_OFFSET, &rgb32, 4);
      }
   }

   for (unsigned i = nr_active_textures; i < nr_textures; ++i) {
      if (!agx_gallium_set_null_native_texture(
             descriptor_generation,
             agx_texture_descriptor_at(T_tex.cpu, descriptor_size, i)))
         agx_native_descriptor_rejected(dev, "null texture");
   }

   const unsigned image_records = cs->so->info.image_records;
   for (unsigned i = 0; i < nr_images; ++i) {
      batch->stage_uniforms[stage].image_buffer_size[i] = 0;
      /* Image descriptors come in pairs after the textures, each followed on
       * G17P by its software tail record. */
      const unsigned image = nr_textures + (image_records * i);
      struct agx_gallium_texture_descriptor *texture =
         agx_texture_descriptor_at(T_tex.cpu, descriptor_size, image);
      struct agx_gallium_pbe_descriptor *pbe =
         agx_pbe_descriptor_at(T_tex.cpu, descriptor_size, image + 1);
      uint8_t *tail = image_records > 2
                         ? (uint8_t *)agx_texture_descriptor_at(
                              T_tex.cpu, descriptor_size, image + 2)
                         : NULL;
      if (tail)
         memset(tail, 0, descriptor_size);

      if (!(ctx->stage[stage].image_mask & BITFIELD64_BIT(i))) {
         if (!agx_gallium_set_null_native_texture(descriptor_generation,
                                                   texture))
            agx_native_descriptor_rejected(dev, "null storage texture");
         if (!agx_gallium_set_null_native_pbe(descriptor_generation, pbe))
            agx_native_descriptor_rejected(dev, "null storage PBE");
         continue;
      }

      struct pipe_image_view *view = &ctx->stage[stage].images[i];
      agx_batch_track_image(batch, view);
      if (agx_native_graphics(dev) && view->resource->target == PIPE_BUFFER)
         batch->stage_uniforms[stage].image_buffer_size[i] =
            agx_texture_buffer_size_el(view->format, view->u.buf.size);

      struct pipe_sampler_view sampler_view = util_image_to_sampler_view(view);

      /* For the texture descriptor, lower cubes to 2D arrays. This matches the
       * transform done in the compiler. Also, force 2D arrays for internal
       * blitter images, this helps reduce shader variants.
       */
      bool internal = (view->access & PIPE_IMAGE_ACCESS_DRIVER_INTERNAL);

      if (target_is_cube(sampler_view.target) ||
          (sampler_view.target == PIPE_TEXTURE_3D && internal))
         sampler_view.target = PIPE_TEXTURE_2D_ARRAY;

      if (!agx_pack_texture(descriptor_generation, texture,
                            agx_resource(view->resource), view->format,
                            &sampler_view))
         agx_native_descriptor_rejected(dev, "storage texture");

      if (!agx_batch_upload_pbe(batch, pbe, view, false, false, false, false))
         agx_native_descriptor_rejected(dev, "storage PBE");

      if (tail)
         agx_g17p_fill_storage_tail(tail, descriptor_size, view, image, false);
   }

   if (stage == MESA_SHADER_FRAGMENT &&
       agx_tilebuffer_spills(&batch->tilebuffer_layout)) {

      void *out =
         (uint8_t *)T_tex.cpu +
         (descriptor_size * agx_nr_tex_descriptors_without_spilled_rts(cs));

      agx_upload_spilled_rt_descriptors(out, batch, descriptor_generation);
      if (agx_native_graphics(dev)) {
         batch->g17p_emrt_heap = agx_upload_g17p_spilled_rt_table(
            batch, out, agx_nr_tex_descriptors_without_spilled_rts(cs));
         if (!batch->g17p_emrt_heap)
            agx_native_descriptor_rejected(dev, "native spilled-RT table");
      }
   }

   batch->texture_count[stage] = nr_tex_descriptors;
   batch->stage_uniforms[stage].texture_base = T_tex.gpu;

   if (agx_native_graphics(dev)) {
      for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; ++i)
         batch->stage_uniforms[stage].texture_handle[i] = i;
   }
}

uint16_t
agx_sampler_heap_add(struct agx_device *dev, struct agx_sampler_heap *heap,
                     struct agx_sampler_packed *sampler)
{
   /* Allocate (maximally sized) BO if we haven't already */
   if (!heap->bo) {
      heap->bo = agx_bo_create(dev, AGX_SAMPLER_HEAP_SIZE * AGX_SAMPLER_LENGTH,
                               0, AGX_BO_WRITEBACK, "Sampler heap");

      assert(heap->count == 0);
   }

   /* TODO search */

   /* Precondition: there is room in the heap */
   assert(heap->count < AGX_SAMPLER_HEAP_SIZE);
   struct agx_sampler_packed *samplers = agx_bo_map(heap->bo);
   memcpy(samplers + heap->count, sampler, sizeof(*sampler));

   return heap->count++;
}

/* Native table entry for an API sampler, with the given border colour. */
static struct agx_gallium_native_sampler
agx_native_sampler_state(const struct pipe_sampler_state *state,
                         enum agx_border_colour border_colour)
{
   const bool compare_enable =
      state->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE;

   return (struct agx_gallium_native_sampler){
      .minimum_lod = state->min_lod,
      .maximum_lod = state->max_lod,
      .maximum_anisotropy =
         util_next_power_of_two(MAX2(state->max_anisotropy, 1)),
      .magnify = agx_filters[state->mag_img_filter],
      .minify = agx_filters[state->min_img_filter],
      .mip_filter = agx_mip_filter_from_pipe(state->min_mip_filter),
      .wrap_s = agx_wrap_from_pipe(state->wrap_s),
      .wrap_t = agx_wrap_from_pipe(state->wrap_t),
      .wrap_r = agx_wrap_from_pipe(state->wrap_r),
      .pixel_coordinates = state->unnormalized_coords,
      .compare_func = agx_compare_funcs[state->compare_func],
      .compare_enable = compare_enable,
      /* An ordinary sample takes the bias as a shader operand, summed with
       * the shader's own in floating point; only a depth comparison, whose
       * level has no per-lane form here, keeps it in the descriptor where the
       * sampler has a bias field. See agx_create_context(). */
      .lod_bias = compare_enable ? CLAMP(state->lod_bias, -16.0, 16.0) : 0.0f,
      .seamful_cube_maps = !state->seamless_cube_map,
      .border_colour = border_colour,
   };
}

static void
agx_upload_samplers(struct agx_batch *batch, struct agx_compiled_shader *cs,
                    mesa_shader_stage stage)
{
   struct agx_context *ctx = batch->ctx;

   unsigned nr_samplers = sampler_count(ctx, stage);
   bool custom_borders = ctx->stage[stage].custom_borders;

   struct agx_device *dev = agx_device(ctx->base.screen);
   const bool native = agx_native_graphics(dev);
   size_t sampler_length = native
      ? agx_chip_sampler_heap_element_size(dev->chip)
      : AGX_SAMPLER_LENGTH + (custom_borders ? AGX_BORDER_LENGTH : 0);

   if (native) {
      /* Keep ordinary sampler handles stable at i+1.  If any sampler needs a
       * custom border, reserve a disjoint clamp-to-zero bank at slot 33, as
       * Honeykrisp does with its second sampler plane. */
      const bool query_lod = cs->so && cs->so->info.uses_query_lod;
      unsigned count = nr_samplers;
      if (custom_borders)
         count = (agx_shadow_gather_custom_border(dev->chip)
                     ? AGX_GALLIUM_G17P_BORDER_NONCOMPARE_BASE
                     : AGX_GALLIUM_G17P_CUSTOM_BORDER_BASE) +
                 ctx->stage[stage].sampler_count;
      if (query_lod)
         count = MAX2(count, AGX_GALLIUM_G17P_QUERY_LOD_BASE +
                             ctx->stage[stage].sampler_count);
      struct agx_ptr T = agx_pool_alloc_aligned(
         &batch->pool, sampler_length * count, 64);
      memset(T.cpu, 0, sampler_length * count);

      /* Native indexed read() and sample() share this table. Match
       * Honeykrisp's ABI by keeping the exact texel-fetch sampler at zero and
       * placing API samplers after it. */
      if (!agx_gallium_pack_g17p_txf_sampler(dev->chip, T.cpu))
         agx_native_descriptor_rejected(dev, "texel-fetch sampler");

      /* Keep API sampler zero at its existing handle one when present.
       * Image-only MS reads still consume handle one, so populate it with
       * the same texel-fetch state rather than an absent table entry. */
      if (!ctx->stage[stage].sampler_count)
         memcpy((uint8_t *)T.cpu + sampler_length, T.cpu, sampler_length);

      for (unsigned i = 0; i < ctx->stage[stage].sampler_count; ++i) {
         struct agx_sampler_state *sampler = ctx->stage[stage].samplers[i];
         uint8_t *out = (uint8_t *)T.cpu + sampler_length * (i + 1);
         if (sampler) {
            const struct pipe_sampler_state *state = &sampler->base;
            const struct agx_gallium_native_sampler native =
               agx_native_sampler_state(
                  state, sampler->uses_custom_border
                            ? AGX_BORDER_COLOUR_OPAQUE_WHITE
                            : sampler->border_colour);
            if (!agx_gallium_pack_native_sampler(dev->chip, (void *)out,
                                                 &native))
               agx_native_descriptor_rejected(dev, "sampler");

            if (sampler->uses_custom_border) {
               uint16_t clamp_handle;
               if (!agx_gallium_g17p_custom_border_handle(dev->chip, i,
                                                            &clamp_handle))
                  agx_native_descriptor_rejected(dev,
                                                 "custom-border sampler");

               uint8_t *clamp = (uint8_t *)T.cpu +
                                sampler_length * clamp_handle;
               struct agx_gallium_native_sampler clamp_state =
                  agx_native_sampler_state(
                     state, AGX_BORDER_COLOUR_TRANSPARENT_BLACK);
               if (agx_shadow_gather_custom_border(dev->chip))
                  clamp_state.compare_enable = false;
               if (!agx_gallium_pack_native_sampler(dev->chip, (void *)clamp,
                                                    &clamp_state))
                  agx_native_descriptor_rejected(dev,
                                                 "custom-border sampler");

               if (agx_shadow_gather_custom_border(dev->chip)) {
                  clamp_state.border_colour = AGX_BORDER_COLOUR_OPAQUE_WHITE;
                  uint8_t *probe = (uint8_t *)T.cpu + sampler_length *
                     (AGX_GALLIUM_G17P_BORDER_NONCOMPARE_BASE + i);
                  if (!agx_gallium_pack_native_sampler(dev->chip, (void *)probe,
                                                       &clamp_state))
                     agx_native_descriptor_rejected(dev,
                                                    "custom-border probe sampler");
               }
            }
         } else {
            const struct agx_gallium_native_sampler null_state = {
               .maximum_lod = INFINITY,
               .maximum_anisotropy = 1,
               .mip_filter = AGX_MIP_FILTER_NEAREST,
               .wrap_s = AGX_WRAP_CLAMP_TO_BORDER,
               .wrap_t = AGX_WRAP_CLAMP_TO_BORDER,
               .wrap_r = AGX_WRAP_CLAMP_TO_BORDER,
               .compare_func = AGX_COMPARE_FUNC_LEQUAL,
               .border_colour = AGX_BORDER_COLOUR_TRANSPARENT_BLACK,
            };
            if (!agx_gallium_pack_native_sampler(dev->chip, (void *)out,
                                                 &null_state))
               agx_native_descriptor_rejected(dev, "null sampler");
         }
      }

      if (query_lod) {
         for (unsigned i = 0; i < ctx->stage[stage].sampler_count; ++i) {
            const struct agx_sampler_state *sampler =
               ctx->stage[stage].samplers[i];
            const uint8_t *ordinary =
               (const uint8_t *)T.cpu + sampler_length * (i + 1);
            uint8_t *query = (uint8_t *)T.cpu + sampler_length *
               (AGX_GALLIUM_G17P_QUERY_LOD_BASE + i);
            /* A Gen4 sampler has no bias field to carry the API bias into
             * the LOD query, so the query reads the ordinary entry. */
            if (!agx_chip_sampler_has_lod_bias(dev->chip)) {
               memcpy(query, ordinary, sampler_length);
               continue;
            }
            agx_unpack(NULL, ordinary, SAMPLER_HAL200, cfg);
            cfg.lod_bias = sampler ? CLAMP(sampler->base.lod_bias, -16.0, 16.0)
                                   : 0.0;
            AGX_SAMPLER_HAL200_pack((uint32_t *)query, &cfg);
         }
      }

      batch->sampler_count[stage] = count;
      batch->samplers[stage] = T.gpu;
      return;
   }

   struct agx_ptr T =
      agx_pool_alloc_aligned(&batch->pool, sampler_length * nr_samplers, 64);

   /* Sampler #0 is reserved for txf */
   agx_pack_txf_sampler(T.cpu);

   /* Remaining samplers are API samplers */
   uint8_t *out_sampler = (uint8_t *)T.cpu + sampler_length;
   for (unsigned i = 0; i < ctx->stage[stage].sampler_count; ++i) {
      struct agx_sampler_state *sampler = ctx->stage[stage].samplers[i];
      struct agx_sampler_packed *out = (struct agx_sampler_packed *)out_sampler;

      if (sampler) {
         *out = sampler->desc;

         if (custom_borders) {
            STATIC_ASSERT(sizeof(sampler->border) == AGX_BORDER_LENGTH);

            memcpy(out_sampler + AGX_SAMPLER_LENGTH, &sampler->border,
                   AGX_BORDER_LENGTH);
         } else {
            assert(!sampler->uses_custom_border && "invalid combination");
         }
      } else {
         memset(out, 0, sampler_length);
      }

      out_sampler += sampler_length;
   }

   batch->sampler_count[stage] = nr_samplers;
   batch->samplers[stage] = T.gpu;
}

static void
agx_update_descriptors(struct agx_batch *batch, struct agx_compiled_shader *cs)
{
   struct agx_context *ctx = batch->ctx;
   if (!cs)
      return;

   mesa_shader_stage stage = cs->stage;
   if (!ctx->stage[stage].dirty)
      return;

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_CONST)
      agx_set_cbuf_uniforms(batch, stage);

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_SSBO)
      agx_set_ssbo_uniforms(batch, stage);

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_IMAGE)
      agx_upload_textures(batch, cs, stage);

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_SAMPLER)
      agx_set_sampler_uniforms(batch, stage);

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_SAMPLER)
      agx_upload_samplers(batch, cs, stage);

   struct agx_stage_uniforms *unif = &batch->stage_uniforms[stage];
   const size_t size =
      agx_shadow_gather_custom_border(agx_device(ctx->base.screen)->chip)
         ? sizeof(*unif)
         : offsetof(struct agx_stage_uniforms, custom_border_compare_func);

   batch->uniforms.tables[AGX_SYSVAL_STAGE(stage)] =
      agx_pool_upload_aligned(&batch->pool, unif, size, 16);
}

struct agx_g17p_graphics_template {
   struct agx_g17p_graphics_template *next;
   struct agx3_g17p_graphics_program_desc desc;
   struct agx3_ioto_uniform_range ranges[AGX3_G17P_GRAPHICS_MAX_RANGES];
   struct agx3_g17p_graphics_program_template program;
};

#define AGX_G17P_GRAPHICS_TEMPLATE_LIMIT 8u

static bool
agx_g17p_graphics_program_cached(enum agx_chip chip,
                                 struct agx_linked_shader *linked,
                                 const struct agx3_g17p_graphics_program_desc *desc,
                                 void *output, size_t capacity,
                                 size_t *written)
{
   struct agx_g17p_graphics_template **slot =
      &linked->g17p_graphics_templates;
   unsigned count = 0;
   while (*slot) {
      struct agx_g17p_graphics_template *entry = *slot;
      uint16_t length;
      /* The composer checks the complete shader/imageblock state, range
       * layout, address-cache topology and immediate instruction widths.
       * Only the draw-varying addresses may be patched; a different shape
       * goes through full generation instead. */
      if (agx3_g17p_graphics_program_template_relocate(
             chip, &entry->desc, desc, &entry->program, output,
             capacity, &length)) {
         *slot = entry->next;
         entry->next = linked->g17p_graphics_templates;
         linked->g17p_graphics_templates = entry;
         *written = length;
         return true;
      }
      slot = &entry->next;
      ++count;
   }

   struct agx_g17p_graphics_template *entry =
      rzalloc(linked, struct agx_g17p_graphics_template);
   if (!entry)
      return agx3_g17p_graphics_program_write_unpublished(
         chip, desc, output, capacity, written);

   entry->desc = *desc;
   memcpy(entry->ranges, desc->uniform_ranges,
          desc->uniform_range_count * sizeof(entry->ranges[0]));
   entry->desc.uniform_ranges = entry->ranges;
   if (!agx3_g17p_graphics_program_template_generate(
          chip, &entry->desc, &entry->program) ||
       entry->program.program.length > capacity) {
      ralloc_free(entry);
      return false;
   }

   /* Bound memory and lookup cost even when an application keeps changing
    * its uniform allocation layout. The coldest entry is evicted. */
   if (count == AGX_G17P_GRAPHICS_TEMPLATE_LIMIT) {
      slot = &linked->g17p_graphics_templates;
      while ((*slot)->next)
         slot = &(*slot)->next;
      ralloc_free(*slot);
      *slot = NULL;
   }
   entry->next = linked->g17p_graphics_templates;
   linked->g17p_graphics_templates = entry;
   *written = entry->program.program.length;
   memcpy(output, entry->program.program.bytes, *written);
   return true;
}

static uint32_t
agx_build_g17p_graphics_program(struct agx_batch *batch,
                                struct agx_compiled_shader *cs,
                                struct agx_linked_shader *linked,
                                uint64_t *full_va)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);
   if (!linked || !linked->bo || !linked->bo->va || !linked->size ||
       cs->b.info.has_preamble || cs->b.info.preamble_scratch_size)
      return 0;

   /* Indirect records change their vertex parameters, not their fragment
    * inputs. Keep the immutable fragment setup in this batch instead of
    * rebuilding and uploading it for every record. Be conservative about
    * all fixed-function and fragment state; shader changes are checked too.
    * The cache is cleared whenever the batch's executable pool is replaced. */
   const unsigned vertex_dirty =
      AGX_DIRTY_VS | AGX_DIRTY_VS_PROG | AGX_DIRTY_VERTEX;
   if (cs->stage == MESA_SHADER_FRAGMENT &&
       !(ctx->dirty & ~vertex_dirty) && batch->g17p_fragment_program &&
       batch->g17p_fragment_shader == cs &&
       batch->g17p_fragment_linked == linked) {
      if (full_va)
         *full_va = batch->g17p_fragment_program;
      return agx_usc_addr(dev, batch->g17p_fragment_program);
   }

   struct agx3_ioto_uniform_range
      ranges[AGX3_G17P_GRAPHICS_MAX_RANGES];
   size_t count = 0;
   const mesa_shader_stage stage = cs->stage;
   /* A hardware vertex program -- the vertex shader, a tessellation
    * evaluation shader run as one, or a geometry shader's copy program --
    * takes the pair exactly when it was compiled to read it. The copy
    * program samples nothing even when its geometry shader, whose texture
    * table this stage's count describes, does. */
   const bool resource_pair =
      batch->texture_count[stage] != 0 &&
      (stage == MESA_SHADER_FRAGMENT || cs->g17p_texture_resources);

   if (batch->texture_count[stage] && !resource_pair &&
       stage == MESA_SHADER_FRAGMENT)
      return 0;
   if (resource_pair) {
      if (!batch->stage_uniforms[stage].texture_base ||
          !batch->samplers[stage] ||
          (batch->ctx->stage[stage].custom_borders &&
           !cs->g17p_custom_border_emulation))
         return 0;
      const uint64_t pointers[2] = {
         batch->stage_uniforms[stage].texture_base,
         batch->samplers[stage],
      };
      const uint64_t pointer_va = agx_pool_upload_aligned(
         &batch->pool, pointers, sizeof(pointers), 8);
      if (!pointer_va)
         return 0;
      ranges[count++] = (struct agx3_ioto_uniform_range){
         .start_halfs = 0,
         .size_halfs = 8,
         .address = pointer_va,
      };
   }

   for (unsigned i = 0; i < cs->push_range_count; ++i) {
      const struct agx_push_range *push = &cs->push[i];
      const uint64_t table = batch->uniforms.tables[push->table];

      /* Fragment uniform zero is deliberately replaced by the resource pair.
       * A textured vertex/TES program instead shifts its complete fixed ABI;
       * trimming a VBO or draw-parameter range would corrupt vertex fetches. */
      if (resource_pair &&
          (stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_TESS_EVAL) &&
          push->uniform < AGX_ABI_G17P_TEXTURE_POINTER_HALFS)
         return 0;

      /* Draw parameters are intentionally absent for draws whose shader does
       * not consume them. This is the same omission as the legacy builder. */
      if (push->table == AGX_SYSVAL_TABLE_PARAMS && !table)
         continue;
      unsigned start = push->uniform;
      unsigned length = push->length;
      uint64_t source = table + push->offset;
      const bool blend_constants =
         resource_pair && stage == MESA_SHADER_FRAGMENT &&
         push->table == AGX_SYSVAL_TABLE_ROOT &&
         push->offset == offsetof(struct agx_draw_uniforms, blend_constant) &&
         push->uniform == AGX_ABI_FUNI_BLEND_R && push->length == 8;

      if (blend_constants) {
         if (!linked->g17p_blend_constant_base)
            return 0;
         start = linked->g17p_blend_constant_base;
      } else if (resource_pair && start < 8) {
         const unsigned end = start + length;
         if (end <= 8)
            continue;
         source += (8 - start) * 2;
         length = end - 8;
         start = 8;
      }

      /* Native Ioto moves uniform dwords. A single live 16-bit stage sysval
       * (for example the custom-border companion sentinel) therefore needs
       * its adjacent half uploaded as harmless padding. The stage table is a
       * fixed driver allocation, so prove that adjacent half is in bounds. */
      if ((length & 1) && push->table == AGX_SYSVAL_STAGE(stage)) {
         if ((uint64_t)push->offset + ((uint64_t)length + 1) * 2 >
             sizeof(struct agx_stage_uniforms))
            return 0;
         ++length;
      }
      if (!table || (start & 1) || (length & 1) ||
          count == ARRAY_SIZE(ranges))
         return 0;

      ranges[count++] = (struct agx3_ioto_uniform_range){
         .start_halfs = start,
         .size_halfs = length,
         .address = source,
      };
   }

   if (cs->b.info.rodata.size_16) {
      const struct agx_rodata *rodata = &cs->b.info.rodata;
      const uint64_t size_B = (uint64_t)rodata->size_16 * 2;
      if (!cs->bo || !cs->bo->va || (rodata->base_uniform & 1) ||
          (rodata->size_16 & 1) || count == ARRAY_SIZE(ranges) ||
          rodata->offset > cs->bo->size ||
          size_B > cs->bo->size - rodata->offset)
         return 0;
      ranges[count++] = (struct agx3_ioto_uniform_range){
         .start_halfs = rodata->base_uniform,
         .size_halfs = rodata->size_16,
         .address = cs->bo->va->addr + rodata->offset,
      };
   }

   if (stage == MESA_SHADER_FRAGMENT && linked->g17p_emrt_heap_base) {
      if (!batch->g17p_emrt_heap || count == ARRAY_SIZE(ranges))
         return 0;
      ranges[count++] = (struct agx3_ioto_uniform_range){
         .start_halfs = linked->g17p_emrt_heap_base,
         .size_halfs = 4,
         .address = batch->g17p_emrt_heap,
      };
   }

   size_t capacity;
   if (!agx3_g17p_graphics_program_bound(dev->chip, ranges, count,
                                          &capacity))
      return 0;

   struct agx_bo *program_bo = NULL;
   struct agx_ptr t = agx_pool_alloc_aligned_with_bo(
      &batch->pipeline_pool, capacity, AGX_IOTO_PROGRAM_ALIGNMENT,
      &program_bo);
   if (!t.cpu || !program_bo || !agx_va_in_usc_window(dev, t.gpu) ||
       !agx_va_in_usc_window(dev,
                            linked->bo->va->addr + linked->entry_offset))
      return 0;

   const uint32_t pass_type =
      cs->stage == MESA_SHADER_FRAGMENT
         ? linked->fragment_control.opaque[0] >> 29
         : AGX_PASS_TYPE_OPAQUE;
   const struct agx3_g17p_graphics_program_desc desc = {
      .program_va = t.gpu,
      .shader_va = linked->bo->va->addr + linked->entry_offset,
      .uniform_ranges = ranges,
      .uniform_range_count = count,
      .shader_binary_size = linked->size,
      .scratch_size =
         MAX2(linked->scratch_size, linked->agx3_spectpr_scratch_size),
      .nr_gprs = linked->nr_gprs,
      .compiler_d_valid = linked->g17p_compiler_d_valid,
      .compiler_d = linked->g17p_compiler_d,
      .fragment = cs->stage == MESA_SHADER_FRAGMENT,
      .writes_sample_mask = pass_type == AGX_PASS_TYPE_PUNCH_THROUGH ||
                            pass_type == AGX_PASS_TYPE_TRANSLUCENT_PUNCH_THROUGH,
      .colour_classes = linked->g17p_colour_classes,
      .imageblock = {
         .sample_size_B = batch->tilebuffer_layout.sample_size_B,
         .nr_samples = batch->tilebuffer_layout.nr_samples,
         .tile_size_px = batch->tilebuffer_layout.tile_size,
      },
   };
   size_t program_length;
   if (!agx_g17p_graphics_program_cached(dev->chip, linked, &desc, t.cpu,
                                         capacity, &program_length))
      return 0;

   agx_bo_assert_range(program_bo, t.gpu - program_bo->va->addr,
                       program_length);
   if (full_va)
      *full_va = t.gpu;
   if (cs->stage == MESA_SHADER_FRAGMENT) {
      batch->g17p_fragment_program = t.gpu;
      batch->g17p_fragment_shader = cs;
      batch->g17p_fragment_linked = linked;
   }
   return agx_usc_addr(dev, t.gpu);
}

static uint32_t
agx_build_pipeline(struct agx_batch *batch, struct agx_compiled_shader *cs,
                   struct agx_linked_shader *linked,
                   mesa_shader_stage phys_stage, unsigned variable_shared_mem,
                   size_t max_subgroups, uint64_t *full_va)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   if (agx_native_graphics(dev) && phys_stage != MESA_SHADER_COMPUTE)
      return agx_build_g17p_graphics_program(batch, cs, linked, full_va);

   unsigned constant_push_ranges = DIV_ROUND_UP(cs->b.info.rodata.size_16, 64);

   size_t usc_size =
      agx_usc_size(constant_push_ranges + cs->push_range_count + 2);

   struct agx_ptr t =
      agx_pool_alloc_aligned(&batch->pipeline_pool, usc_size, 64);

   struct agx_usc_builder b = agx_usc_builder(t.cpu, usc_size);

   mesa_shader_stage stage = cs->stage;

   if (batch->texture_count[stage]) {
      agx_usc_pack(&b, TEXTURE, cfg) {
         cfg.start = 0;
         cfg.count =
            MIN2(batch->texture_count[stage], AGX_NUM_TEXTURE_STATE_REGS);
         cfg.buffer = batch->stage_uniforms[stage].texture_base;
      }
   }

   if (batch->sampler_count[stage]) {
      agx_usc_pack(&b, SAMPLER, cfg) {
         cfg.start = 0;
         cfg.count = batch->sampler_count[stage];
         cfg.buffer = batch->samplers[stage];
      }
   }

   for (unsigned i = 0; i < cs->push_range_count; ++i) {
      unsigned table = cs->push[i].table;
      uint64_t table_ptr = batch->uniforms.tables[table];

      /* Params may be omitted if the VS prolog does not read them, but the
       * reservation is always there in the API shader just in case.
       */
      if (table == AGX_SYSVAL_TABLE_PARAMS && !table_ptr)
         continue;

      assert(table_ptr);

      agx_usc_uniform(&b, cs->push[i].uniform, cs->push[i].length,
                      table_ptr + cs->push[i].offset);
   }

   if (cs->bo) {
      /* HAL200 reconstructs executable-window uniform pointers relative to
       * USC_EXEC_BASE, just like shader pointers.  Passing the full G17P VA
       * to the legacy 40-bit USC Uniform packer either trips its bounds check
       * or relies on accidental truncation in release builds. */
      const uint64_t rodata_base =
         agx_native_graphics(dev) ? agx_usc_addr(dev, cs->bo->va->addr)
                                    : cs->bo->va->addr;
      agx_usc_immediates(&b, &cs->b.info.rodata, rodata_base);
   }

   uint32_t max_scratch_size =
      MAX2(cs->b.info.scratch_size, cs->b.info.preamble_scratch_size);

   if (max_scratch_size > 0) {
      unsigned preamble_size = (cs->b.info.preamble_scratch_size > 0) ? 1 : 0;

      switch (phys_stage) {
      case MESA_SHADER_FRAGMENT:
         agx_scratch_alloc(&ctx->scratch_fs, max_scratch_size, max_subgroups);
         batch->fs_scratch = true;
         batch->fs_preamble_scratch =
            MAX2(batch->fs_preamble_scratch, preamble_size);
         break;
      case MESA_SHADER_VERTEX:
         agx_scratch_alloc(&ctx->scratch_vs, max_scratch_size, max_subgroups);
         batch->vs_scratch = true;
         batch->vs_preamble_scratch =
            MAX2(batch->vs_preamble_scratch, preamble_size);
         break;
      default:
         agx_scratch_alloc(&ctx->scratch_cs, max_scratch_size, max_subgroups);
         batch->cs_scratch = true;
         batch->cs_preamble_scratch =
            MAX2(batch->cs_preamble_scratch, preamble_size);
         break;
      }
   }

   if (stage == MESA_SHADER_FRAGMENT) {
      agx_usc_push_packed(&b, SHARED, &batch->tilebuffer_layout.usc);
   } else {
      agx_usc_shared_non_fragment(&b, &cs->b.info, variable_shared_mem);
   }

   if (linked) {
      agx_usc_push_packed(&b, SHADER, linked->shader);
      agx_usc_push_packed(&b, REGISTERS, linked->regs);

      if (stage == MESA_SHADER_FRAGMENT)
         agx_usc_push_packed(&b, FRAGMENT_PROPERTIES, linked->fragment_props);
   } else {
      agx_usc_pack(&b, SHADER, cfg) {
         cfg.code =
            agx_usc_addr(dev, cs->bo->va->addr + cs->b.info.main_offset);
         cfg.unk_2 = 3;
      }

      agx_usc_pack(&b, REGISTERS, cfg) {
         cfg.register_count = cs->b.info.nr_gprs;
         cfg.spill_size = cs->b.info.scratch_size
                             ? agx_scratch_get_bucket(cs->b.info.scratch_size)
                             : 0;
      }
   }

   if (cs->b.info.has_preamble) {
      agx_usc_pack(&b, PRESHADER, cfg) {
         cfg.code =
            agx_usc_addr(dev, cs->bo->va->addr + cs->b.info.preamble_offset);
      }
   } else {
      agx_usc_pack(&b, NO_PRESHADER, cfg)
         ;
   }

   if (full_va)
      *full_va = t.gpu;

   return agx_usc_addr(dev, t.gpu);
}

static bool
agx_g17p_compute_failed(const char *step)
{
   mesa_loge("asahi: G17P compute dispatch failed at: %s", step);
   return false;
}

/*
 * A fast-linked program (a software vertex shader behind the prolog that
 * fetches its attributes and publishes the vertex and instance IDs from the
 * invocation index) already lives in its own AGX3 code container, so it is
 * dispatched from there; a bare main program is wrapped in a container here.
 */
static bool
agx_g17p_append_compute(struct agx_batch *batch,
                        const struct agx_shader_info *info,
                        const void *binary, struct agx_bo *constant_bo,
                        const struct agx_linked_shader *linked,
                        struct agx_grid grid, struct agx_workgroup wg,
                        const struct agx3_ioto_uniform_range *ranges,
                        size_t range_count, unsigned variable_shared_mem)
{
   struct agx_device *dev = agx_device(batch->ctx->base.screen);
   /* Mode 2 reads the whole grid -- three global thread counts then three
    * local sizes -- from six u32 in GPU memory, so the workgroup this call
    * was handed is not the one that launches and must not be validated. */
   const bool indirect_local = grid.mode == AGX_CDM_MODE_INDIRECT_LOCAL;
   /* The compiler places promoted constants ahead of main and pads main to
    * 0x80, so a shader with rodata has main_offset >= 0x80 and only rodata
    * before it.  Those constants are published as a uniform range by the
    * caller; the code block gets the generated compute constant program, as
    * the D3D12 native compute path already does. */
   const bool rodata_prefix =
      info && info->rodata.size_16 && info->main_offset &&
      !(info->main_offset % 0x80) &&
      info->rodata.offset + (uint64_t)info->rodata.size_16 * 2 <=
         info->main_offset;
   /* The thread-private frame SPECTPR declares: register spills, and the
    * 16-byte word a device atomic returns its old value through. Declaring
    * less than the code uses is not refused by anything; the dispatch just
    * never completes. Same frame hk_cmd_dispatch.c declares. */
   const uint32_t tps_size =
      !info ? 0
      : linked ? MAX2(linked->scratch_size, linked->agx3_spectpr_scratch_size)
               : MAX2(info->scratch_size, info->agx3_spectpr_scratch_size);
   if (!agx_native_graphics(dev) || !info || !binary ||
       (grid.mode != AGX_CDM_MODE_DIRECT && !indirect_local) ||
       (indirect_local &&
        (!grid.ptr || grid.ptr >= (UINT64_C(1) << 48) || (grid.ptr & 3))) ||
       !info->main_size || !info->nr_gprs ||
       info->has_preamble || info->preamble_scratch_size ||
       tps_size > AGX3_G17P_MAX_SPILL_B || (tps_size & 3) ||
       !info->agx3_staging_known ||
       (info->main_offset != 0 && !rodata_prefix &&
        info->main_offset != AGX3_CONTAINER_CONSTANT_PROGRAM_SIZE) ||
       info->main_offset > info->binary_size ||
       info->main_size > info->binary_size - info->main_offset ||
       (range_count && !ranges) ||
       (!indirect_local && (!wg.x || !wg.y || !wg.z)) ||
       (linked && (!linked->bo || !linked->bo->va || !linked->size ||
                   !linked->nr_gprs || !linked->g17p_compiler_d_valid))) {
      if (info) {
         mesa_loge("asahi: rejected G17P compute metadata: grid=%u "
                   "preamble=%u scratch=%u/%u main=%u+%u/%u "
                   "gprs=%u staging_known=%u workgroup=%ux%ux%u",
                   (unsigned)grid.mode, (unsigned)info->has_preamble,
                   info->preamble_scratch_size, info->scratch_size,
                   info->main_offset, info->main_size, info->binary_size,
                   info->nr_gprs, (unsigned)info->agx3_staging_known,
                   wg.x, wg.y, wg.z);
      }
      return false;
   }

   /* An empty direct grid -- a geometry or transform feedback setup kernel
    * for a draw with nothing to process -- launches no invocations. The
    * native launch record cannot encode a zero size, so emit nothing, as
    * the hardware would have done with the legacy record. */
   if (!indirect_local &&
       (!grid.count[0] || !grid.count[1] || !grid.count[2]))
      return true;

   uint64_t shader_va;
   uint32_t shader_binary_size, nr_gprs, compiler_d;
   struct agx_bo *shader_bo = NULL;
   size_t shader_sync_offset = 0, shader_sync_size = 0;

   if (linked) {
      shader_va = linked->bo->va->addr + linked->entry_offset;
      shader_binary_size = linked->size;
      nr_gprs = linked->nr_gprs;
      compiler_d = linked->g17p_compiler_d;
   } else {
      const size_t block_size = agx3_code_block_size(info->main_size);
      if (!block_size || block_size > SIZE_MAX - AGX3_CONTAINER_LEAD_SIZE)
         return agx_g17p_compute_failed("code block size");

      struct agx_ptr shader = agx_pool_alloc_aligned_with_bo(
         &batch->pipeline_pool, AGX3_CONTAINER_LEAD_SIZE + block_size, 128,
         &shader_bo);
      if (!shader.cpu || !shader_bo)
         return agx_g17p_compute_failed("shader allocation");

      agx3_build_container_lead(shader.cpu, dev->chip,
                                dev->params.num_cores_per_cluster);
      const uint8_t *bytes = binary;
      if (!agx3_build_code_block(
             (uint8_t *)shader.cpu + AGX3_CONTAINER_LEAD_SIZE, block_size,
             bytes + info->main_offset, info->main_size, info->main_size,
             info->main_offset && !rodata_prefix ? bytes : NULL, false))
         return agx_g17p_compute_failed("code block build");
      shader_va =
         shader.gpu + AGX3_CONTAINER_LEAD_SIZE + AGX3_CODE_BLOCK_ENTRY_OFFSET;
      shader_binary_size = info->main_size;
      nr_gprs = info->nr_gprs;
      compiler_d = (uint32_t)info->agx3_staging_halfs * 2;
      shader_sync_offset = shader.gpu - shader_bo->va->addr;
      shader_sync_size = AGX3_CONTAINER_LEAD_SIZE + block_size;
   }

   struct agx_bo *program_bo = NULL;
   struct agx_ptr program = agx_pool_alloc_aligned_with_bo(
      &batch->pipeline_pool, AGX3_G17P_COMPUTE_PROGRAM_MAX_SIZE,
      AGX_IOTO_PROGRAM_ALIGNMENT, &program_bo);
   if (!program.cpu || !program_bo || !agx_va_in_usc_window(dev, shader_va) ||
       !agx_va_in_usc_window(dev, program.gpu))
      return agx_g17p_compute_failed("program allocation or USC window");

   struct agx3_cdm_direct_template_sources launch_sources;
   if (!agx3_cdm_direct_template_sources_compute(
          info->uses_resource_info_query &&
             agx_chip_query_ti_spill(dev->chip),
          info->uses_ray_traversal, info->ray_traversal_depth_nonzero,
          &launch_sources))
      return agx_g17p_compute_failed("launch template");

   const uint64_t shared = (uint64_t)info->local_size + variable_shared_mem;
   if (shared > UINT32_MAX)
      return agx_g17p_compute_failed("shared memory size");
   const struct agx3_g17p_compute_program_desc desc = {
      .program_va = program.gpu,
      .shader_va = shader_va,
      .uniform_ranges = ranges,
      .uniform_range_count = range_count,
      .shader_binary_size = shader_binary_size,
      .scratch_size = tps_size,
      .nr_gprs = nr_gprs,
      .compiler_d_valid = true,
      .compiler_d = compiler_d,
      .threadgroup_memory_size_B =
         shared ? MAX2(256u, ALIGN_POT((uint32_t)shared, 64u)) : 0,
      .launch_sources = launch_sources,
      /* The program announces how it will be launched.  An indirectly
       * launched one takes its dimensions from the geometry block; a record
       * that selects indirect mode while its program still announces
       * DIRECT_ADD3 runs no invocations at all.  hk_cmd_dispatch.c picks the
       * kind the same way. */
      .ldshdr_f1 = indirect_local ? AGX3_G17P_INDIRECT_MAIN_LDSHDR_F1
                                  : AGX_IOTO_G17P_COMPUTE_LDSHDR_F1_DIRECT_ADD3,
   };
   struct agx3_g17p_compute_program native;
   if (!agx3_g17p_compute_program_generate(dev->chip, &desc, &native))
      return agx_g17p_compute_failed("Ioto program generation");
   memcpy(program.cpu, native.bytes, native.length);

   if (shader_bo)
      agx_bo_assert_range(shader_bo, shader_sync_offset, shader_sync_size);
   agx_bo_assert_range(program_bo, program.gpu - program_bo->va->addr,
                       native.length);

   struct agx3_compute_dispatch_sink sink = {
      .data = batch->cdm.current,
      .capacity = batch->cdm.end - batch->cdm.current,
   };
   /* The software graphics stages (vertex, tessellation and geometry
    * programs run as compute) hand their results to the next dispatch of the
    * stream through memory, with no barrier of their own in between. The
    * launch's weak tail orders them on HAL200. On G15, a dependency inside
    * the stream is published only by the wide flush, so there every launch
    * ends in it, as Honeykrisp's software stages do. */
   const uint8_t barrier_flags =
      agx_chip_cdm_wide_visibility(dev->chip) ? 0x5 : 0;
   if (indirect_local) {
      /* The kernel that writes this grid is already its own dispatch earlier
       * in the stream, behind a full barrier, so the launch stands alone. */
      if (!agx3_g17p_native_indirect_compute_launch_append(
             &native.launch, grid.ptr, barrier_flags, &sink))
         return agx_g17p_compute_failed("indirect launch append");
   } else {
      const struct agx3_compute_direct_grid direct = {
         .source = AGX3_CDM_DIRECT_GRID_GLOBAL_THREADS,
         .size = {grid.count[0], grid.count[1], grid.count[2]},
         .local_size = {wg.x, wg.y, wg.z},
         .dimensions = grid.count[2] != 1 || wg.z != 1
                          ? 3
                       : grid.count[1] != 1 || wg.y != 1 ? 2 : 1,
      };
      if (!agx3_g17p_native_compute_dispatch_append_with_barrier(
             &native.launch, &direct, barrier_flags, &sink))
         return agx_g17p_compute_failed("direct launch append");
   }
   batch->cdm.current += sink.length;
   if (constant_bo)
      agx_batch_add_bo(batch, constant_bo);
   if (linked)
      agx_batch_add_bo(batch, linked->bo);
   return true;
}

static void
agx_launch_internal(struct agx_batch *batch, struct agx_grid grid,
                    struct agx_workgroup wg,
                    struct agx_cdm_launch_word_0_packed launch,
                    mesa_shader_stage stage, uint32_t usc)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   /* TODO: Ensure space if we allow multiple kernels in a batch */
   uint32_t *out = (uint32_t *)batch->cdm.current;

   out = agx_cdm_launch(out, dev->chip, grid, wg, launch, usc);
   out = agx_cdm_barrier(out, dev->chip);

   batch->cdm.current = (void *)out;
   assert(batch->cdm.current <= batch->cdm.end &&
          "Failed to reserve sufficient space in encoder");
}

void
agx_launch_precomp(struct agx_batch *batch, struct agx_grid grid,
                   enum agx_barrier barrier, enum libagx_program program,
                   void *args, size_t arg_size)
{
   struct agx_device *dev = agx_device(batch->ctx->base.screen);
   struct agx_precompiled_shader *cs =
      agx_get_precompiled(&batch->ctx->bg_eot, program);

   if (agx_native_graphics(dev)) {
      if (!cs || !cs->bo || !cs->info.main_size || !cs->info.nr_gprs ||
          cs->info.nr_preamble_gprs ||
          (cs->info.rodata.size_16 && !cs->rodata_offset) ||
          (cs->info.printf_uniform &&
           cs->info.printf_uniform < AGX_ABI_AGX3_PRECOMP_ARG_HALFS) ||
          !cs->info.agx3_staging_known || !args || !arg_size ||
          arg_size > UINT16_MAX * 2u ||
          cs->entry_offset > UINT64_MAX - cs->ptr ||
          !cs->b.workgroup.x || !cs->b.workgroup.y || !cs->b.workgroup.z ||
          (agx_is_indirect(grid) &&
           (grid.mode != AGX_CDM_MODE_INDIRECT_LOCAL || !grid.ptr ||
            grid.ptr >= (UINT64_C(1) << 48) || (grid.ptr & 3)))) {
         mesa_loge("asahi: refusing precompiled program %u on G17P: "
                   "grid=%u preamble_gprs=%u rodata=%u/%u printf=%u "
                   "staging_known=%u args=%zu",
                   (unsigned)program, (unsigned)grid.mode,
                   cs ? cs->info.nr_preamble_gprs : 0,
                   cs ? cs->info.rodata.size_16 : 0,
                   cs ? (unsigned)cs->rodata_offset : 0,
                   cs ? cs->info.printf_uniform : 0,
                   cs ? (unsigned)cs->info.agx3_staging_known : 0, arg_size);
         agx_native_descriptor_rejected(dev, "precompiled compute program");
      }

      /* Mode 2 reads the whole grid -- three global thread counts, then
       * three local sizes -- from six u32 in GPU memory, as
       * libagx_tess_setup_indirect() writes them for the tessellation
       * kernels. */
      const bool indirect_local = grid.mode == AGX_CDM_MODE_INDIRECT_LOCAL;
      if (!indirect_local &&
          (!grid.count[0] || !grid.count[1] || !grid.count[2]))
         return;

      /* Match the precompiler's rebased arguments and optional runtime printf
       * address. Pad packed argument structs for Ioto's whole-dword loads. */
      const size_t printf_offset = cs->info.printf_uniform
         ? (size_t)(cs->info.printf_uniform - AGX_ABI_AGX3_PRECOMP_ARG_HALFS) * 2
         : 0;
      const size_t upload_size = ALIGN_POT(
         cs->info.printf_uniform ? MAX2(arg_size, printf_offset + 8) : arg_size, 4);
      struct agx_ptr uploaded = agx_pool_alloc_aligned(&batch->pool, upload_size, 4);
      if (!uploaded.cpu || upload_size / 2 > UINT16_MAX)
         agx_native_descriptor_rejected(dev, "precompiled argument upload");
      memcpy(uploaded.cpu, args, arg_size);
      memset((uint8_t *)uploaded.cpu + arg_size, 0, upload_size - arg_size);
      if (cs->info.printf_uniform)
         memcpy((uint8_t *)uploaded.cpu + printf_offset,
                &dev->printf_buffer_address, sizeof(uint64_t));
      const uint64_t uploaded_data = uploaded.gpu;
      /* Above the halves the AGX3 hardware ABI owns, matching where the
       * precompiler now places these loads. */
      const struct agx3_ioto_uniform_range range = {
         .start_halfs = AGX_ABI_AGX3_PRECOMP_ARG_HALFS,
         .size_halfs = upload_size / 2,
         .address = uploaded_data,
      };

      /* Constants the compiler promoted out of the code are published as a
       * second range, exactly as the legacy USC path maps them with
       * agx_usc_immediates(). Their base is already in this ABI's numbering:
       * agx_opt_promote_constants() allocates above push_count, which starts
       * at the reserved preamble. Without this the kernel reads whatever the
       * uniform file happened to hold -- which is why the launch-shape check
       * refused a kernel carrying rodata rather than dispatching one. */
      struct agx3_ioto_uniform_range ranges[2] = {range};
      unsigned range_count = 1;
      if (cs->info.rodata.size_16) {
         ranges[range_count++] = (struct agx3_ioto_uniform_range){
            .start_halfs = cs->info.rodata.base_uniform,
            .size_halfs = cs->info.rodata.size_16,
            .address = cs->ptr + cs->rodata_offset,
         };
      }
      struct agx3_cdm_direct_template_sources launch_sources;
      if (!uploaded_data ||
          !agx3_cdm_direct_template_sources_compute(false, false, false,
                                                     &launch_sources))
         agx_native_descriptor_rejected(dev, "precompiled compute resources");

      struct agx_bo *program_bo = NULL;
      struct agx_ptr native_program = agx_pool_alloc_aligned_with_bo(
         &batch->pipeline_pool, AGX3_G17P_COMPUTE_PROGRAM_MAX_SIZE,
         AGX_IOTO_PROGRAM_ALIGNMENT, &program_bo);
      const struct agx3_g17p_compute_program_desc desc = {
         .program_va = native_program.gpu,
         .shader_va = cs->ptr + cs->entry_offset,
         .uniform_ranges = ranges,
         .uniform_range_count = range_count,
         .shader_binary_size = cs->info.main_size,
         /* The kernel's thread-private spill frame, as hk declares it. The
          * software tessellator's emitting pass spills (68 bytes for
          * triangles); launched with no frame it never completes, which is
          * how every tessellated draw with a level above one used to time
          * out the compute queue. */
         .scratch_size = cs->info.agx3_tps_size,
         .nr_gprs = cs->info.nr_gprs,
         .compiler_d_valid = cs->info.agx3_staging_known,
         .compiler_d = (uint32_t)cs->info.agx3_staging_halfs * 2,
         .launch_sources = launch_sources,
         /* Precompiled kernels use threadgroup memory too -- the restart
          * unroll and the prefix sums keep their cross-subgroup scratch
          * there -- and the API compute path above declares it while this
          * one did not.  Leaving it zero does not fail the dispatch, so the
          * shortfall is silent.  Same rounding as that path. */
         .threadgroup_memory_size_B =
            cs->info.local_size
               ? MAX2(256u, ALIGN_POT(cs->info.local_size, 64u))
               : 0,
         /* The program announces how it is launched, as the API compute
          * path's does (agx_g17p_append_compute()). */
         .ldshdr_f1 = indirect_local
                         ? AGX3_G17P_INDIRECT_MAIN_LDSHDR_F1
                         : AGX_IOTO_G17P_COMPUTE_LDSHDR_F1_DIRECT_ADD3,
      };
      struct agx3_g17p_compute_program native;
      if (!native_program.cpu || !program_bo ||
          !agx_va_in_usc_window(dev, native_program.gpu) ||
          !agx_va_in_usc_window(dev, desc.shader_va) ||
          !agx3_g17p_compute_program_generate(dev->chip, &desc, &native))
         agx_native_descriptor_rejected(dev, "precompiled compute Ioto");
      memcpy(native_program.cpu, native.bytes, native.length);
      agx_bo_assert_range(program_bo, native_program.gpu - program_bo->va->addr,
                          native.length);

      const struct agx3_compute_direct_grid direct = {
         .source = AGX3_CDM_DIRECT_GRID_GLOBAL_THREADS,
         .size = {grid.count[0], grid.count[1], grid.count[2]},
         .local_size = {cs->b.workgroup.x, cs->b.workgroup.y,
                        cs->b.workgroup.z},
         .dimensions = grid.count[2] != 1 || cs->b.workgroup.z != 1
                          ? 3
                       : grid.count[1] != 1 || cs->b.workgroup.y != 1 ? 2 : 1,
      };
      struct agx3_compute_dispatch_sink sink = {
         .data = batch->cdm.current,
         .capacity = batch->cdm.end - batch->cdm.current,
      };
      /* Meta copies and fills feed later graphics and CPU transfers. Preserve
       * AGX_BARRIER_ALL's cache-visibility contract with Apple's grounded
       * strong post-dispatch barrier. Direct API compute remains on the
       * existing minimal flags-0 barrier. */
      const uint8_t barrier_flags =
         (barrier & AGX_BARRIER_ALL) ? 0x5 : 0;
      if (indirect_local
             ? !agx3_g17p_native_indirect_compute_launch_append(
                  &native.launch, grid.ptr, barrier_flags, &sink)
             : !agx3_g17p_native_compute_dispatch_append_with_barrier(
                  &native.launch, &direct, barrier_flags, &sink))
         agx_native_descriptor_rejected(dev, "precompiled compute launch");
      batch->cdm.current += sink.length;
      agx_batch_add_bo(batch, cs->bo);
      return;
   }

   struct agx_ptr t =
      agx_pool_alloc_aligned(&batch->pipeline_pool, agx_usc_size(15), 64);

   uint64_t uploaded_data =
      agx_pool_upload_aligned(&batch->pool, args, arg_size, 4);

   uint32_t usc = agx_usc_addr(dev, t.gpu);
   agx_usc_words_precomp(t.cpu, &cs->b, uploaded_data, arg_size);

   agx_batch_add_bo(batch, cs->bo);
   agx_launch_internal(batch, grid, cs->b.workgroup, cs->b.launch,
                       MESA_SHADER_COMPUTE, usc);
}

/*
 * Background or end-of-tile program for a G17P multi-target layout that the
 * dedicated RT0/dual/wide programs cannot express: mixed formats, or targets
 * that do not share one operation. Only the targets the operation touches are
 * numbered, each keeping its tile offset; reloads come before clears, because
 * a reload reads its texture and sampler pointers from uniform halves 0..7,
 * which a target-zero clear value would otherwise overlap. This is the
 * Gallium form of hk_build_g17p_mixed_bg_eot().
 */
static struct asahi_bg_eot
agx_g17p_build_mixed_bg_eot(struct agx_batch *batch,
                            const struct agx_bg_eot_key *framebuffer_key,
                            bool store)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);
   const enum agx_descriptor_generation descriptor_generation =
      agx_gallium_descriptor_generation(dev->chip);
   const unsigned descriptor_size =
      agx_gallium_texture_descriptor_size(descriptor_generation);
   const unsigned samples = MAX2(framebuffer_key->tib.nr_samples, 1);

   /* Multisample targets are stored unresolved; a resolve or a
    * single-sample WSI reload needs the single-target programs. */
   if (samples > 1 && (batch->key.resolve || framebuffer_key->single_sample_reload))
      agx_native_descriptor_rejected(dev, "multisample mixed MRT resolve");
   if (descriptor_size != AGX_IOTO_G17P_RT0_PBE_ARGUMENT_SIZE ||
       agx_gallium_pbe_descriptor_size(descriptor_generation) !=
          AGX_IOTO_G17P_RT0_PBE_ARGUMENT_SIZE)
      agx_native_descriptor_rejected(dev, "mixed MRT descriptor geometry");

   struct agx_bg_eot_key key = *framebuffer_key;
   memset(key.tib.logical_format, 0, sizeof(key.tib.logical_format));
   memset(key.tib.spilled, 0, sizeof(key.tib.spilled));
   memset(key.tib._offset_B, 0, sizeof(key.tib._offset_B));
   memset(key.op, 0, sizeof(key.op));
   key.mixed_mrt = true;
   key.single_sample_reload = false;
   key.multisample_store = store && samples > 1;
   key.reserved_preamble = 0;

   static const enum agx_bg_eot_op order[] = {AGX_BG_LOAD, AGX_BG_CLEAR,
                                              AGX_EOT_STORE};
   const struct pipe_surface *surfs[PIPE_MAX_COLOR_BUFS];
   unsigned rts[PIPE_MAX_COLOR_BUFS];
   unsigned count = 0, reloads = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(order); ++i) {
      for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
         if (framebuffer_key->op[rt] != order[i])
            continue;

         const struct pipe_surface *surf = &batch->key.cbufs[rt];
         if (!surf->texture ||
             util_res_sample_count(surf->texture) != samples)
            agx_native_descriptor_rejected(dev, "mixed MRT attachment");

         key.tib.logical_format[count] =
            framebuffer_key->tib.logical_format[rt];
         key.tib._offset_B[count] = framebuffer_key->tib._offset_B[rt];
         key.op[count] = order[i];
         rts[count] = rt;
         surfs[count++] = surf;
         reloads += order[i] == AGX_BG_LOAD;
      }
   }
   assert(count > 0 && "uniform layouts take the dedicated programs");

   const bool clears = !store && count > reloads;
   if (clears)
      key.reserved_preamble = 4 + 8 * count;

   struct agx_bg_eot_shader *shader = agx_get_bg_eot_shader(&ctx->bg_eot, &key);
   if (!shader || !shader->bo || !shader->bo->va || !shader->ptr ||
       !shader->info.binary_size ||
       shader->info.main_offset >= shader->info.binary_size ||
       shader->info.main_size >
          shader->info.binary_size - shader->info.main_offset ||
       shader->info.has_preamble || shader->info.rodata.size_16 ||
       !shader->info.nr_gprs ||
       (!store && (shader->info.push_count != key.reserved_preamble ||
                   shader->info.uses_txf != (reloads > 0)))) {
      mesa_loge("asahi: G17P mixed MRT %s shader is unproven: push=%u "
                "want=%u txf=%u count=%u reloads=%u samples=%u",
                store ? "end-of-tile" : "background",
                shader ? shader->info.push_count : 0, key.reserved_preamble,
                shader ? shader->info.uses_txf : 0, count, reloads, samples);
      agx_native_descriptor_rejected(dev, "mixed MRT shader");
   }
   agx_batch_add_bo(batch, shader->bo);

   const enum agx3_g17p_rt0_bg_op bg_op =
      reloads == count ? AGX3_G17P_RT0_BG_RELOAD
      : reloads        ? AGX3_G17P_RT0_BG_CLEAR_RELOAD
                       : AGX3_G17P_RT0_BG_CLEAR;
   const size_t capacity =
      store       ? (count > 4 ? AGX_IOTO_G17P_OCTA_EOT_PROGRAM_MAX_SIZE
                               : AGX_IOTO_G17P_QUAD_EOT_PROGRAM_MAX_SIZE)
      : count > 4 ? AGX_IOTO_G17P_OCTA_BACKGROUND_PROGRAM_MAX_SIZE
      : bg_op == AGX3_G17P_RT0_BG_CLEAR_RELOAD
         ? AGX_IOTO_G17P_CLEAR_RELOAD_PROGRAM_MAX_SIZE
         : AGX_IOTO_G17P_RT0_RELOAD_PROGRAM_MAX_SIZE;
   struct agx_bo *program_bo = NULL;
   struct agx_ptr program_ptr = agx_pool_alloc_aligned_with_bo(
      &batch->pipeline_pool, capacity, AGX_IOTO_PROGRAM_ALIGNMENT, &program_bo);
   if (!program_ptr.cpu || !program_bo ||
       !agx_va_in_usc_window(dev, program_ptr.gpu) ||
       !agx_va_in_usc_window(dev, shader->ptr + shader->info.main_offset))
      agx_native_descriptor_rejected(dev, "mixed MRT program allocation");

   const struct agx_ioto_g17p_imageblock imageblock = {
      .sample_size_B = key.tib.sample_size_B,
      .nr_samples = key.tib.nr_samples,
      .tile_size_px = key.tib.tile_size,
   };
   size_t program_length;
   uint64_t resource_spec;
   if (store) {
      struct agx_ptr pbe =
         agx_pool_alloc_aligned(&batch->pool, descriptor_size * count, 32);
      for (unsigned i = 0; i < count; ++i) {
         struct pipe_image_view view = image_view_for_surface(surfs[i]);
         view.format = agx_g17p_rt0_pbe_format(view.format);
         const bool no_compress =
            batch->feedback & (PIPE_CLEAR_COLOR0 << rts[i]);
         if (!agx_batch_upload_pbe(
                batch, agx_pbe_descriptor_at(pbe.cpu, descriptor_size, i),
                &view, true, true, false, no_compress))
            agx_native_descriptor_rejected(dev, "mixed MRT end-of-tile PBE");
      }

      const struct agx3_g17p_rt0_eot_program_desc desc = {
         .program_va = program_ptr.gpu,
         .shader_va = shader->ptr + shader->info.main_offset,
         .pbe_argument_va = pbe.gpu,
         .imageblock_offset_B = key.tib._offset_B[0],
         .attachment_count = count,
         .imageblock_offset1_B = key.tib._offset_B[1],
         .imageblock_offset2_B = key.tib._offset_B[2],
         .imageblock_offset3_B = key.tib._offset_B[3],
         .imageblock_offset4_B = key.tib._offset_B[4],
         .imageblock_offset5_B = key.tib._offset_B[5],
         .imageblock_offset6_B = key.tib._offset_B[6],
         .imageblock_offset7_B = key.tib._offset_B[7],
         .mixed_mrt = count > 1,
         .shader_binary_size = shader->info.binary_size,
         .scratch_size = shader->info.scratch_size,
         .nr_gprs = shader->info.nr_gprs,
         .staging_halfs = shader->info.agx3_staging_known
                             ? shader->info.agx3_staging_halfs
                             : 0,
         .imageblock = imageblock,
      };
      struct agx3_g17p_rt0_eot_program native;
      if (!agx3_g17p_rt0_eot_program_generate(dev->chip, &desc, &native) ||
          native.length > capacity)
         agx_native_descriptor_rejected(dev, "mixed MRT end-of-tile program");
      memcpy(program_ptr.cpu, native.bytes, native.length);
      program_length = native.length;
      resource_spec = AGX_IOTO_G17P_EOT_RESOURCE_SPEC;
   } else {
      struct agx3_g17p_rt0_bg_program_desc desc = {
         .program_va = program_ptr.gpu,
         .shader_va = shader->ptr + shader->info.main_offset,
         .shader_binary_size = shader->info.binary_size,
         .scratch_size = shader->info.scratch_size,
         .nr_gprs = shader->info.nr_gprs,
         .staging_halfs = shader->info.agx3_staging_halfs,
         .clear_push_halfs = shader->info.push_count,
         .clear_start_halfs = 4 + 8 * reloads,
         .staging_known = shader->info.agx3_staging_known,
         .op = bg_op,
         .imageblock = imageblock,
      };

      if (clears) {
         union pipe_color_union clear_values[PIPE_MAX_COLOR_BUFS];
         for (unsigned i = reloads; i < count; ++i) {
            if (!batch->uploaded_clear_color[rts[i]])
               agx_native_descriptor_rejected(dev, "mixed MRT clear value");
            clear_values[i - reloads] = batch->clear_color[rts[i]];
         }
         desc.clear_value_va = agx_pool_upload_aligned(
            &batch->pool, clear_values,
            sizeof(clear_values[0]) * (count - reloads), 16);
         if (!desc.clear_value_va)
            agx_native_descriptor_rejected(dev, "mixed MRT clear upload");
      }

      if (reloads) {
         /* Reload n reads native texture index 2n; the odd entries stay
          * zero. */
         const size_t texture_size = descriptor_size * (2 * reloads - 1);
         struct agx_ptr textures =
            agx_pool_alloc_aligned(&batch->pool, texture_size, 64);
         memset(textures.cpu, 0, texture_size);
         for (unsigned i = 0; i < reloads; ++i) {
            struct pipe_sampler_view sampler_view =
               sampler_view_for_surface(surfs[i]);
            if (!agx_pack_texture(
                   descriptor_generation,
                   agx_texture_descriptor_at(textures.cpu, descriptor_size,
                                             2 * i),
                   agx_resource(surfs[i]->texture), surfs[i]->format,
                   &sampler_view))
               agx_native_descriptor_rejected(dev, "mixed MRT texture");
         }

         const unsigned sampler_size =
            agx_chip_sampler_heap_element_size(dev->chip);
         if (sampler_size != AGX_GALLIUM_SAMPLER_AGX3_SIZE)
            agx_native_descriptor_rejected(dev, "mixed MRT sampler geometry");
         struct agx_ptr sampler =
            agx_pool_alloc_aligned(&batch->pool, sampler_size, 64);
         if (!agx_gallium_pack_g17p_txf_sampler(dev->chip, sampler.cpu))
            agx_native_descriptor_rejected(dev, "mixed MRT sampler");

         const uint64_t pointers[2] = {textures.gpu, sampler.gpu};
         const uint64_t table = agx_pool_upload_aligned(
            &batch->pool, pointers, sizeof(pointers), 8);
         desc.texture_pointer_va = table;
         desc.sampler_pointer_va = table + sizeof(uint64_t);
      }

      struct agx3_g17p_rt0_bg_program native;
      if (!agx3_g17p_rt0_bg_program_generate(dev->chip, &desc, &native) ||
          native.length > capacity ||
          !agx3_g17p_rt0_bg_resource_spec(dev->chip, bg_op, &resource_spec))
         agx_native_descriptor_rejected(dev, "mixed MRT background program");
      memcpy(program_ptr.cpu, native.bytes, native.length);
      program_length = native.length;
   }

   agx_bo_assert_range(program_bo, program_ptr.gpu - program_bo->va->addr,
                       program_length);

   return (struct asahi_bg_eot){
      .usc = agx_usc_addr(dev, program_ptr.gpu),
      .rsrc_spec = resource_spec,
   };
}

struct asahi_bg_eot
agx_build_bg_eot(struct agx_batch *batch, bool store, bool partial_render)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);
   const enum agx_descriptor_generation descriptor_generation =
      agx_gallium_descriptor_generation(dev->chip);
   const unsigned descriptor_size =
      agx_gallium_texture_descriptor_size(descriptor_generation);
   if (!descriptor_size)
      agx_native_descriptor_rejected(dev, "background heap geometry");

   /* Construct the key */
   struct agx_bg_eot_key key = {
      .tib = batch->tilebuffer_layout,
      /* An API resolve target contains the output, not necessarily the
       * previous multisample contents. Only the loader's explicit seed uses
       * that image as a background source. */
      .single_sample_reload =
         agx_native_graphics(dev) && !store && !partial_render &&
         batch->tilebuffer_layout.nr_samples > 1 && batch->key.resolve &&
         batch->g17p_resolve_reload,
      /* A persistent attachment or partial render must keep each sample.
       * Resolving it here would discard data needed by the next reload. */
      .multisample_store =
         agx_native_graphics(dev) && store &&
         batch->tilebuffer_layout.nr_samples > 1 &&
         (partial_render || !batch->key.resolve || batch->g17p_resolve_preserve),
      .store_and_resolve =
         agx_native_graphics(dev) && store && !partial_render &&
         batch->tilebuffer_layout.nr_samples > 1 && batch->key.resolve &&
         batch->g17p_resolve_preserve,
   };

   bool needs_textures_for_spilled_rts =
      agx_tilebuffer_spills(&batch->tilebuffer_layout) && !partial_render &&
      !store;

   for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
      const struct pipe_surface *surf = &batch->key.cbufs[rt];

      if (surf->texture == NULL)
         continue;

      if (agx_native_graphics(dev) && batch->tilebuffer_layout.spilled[rt]) {
         key.op[rt] = AGX_BG_EOT_NONE;
      } else if (store) {
         /* TODO: Suppress stores to discarded render targets */
         key.op[rt] = AGX_EOT_STORE;
      } else if (batch->tilebuffer_layout.spilled[rt] && partial_render) {
         /* Partial render programs exist only to store/load the tilebuffer to
          * main memory. When render targets are already spilled to main memory,
          * there's nothing to do.
          */
         key.op[rt] = AGX_BG_EOT_NONE;
      } else {
         bool valid = (batch->load & (PIPE_CLEAR_COLOR0 << rt));
         bool clear = (batch->clear & (PIPE_CLEAR_COLOR0 << rt));
         bool load = valid && !clear;

         /* Don't read back spilled render targets, they're already in memory */
         load &= !batch->tilebuffer_layout.spilled[rt];

         /* The background program used for partial renders must always load
          * whatever was stored in the mid-frame end-of-tile program.
          */
         load |= partial_render;

         key.op[rt] = load    ? AGX_BG_LOAD
                      : clear ? AGX_BG_CLEAR
                              : AGX_BG_EOT_NONE;
      }
   }

   if (agx_native_graphics(dev)) {
      if (agx_tilebuffer_spills(&key.tib)) {
         bool active = false;
         for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
            active |= key.op[rt] != AGX_BG_EOT_NONE;
            if (key.tib.spilled[rt]) {
               key.tib.logical_format[rt] = PIPE_FORMAT_NONE;
               key.tib.spilled[rt] = false;
               key.tib._offset_B[rt] = 0;
            }
         }
         if (active)
            return agx_g17p_build_mixed_bg_eot(batch, &key, store);
      }
      /* Match each admitted tile layout to its complete background/EOT
       * program and argument layout. */
      if (!agx_gallium_g17p_tile_layout_supported(&key.tib)) {
         const struct agx_tilebuffer_layout *tib = &batch->tilebuffer_layout;
         mesa_loge("asahi: G17P tile layout is unproven: %u samples, "
                   "%u B/sample, %u px tile, layered=%u",
                   tib->nr_samples, tib->sample_size_B, tib->tile_size,
                   tib->layered);
         for (unsigned rt = 0; rt < ARRAY_SIZE(tib->logical_format); ++rt) {
            if (tib->logical_format[rt] != PIPE_FORMAT_NONE)
               mesa_loge("asahi:   target %u is %s at byte %u%s", rt,
                         util_format_short_name(tib->logical_format[rt]),
                         tib->_offset_B[rt],
                         tib->spilled[rt] ? " (spilled)" : "");
         }
         agx_native_descriptor_rejected(dev, "background tile layout");
      }

      const enum agx_g17p_mrt_layout mrt =
         agx_g17p_classify_mrt(&key.tib);
      if (key.tib.layered && mrt == AGX_G17P_MRT_MIXED) {
         for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
            if (key.op[rt] != AGX_BG_EOT_NONE)
               return agx_g17p_build_mixed_bg_eot(batch, &key, store);
         }
      }
      const bool wboit_mrt =
         !key.tib.layered && mrt == AGX_G17P_MRT_WBOIT;
      const bool rgba8_mrt =
         !key.tib.layered && mrt == AGX_G17P_MRT_RGBA8;
      const bool dual_mrt = wboit_mrt || rgba8_mrt;
      const unsigned rgba8_mask = agx_g17p_rgba8_mrt_mask(&key.tib);
      const unsigned wide_count = util_bitcount(rgba8_mask);
      const unsigned first_rt = rgba8_mask ? ffs(rgba8_mask) - 1 : 0;
      const enum agx_bg_eot_op op = key.op[first_rt];

      for (unsigned rt = 1; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
         const enum agx_bg_eot_op rt_op = key.op[rt];
         const bool rt1_supported =
            ((rt == 1 && dual_mrt) || (rgba8_mask & BITFIELD_BIT(rt))) &&
            (store ? rt_op == AGX_EOT_STORE
                   : (rt_op == AGX_BG_CLEAR || rt_op == AGX_BG_LOAD));
         /* Anything the dedicated programs cannot express -- mixed
          * formats, or targets that do not share one operation -- takes
          * the general mixed-format programs. */
         if ((rt_op != AGX_BG_EOT_NONE && !rt1_supported) ||
             ((rgba8_mask & BITFIELD_BIT(rt)) && rt_op != op))
            return agx_g17p_build_mixed_bg_eot(batch, &key, store);
      }

      const bool dual_mrt_active = dual_mrt &&
                                   key.op[1] != AGX_BG_EOT_NONE;
      const unsigned attachment_count = key.store_and_resolve ? 2 :
         wide_count && op != AGX_BG_EOT_NONE ? wide_count : dual_mrt_active ? 2 : 1;
      const unsigned attachment_mask =
         key.store_and_resolve ? BITFIELD_BIT(first_rt) :
         rgba8_mask ? rgba8_mask : BITFIELD_MASK(attachment_count);
      const unsigned slot_count =
         rgba8_mask ? util_last_bit(rgba8_mask) : attachment_count;
      if (dual_mrt_active && key.op[1] != op)
         return agx_g17p_build_mixed_bg_eot(batch, &key, store);
      if (store ? (op != AGX_EOT_STORE && op != AGX_BG_EOT_NONE)
                : (op != AGX_BG_CLEAR && op != AGX_BG_LOAD &&
                   op != AGX_BG_EOT_NONE))
         agx_native_descriptor_rejected(dev, "background operation");

      if (!store && op == AGX_BG_CLEAR)
         key.reserved_preamble =
            wide_count ? 4 + 8 * slot_count
            : rgba8_mrt && dual_mrt_active
               ? AGX3_G17P_RGBA8_MRT_CLEAR_PUSH_HALFS
            : wboit_mrt && dual_mrt_active
               ? AGX3_G17P_WBOIT_MRT_CLEAR_PUSH_HALFS
               : AGX3_G17P_RT0_CLEAR_PUSH_HALFS;

      struct agx_bg_eot_shader *shader =
         agx_get_bg_eot_shader(&ctx->bg_eot, &key);
      const bool shader_supported =
         shader && shader->bo && shader->bo->va && shader->ptr &&
         shader->info.binary_size &&
         /* The G17P per-sample lowering repeats the body once per sample, so
          * this size guard scales with the sample count: a four-sample reload
          * is 990 bytes where the single-sample one is well under 512. */
         shader->info.binary_size <=
            (wide_count
                ? 4096u
                : 512u * MAX2(batch->tilebuffer_layout.nr_samples, 1u)) &&
         shader->info.main_offset < shader->info.binary_size &&
         shader->info.main_size <=
            shader->info.binary_size - shader->info.main_offset &&
         !shader->info.has_preamble && !shader->info.rodata.size_16 &&
         shader->info.nr_gprs;
      const bool resources_match =
         shader &&
         (store ||
          (op == AGX_BG_EOT_NONE
              ? shader->info.push_count == 0 && !shader->info.uses_txf
              : agx3_g17p_rt0_bg_shader_resources_match(
                   op == AGX_BG_CLEAR ? AGX3_G17P_RT0_BG_CLEAR
                                      : AGX3_G17P_RT0_BG_RELOAD,
                   shader->info.push_count, shader->info.uses_txf)));
      if (!shader_supported || !resources_match) {
         mesa_loge("asahi: G17P background shader is unproven: op %u, "
                   "%u pushed halves, txf=%u, preamble=%u, rodata=%u, "
                   "gprs=%u, %u samples, binary %u B (bound %u), main %u@%u",
                   op, shader ? shader->info.push_count : 0,
                   shader ? shader->info.uses_txf : 0,
                   shader ? shader->info.has_preamble : 0,
                   shader ? shader->info.rodata.size_16 : 0,
                   shader ? shader->info.nr_gprs : 0,
                   batch->tilebuffer_layout.nr_samples,
                   shader ? shader->info.binary_size : 0,
                   wide_count ? 4096u
                              : 512u * MAX2(batch->tilebuffer_layout.nr_samples,
                                            1u),
                   shader ? shader->info.main_size : 0,
                   shader ? shader->info.main_offset : 0);
         agx_native_descriptor_rejected(dev, "background shader");
      }
      agx_batch_add_bo(batch, shader->bo);

      size_t capacity;
      if (op == AGX_BG_EOT_NONE)
         capacity = AGX_IOTO_G17P_FRAGMENT_NO_RESOURCE_PROGRAM_SIZE;
      else if (wide_count || key.store_and_resolve)
         /* A wide background is not an RT0 reload: eight clear values need
          * eight load chunks, and the generated program runs past the RT0
          * bound (344 bytes against 246 for eight RGBA8 targets, which is
          * what KHR-GL30.draw_buffers clears). */
         capacity = store ? AGX_IOTO_G17P_OCTA_EOT_PROGRAM_MAX_SIZE
                          : AGX_IOTO_G17P_OCTA_BACKGROUND_PROGRAM_MAX_SIZE;
      else if (store)
         capacity = AGX_IOTO_G17P_RT0_EOT_PROGRAM_SIZE;
      else if (op == AGX_BG_CLEAR)
         capacity = rgba8_mrt && dual_mrt_active
                       ? AGX_IOTO_G17P_RGBA8_MRT_CLEAR_PROGRAM_SIZE
                    : wboit_mrt && dual_mrt_active
                       ? AGX_IOTO_G17P_WBOIT_MRT_CLEAR_PROGRAM_SIZE
                       : AGX_IOTO_G17P_RT0_CLEAR_PROGRAM_SIZE;
      else
         capacity = AGX_IOTO_G17P_RT0_RELOAD_PROGRAM_MAX_SIZE;
      struct agx_bo *program_bo = NULL;
      struct agx_ptr program_ptr = agx_pool_alloc_aligned_with_bo(
         &batch->pipeline_pool, capacity, AGX_IOTO_PROGRAM_ALIGNMENT,
         &program_bo);
      if (!program_ptr.cpu || !program_bo ||
          !agx_va_in_usc_window(dev, program_ptr.gpu) ||
          !agx_va_in_usc_window(dev,
                                shader->ptr + shader->info.main_offset))
         agx_native_descriptor_rejected(dev, "background program allocation");

      size_t program_length = 0;
      uint64_t resource_spec = 0;
      if (store && op == AGX_EOT_STORE) {
         const struct pipe_surface *surf = &batch->key.cbufs[first_rt];
         struct pipe_surface resolve_surf;
         if (key.tib.nr_samples > 1 && !key.multisample_store) {
            if (!agx_g17p_direct_resolve_surface(batch, &resolve_surf))
               agx_native_descriptor_rejected(dev, "direct colour resolve");
            surf = &resolve_surf;
         }
         const struct pipe_surface *surf1 =
            dual_mrt_active ? &batch->key.cbufs[1] : NULL;
         if (!surf->texture || (dual_mrt_active && !surf1->texture) ||
             descriptor_size != AGX_IOTO_G17P_RT0_PBE_ARGUMENT_SIZE)
            agx_native_descriptor_rejected(dev, "end-of-tile attachment");
         if (key.multisample_store &&
             (dual_mrt_active ||
              util_res_sample_count(surf->texture) != key.tib.nr_samples))
            agx_native_descriptor_rejected(dev, "multisample store attachment");

         const size_t pbe_size = descriptor_size * attachment_count;
         struct agx_ptr pbe =
            agx_pool_alloc_aligned(&batch->pool, pbe_size, 32);
         unsigned pbe_index = 0;
         u_foreach_bit(rt, attachment_mask) {
            const struct pipe_surface *attachment =
               rt == first_rt ? surf : &batch->key.cbufs[rt];
            if (!attachment->texture)
               agx_native_descriptor_rejected(dev, "end-of-tile MRT attachment");
            struct pipe_image_view view = image_view_for_surface(attachment);
            view.format = agx_g17p_rt0_pbe_format(view.format);
            const bool no_compress =
               batch->feedback & (PIPE_CLEAR_COLOR0 << rt);
            struct agx_gallium_pbe_descriptor *argument =
               agx_pbe_descriptor_at(pbe.cpu, descriptor_size, pbe_index++);
            if (!agx_batch_upload_pbe(batch, argument, &view, true, true,
                                      false, no_compress))
               agx_native_descriptor_rejected(dev, "end-of-tile PBE");
         }

         if (key.store_and_resolve) {
            if (!agx_g17p_direct_resolve_surface(batch, &resolve_surf))
               agx_native_descriptor_rejected(dev, "source-preserving resolve");
            struct pipe_image_view view = image_view_for_surface(&resolve_surf);
            view.format = agx_g17p_rt0_pbe_format(view.format);
            struct agx_gallium_pbe_descriptor *argument =
               agx_pbe_descriptor_at(pbe.cpu, descriptor_size, pbe_index++);
            if (!agx_batch_upload_pbe(batch, argument, &view, true, true,
                                      false, false))
               agx_native_descriptor_rejected(dev, "resolve destination PBE");
         }

         struct agx3_g17p_rt0_eot_program_desc desc = {
            .program_va = program_ptr.gpu,
            .shader_va = shader->ptr + shader->info.main_offset,
            .pbe_argument_va = pbe.gpu,
            .imageblock_offset_B =
               agx_tilebuffer_offset_B(&key.tib, first_rt),
            .attachment_count = attachment_count,
            /* Several targets stored at once is the mixed-format builtin,
             * whichever API named the layout: the selectors already come
             * from each target's own logical format (agx_bg_eot.c). */
            .mixed_mrt = attachment_count >= 2,
            .imageblock_offset1_B = key.store_and_resolve
               ? agx_tilebuffer_offset_B(&key.tib, first_rt)
               : dual_mrt_active ? agx_tilebuffer_offset_B(&key.tib, 1) : 0,
            .shader_binary_size = shader->info.binary_size,
            .scratch_size = shader->info.scratch_size,
            .nr_gprs = shader->info.nr_gprs,
            .staging_halfs = shader->info.agx3_staging_known
                                ? shader->info.agx3_staging_halfs
                                : 0,
            .imageblock = {
               .sample_size_B = key.tib.sample_size_B,
               .nr_samples = key.tib.nr_samples,
               .tile_size_px = key.tib.tile_size,
            },
         };
         uint8_t *const offsets[] = {
            &desc.imageblock_offset1_B, &desc.imageblock_offset2_B,
            &desc.imageblock_offset3_B, &desc.imageblock_offset4_B,
            &desc.imageblock_offset5_B, &desc.imageblock_offset6_B,
            &desc.imageblock_offset7_B,
         };
         unsigned offset_index = 0;
         u_foreach_bit(rt, attachment_mask & ~BITFIELD_BIT(first_rt)) {
            if (offset_index >= ARRAY_SIZE(offsets))
               break;
            *offsets[offset_index++] = agx_tilebuffer_offset_B(&key.tib, rt);
         }
         struct agx3_g17p_rt0_eot_program native;
         if (!agx3_g17p_rt0_eot_program_generate(dev->chip, &desc, &native) ||
             native.length > capacity)
            agx_native_descriptor_rejected(dev, "end-of-tile Ioto program");
         memcpy(program_ptr.cpu, native.bytes, native.length);
         program_length = native.length;
         resource_spec = AGX_IOTO_G17P_EOT_RESOURCE_SPEC;
      } else if (op == AGX_BG_EOT_NONE) {
         const struct agx3_g17p_graphics_program_desc desc = {
            .program_va = program_ptr.gpu,
            .shader_va = shader->ptr + shader->info.main_offset,
            .shader_binary_size = shader->info.binary_size,
            .scratch_size = shader->info.scratch_size,
            .nr_gprs = shader->info.nr_gprs,
            .compiler_d_valid = shader->info.agx3_staging_known,
            .compiler_d = (uint32_t)shader->info.agx3_staging_halfs * 2,
            .fragment = true,
            .imageblock = {
               .sample_size_B = key.tib.sample_size_B,
               .nr_samples = key.tib.nr_samples,
               .tile_size_px = key.tib.tile_size,
            },
         };
         struct agx3_g17p_graphics_program native;
         if (!agx3_g17p_graphics_program_generate(dev->chip, &desc, &native) ||
             native.length > capacity ||
             !agx3_g17p_rt0_bg_resource_spec(
                dev->chip, AGX3_G17P_RT0_BG_NONE, &resource_spec))
            agx_native_descriptor_rejected(dev, "empty background Ioto program");
         memcpy(program_ptr.cpu, native.bytes, native.length);
         program_length = native.length;
      } else {
         const struct pipe_surface *surf = &batch->key.cbufs[first_rt];
         struct pipe_surface resolve_surf;
         if (key.single_sample_reload) {
            if (!agx_g17p_direct_resolve_surface(batch, &resolve_surf))
               agx_native_descriptor_rejected(dev,
                                              "single-sample WSI reload");
            surf = &resolve_surf;
         }
         if (!surf->texture)
            agx_native_descriptor_rejected(dev, "background attachment");

         struct agx3_g17p_rt0_bg_program_desc desc = {
            .program_va = program_ptr.gpu,
            .shader_va = shader->ptr + shader->info.main_offset,
            .shader_binary_size = shader->info.binary_size,
            .scratch_size = shader->info.scratch_size,
            .nr_gprs = shader->info.nr_gprs,
            .staging_halfs = shader->info.agx3_staging_halfs,
            .clear_push_halfs = shader->info.push_count,
            .staging_known = shader->info.agx3_staging_known,
            .op = op == AGX_BG_CLEAR ? AGX3_G17P_RT0_BG_CLEAR
                                     : AGX3_G17P_RT0_BG_RELOAD,
            .imageblock = {
               .sample_size_B = key.tib.sample_size_B,
               .nr_samples = key.tib.nr_samples,
               .tile_size_px = key.tib.tile_size,
            },
         };

         if (op == AGX_BG_CLEAR) {
            if (!batch->uploaded_clear_color[first_rt])
               agx_native_descriptor_rejected(dev, "background clear value");
            if (attachment_count > 1 || first_rt != 0) {
               union pipe_color_union clear_values[PIPE_MAX_COLOR_BUFS] = {0};
               u_foreach_bit(rt, attachment_mask) {
                  if (!batch->uploaded_clear_color[rt])
                     agx_native_descriptor_rejected(dev, "background MRT clear value");
                  clear_values[rt] = batch->clear_color[rt];
               }
               const size_t clear_size =
                  wboit_mrt ? 20 : slot_count * sizeof(clear_values[0]);
               desc.clear_value_va = agx_pool_upload_aligned(
                  &batch->pool, clear_values, clear_size, 16);
               if (!desc.clear_value_va)
                  agx_native_descriptor_rejected(dev,
                                                 "background MRT clear upload");
            } else {
               desc.clear_value_va = batch->uploaded_clear_color[0];
            }
         } else {
            /* Each RT occupies an even texture index in the reload shader. */
            const unsigned texture_count = 2 * slot_count - 1;
            struct agx_ptr textures = agx_pool_alloc_aligned(
               &batch->pool, descriptor_size * texture_count, 64);
            memset(textures.cpu, 0, descriptor_size * texture_count);
            u_foreach_bit(rt, attachment_mask) {
               const struct pipe_surface *attachment =
                  rt == first_rt ? surf : &batch->key.cbufs[rt];
               if (!attachment->texture)
                  agx_native_descriptor_rejected(dev, "background MRT attachment");
               struct pipe_sampler_view sampler_view =
                  sampler_view_for_surface(attachment);
               struct agx_gallium_texture_descriptor *texture =
                  agx_texture_descriptor_at(textures.cpu, descriptor_size,
                                            rt * 2);
               if (!agx_pack_texture(descriptor_generation, texture,
                                     agx_resource(attachment->texture),
                                     attachment->format, &sampler_view))
                  agx_native_descriptor_rejected(dev, "background texture");
            }

            const unsigned sampler_size =
               agx_chip_sampler_heap_element_size(dev->chip);
            if (sampler_size != AGX_GALLIUM_SAMPLER_AGX3_SIZE)
               agx_native_descriptor_rejected(dev, "background sampler geometry");
            /* Staged multisample texel fetches select sampler one. Ordinary
             * reloads use zero; both entries have the native txf state. */
            const unsigned sampler_count =
               key.tib.nr_samples > 1 && !key.single_sample_reload ? 2 : 1;
            struct agx_ptr sampler = agx_pool_alloc_aligned(
               &batch->pool, sampler_count * sampler_size, 64);
            if (!agx_gallium_pack_g17p_txf_sampler(dev->chip, sampler.cpu))
               agx_native_descriptor_rejected(dev, "background texel-fetch sampler");

            if (sampler_count > 1)
               memcpy((uint8_t *)sampler.cpu + sampler_size, sampler.cpu,
                      sampler_size);

            const uint64_t pointers[2] = {textures.gpu, sampler.gpu};
            const uint64_t table = agx_pool_upload_aligned(
               &batch->pool, pointers, sizeof(pointers), 8);
            desc.texture_pointer_va = table;
            desc.sampler_pointer_va = table + sizeof(uint64_t);
         }

         struct agx3_g17p_rt0_bg_program native;
         if (!agx3_g17p_rt0_bg_program_generate(dev->chip, &desc, &native) ||
             native.length > capacity ||
             !agx3_g17p_rt0_bg_resource_spec(dev->chip, desc.op,
                                              &resource_spec))
            agx_native_descriptor_rejected(dev, "background Ioto program");
         memcpy(program_ptr.cpu, native.bytes, native.length);
         program_length = native.length;
      }

      agx_bo_assert_range(program_bo, program_ptr.gpu - program_bo->va->addr,
                          program_length);

      return (struct asahi_bg_eot){
         .usc = agx_usc_addr(dev, program_ptr.gpu),
         .rsrc_spec = resource_spec,
      };
   }

   /* Begin building the legacy pipeline. */
   size_t usc_size = agx_usc_size(3 + PIPE_MAX_COLOR_BUFS);
   struct agx_ptr t =
      agx_pool_alloc_aligned(&batch->pipeline_pool, usc_size, 64);
   struct agx_usc_builder b = agx_usc_builder(t.cpu, usc_size);

   bool needs_sampler = false;
   unsigned uniforms = 0;
   unsigned nr_tex = 0;

   for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
      if (key.op[rt] == AGX_BG_LOAD) {
         /* Each reloaded render target is textured */
         needs_sampler = true;

         /* Will be uploaded later, this would be clobbered */
         if (needs_textures_for_spilled_rts)
            continue;

         struct agx_ptr texture =
            agx_pool_alloc_aligned(&batch->pool, descriptor_size, 64);
         const struct pipe_surface *surf = &batch->key.cbufs[rt];
         assert(surf->texture != NULL && "cannot load nonexistent attachment");

         struct agx_resource *rsrc = agx_resource(surf->texture);
         struct pipe_sampler_view sampler_view = sampler_view_for_surface(surf);

         if (!agx_pack_texture(descriptor_generation, texture.cpu, rsrc,
                               surf->format, &sampler_view))
            agx_native_descriptor_rejected(dev, "background texture");

         agx_usc_pack(&b, TEXTURE, cfg) {
            /* Shifted to match eMRT indexing, could be optimized */
            cfg.start = rt * 2;
            cfg.count = 1;
            cfg.buffer = texture.gpu;
         }

         nr_tex = (rt * 2) + 1;
      } else if (key.op[rt] == AGX_BG_CLEAR) {
         assert(batch->uploaded_clear_color[rt] && "set when cleared");
         agx_usc_uniform(&b, 4 + (8 * rt), 8, batch->uploaded_clear_color[rt]);
         uniforms = MAX2(uniforms, 4 + (8 * rt) + 8);
      } else if (key.op[rt] == AGX_EOT_STORE) {
         struct pipe_image_view view =
            image_view_for_surface(&batch->key.cbufs[rt]);
         struct agx_ptr pbe =
            agx_pool_alloc_aligned(&batch->pool, descriptor_size, 256);

         /* The tilebuffer is already in sRGB space if needed. Do not convert */
         view.format = util_format_linear(view.format);

         bool no_compress = batch->feedback & (PIPE_CLEAR_COLOR0 << rt);
         if (!agx_batch_upload_pbe(batch, pbe.cpu, &view, true, true, false,
                                   no_compress))
            agx_native_descriptor_rejected(dev, "end-of-tile PBE");

         agx_usc_pack(&b, TEXTURE, cfg) {
            cfg.start = rt;
            cfg.count = 1;
            cfg.buffer = pbe.gpu;
         }

         nr_tex = rt + 1;
      }
   }

   if (needs_textures_for_spilled_rts) {
      /* Upload texture/PBE descriptors for each render target so we can clear
       * spilled render targets.
       */
      struct agx_ptr descs = agx_pool_alloc_aligned(
         &batch->pool, descriptor_size * 2 * batch->key.nr_cbufs, 64);
      agx_upload_spilled_rt_descriptors(descs.cpu, batch,
                                        descriptor_generation);

      agx_usc_pack(&b, TEXTURE, cfg) {
         cfg.start = 0;
         cfg.count = 2 * batch->key.nr_cbufs;
         cfg.buffer = descs.gpu;
      }

      nr_tex = MAX2(nr_tex, 2 * batch->key.nr_cbufs);

      /* Bind the base as u0_u1 for bindless access */
      agx_usc_uniform(&b, 0, 4,
                      agx_pool_upload_aligned(&batch->pool, &descs.gpu, 8, 8));
      uniforms = MAX2(uniforms, 4);
   }

   /* All render targets share a sampler */
   if (needs_sampler) {
      struct agx_ptr sampler =
         agx_pool_alloc_aligned(&batch->pool, AGX_SAMPLER_LENGTH, 64);

      agx_pack(sampler.cpu, SAMPLER, cfg) {
         cfg.minimum_lod = 0.0f;
         cfg.maximum_lod = INFINITY;
         cfg.magnify = AGX_FILTER_LINEAR;
         cfg.minify = AGX_FILTER_NEAREST;
         cfg.mip_filter = AGX_MIP_FILTER_NONE;
         cfg.wrap_s = AGX_WRAP_CLAMP_TO_EDGE;
         cfg.wrap_t = AGX_WRAP_CLAMP_TO_EDGE;
         cfg.wrap_r = AGX_WRAP_CLAMP_TO_EDGE;
         cfg.pixel_coordinates = true;
         cfg.compare_func = AGX_COMPARE_FUNC_ALWAYS;
      }

      agx_usc_pack(&b, SAMPLER, cfg) {
         cfg.start = 0;
         cfg.count = 1;
         cfg.buffer = sampler.gpu;
      }
   }

   agx_usc_push_packed(&b, SHARED, &batch->tilebuffer_layout.usc);

   /* Get the shader */
   key.reserved_preamble = uniforms;
   struct agx_bg_eot_shader *shader = agx_get_bg_eot_shader(&ctx->bg_eot, &key);
   if (!shader)
      agx_native_descriptor_rejected(dev, "background shader");

   agx_batch_add_bo(batch, shader->bo);
   assert(shader->info.rodata.size_16 == 0);

   agx_usc_pack(&b, SHADER, cfg) {
      cfg.code = agx_usc_addr(dev, shader->ptr + shader->info.main_offset);
      cfg.unk_2 = 0;
   }

   agx_usc_pack(&b, REGISTERS, cfg)
      cfg.register_count = shader->info.nr_gprs;

   if (shader->info.has_preamble) {
      agx_usc_pack(&b, PRESHADER, cfg) {
         cfg.code =
            agx_usc_addr(dev, shader->ptr + shader->info.preamble_offset);
      }
   } else {
      agx_usc_pack(&b, NO_PRESHADER, cfg)
         ;
   }

   struct asahi_bg_eot ret = {.usc = t.gpu};

   struct agx_counts_packed counts;
   agx_pack(&counts, COUNTS, cfg) {
      cfg.uniform_register_count = shader->info.push_count;
      cfg.preshader_register_count = shader->info.nr_preamble_gprs;
      cfg.texture_state_register_count = nr_tex;
      cfg.sampler_state_register_count =
         agx_translate_sampler_state_count(needs_sampler ? 1 : 0, false);

      if (!store)
         cfg.unknown = 0xFFFF;
   }

   memcpy(&ret.rsrc_spec, &counts, sizeof(counts));

   return ret;
}

/*
 * Return the standard sample positions, packed into a 32-bit word with fixed
 * point nibbles for each x/y component of the (at most 4) samples. This is
 * suitable for programming the PPP_MULTISAMPLECTL control register.
 */
static uint32_t
agx_default_sample_positions(unsigned nr_samples)
{
   switch (nr_samples) {
   case 1:
      return 0x88;
   case 2:
      return 0x44cc;
   case 4:
      return 0xeaa26e26;
   default:
      UNREACHABLE("Invalid sample count");
   }
}

void
agx_batch_init_state(struct agx_batch *batch)
{
   if (batch->initialized)
      return;

   if (agx_batch_is_compute(batch)) {
      batch->initialized = true;

      struct agx_context *ctx = batch->ctx;
      struct agx_device *dev = agx_device(ctx->base.screen);
      uint8_t *out = batch->cdm.current;

      /* See below */
      agx_push(out, CDM_BARRIER, cfg) {
         cfg.usc_cache_inval = true;
         cfg.unk_5 = true;
         cfg.unk_6 = true;
         cfg.unk_8 = true;
         // cfg.unk_11 = true;
         // cfg.unk_20 = true;
         if (dev->params.num_clusters_total > 1) {
            // cfg.unk_24 = true;
            if (dev->params.gpu_generation == 13) {
               cfg.unk_4 = true;
               // cfg.unk_26 = true;
            }
         }
      }

      return;
   }

   struct agx_device *dev = agx_device(batch->ctx->base.screen);

   /* Emit state on the batch that we don't change and so don't dirty track */
   uint8_t *out = batch->vdm.current;

   /* Barrier to enforce GPU-CPU coherency, in case this batch is back to back
    * with another that caused stale data to be cached and the CPU wrote to it
    * in the meantime.
    */
   if (agx_native_graphics(dev)) {
      struct agx_vdm_barrier_core barrier;
      const bool packed = agx_vdm_barrier_core_pack(dev->chip, &barrier);
      assert(packed);
      if (!packed)
         return;

      memcpy(out, barrier.bytes, barrier.length);
      out += barrier.length;
   } else {
      agx_push(out, VDM_BARRIER, cfg) {
         cfg.usc_cache_inval = true;
      }
   }

   if (agx_native_graphics(dev)) {
      const struct agx_ppp_w_clamp_cull2_fields fields = {
         .w_clamp_raw = 0,
         .cull2_raw = UINT32_C(0x80),
      };
      struct agx_ppp_w_clamp_cull2_core initial;
      const bool packed =
         agx_ppp_w_clamp_cull2_core_pack(dev->chip, &fields, &initial);
      assert(packed);
      if (!packed)
         return;

      struct agx_ptr T = agx_pool_alloc_aligned(
         &batch->pool, initial.length, AGX_PPP_HEADER_ALIGN);
      memcpy(T.cpu, initial.bytes, initial.length);

      struct agx_ppp_update ppp = {
         .head = (uint8_t *)T.cpu + initial.length,
         .gpu_base = T.gpu,
         .total_size = initial.length,
         .cpu_base = T.cpu,
      };
      const bool emitted = agx_gallium_ppp_fini(batch, &out, &ppp);
      assert(emitted);
      if (!emitted)
         return;
   } else {
      struct AGX_PPP_HEADER present = {
         .w_clamp = true,
         .occlusion_query_2 = true,
         .output_unknown = true,
         .varying_word_2 = true,
         .viewport_count = 1, /* irrelevant */
      };

      size_t size = agx_ppp_update_size(&present);
      struct agx_ptr T =
         agx_pool_alloc_aligned(&batch->pool, size, AGX_PPP_HEADER_ALIGN);
      struct agx_ppp_update ppp = agx_new_ppp_update(T, size, &present);

      /* clang-format off */
      agx_ppp_push(&ppp, W_CLAMP, cfg) cfg.w_clamp = 1e-10;
      agx_ppp_push(&ppp, FRAGMENT_OCCLUSION_QUERY_2, cfg);
      agx_ppp_push(&ppp, OUTPUT_UNKNOWN, cfg);
      agx_ppp_push(&ppp, VARYING_2, cfg);
      /* clang-format on */

      const bool emitted = agx_gallium_ppp_fini(batch, &out, &ppp);
      assert(emitted);
      if (!emitted)
         return;
   }
   batch->vdm.current = out;

   /* Mark it as initialized now, since agx_batch_writes() will check this. */
   batch->initialized = true;

   /* Choose a tilebuffer layout given the framebuffer key */
   enum pipe_format formats[PIPE_MAX_COLOR_BUFS] = {0};
   for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
      formats[i] = batch->key.cbufs[i].format;
   }

   batch->tilebuffer_layout = agx_gallium_tilebuffer_layout(
      agx_device(batch->ctx->base.screen), formats, batch->key.nr_cbufs,
      util_framebuffer_get_num_samples(&batch->key),
      util_framebuffer_get_num_layers(&batch->key) > 1);

   if (agx_device(batch->ctx->base.screen)->debug & AGX_DBG_SMALLTILE)
      batch->tilebuffer_layout.tile_size = 16 * 16;

   /* If the layout spilled render targets, we need to decompress those render
    * targets to ensure we can write to them.
    */
   if (agx_tilebuffer_spills(&batch->tilebuffer_layout)) {
      for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
         if (!batch->tilebuffer_layout.spilled[i])
            continue;

         struct pipe_surface *surf = &batch->key.cbufs[i];
         if (!surf->texture)
            continue;

         struct agx_resource *rsrc = agx_resource(surf->texture);
         struct ail_layout *layout = &rsrc->layout;
         unsigned level = surf->level;

         if (!ail_is_level_logically_compressed(layout, level))
            continue;

         if (true || (rsrc->base.bind & PIPE_BIND_SHARED)) {
            agx_decompress_inplace(batch, surf, "Render target spilled");
         } else {
            agx_decompress(batch->ctx, rsrc, "Render target spilled");
         }
      }
   }

   if (batch->key.zsbuf.texture) {
      unsigned level = batch->key.zsbuf.level;
      struct agx_resource *rsrc = agx_resource(batch->key.zsbuf.texture);

      agx_batch_writes(batch, rsrc, level);

      if (rsrc->separate_stencil)
         agx_batch_writes(batch, rsrc->separate_stencil, level);
   }

   for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
      if (batch->key.cbufs[i].texture) {
         struct agx_resource *rsrc = agx_resource(batch->key.cbufs[i].texture);
         unsigned level = batch->key.cbufs[i].level;

         if (agx_resource_valid(rsrc, level))
            batch->load |= PIPE_CLEAR_COLOR0 << i;

         agx_batch_writes(batch, rsrc, level);
         assert(agx_resource_valid(rsrc, level));
      }
   }

   if (batch->key.resolve) {
      struct agx_resource *resolve = agx_resource(batch->key.resolve);
      agx_batch_writes(batch, resolve, 0);
      assert(agx_resource_valid(resolve, 0));
   }

   /* Set up standard sample positions */
   batch->uniforms.ppp_multisamplectl =
      agx_default_sample_positions(batch->tilebuffer_layout.nr_samples);
}

static enum agx_object_type
agx_point_object_type(struct agx_rasterizer *rast)
{
   return (rast->base.sprite_coord_mode == PIPE_SPRITE_COORD_UPPER_LEFT)
             ? AGX_OBJECT_TYPE_POINT_SPRITE_UV01
             : AGX_OBJECT_TYPE_POINT_SPRITE_UV10;
}

#define MAX_PPP_UPDATES 2
#define IS_DIRTY(ST)    !!(ctx->dirty & AGX_DIRTY_##ST)

static uint32_t
agx_g17p_stencil_word(const struct pipe_stencil_state *stencil)
{
   return agx_ppp_hal200_stencil_pack(
      stencil->enabled, stencil->func,
      agx_stencil_ops[stencil->fail_op],
      agx_stencil_ops[stencil->zfail_op],
      agx_stencil_ops[stencil->zpass_op], stencil->valuemask,
      stencil->writemask);
}

static uint8_t *
agx_g17p_emit_bind_state(struct agx_batch *batch, uint8_t *out,
                         struct agx_compiled_shader *vs,
                         uint32_t *draw_state_offset)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);
   struct agx_rasterizer *rast = ctx->rast;
   if (!agx_native_graphics(dev) || !draw_state_offset)
      return NULL;

   /* Gallium does not track the highest initialized viewport. When a shader
    * can select a nonzero viewport, preserve the established AGX path and
    * upload the complete API-visible array. Keep ordinary shaders on the
    * byte-identical one-viewport fast path. */
   const unsigned viewport_count =
      vs->b.info.nonzero_viewport ? AGX_MAX_VIEWPORTS : 1;
   static_assert(AGX_MAX_VIEWPORTS ==
                 AGX_PPP_VIEWPORT_PAYLOAD_MAX_VIEWPORTS);

   const bool is_points = batch->reduced_prim == MESA_PRIM_POINTS;
   const bool is_lines = batch->reduced_prim == MESA_PRIM_LINES;
   const enum agx_object_type object_type =
      is_points  ? agx_point_object_type(rast)
      : is_lines ? AGX_OBJECT_TYPE_LINE
                 : AGX_OBJECT_TYPE_TRIANGLE;

   const bool depth_bias_enable =
      rast->depth_bias && object_type == AGX_OBJECT_TYPE_TRIANGLE;
   unsigned minx[AGX_MAX_VIEWPORTS], miny[AGX_MAX_VIEWPORTS];
   unsigned maxx[AGX_MAX_VIEWPORTS], maxy[AGX_MAX_VIEWPORTS];
   struct agx_ppp_hal200_raster_state raster_states[AGX_MAX_VIEWPORTS];
   for (unsigned i = 0; i < viewport_count; ++i) {
      agx_get_scissor_extents(
         &ctx->viewport[i], rast->base.scissor ? &ctx->scissor[i] : NULL,
         &batch->key, &minx[i], &miny[i], &maxx[i], &maxy[i]);
      float minz, maxz;
      util_viewport_zmin_zmax(&ctx->viewport[i], rast->base.clip_halfz, &minz,
                              &maxz);
      const struct agx_ppp_hal200_raster_fields raster_fields = {
         .scissor_enable = true,
         .depth_bias_enable = depth_bias_enable,
         .max_x = MIN2(maxx[i], UINT16_MAX),
         .min_x = MIN2(minx[i], UINT16_MAX),
         .max_y = MIN2(maxy[i], UINT16_MAX),
         .min_y = MIN2(miny[i], UINT16_MAX),
         .min_z_raw = fui(minz),
         .max_z_raw = fui(maxz),
         .depth_bias_raw =
            fui(depth_bias_enable ? rast->base.offset_units * 2.0f : 0.0f),
         .slope_scale_raw =
            fui(depth_bias_enable ? rast->base.offset_scale : 0.0f),
         .clamp_raw = fui(depth_bias_enable ? rast->base.offset_clamp : 0.0f),
      };
      if (!agx_ppp_hal200_raster_state_build(
             dev->chip, &raster_fields, &raster_states[i]))
         return NULL;
   }

   uint8_t *scissors = util_dynarray_grow_bytes(
      &batch->scissor, viewport_count, sizeof(raster_states[0].scissor));
   uint8_t *depth_bias_records = util_dynarray_grow_bytes(
      &batch->depth_bias, viewport_count,
      sizeof(raster_states[0].depth_bias));
   for (unsigned i = 0; i < viewport_count; ++i) {
      memcpy(scissors + i * sizeof(raster_states[i].scissor),
             raster_states[i].scissor, sizeof(raster_states[i].scissor));
      memcpy(depth_bias_records + i * sizeof(raster_states[i].depth_bias),
             raster_states[i].depth_bias,
             sizeof(raster_states[i].depth_bias));
   }
   const struct agx_ppp_hal200_raster_state *raster_state = &raster_states[0];

   struct agx_ppp_viewport_payload_fields viewport_fields = {
      .viewport_count = viewport_count,
   };
   for (unsigned i = 0; i < viewport_count; ++i) {
      uint8_t region_clip[AGX_REGION_CLIP_LENGTH];
      agx_pack(region_clip, REGION_CLIP, cfg) {
         cfg.enable = true;
         cfg.min_x = minx[i] / 32;
         cfg.min_y = miny[i] / 32;
         cfg.max_x = DIV_ROUND_UP(MAX2(maxx[i], 1), 32);
         cfg.max_y = DIV_ROUND_UP(MAX2(maxy[i], 1), 32);
      }
      memcpy(&viewport_fields.region_clip_raw[i], region_clip,
             sizeof(region_clip));
      const struct pipe_viewport_state *vp = &ctx->viewport[i];
      viewport_fields.viewports[i] =
         (struct agx_ppp_viewport_transform_raw){
            .translate_x = fui(vp->translate[0]),
            .scale_x = fui(vp->scale[0]),
            .translate_y = fui(vp->translate[1]),
            .scale_y = fui(vp->scale[1]),
            .translate_z = fui(rast->base.clip_halfz
                                  ? vp->translate[2]
                                  : vp->translate[2] - vp->scale[2]),
            .scale_z = fui(rast->base.clip_halfz ? vp->scale[2]
                                                 : vp->scale[2] * 2.0f),
         };
   }
   struct agx_ppp_viewport_payload_core viewport;
   if (!agx_ppp_viewport_payload_core_pack(dev->chip, &viewport_fields,
                                            &viewport))
      return NULL;

   uint64_t fragment_program = 0;
   agx_build_pipeline(batch, ctx->fs, ctx->linked.fs,
                      MESA_SHADER_FRAGMENT, 0, 0, &fragment_program);
   if (agx_drop_for_null_g17p_program(ctx, ctx->fs, fragment_program,
                                      "fragment"))
      return NULL;

   const struct agx_ppp_hal200_fragment_fields fs_fields = {
      .resource_counts = ctx->linked.fs->cf.nr_bindings << 16,
      .varying_table = batch->varyings,
      .stage_program = fragment_program,
   };
   struct agx_ppp_hal200_graphics_core fs_record;
   if (!agx_ppp_hal200_fragment_core_pack(dev->chip, &fs_fields, &fs_record))
      return NULL;

   uint32_t output_size;
   if (!agx_ppp_hal200_output_size_pack(vs->uvs.size, 0, &output_size))
      return NULL;
   uint32_t viewport_count_word;
   if (!agx_ppp_hal200_viewport_count_pack(dev->chip, viewport_count,
                                            &viewport_count_word))
      return NULL;

   const bool stencil_test = ctx->zs->base.stencil[0].enabled;
   enum agx_visibility_mode visibility = AGX_VISIBILITY_MODE_NONE;
   uint16_t query_index = 0;
   if (ctx->active_queries && ctx->occlusion_query) {
      visibility = ctx->occlusion_query->type == PIPE_QUERY_OCCLUSION_COUNTER
                      ? AGX_VISIBILITY_MODE_COUNTING
                      : AGX_VISIBILITY_MODE_BOOLEAN;
      query_index = agx_get_oq_index(batch, ctx->occlusion_query);
   }
   const uint32_t linked_fragment_control =
      ctx->linked.fs->fragment_control.opaque[0];
   const uint32_t pass_type = linked_fragment_control >> 29;
   if (pass_type != AGX_PASS_TYPE_OPAQUE &&
       pass_type != AGX_PASS_TYPE_TRANSLUCENT &&
       pass_type != AGX_PASS_TYPE_PUNCH_THROUGH &&
       pass_type != AGX_PASS_TYPE_TRANSLUCENT_PUNCH_THROUGH)
      return NULL;

   uint32_t fragment_control = raster_state->fragment_control |
                               (stencil_test ? UINT32_C(0x000c0000) : 0);
   uint32_t secondary_control = UINT32_C(0x00000200);
   const struct agx_ppp_hal200_fragment_control_fields control_fields = {
      .linked_control = linked_fragment_control,
      .shader_tri_merge_disable =
         (dev->chip == AGX_CHIP_G17P &&
          ctx->fs->b.info.needs_quad_derivatives) ||
         (batch->tilebuffer_layout.nr_samples > 1 &&
          ctx->fs->epilog_key.write_z) ||
         /* The linker's own merge-disable bit. */
         (!agx_chip_tri_merge_across_side_effects(dev->chip) &&
          (linked_fragment_control & (UINT32_C(1) << 26))),
      .coverage_tri_merge_disable =
         pass_type == AGX_PASS_TYPE_PUNCH_THROUGH ||
         pass_type == AGX_PASS_TYPE_TRANSLUCENT_PUNCH_THROUGH,
      .primitive_tri_merge_disable =
         object_type != AGX_OBJECT_TYPE_TRIANGLE ||
         rast->polygon_mode != AGX_POLYGON_MODE_FILL,
   };
   if (!agx_ppp_hal200_fragment_controls_pack(
          dev->chip, &control_fields, fragment_control, secondary_control,
          &fragment_control, &secondary_control))
      return NULL;

   uint32_t occlusion_query;
   if (!agx_ppp_hal200_occlusion_pack(dev->chip, visibility, query_index,
                                      fragment_control, &fragment_control,
                                      &occlusion_query))
      return NULL;

   struct agx_fragment_face_packed front_face, back_face;
   agx_pack(&front_face, FRAGMENT_FACE, cfg) {
      cfg.stencil_reference = ctx->stencil_ref.ref_value[0];
      cfg.line_width = rast->line_width;
      cfg.polygon_mode = rast->polygon_mode;
   }
   agx_pack(&back_face, FRAGMENT_FACE, cfg) {
      cfg.stencil_reference =
         ctx->stencil_ref.ref_value[ctx->zs->base.stencil[1].enabled ? 1 : 0];
      cfg.line_width = rast->line_width;
      cfg.polygon_mode = rast->polygon_mode;
   }
   const bool depth_test =
      batch->key.zsbuf.texture && ctx->zs->base.depth_enabled;
   if (!agx_ppp_hal200_depth_face_pack(
          dev->chip, depth_test, ctx->zs->base.depth_writemask,
          ctx->zs->base.depth_func, front_face.opaque[0],
          &front_face.opaque[0]) ||
       !agx_ppp_hal200_depth_face_pack(
          dev->chip, depth_test, ctx->zs->base.depth_writemask,
          ctx->zs->base.depth_func, back_face.opaque[0],
          &back_face.opaque[0]))
      return NULL;

   struct agx_fragment_face_2_packed face_2;
   agx_pack_fragment_face_2(&face_2, object_type, &ctx->fs->b.info);
   /* G17P's native depth contract must preserve late shader side effects.
    * An unchanged depth output alone does not permit early rejection when
    * image or SSBO stores precede the fragment tests. Match Honeykrisp's
    * native face packing rather than inheriting the AGX2 depth-only word. */
   if (!agx_ppp_hal200_fragment_face_depth_pack(
          dev->chip, agx_translate_depth_layout(ctx->fs->b.info.depth_layout),
          ctx->fs->b.info.early_fragment_tests,
          ctx->fs->so && ctx->fs->so->info.writes_memory,
          face_2.opaque[0], &face_2.opaque[0]))
      return NULL;

   /* The output select carries the vertex program's clip-distance planes,
    * point size and viewport/layer selectors alongside the fragment bits;
    * Apple's clip3 pipeline keeps the first three plane bits in this word.
    * The G17P layout matches AGX2's OUTPUT_SELECT, so the compiler's packed
    * selects merge as they do on other chips.
    */
   uint32_t output_select;
   if (!agx_ppp_hal200_output_select_pack(
          dev->chip, vs->uvs.osel.opaque[0], ctx->linked.fs->cf.nr_bindings,
          ctx->linked.fs->cf.reads_z, &output_select))
      return NULL;

   const struct agx_ppp_hal200_graphics_fields output_fields = {
      .words = {output_select, 0, viewport_count_word, output_size,
                AGX_PPP_HAL200_PRIMARY_FACE_HEADER, fragment_control},
   };
   const struct agx_ppp_hal200_graphics_fields primary_fields = {
      .words = {fragment_control, front_face.opaque[0],
                agx_g17p_stencil_word(&ctx->zs->base.stencil[0]),
                back_face.opaque[0],
                agx_g17p_stencil_word(ctx->zs->base.stencil[1].enabled
                                        ? &ctx->zs->base.stencil[1]
                                        : &ctx->zs->base.stencil[0]),
                occlusion_query},
   };
   const struct agx_ppp_hal200_graphics_fields secondary_fields = {
      .words = {secondary_control, face_2.opaque[0], face_2.opaque[0],
                UINT32_C(0x0001ffff)},
   };

   uint32_t cull = UINT32_C(0x00000480);
   if (!agx_ppp_hal200_cull_mode_pack(dev->chip, rast->base.cull_face,
                                      agx_front_face_ccw(ctx), cull, &cull) ||
       !agx_ppp_hal200_cull_provoking_pack(
          dev->chip, rast->base.flatshade_first ? 0 : 2, cull, &cull) ||
       !agx_ppp_hal200_depth_clip_pack(dev->chip, rast->base.depth_clip_near,
                                       cull, &cull) ||
       !agx_ppp_hal200_rasterizer_discard_pack(
          dev->chip, rast->base.rasterizer_discard, secondary_control, cull,
          &secondary_control, &cull))
      return NULL;

   struct agx_ppp_hal200_graphics_core output_record, primary_record;
   struct agx_ppp_hal200_graphics_core secondary_record, cull_record;
   struct agx_ppp_hal200_graphics_fields final_secondary = secondary_fields;
   final_secondary.words[0] = secondary_control;
   const struct agx_ppp_hal200_graphics_fields cull_fields = {
      .words = {cull},
   };
   const struct agx_ppp_w_clamp_cull2_fields w_cull2_fields = {
      .w_clamp_raw = 0,
      .cull2_raw = UINT32_C(0x80) |
                   (batch->generate_primitive_id ? UINT32_C(0x1000) : 0),
   };
   struct agx_ppp_w_clamp_cull2_core w_cull2;
   if (!agx_ppp_hal200_graphics_core_pack(
          dev->chip, AGX_PPP_HAL200_GRAPHICS_OUTPUT, &output_fields,
          &output_record) ||
       !agx_ppp_hal200_graphics_core_pack(
          dev->chip, AGX_PPP_HAL200_GRAPHICS_PRIMARY_FACE, &primary_fields,
          &primary_record) ||
       !agx_ppp_hal200_graphics_core_pack(
          dev->chip, AGX_PPP_HAL200_GRAPHICS_SECONDARY_FACE, &final_secondary,
          &secondary_record) ||
       !agx_ppp_hal200_graphics_core_pack(
          dev->chip, AGX_PPP_HAL200_GRAPHICS_CULL, &cull_fields,
          &cull_record) ||
       !agx_ppp_w_clamp_cull2_core_pack(dev->chip, &w_cull2_fields,
                                         &w_cull2))
      return NULL;

   uint8_t bind_group[AGX_G17P_BIND_GROUP_BYTES] = {0};
   memcpy(&bind_group[0x00], fs_record.bytes, fs_record.length);
   memcpy(&bind_group[0x1c], output_record.bytes, output_record.length);
   memcpy(&bind_group[0x30], primary_record.bytes, primary_record.length);
   memcpy(&bind_group[0x4c], secondary_record.bytes, secondary_record.length);
   memcpy(&bind_group[0x60], w_cull2.bytes, w_cull2.length);
   memcpy(&bind_group[0x6c], cull_record.bytes, cull_record.length);

   const size_t state_size = agx_g17p_bind_state_size(viewport.length);
   struct agx_ptr state =
      agx_pool_alloc_aligned(&batch->pool, state_size, 64);
   uint64_t compact_state;
   struct agx_g17p_bind_state_upload uploaded;
   if (!state_size || !state.cpu ||
       !agx_gallium_vdm_compact(batch, state.gpu, &compact_state) ||
       !agx_g17p_upload_bind_state(
          state.cpu, state_size, compact_state, bind_group,
          viewport.bytes, viewport.length,
          batch->linked_varyings.counts_32.opaque[0],
          batch->linked_varyings.counts_16.opaque[0], &uploaded))
      return NULL;

   struct agx_ptr indices = agx_pool_alloc_aligned(&batch->pool, 8, 4);
   uint64_t compact_indices;
   if (!indices.cpu ||
       !agx_gallium_vdm_compact(batch, indices.gpu, &compact_indices) ||
       !agx_g17p_upload_draw_indices(
          indices.cpu, 8, compact_indices, batch->scissor.size,
          batch->depth_bias.size, viewport_count, draw_state_offset))
      return NULL;

   memcpy(out, uploaded.tail.bytes, uploaded.tail.length);
   return out + uploaded.tail.length;
}

static uint8_t *
agx_encode_state(struct agx_batch *batch, uint8_t *out,
                 uint32_t *draw_state_offset)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   /* If nothing is dirty, encode nothing */
   if (!ctx->dirty)
      return out;

   if (draw_state_offset)
      *draw_state_offset = 0;

   struct agx_rasterizer *rast = ctx->rast;
   unsigned ppp_updates = 0;

   struct agx_compiled_shader *vs = ctx->vs;
   if (ctx->gs)
      vs = ctx->gs->gs_copy;

   bool varyings_dirty = false;

   if (IS_DIRTY(VS_PROG) || IS_DIRTY(FS_PROG) || IS_DIRTY(RS) ||
       IS_DIRTY(PRIM)) {

      unsigned bindings = ctx->linked.fs->cf.nr_bindings;
      if (bindings) {
         size_t linkage_size =
            AGX_CF_BINDING_HEADER_LENGTH + (bindings * AGX_CF_BINDING_LENGTH);

         struct agx_ptr t =
            agx_pool_alloc_aligned(&batch->pipeline_pool, linkage_size,
                                   agx_native_graphics(dev) ? 64 : 16);

         agx_link_varyings_vs_fs(t.cpu, &batch->linked_varyings,
                                 vs->uvs.user_size, &ctx->linked.fs->cf,
                                 ctx->rast->base.flatshade_first ? 0 : 2,
                                 (batch->reduced_prim == MESA_PRIM_POINTS)
                                    ? ctx->rast->base.sprite_coord_enable
                                    : 0,
                                 &batch->generate_primitive_id);

         batch->varyings =
            agx_native_graphics(dev) ? t.gpu : agx_usc_addr(dev, t.gpu);
      } else {
         batch->varyings = 0;
         batch->generate_primitive_id = false;
      }

      varyings_dirty = true;
      ppp_updates++;
   }

   /* HAL200 retains the vertex state update independently of its bind tail
    * and per-draw selector, just as the older VDM retains VS state. The old
    * immutable uniform upload remains valid until vertex inputs change. */
   if (IS_DIRTY(VS) || varyings_dirty) {
      struct agx_vdm_state_vertex_shader_word_0_packed counts;
      agx_pack(&counts, VDM_STATE_VERTEX_SHADER_WORD_0, cfg) {
         cfg.uniform_register_count = vs->b.info.push_count;
         cfg.preshader_register_count = vs->b.info.nr_preamble_gprs;
         cfg.texture_state_register_count = agx_nr_tex_descriptors(batch, vs);
         cfg.sampler_state_register_count =
            translate_sampler_state_count(ctx, vs->stage);
      }

      uint64_t pipeline = 0;
      const uint32_t packed_pipeline =
         agx_build_pipeline(batch, vs,
                            ctx->gs ? ctx->linked.gs_copy : ctx->linked.vs,
                            MESA_SHADER_VERTEX, 0, 0, &pipeline);

      if (agx_native_graphics(dev)) {
         if (agx_drop_for_null_g17p_program(ctx, vs, pipeline, "vertex"))
            return NULL;

         const uint32_t provoking = ctx->rast->base.flatshade_first ? 0 : 2;
         const struct agx_vdm_state_update_info info = {
            .raw_vertex_shader_word0 = counts.opaque[0],
            .pipeline = pipeline,
            .raw_pipeline_template = 0,
            .raw_vertex_outputs = {
               vs->uvs.vdm.opaque[0] +
                  (batch->generate_primitive_id ? UINT32_C(0x100) : 0),
               (ctx->rast->base.rasterizer_discard ? 0x30 + provoking
                                                  : provoking) |
                  (batch->generate_primitive_id ? UINT32_C(0x40) : 0),
            },
            .raw_vertex_unknown = 0,
         };
         struct agx_vdm_state_update encoded;
         const bool packed =
            agx_vdm_state_update_pack(dev->chip, &info, &encoded);
         assert(packed);
         if (!packed)
            return NULL;

         memcpy(out, encoded.bytes, encoded.length);
         out += encoded.length;
      } else {
         agx_push(out, VDM_STATE, cfg) {
            cfg.vertex_shader_word_0_present = true;
            cfg.vertex_shader_word_1_present = true;
            cfg.vertex_outputs_present = true;
            cfg.vertex_unknown_present = true;
         }
         agx_push_packed(out, counts, VDM_STATE_VERTEX_SHADER_WORD_0);
         agx_push(out, VDM_STATE_VERTEX_SHADER_WORD_1, cfg) {
            cfg.pipeline = packed_pipeline;
         }
         agx_push_packed(out, vs->uvs.vdm, VDM_STATE_VERTEX_OUTPUTS);
         agx_push(out, VDM_STATE_VERTEX_UNKNOWN, cfg) {
            cfg.flat_shading_control = ctx->rast->base.flatshade_first
                                          ? AGX_VDM_VERTEX_0
                                          : AGX_VDM_VERTEX_2;
            cfg.unknown_4 = cfg.unknown_5 =
               ctx->rast->base.rasterizer_discard;
            cfg.generate_primitive_id = batch->generate_primitive_id;
         }

         /* Pad up to a multiple of 8 bytes */
         memset(out, 0, 4);
         out += 4;
      }

      if (unlikely(dev->debug & AGX_DBG_PERF))
         ctx->perf.vs_emits++;
   }

   if (agx_native_graphics(dev))
      return agx_g17p_emit_bind_state(batch, out, vs, draw_state_offset);

   struct agx_pool *pool = &batch->pool;

   if ((ctx->dirty & AGX_DIRTY_RS) && ctx->rast->depth_bias) {
      agx_upload_depth_bias(batch, &ctx->rast->base);
      ctx->dirty |= AGX_DIRTY_SCISSOR_ZBIAS;
   }

   if (ctx->dirty & (AGX_DIRTY_VIEWPORT | AGX_DIRTY_SCISSOR_ZBIAS |
                     AGX_DIRTY_RS | AGX_DIRTY_VS)) {

      agx_upload_viewport_scissor(pool, batch, &out, ctx->viewport,
                                  ctx->rast->base.scissor ? ctx->scissor : NULL,
                                  ctx->rast->base.clip_halfz,
                                  vs->b.info.nonzero_viewport);
   }

   bool is_points = batch->reduced_prim == MESA_PRIM_POINTS;
   bool is_lines = batch->reduced_prim == MESA_PRIM_LINES;

   bool object_type_dirty =
      IS_DIRTY(PRIM) || (is_points && IS_DIRTY(SPRITE_COORD_MODE));

   bool fragment_face_dirty =
      IS_DIRTY(ZS) || IS_DIRTY(STENCIL_REF) || IS_DIRTY(RS);

   enum agx_object_type object_type = is_points  ? agx_point_object_type(rast)
                                      : is_lines ? AGX_OBJECT_TYPE_LINE
                                                 : AGX_OBJECT_TYPE_TRIANGLE;

   struct AGX_PPP_HEADER dirty = {
      .fragment_control =
         IS_DIRTY(ZS) || IS_DIRTY(RS) || IS_DIRTY(PRIM) || IS_DIRTY(QUERY),
      .fragment_control_2 = IS_DIRTY(FS_PROG) || IS_DIRTY(RS),
      .fragment_front_face = fragment_face_dirty,
      .fragment_front_face_2 = object_type_dirty || IS_DIRTY(FS_PROG),
      .fragment_front_stencil = IS_DIRTY(ZS),
      .fragment_back_face = fragment_face_dirty,
      .fragment_back_face_2 = object_type_dirty || IS_DIRTY(FS_PROG),
      .fragment_back_stencil = IS_DIRTY(ZS),
      .output_select = varyings_dirty,
      .varying_counts_32 = varyings_dirty,
      .varying_counts_16 = varyings_dirty,
      /* Also dirty with tess but agx_draw_patches dirties RS for that */
      .cull = IS_DIRTY(RS),
      .cull_2 = varyings_dirty,
      .fragment_shader =
         (IS_DIRTY(FS) || varyings_dirty || IS_DIRTY(SAMPLE_MASK)) &&
         !ctx->linked.fs->no_op,
      .occlusion_query = IS_DIRTY(QUERY),
      .output_size = IS_DIRTY(VS_PROG),
      .viewport_count = 1, /* irrelevant */
   };

   size_t size = agx_ppp_update_size(&dirty);
   struct agx_ptr T =
      agx_pool_alloc_aligned(&batch->pool, size, AGX_PPP_HEADER_ALIGN);
   struct agx_ppp_update ppp = agx_new_ppp_update(T, size, &dirty);

   if (dirty.fragment_control) {
      agx_ppp_push(&ppp, FRAGMENT_CONTROL, cfg) {
         if (ctx->active_queries && ctx->occlusion_query) {
            if (ctx->occlusion_query->type == PIPE_QUERY_OCCLUSION_COUNTER)
               cfg.visibility_mode = AGX_VISIBILITY_MODE_COUNTING;
            else
               cfg.visibility_mode = AGX_VISIBILITY_MODE_BOOLEAN;
         }

         cfg.stencil_test_enable = ctx->zs->base.stencil[0].enabled;
         cfg.two_sided_stencil = ctx->zs->base.stencil[1].enabled;
         cfg.depth_bias_enable =
            rast->depth_bias && object_type == AGX_OBJECT_TYPE_TRIANGLE;

         /* Always enable scissoring so we may scissor to the viewport (TODO:
          * optimize this out if the viewport is the default and the app does
          * not use the scissor test)
          */
         cfg.scissor_enable = true;

         /* This avoids broken derivatives along primitive edges */
         cfg.disable_tri_merging = is_lines || is_points;
      }
   }

   if (dirty.fragment_control_2) {
      /* Annoying, rasterizer_discard seems to be ignored (sometimes?) in the
       * main fragment control word and has to be combined into the secondary
       * word for reliable behaviour.
       */
      agx_ppp_push_merged(&ppp, FRAGMENT_CONTROL, cfg,
                          ctx->linked.fs->fragment_control) {
         cfg.tag_write_disable = rast->base.rasterizer_discard;
      }
   }

   if (dirty.fragment_front_face) {
      agx_ppp_push_merged(&ppp, FRAGMENT_FACE, cfg, ctx->zs->depth) {
         cfg.stencil_reference = ctx->stencil_ref.ref_value[0];
         cfg.line_width = rast->line_width;
         cfg.polygon_mode = rast->polygon_mode;
      }
   }

   if (dirty.fragment_front_face_2)
      agx_ppp_fragment_face_2(&ppp, object_type, &ctx->fs->b.info);

   if (dirty.fragment_front_stencil) {
      agx_ppp_push_packed(&ppp, ctx->zs->front_stencil.opaque,
                          FRAGMENT_STENCIL);
   }

   if (dirty.fragment_back_face) {
      agx_ppp_push_merged(&ppp, FRAGMENT_FACE, cfg, ctx->zs->depth) {
         bool twosided = ctx->zs->base.stencil[1].enabled;
         cfg.stencil_reference = ctx->stencil_ref.ref_value[twosided ? 1 : 0];
         cfg.line_width = rast->line_width;
         cfg.polygon_mode = rast->polygon_mode;
      }
   }

   if (dirty.fragment_back_face_2)
      agx_ppp_fragment_face_2(&ppp, object_type, &ctx->fs->b.info);

   if (dirty.fragment_back_stencil)
      agx_ppp_push_packed(&ppp, ctx->zs->back_stencil.opaque, FRAGMENT_STENCIL);

   assert(dirty.varying_counts_32 == dirty.varying_counts_16);
   assert(dirty.varying_counts_32 == dirty.output_select);

   if (dirty.output_select) {
      agx_ppp_push_merged_blobs(&ppp, AGX_OUTPUT_SELECT_LENGTH, &vs->uvs.osel,
                                &ctx->linked.fs->osel);

      agx_ppp_push_packed(&ppp, &batch->linked_varyings.counts_32,
                          VARYING_COUNTS);

      agx_ppp_push_packed(&ppp, &batch->linked_varyings.counts_16,
                          VARYING_COUNTS);
   }

   if (dirty.cull) {
      agx_ppp_push_merged(&ppp, CULL, cfg, ctx->rast->cull) {
         cfg.front_face_ccw = agx_front_face_ccw(ctx);
      }
   }

   if (dirty.cull_2) {
      agx_ppp_push(&ppp, CULL_2, cfg) {
         cfg.needs_primitive_id = batch->generate_primitive_id;
         cfg.clamp_w = true;
      }
   }

   if (dirty.fragment_shader) {
      unsigned frag_tex_count = ctx->stage[MESA_SHADER_FRAGMENT].texture_count;

      agx_ppp_push(&ppp, FRAGMENT_SHADER_WORD_0, cfg) {
         cfg.uniform_register_count = ctx->fs->b.info.push_count;
         cfg.preshader_register_count = ctx->fs->b.info.nr_preamble_gprs;
         cfg.texture_state_register_count =
            agx_nr_tex_descriptors(batch, ctx->fs);
         cfg.sampler_state_register_count =
            translate_sampler_state_count(ctx, MESA_SHADER_FRAGMENT);
         cfg.cf_binding_count = ctx->linked.fs->cf.nr_bindings;
      }

      agx_ppp_push(&ppp, FRAGMENT_SHADER_WORD_1, cfg) {
         cfg.pipeline = agx_build_pipeline(batch, ctx->fs, ctx->linked.fs,
                                           MESA_SHADER_FRAGMENT, 0, 0, NULL);
      }

      agx_ppp_push(&ppp, FRAGMENT_SHADER_WORD_2, cfg) {
         cfg.cf_bindings = batch->varyings;
      }

      agx_ppp_push(&ppp, FRAGMENT_SHADER_WORD_3, cfg) {
         /* XXX: This is wrong */
         cfg.unknown = frag_tex_count >= 4;
      }
   }

   if (dirty.occlusion_query) {
      agx_ppp_push(&ppp, FRAGMENT_OCCLUSION_QUERY, cfg) {
         if (ctx->active_queries && ctx->occlusion_query) {
            cfg.index = agx_get_oq_index(batch, ctx->occlusion_query);
         }
      }
   }

   if (dirty.output_size) {
      agx_ppp_push(&ppp, OUTPUT_SIZE, cfg)
         cfg.count = vs->uvs.size;
   }

   bool emitted = agx_gallium_ppp_fini(batch, &out, &ppp);
   assert(emitted);
   (void)emitted;
   ppp_updates++;

   assert(ppp_updates <= MAX_PPP_UPDATES);
   return out;
}

static enum agx_primitive
agx_primitive_for_pipe(enum mesa_prim mode)
{
   switch (mode) {
   case MESA_PRIM_POINTS:
      return AGX_PRIMITIVE_POINTS;
   case MESA_PRIM_LINES:
      return AGX_PRIMITIVE_LINES;
   case MESA_PRIM_LINE_STRIP:
      return AGX_PRIMITIVE_LINE_STRIP;
   case MESA_PRIM_LINE_LOOP:
      return AGX_PRIMITIVE_LINE_LOOP;
   case MESA_PRIM_TRIANGLES:
      return AGX_PRIMITIVE_TRIANGLES;
   case MESA_PRIM_TRIANGLE_STRIP:
      return AGX_PRIMITIVE_TRIANGLE_STRIP;
   case MESA_PRIM_TRIANGLE_FAN:
      return AGX_PRIMITIVE_TRIANGLE_FAN;
   case MESA_PRIM_QUADS:
      return AGX_PRIMITIVE_QUADS;
   case MESA_PRIM_QUAD_STRIP:
      return AGX_PRIMITIVE_QUAD_STRIP;
   default:
      UNREACHABLE("todo: other primitive types");
   }
}

static uint8_t *
agx_g17p_emit_draw_body(enum agx_chip chip, uint8_t *out,
                        const struct agx_draw *draw,
                        enum agx_primitive primitive, uint32_t restart_index,
                        uint32_t draw_state_offset)
{
   if (draw->indexed) {
      const struct agx_vdm_hal200_indexed_draw_fields fields = {
         .primitive = primitive,
         .index_size_B = agx_index_size_to_B(draw->index_size),
         .restart_enable = draw->restart,
         .restart_index = restart_index,
         .draw_state_context_offset = draw_state_offset,
         .index_buffer = draw->index_buffer,
         .index_buffer_range_B = draw->index_buffer_range_B,
         .first_index = draw->start,
         .index_count = draw->b.count[0],
         .instance_count = draw->b.count[1],
         .base_vertex = (int32_t)draw->index_bias,
      };
      if (draw_state_offset) {
         struct agx_vdm_hal200_indexed_draw packed;
         const bool valid = agx_vdm_hal200_indexed_draw_pack(
            chip, &fields, &packed);
         assert(valid);
         if (!valid)
            return out;
         memcpy(out, packed.bytes, packed.length);
         return out + packed.length;
      } else {
         struct agx_vdm_hal200_indexed_draw_body packed;
         const bool valid = agx_vdm_hal200_indexed_draw_body_pack(
            chip, &fields, &packed);
         assert(valid);
         if (!valid)
            return out;
         memcpy(out, packed.bytes, packed.length);
         return out + packed.length;
      }
   }

   const struct agx_vdm_hal200_direct_draw_fields fields = {
      .draw_state_context_offset = draw_state_offset,
      .primitive = primitive,
      .vertex_count = draw->b.count[0],
      .instance_count = draw->b.count[1],
      .vertex_start = draw->start,
   };
   if (draw_state_offset) {
      struct agx_vdm_hal200_direct_draw packed;
      const bool valid = agx_vdm_hal200_direct_draw_pack(
         chip, &fields, &packed);
      if (!valid) {
         mesa_loge("asahi: invalid G17P direct draw: primitive=%u vertices=%u "
                   "instances=%u start=%u state_offset=0x%x",
                   fields.primitive, fields.vertex_count, fields.instance_count,
                   fields.vertex_start, fields.draw_state_context_offset);
      }
      assert(valid);
      if (!valid)
         return out;
      memcpy(out, packed.bytes, packed.length);
      return out + packed.length;
   } else {
      struct agx_vdm_hal200_direct_draw_body packed;
      const bool valid = agx_vdm_hal200_direct_draw_body_pack(
         chip, &fields, &packed);
      assert(valid);
      if (!valid)
         return out;
      memcpy(out, packed.bytes, packed.length);
      return out + packed.length;
   }
}

static uint8_t *
agx_g17p_emit_indirect_draw(enum agx_chip chip, uint8_t *out,
                            const struct agx_draw *draw,
                            enum agx_primitive primitive,
                            uint32_t restart_index,
                            uint32_t draw_state_offset)
{
   const struct agx_vdm_hal200_indirect_draw_fields fields = {
      .draw_state_context_offset = draw_state_offset,
      .primitive = primitive,
      .indexed = draw->indexed,
      .index_size_B = draw->indexed ? agx_index_size_to_B(draw->index_size) : 0,
      .restart_enable = draw->restart,
      .restart_index = restart_index,
      .index_buffer = draw->index_buffer,
      .index_buffer_range_B = draw->index_buffer_range_B,
      .indirect_buffer = draw->b.ptr,
   };
   struct agx_vdm_hal200_indirect_draw packed;
   const bool valid = agx_vdm_hal200_indirect_draw_pack(
      chip, &fields, &packed);
   assert(valid);
   if (!valid)
      return out;

   memcpy(out, packed.bytes, packed.length);
   return out + packed.length;
}

static uint64_t
agx_index_buffer_rsrc_ptr(struct agx_batch *batch,
                          const struct pipe_draw_info *info, size_t *extent)
{
   assert(!info->has_user_indices && "cannot use user pointers with indirect");

   struct agx_resource *rsrc = agx_resource(info->index.resource);
   agx_batch_reads(batch, rsrc);

   *extent =
      ALIGN_POT(rsrc->layout.size_B - rsrc->layout.level_offsets_B[0], 4);
   return agx_map_gpu(rsrc);
}

static uint64_t
agx_index_buffer_direct_ptr(struct agx_batch *batch,
                            const struct pipe_draw_start_count_bias *draw,
                            const struct pipe_draw_info *info, size_t *extent)
{
   off_t offset = draw->start * info->index_size;
   uint32_t max_extent = draw->count * info->index_size;

   if (!info->has_user_indices) {
      uint64_t base = agx_index_buffer_rsrc_ptr(batch, info, extent);

      /* G17P fetches indices from a dword-aligned address: the low two bits
       * of the index list pointer are dropped, so a u16 draw starting at an
       * odd index (or a u8 draw at index % 4 != 0) reads from the preceding
       * aligned address. Copy such ranges to an aligned pool upload.
       */
      if (agx_native_graphics(agx_device(batch->ctx->base.screen)) &&
          ((base + offset) & 3)) {
         struct agx_resource *rsrc = agx_resource(info->index.resource);
         const uint8_t *cpu = (const uint8_t *)agx_bo_map(rsrc->bo) +
                              rsrc->layout.level_offsets_B[0];

         *extent = ALIGN_POT(max_extent, 4);
         return agx_pool_upload_aligned(&batch->pool, cpu + offset,
                                        max_extent, 64);
      }

      *extent = ALIGN_POT(MIN2(*extent - offset, max_extent), 4);
      return base + offset;
   } else {
      *extent = ALIGN_POT(max_extent, 4);

      return agx_pool_upload_aligned(&batch->pool,
                                     ((uint8_t *)info->index.user) + offset,
                                     draw->count * info->index_size, 64);
   }
}

static uint64_t
agx_index_buffer_ptr(struct agx_batch *batch, const struct pipe_draw_info *info,
                     const struct pipe_draw_start_count_bias *draw,
                     size_t *extent)
{
   if (draw)
      return agx_index_buffer_direct_ptr(batch, draw, info, extent);
   else
      return agx_index_buffer_rsrc_ptr(batch, info, extent);
}

static void
agx_ensure_cmdbuf_has_space(struct agx_batch *batch, struct agx_encoder *enc,
                            size_t space)
{
   bool vdm = enc == &batch->vdm;
   assert(vdm || (enc == &batch->cdm));

   size_t link_length =
      vdm ? AGX_VDM_STREAM_LINK_LENGTH : AGX_CDM_STREAM_LINK_LENGTH;

   /* Assert that we have space for a link tag */
   assert((enc->current + link_length) <= enc->end && "Encoder overflowed");

   /* Always leave room for a link tag, in case we run out of space later,
    * plus padding because VDM apparently overreads?
    *
    * 0x200 is not enough. 0x400 seems to work. 0x800 for safety.
    */
   space += link_length + 0x800;

   /* If there is room in the command buffer, we're done */
   if (likely((enc->end - enc->current) >= space))
      return;

   /* Otherwise, we need to allocate a new command buffer. We use memory owned
    * by the batch to simplify lifetime management for the BO.
    */
   size_t size = 65536;
   struct agx_bo *target_bo = NULL;
   struct agx_ptr T = agx_pool_alloc_aligned_with_bo(
      &batch->pool, size, 256, &target_bo);

   /* Jump from the old command buffer to the new command buffer */
   struct agx_device *dev = agx_device(batch->ctx->base.screen);
   if (vdm && agx_native_graphics(dev)) {
      const uint64_t alias =
         agx_batch_get_g17p_vdm_alias(batch, target_bo, T.gpu);
      uint32_t compact = 0;
      struct agx_vdm_stream_link_core link;
      const bool packed = agx_g17p_vdm_alias_compact(alias, &compact) &&
                          agx_vdm_stream_link_core_pack(dev->chip, compact,
                                                       &link);
      assert(packed);
      if (!packed)
         return;

      memcpy(enc->current, link.bytes, link.length);
   } else {
      agx_cs_jump((uint32_t *)enc->current, T.gpu, vdm);
   }

   /* Swap out the command buffer */
   enc->current = T.cpu;
   enc->end = enc->current + size;
}

static void
agx_ia_update(struct agx_batch *batch, const struct pipe_draw_info *info,
              uint64_t draw, uint64_t ib, uint64_t ib_range_el)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   if (!batch->cdm.bo) {
      batch->cdm = agx_encoder_allocate(batch, dev);
   }

   uint64_t ia_vertices = agx_get_query_address(
      batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_IA_VERTICES]);

   uint64_t ia_primitives = agx_get_query_address(
      batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_IA_PRIMITIVES]);

   uint64_t vs_invocations = agx_get_query_address(
      batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_VS_INVOCATIONS]);

   uint64_t c_prims = agx_get_query_address(
      batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_C_PRIMITIVES]);

   uint64_t c_invs = agx_get_query_address(
      batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_C_INVOCATIONS]);

   /* With a geometry/tessellation shader, clipper counters are written by the
    * pre-GS/tess prefix sum kernel since they depend on the output on the
    * geometry/tessellation shader. Without a geometry/tessellation shader,
    * they are written along with IA.
    */
   if (ctx->stage[MESA_SHADER_GEOMETRY].shader ||
       ctx->stage[MESA_SHADER_TESS_EVAL].shader) {

      c_prims = AGX_SCRATCH_PAGE_ADDRESS;
      c_invs = AGX_SCRATCH_PAGE_ADDRESS;
   }

   if (info->primitive_restart) {
      perf_debug(dev, "Input assembly counters with primitive restart");

      libagx_increment_ia_restart(batch, agx_1d(1024), AGX_BARRIER_ALL,
                                  ia_vertices, ia_primitives, vs_invocations,
                                  c_prims, c_invs, draw, ib, ib_range_el,
                                  info->restart_index, info->index_size,
                                  info->mode, ctx->patch_vertices);
   } else {
      perf_debug(dev, "Input assembly counters");

      libagx_increment_ia(batch, agx_1d(1), AGX_BARRIER_ALL, ia_vertices,
                          ia_primitives, vs_invocations, c_prims, c_invs, draw,
                          info->mode, ctx->patch_vertices);
   }
}

static uint64_t
agx_batch_heap(struct agx_batch *batch)
{
   struct agx_context *ctx = batch->ctx;

   if (!batch->heap) {
      uint32_t size = 128 * 1024 * 1024;

      if (!ctx->heap) {
         ctx->heap = pipe_buffer_create(ctx->base.screen, PIPE_BIND_GLOBAL,
                                        PIPE_USAGE_DEFAULT, size);
      }

      struct poly_heap heap = {
         .base = agx_resource(ctx->heap)->bo->va->addr,
         .size = size,
      };

      agx_batch_writes(batch, agx_resource(ctx->heap), 0);

      batch->heap =
         agx_pool_upload_aligned(&batch->pool, &heap, sizeof(heap), 8);
   }

   return batch->heap;
}

static uint64_t
agx_batch_geometry_params(struct agx_batch *batch, uint64_t input_index_buffer,
                          size_t index_buffer_size_B,
                          const struct pipe_draw_info *info,
                          const struct pipe_draw_start_count_bias *draw,
                          const struct pipe_draw_indirect_info *indirect)
{
   const uint32_t wg_size[3] = {64, 1, 1};

   struct poly_vertex_params vp;
   poly_vertex_params_init(&vp, batch->ctx->vs->b.info.outputs, wg_size);

   if (info->index_size) {
      vp.index_size_B = info->index_size;
      vp.index_buffer = input_index_buffer;
      vp.index_buffer_range_el = index_buffer_size_B / info->index_size;
   }

   struct poly_geometry_params params;
   poly_geometry_params_init(&params, info->mode, wg_size);

   params.flat_outputs =
      batch->ctx->stage[MESA_SHADER_FRAGMENT].shader->info.inputs_flat_shaded;

   params.xfb_offs_ptrs[0] = AGX_ZERO_PAGE_ADDRESS;
   params.xfb_offs_ptrs[1] = AGX_ZERO_PAGE_ADDRESS;
   params.xfb_offs_ptrs[2] = AGX_ZERO_PAGE_ADDRESS;
   params.xfb_offs_ptrs[3] = AGX_ZERO_PAGE_ADDRESS;

   for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->streamout.targets); ++i) {
      struct agx_streamout_target *so =
         agx_so_target(batch->ctx->streamout.targets[i]);
      struct agx_resource *rsrc = so ? agx_resource(so->offset) : NULL;

      uint32_t size;
      params.xfb_base_original[i] = agx_batch_get_so_address(batch, i, &size);
      params.xfb_size[i] = size;

      if (rsrc) {
         params.xfb_offs_ptrs[i] = agx_map_gpu(rsrc);
         agx_batch_writes(batch, rsrc, 0);
         batch->incoherent_writes = true;
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->prims_generated); ++i) {
      params.prims_generated_counter[i] =
         agx_get_query_address(batch, batch->ctx->prims_generated[i]);
   }

   /* The pre-GS kernel also adds to query_prims_generated_counter, which
    * backs Vulkan's VK_EXT_primitives_generated_query. GL's primitives
    * generated query already counts through prims_generated_counter, so
    * point this one at the scratch page rather than leaving it zero.
    */
   for (unsigned i = 0; i < ARRAY_SIZE(params.query_prims_generated_counter);
        ++i) {
      params.query_prims_generated_counter[i] = AGX_SCRATCH_PAGE_ADDRESS;
   }

   for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->tf_prims_generated); ++i) {
      params.xfb_prims_generated_counter[i] =
         agx_get_query_address(batch, batch->ctx->tf_prims_generated[i]);
   }

   if (batch->ctx->active_queries && batch->ctx->streamout.num_targets > 0) {
      for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->tf_overflow); ++i) {
         params.xfb_overflow[i] =
            agx_get_query_address(batch, batch->ctx->tf_overflow[i]);
      }

      params.xfb_any_overflow =
         agx_get_query_address(batch, batch->ctx->tf_any_overflow);
   } else {
      for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->tf_overflow); ++i) {
         params.xfb_overflow[i] = AGX_SCRATCH_PAGE_ADDRESS;
      }

      params.xfb_any_overflow = AGX_SCRATCH_PAGE_ADDRESS;
   }

   /* Calculate input primitive count for direct draws, and allocate the vertex
    * & count buffers. GPU calculates and allocates for indirect draws.
    */
   batch->uniforms.vertex_outputs = vp.outputs;
   params.count_buffer_stride = batch->ctx->gs->gs.count_words * 4;

   bool prefix_sum = batch->ctx->gs->gs.prefix_sum;
   if (!prefix_sum && params.count_buffer_stride) {
      struct agx_ptr T = agx_pool_alloc_aligned(&batch->pool, 16, 4);
      memset(T.cpu, 0, 16);
      params.count_buffer = T.gpu;
   }

   if (!indirect) {
      poly_vertex_params_set_draw(&vp, draw->count, info->instance_count);

      struct poly_gs_info *gsi = &batch->ctx->gs->gs;
      poly_geometry_params_set_draw(&params, info->mode, gsi->shape,
                                    gsi->max_indices, draw->count,
                                    info->instance_count);

      unsigned vb_size = poly_tcs_in_size(draw->count * info->instance_count,
                                          batch->uniforms.vertex_outputs);
      unsigned size = params.input_primitives * params.count_buffer_stride;

      if (size && prefix_sum) {
         params.count_buffer =
            agx_pool_alloc_aligned(&batch->pool, size, 4).gpu;
      }

      if (vb_size) {
         vp.output_buffer =
            agx_pool_alloc_aligned(&batch->pool, vb_size, 4).gpu;
      }

      if (gsi->shape == POLY_GS_SHAPE_DYNAMIC_INDEXED) {
         unsigned idx_size = params.input_primitives * gsi->max_indices;

         params.output_index_buffer =
            agx_pool_alloc_aligned_with_bo(&batch->pool, idx_size * 4, 4,
                                           &batch->geom_index_bo)
               .gpu;
         batch->geom_index = params.output_index_buffer;
      }
   }

   batch->uniforms.vertex_params =
      agx_pool_upload_aligned(&batch->pool, &vp, sizeof(vp), 8);

   return agx_pool_upload_aligned_with_bo(&batch->pool, &params, sizeof(params),
                                          8, &batch->geom_params_bo);
}

static uint64_t
agx_indirect_buffer_ptr(struct agx_batch *batch,
                        const struct pipe_draw_indirect_info *indirect)
{
   assert(indirect->buffer && "drawauto already handled");

   struct agx_resource *rsrc = agx_resource(indirect->buffer);
   agx_batch_reads(batch, rsrc);
   return agx_map_gpu(rsrc) + indirect->offset;
}

static void
agx_launch_gs_prerast(struct agx_batch *batch,
                      const struct pipe_draw_info *info,
                      const struct pipe_draw_start_count_bias *draws,
                      const struct pipe_draw_indirect_info *indirect)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);
   struct agx_compiled_shader *gs = ctx->gs;

   if (ctx->stage[MESA_SHADER_GEOMETRY].shader->is_xfb_passthrough)
      perf_debug(dev, "Transform feedbck");
   else
      perf_debug(dev, "Geometry shader");

   /* This is a graphics batch, so it may not have had a CDM encoder allocated
    * yet. Allocate that so we can start enqueueing compute work.
    */
   if (!batch->cdm.bo) {
      batch->cdm = agx_encoder_allocate(batch, dev);
   }

   agx_ensure_cmdbuf_has_space(
      batch, &batch->cdm,
      8 * (AGX_CDM_LAUNCH_WORD_0_LENGTH + AGX_CDM_LAUNCH_WORD_1_LENGTH +
           AGX_CDM_UNK_G14X_LENGTH + AGX_CDM_INDIRECT_LENGTH +
           AGX_CDM_GLOBAL_SIZE_LENGTH + AGX_CDM_LOCAL_SIZE_LENGTH +
           AGX_CDM_BARRIER_LENGTH));

   assert(!info->primitive_restart && "should have been lowered");

   uint64_t vp = batch->uniforms.vertex_params;
   uint64_t gp = batch->uniforms.geometry_params;
   struct agx_grid grid_vs, grid_gs;
   struct agx_workgroup wg = agx_workgroup(64, 1, 1);

   /* Setup grids */
   if (indirect) {
      uint64_t ib = 0;
      size_t ib_extent = 0;

      if (info->index_size) {
         ib = agx_index_buffer_ptr(batch, info, indirect ? NULL : draws,
                                   &ib_extent);
      }

      struct libagx_gs_setup_indirect_args gsi = {
         .index_buffer = ib,
         .index_buffer_range_el = ib_extent / info->index_size,
         .draw = agx_indirect_buffer_ptr(batch, indirect),
         .vp = batch->uniforms.vertex_params,
         .p = batch->uniforms.geometry_params,
         .heap = agx_batch_heap(batch),
         .vs_outputs = batch->uniforms.vertex_outputs,
         .index_size_B = info->index_size,
         .prim = info->mode,
         .is_prefix_summing = gs->gs.prefix_sum,
         .max_indices = gs->gs.max_indices,
         .shape = gs->gs.shape,
      };

      libagx_gs_setup_indirect_struct(batch, agx_1d(1), AGX_BARRIER_ALL, gsi);

      grid_vs = agx_grid_indirect_local(
         vp + offsetof(struct poly_vertex_params, grid));

      grid_gs = agx_grid_indirect_local(
         gp + offsetof(struct poly_geometry_params, grid));
   } else {
      grid_vs = agx_3d(draws->count, info->instance_count, 1);

      grid_gs =
         agx_3d(u_decomposed_prims_for_vertices(info->mode, draws->count),
                info->instance_count, 1);
   }

   /* Launch the vertex shader first */
   agx_launch(batch, grid_vs, wg, ctx->vs, ctx->linked.vs, ctx->vs->stage, 0, 0);

   /* Transform feedback and various queries require extra dispatching,
    * determine if we need that here.
    */
   enum pipe_statistics_query_index gs_queries[] = {
      PIPE_STAT_QUERY_GS_INVOCATIONS,
      PIPE_STAT_QUERY_GS_PRIMITIVES,
      PIPE_STAT_QUERY_C_PRIMITIVES,
      PIPE_STAT_QUERY_C_INVOCATIONS,
   };

   bool xfb_or_queries = ctx->stage[MESA_SHADER_GEOMETRY].shader->has_xfb_info;

   for (unsigned i = 0; i < ARRAY_SIZE(gs_queries); ++i) {
      xfb_or_queries |= (ctx->pipeline_statistics[gs_queries[i]] != NULL);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(ctx->prims_generated); ++i) {
      xfb_or_queries |= (ctx->prims_generated[i] != NULL);
   }

   /* If there is a count shader, launch it and prefix sum the results. */
   if (gs->gs_count && xfb_or_queries) {
      perf_debug(dev, "Geometry shader count");
      agx_launch(batch, grid_gs, wg, gs->gs_count, NULL, MESA_SHADER_GEOMETRY,
                 0, 0);
   }

   if (gs->gs.prefix_sum) {
      libagx_prefix_sum_geom(batch, agx_1d(1024 * gs->gs.count_words),
                             AGX_BARRIER_ALL, gp);
   }

   /* Pre-GS shader */
   if (xfb_or_queries) {
      perf_debug(dev, "Geometry shader transform feedback / query program");
      agx_launch(batch, agx_1d(1), agx_workgroup(1, 1, 1), gs->pre_gs, NULL,
                 MESA_SHADER_COMPUTE, 0, 0);
   }

   /* Pre-rast geometry shader */
   agx_launch(batch, grid_gs, wg, gs, NULL, MESA_SHADER_GEOMETRY, 0, 0);
}

static void
agx_draw_without_restart(struct agx_batch *batch,
                         const struct pipe_draw_info *info,
                         unsigned drawid_offset,
                         const struct pipe_draw_indirect_info *indirect,
                         const struct pipe_draw_start_count_bias *draw)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   perf_debug(dev, "Unrolling primitive restart due to GS/XFB");

   agx_batch_init_state(batch);

   size_t ib_extent = 0;
   uint64_t ib;

   /* The rest of this function handles only the general case of indirect
    * multidraws, so synthesize an indexed indirect draw now if we need one for
    * a direct draw (necessarily only one). This unifies the code paths.
    */
   struct pipe_draw_indirect_info indirect_synthesized = {.draw_count = 1};

   if (!indirect) {
      /* Adds in the offset so set to 0 in the desc */
      ib = agx_index_buffer_direct_ptr(batch, draw, info, &ib_extent);

      uint32_t desc[5] = {draw->count, info->instance_count, 0,
                          draw->index_bias, info->start_instance};

      u_upload_data_ref(ctx->base.const_uploader, 0, sizeof(desc), 4, &desc,
                        &indirect_synthesized.offset,
                        &indirect_synthesized.buffer);

      indirect = &indirect_synthesized;
   } else {
      /* Does not add in offset, the unroll kernel uses the desc's offset */
      ib = agx_index_buffer_rsrc_ptr(batch, info, &ib_extent);
   }

   /* Next, we unroll the index buffer used by the indirect draw */
   if (!batch->cdm.bo)
      batch->cdm = agx_encoder_allocate(batch, dev);

   /* Allocate output indirect draw descriptors. This is exact. */
   struct agx_resource out_draws_rsrc = {0};
   struct agx_ptr out_draws = agx_pool_alloc_aligned_with_bo(
      &batch->pool, 5 * sizeof(uint32_t) * indirect->draw_count, 4,
      &out_draws_rsrc.bo);

   struct libagx_unroll_restart_args unroll = {
      .heap = agx_batch_heap(batch),
      .index_buffer = ib,
      .out_draw = out_draws.gpu,
      .restart_index = info->restart_index,
      .index_buffer_size_el = ib_extent / info->index_size,
      .index_size_log2 = util_logbase2(info->index_size),
      .flatshade_first = batch->ctx->rast->base.flatshade_first,
      .in_draw = agx_indirect_buffer_ptr(batch, indirect),
   };

   /* Unroll the index buffer for each draw */
   libagx_unroll_restart_struct(batch, agx_1d(1024 * indirect->draw_count),
                                AGX_BARRIER_ALL, unroll,
                                poly_compact_prim(info->mode));

   /* Now draw the results without restart */
   struct pipe_draw_info new_info = {
      .mode = u_decomposed_prim(info->mode),
      .index_size = info->index_size,
      .index.resource = ctx->heap,
      .increment_draw_id = info->increment_draw_id,
      .index_bias_varies = info->index_bias_varies,
   };

   struct pipe_draw_indirect_info new_indirect = *indirect;
   new_indirect.buffer = &out_draws_rsrc.base;
   new_indirect.offset = out_draws.gpu - out_draws_rsrc.bo->va->addr;
   new_indirect.stride = 5 * sizeof(uint32_t);

   ctx->active_draw_without_restart = true;
   ctx->base.draw_vbo(&ctx->base, &new_info, drawid_offset, &new_indirect, NULL,
                      1);
   ctx->active_draw_without_restart = false;
}

static bool
agx_needs_passthrough_gs(struct agx_context *ctx,
                         const struct pipe_draw_info *info,
                         const struct pipe_draw_indirect_info *indirect,
                         bool *xfb_only)
{
   /* If there is already a geometry shader in the pipeline, we do not need to
    * apply a passthrough GS of our own.
    */
   if (ctx->stage[MESA_SHADER_GEOMETRY].shader)
      return false;

   /* Rendering adjacency requires a GS, add a passthrough since we don't have
    * one.
    */
   if (mesa_prim_has_adjacency(info->mode)) {
      perf_debug_ctx(ctx, "Using passthrough GS due to adjacency primitives");
      return true;
   }

   /* TODO: Handle fans properly, we need to plumb a sysval. */
   if (info->mode == MESA_PRIM_TRIANGLE_FAN &&
       ctx->rast->base.flatshade_first &&
       ctx->stage[MESA_SHADER_FRAGMENT].shader->info.inputs_flat_shaded) {

      perf_debug_ctx(ctx, "Using passthrough GS due to first tri fans");
      return true;
   }

   /* TODO: this is really sloppy, we should add a VDM kernel for this. */
   if ((indirect || info->mode == MESA_PRIM_PATCHES) && ctx->active_queries &&
       ctx->prims_generated[0]) {
      perf_debug_ctx(ctx, "Using passthrough GS due to indirect prim query");
      return true;
   }

   /* Edge flags are emulated with a geometry shader */
   if (has_edgeflags(ctx, info->mode)) {
      perf_debug_ctx(ctx, "Using passthrough GS due to edge flags");
      return true;
   }

   /* Transform feedback is layered on geometry shaders, so if transform
    * feedback is used, we need a GS.
    */
   struct agx_uncompiled_shader *last_vtx =
      ctx->stage[MESA_SHADER_TESS_EVAL].shader
         ?: ctx->stage[MESA_SHADER_VERTEX].shader;

   if (last_vtx->has_xfb_info && ctx->streamout.num_targets) {
      *xfb_only = true;
      return true;
   }

   /* Otherwise, we don't need one */
   return false;
}

static enum mesa_prim
agx_tess_output_prim(struct agx_uncompiled_shader *tcs,
                     struct agx_uncompiled_shader *tes)
{
   if ((tcs && tcs->tess.point_mode) || tes->tess.point_mode) {
      return MESA_PRIM_POINTS;
   } else if (TESS_PRIMITIVE_ISOLINES ==
              MAX2(tcs ? tcs->tess.primitive : 0, tes->tess.primitive)) {
      return MESA_PRIM_LINES;
   } else {
      return MESA_PRIM_TRIANGLES;
   }
}

static struct agx_uncompiled_shader *
agx_get_passthrough_gs(struct agx_context *ctx,
                       struct agx_uncompiled_shader *prev_cso,
                       enum mesa_prim mode, bool xfb_passthrough)
{
   bool edgeflags = has_edgeflags(ctx, mode);

   if (mode == MESA_PRIM_PATCHES) {
      mode = agx_tess_output_prim(ctx->stage[MESA_SHADER_TESS_CTRL].shader,
                                  ctx->stage[MESA_SHADER_TESS_EVAL].shader);
   }

   /* Only handle the polygon mode when edge flags are in use, because
    * nir_passthrough_gs doesn't handle transform feedback + polygon mode
    * properly. Technically this can break edge flags + transform feedback
    * but that's firmly in "doctor, it hurts when I do this" territory, and
    * I'm not sure that's even possible to hit. TODO: Reevaluate.
    */
   unsigned poly_mode =
      edgeflags ? ctx->rast->base.fill_front : PIPE_POLYGON_MODE_FILL;

   if (prev_cso->passthrough_progs[mode][poly_mode][edgeflags])
      return prev_cso->passthrough_progs[mode][poly_mode][edgeflags];

   struct blob_reader reader;
   blob_reader_init(&reader, prev_cso->early_serialized_nir.data,
                    prev_cso->early_serialized_nir.size);
   nir_shader *prev = nir_deserialize(NULL, &agx_nir_options, &reader);

   nir_shader *gs = nir_create_passthrough_gs(
      &agx_nir_options, prev, mode, rast_prim(mode, poly_mode), edgeflags,
      false /* force line strip out */, false);

   /* The passthrough program takes the vertex shader's place in front of the
    * fragment shader, so it inherits what linking told the vertex shader
    * about the fragment interpolation qualifiers. The copy program derived
    * from it can then inline its UVS indices like the vertex shader does,
    * which the AGX3 vertex writeout requires. Other chips keep the generic
    * passthrough program: there, inheriting the qualifiers breaks flat
    * varyings drawn through transform feedback.
    */
   if (agx_native_graphics(agx_device(ctx->base.screen))) {
      gs->info.known_interpolation_qualifiers =
         prev->info.known_interpolation_qualifiers;
      gs->info.linear_varyings = prev->info.linear_varyings;
      gs->info.perspective_varyings = prev->info.perspective_varyings;
   }

   ralloc_free(prev);

   struct agx_uncompiled_shader *cso = pipe_shader_from_nir(&ctx->base, gs);
   cso->is_xfb_passthrough = xfb_passthrough;
   prev_cso->passthrough_progs[mode][poly_mode][edgeflags] = cso;
   return cso;
}

static void
agx_apply_passthrough_gs(struct agx_context *ctx,
                         const struct pipe_draw_info *info,
                         unsigned drawid_offset,
                         const struct pipe_draw_indirect_info *indirect,
                         const struct pipe_draw_start_count_bias *draws,
                         unsigned num_draws, bool xfb_passthrough)
{
   mesa_shader_stage prev_stage = ctx->stage[MESA_SHADER_TESS_EVAL].shader
                                     ? MESA_SHADER_TESS_EVAL
                                     : MESA_SHADER_VERTEX;
   struct agx_uncompiled_shader *prev_cso = ctx->stage[prev_stage].shader;

   assert(ctx->stage[MESA_SHADER_GEOMETRY].shader == NULL);

   /* Draw with passthrough */
   ctx->base.bind_gs_state(
      &ctx->base,
      agx_get_passthrough_gs(ctx, prev_cso, info->mode, xfb_passthrough));
   ctx->base.draw_vbo(&ctx->base, info, drawid_offset, indirect, draws,
                      num_draws);
   ctx->base.bind_gs_state(&ctx->base, NULL);
}

static void
util_draw_multi_unroll_indirect(struct pipe_context *pctx,
                                const struct pipe_draw_info *info,
                                const struct pipe_draw_indirect_info *indirect,
                                const struct pipe_draw_start_count_bias *draws)
{
   for (unsigned i = 0; i < indirect->draw_count; ++i) {
      const struct pipe_draw_indirect_info subindirect = {
         .buffer = indirect->buffer,
         .count_from_stream_output = indirect->count_from_stream_output,
         .offset = indirect->offset + (i * indirect->stride),
         .draw_count = 1,
      };

      pctx->draw_vbo(pctx, info, i, &subindirect, draws, 1);
   }
}

static void
util_draw_multi_upload_indirect(struct pipe_context *pctx,
                                const struct pipe_draw_info *info,
                                const struct pipe_draw_indirect_info *indirect,
                                const struct pipe_draw_start_count_bias *draws)
{
   struct pipe_draw_indirect_info indirect_ = *indirect;
   u_upload_data_ref(pctx->const_uploader, 0, 4, 4, &indirect->draw_count,
                     &indirect_.indirect_draw_count_offset,
                     &indirect_.indirect_draw_count);

   pctx->draw_vbo(pctx, info, 0, &indirect_, draws, 1);
}

static void
agx_upload_draw_params(struct agx_batch *batch,
                       const struct pipe_draw_indirect_info *indirect,
                       const struct pipe_draw_start_count_bias *draws,
                       const struct pipe_draw_info *info)
{
   if (indirect) {
      uint64_t address = agx_indirect_buffer_ptr(batch, indirect);

      /* To implement draw parameters, we use the last 2 words of the
       * indirect draw descriptor. Offset by 3 words for indexed draw (5
       * total) and 2 words for non-indexed (4 total).  See the layouts of
       * indexed vs non-indexed draw descriptors.
       *
       * This gives us a consistent layout
       *
       *    uint32_t first_vertex;
       *    uint32_t base_instance;
       *
       * and we can implement load_first_vertex & load_base_instance without
       * checking for indexing.
       */
      uint32_t offset = info->index_size ? 3 : 2;
      batch->uniforms.tables[AGX_SYSVAL_TABLE_PARAMS] = address + offset * 4;
   } else {
      /* Upload just those two words. */
      uint32_t params[2] = {
         info->index_size ? draws->index_bias : draws->start,
         info->start_instance,
      };

      batch->uniforms.tables[AGX_SYSVAL_TABLE_PARAMS] =
         agx_pool_upload_aligned(&batch->pool, params, sizeof(params), 4);
   }
}

static void
agx_draw_patches(struct agx_context *ctx, const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws)
{
   struct agx_device *dev = agx_device(ctx->base.screen);
   perf_debug(dev, "Tessellation");

   struct agx_uncompiled_shader *tcs = ctx->stage[MESA_SHADER_TESS_CTRL].shader;
   struct agx_uncompiled_shader *tes = ctx->stage[MESA_SHADER_TESS_EVAL].shader;

   assert(tes != NULL && "required with patches");

   unsigned patch_vertices = ctx->patch_vertices;

   /* OpenGL allows omitting the tcs, fill in a passthrough program if needed.
    * In principle, we could optimize this case, but I don't think it matters.
    */
   bool unbind_tcs_when_done = false;
   if (!tcs) {
      struct agx_uncompiled_shader *vs = ctx->stage[MESA_SHADER_VERTEX].shader;

      assert(patch_vertices >= 1 &&
             patch_vertices <= ARRAY_SIZE(vs->passthrough_tcs));

      if (!vs->passthrough_tcs[patch_vertices - 1]) {
         struct blob_reader reader;
         blob_reader_init(&reader, vs->early_serialized_nir.data,
                          vs->early_serialized_nir.size);
         nir_shader *vs_nir = nir_deserialize(NULL, &agx_nir_options, &reader);
         nir_shader *nir = nir_create_passthrough_tcs(&agx_nir_options, vs_nir,
                                                      patch_vertices);
         ralloc_free(vs_nir);

         /* Lower the tess level sysvals and gather info, since mesa/st won't do
          * either for us.
          */
         NIR_PASS(_, nir, nir_lower_system_values);

         nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

         vs->passthrough_tcs[patch_vertices - 1] =
            pipe_shader_from_nir(&ctx->base, nir);
      }

      tcs = vs->passthrough_tcs[patch_vertices - 1];
      ctx->base.bind_tcs_state(&ctx->base, tcs);
      unbind_tcs_when_done = true;
   }

   enum tess_primitive_mode mode =
      MAX2(tcs->tess.primitive, tes->tess.primitive);
   enum gl_tess_spacing spacing = MAX2(tcs->tess.spacing, tes->tess.spacing);

   enum pipe_tess_spacing pspacing = spacing == TESS_SPACING_EQUAL
                                        ? PIPE_TESS_SPACING_EQUAL
                                     : spacing == TESS_SPACING_FRACTIONAL_ODD
                                        ? PIPE_TESS_SPACING_FRACTIONAL_ODD
                                        : PIPE_TESS_SPACING_FRACTIONAL_EVEN;

   bool point_mode = MAX2(tcs->tess.point_mode, tes->tess.point_mode);
   enum mesa_prim out_prim = agx_tess_output_prim(tcs, tes);

   enum poly_tess_partitioning partitioning =
      (enum poly_tess_partitioning)pspacing;

   struct agx_bo *draw_bo = NULL;
   size_t draw_stride = 5 * sizeof(uint32_t);

   struct agx_batch *batch = agx_get_batch(ctx);
   agx_batch_init_state(batch);

   if (!batch->cdm.bo) {
      batch->cdm = agx_encoder_allocate(batch, dev);
   }

   agx_upload_draw_params(batch, indirect, draws, info);
   agx_upload_vbos(batch);
   agx_update_vs(batch, info->index_size);
   agx_update_tcs(ctx, info);

   /* Bail before building any tessellation state on a program that cannot
    * run, undoing the passthrough TCS this function may have bound. The
    * vertex CSO and ctx->in_tess are not touched until later, so there is
    * nothing else to restore here.
    */
   if (agx_drop_for_unusable_program(ctx, ctx->vs, ctx->linked.vs != NULL,
                                     "vertex") ||
       agx_drop_for_unusable_program(ctx, ctx->tcs, true,
                                     "tessellation control")) {
      if (unbind_tcs_when_done)
         ctx->base.bind_tcs_state(&ctx->base, NULL);
      return;
   }

   /* XXX */
   ctx->stage[MESA_SHADER_TESS_CTRL].dirty = ~0;
   ctx->stage[MESA_SHADER_TESS_EVAL].dirty = ~0;
   agx_update_descriptors(batch, ctx->vs);
   agx_update_descriptors(batch, ctx->tcs);

   batch->uniforms.vertex_outputs = ctx->vs->b.info.outputs;

   const uint32_t wg_size[3] = {64, 1, 1};

   struct poly_vertex_params vp;
   poly_vertex_params_init(&vp, batch->ctx->vs->b.info.outputs, wg_size);

   if (info->index_size) {
      size_t ib_extent = 0;
      vp.index_size_B = info->index_size;
      vp.index_buffer = agx_index_buffer_ptr(batch, info, draws, &ib_extent);
      vp.index_buffer_range_el = ib_extent / info->index_size;
   }

   /* Setup parameters */
   uint64_t heap = agx_batch_heap(batch);
   assert((tcs->tess.output_stride & 3) == 0 && "must be aligned");

   struct poly_tess_params params = {
      .heap = heap,
      .tcs_stride_el = tcs->tess.output_stride / 4,
      .statistic = agx_get_query_address(
         batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_DS_INVOCATIONS]),
      .input_patch_size = patch_vertices,
      .output_patch_size = tcs->tess.output_patch_size,
      .tcs_patch_constants = tcs->tess.nr_patch_outputs,
      .tcs_per_vertex_outputs = tcs->tess.per_vertex_outputs,
      .patch_coord_buffer = agx_resource(ctx->heap)->bo->va->addr,
      .partitioning = partitioning,
      .points_mode = point_mode,
      .isolines = mode == TESS_PRIMITIVE_ISOLINES,
   };

   if (!point_mode && tes->tess.primitive != TESS_PRIMITIVE_ISOLINES) {
      params.ccw = !tes->tess.ccw;
   }

   memcpy(&params.tess_level_outer_default, ctx->default_outer_level,
          sizeof(ctx->default_outer_level));
   memcpy(&params.tess_level_inner_default, ctx->default_inner_level,
          sizeof(ctx->default_inner_level));

   struct agx_grid vs_grid, tcs_grid, tess_grid;

   if (indirect == NULL) {
      poly_vertex_params_set_draw(&vp, draws->count, info->instance_count);

      unsigned in_patches = draws->count / patch_vertices;
      if (in_patches == 0)
         return;

      /* TCS invocation counter increments once per-patch */
      agx_query_increment_cpu(
         ctx, ctx->pipeline_statistics[PIPE_STAT_QUERY_HS_INVOCATIONS],
         in_patches);

      unsigned unrolled_patches = in_patches * info->instance_count;

      uint32_t alloc = 0;
      uint32_t tcs_out_offs = alloc;
      alloc += unrolled_patches * tcs->tess.output_stride;

      uint32_t patch_coord_offs = alloc;
      alloc += unrolled_patches * 4;

      uint32_t count_offs = alloc;
      alloc += unrolled_patches * sizeof(uint32_t);

      uint32_t draw_offs = alloc;
      alloc += draw_stride;

      struct agx_ptr blob =
         agx_pool_alloc_aligned_with_bo(&batch->pool, alloc, 4, &draw_bo);

      params.tcs_buffer = blob.gpu + tcs_out_offs;
      params.patches_per_instance = in_patches;
      params.coord_allocs = blob.gpu + patch_coord_offs;
      params.nr_patches = unrolled_patches;
      params.out_draws = blob.gpu + draw_offs;
      params.counts = blob.gpu + count_offs;

      unsigned vb_size = poly_tcs_in_size(draws->count * info->instance_count,
                                          batch->uniforms.vertex_outputs);
      vp.output_buffer = agx_pool_alloc_aligned(&batch->pool, vb_size, 4).gpu;

      vs_grid = agx_3d(draws->count, info->instance_count, 1);
      tcs_grid = agx_3d(in_patches * tcs->tess.output_patch_size,
                        info->instance_count, 1);

      tess_grid = agx_1d(unrolled_patches);
   } else if (indirect) {
      params.out_draws =
         agx_pool_alloc_aligned_with_bo(&batch->pool, draw_stride, 4, &draw_bo)
            .gpu;
   }

   uint64_t vertex_state =
      agx_pool_upload_aligned(&batch->pool, &vp, sizeof(vp), 8);
   batch->uniforms.vertex_params = vertex_state;

   uint64_t state =
      agx_pool_upload_aligned(&batch->pool, &params, sizeof(params), 4);
   batch->uniforms.tess_params = state;

   if (indirect) {
      perf_debug(dev, "Indirect tessellation");

      uint32_t grid_stride = sizeof(uint32_t) * 6;

      uint64_t indirect_ptr = agx_indirect_buffer_ptr(batch, indirect);

      uint64_t tcs_statistic = agx_get_query_address(
         batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_HS_INVOCATIONS]);

      /* Allocate 3x indirect global+local grids for VS/TCS/tess */
      uint64_t grids =
         agx_pool_alloc_aligned(&batch->pool, grid_stride * 3, 4).gpu;

      libagx_tess_setup_indirect(batch, agx_1d(1), AGX_BARRIER_ALL, state,
                                 grids, vertex_state, indirect_ptr, 0, 0,
                                 0 /* XXX: Index buffer */,
                                 ctx->vs->b.info.outputs, tcs_statistic);

      vs_grid = agx_grid_indirect_local(grids + 0 * grid_stride);
      tcs_grid = agx_grid_indirect_local(grids + 1 * grid_stride);
      tess_grid = agx_grid_indirect_local(grids + 2 * grid_stride);
   }

   agx_launch(batch, vs_grid, agx_workgroup(64, 1, 1), ctx->vs, ctx->linked.vs,
              MESA_SHADER_VERTEX, 0, 0);

   agx_launch(batch, tcs_grid, agx_workgroup(tcs->tess.output_patch_size, 1, 1),
              ctx->tcs, NULL, MESA_SHADER_TESS_CTRL, 0, 0);

   uint64_t c_prims = agx_get_query_address(
      batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_C_PRIMITIVES]);
   uint64_t c_invs = agx_get_query_address(
      batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_C_INVOCATIONS]);

   /* If there's a geometry shader, it will increment the clipper stats.
    * Otherwise, we do when tessellating.
    */
   if (ctx->stage[MESA_SHADER_GEOMETRY].shader) {
      c_prims = AGX_SCRATCH_PAGE_ADDRESS;
      c_invs = AGX_SCRATCH_PAGE_ADDRESS;
   }

   /* Generate counts, then prefix sum them, then finally tessellate. */
   libagx_tessellate(batch, tess_grid, AGX_BARRIER_ALL, mode,
                     POLY_TESS_MODE_COUNT, state);
   libagx_prefix_sum_tess(batch, agx_1d(1024), AGX_BARRIER_ALL, state, c_prims,
                          c_invs, 0, c_prims || c_invs);
   libagx_tessellate(batch, tess_grid, AGX_BARRIER_ALL, mode,
                     POLY_TESS_MODE_WITH_COUNTS, state);

   /* Face culling state needs to be specialized for tess */
   ctx->dirty |= AGX_DIRTY_RS;

   /* Run TES as VS */
   void *vs_cso = ctx->stage[MESA_SHADER_VERTEX].shader;
   void *tes_cso = ctx->stage[MESA_SHADER_TESS_EVAL].shader;
   ctx->base.bind_vs_state(&ctx->base, tes_cso);
   ctx->in_tess = true;

   struct pipe_draw_info draw_info = {
      .mode = out_prim,
      .index_size = 4,
      .index.resource = ctx->heap,
      .instance_count = 1,
   };

   /* Wrap the pool allocation in a fake resource for meta-Gallium use */
   struct agx_resource indirect_rsrc = {.bo = draw_bo};

   struct pipe_draw_indirect_info copy_indirect = {
      .buffer = &indirect_rsrc.base,
      .offset = params.out_draws - draw_bo->va->addr,
      .stride = draw_stride,
      .draw_count = 1,
   };

   ctx->base.draw_vbo(&ctx->base, &draw_info, 0, &copy_indirect, NULL, 1);

   /* Restore vertex state */
   ctx->base.bind_vs_state(&ctx->base, vs_cso);
   ctx->in_tess = false;

   if (unbind_tcs_when_done) {
      ctx->base.bind_tcs_state(&ctx->base, NULL);
   }
}

/*
 * From the ARB_texture_barrier spec:
 *
 *  Specifically, the values of rendered fragments are undefined if any
 *  shader stage fetches texels and the same texels are written via fragment
 *  shader outputs, even if the reads and writes are not in the same Draw
 *  call, unless any of the following exceptions apply:
 *
 *  - The reads and writes are from/to disjoint sets of texels (after
 *    accounting for texture filtering rules).
 *
 *  - There is only a single read and write of each texel, and the read is in
 *    the fragment shader invocation that writes the same texel (e.g. using
 *    "texelFetch2D(sampler, ivec2(gl_FragCoord.xy), 0);").
 *
 *  - If a texel has been written, then in order to safely read the result
 *    a texel fetch must be in a subsequent Draw separated by the command
 *
 *      void TextureBarrier(void);
 *
 *    TextureBarrier() will guarantee that writes have completed and caches
 *    have been invalidated before subsequent Draws are executed."
 *
 * The wording is subtle, but we are not required to flush implicitly for
 * feedback loops, even though we're a tiler. What we are required to do is
 * decompress framebuffers involved in feedback loops, because otherwise
 * the hardware will race itself with exception #1, where we have a disjoint
 * group texels that intersects a compressed tile being written out.
 */
static void
agx_legalize_feedback_loop_surf(struct agx_context *ctx,
                                struct agx_resource *rsrc,
                                struct pipe_surface *surf, unsigned bit)
{
   if (agx_resource(surf->texture) != rsrc || !rsrc->layout.compressed)
      return;

   /* Decompress if we can and shadow if we can't. */
   if (rsrc->base.bind & PIPE_BIND_SHARED) {
      struct agx_batch *batch = agx_get_batch(ctx);

      /* If we already did in-place decompression for this one */
      if (batch->feedback & bit)
         return;

      /* Use our current context batch. If it already touched
       * this buffer, that will have been flushed above.
       */
      agx_decompress_inplace(batch, surf, "Texture feedback loop");

      /* Mark it as a feedback cbuf, so it will be written to
       * uncompressed despite having a compressed layout.
       */
      batch->feedback |= bit;
   } else {
      agx_decompress(ctx, rsrc, "Texture feedback loop");
   }

   /* Not required by the spec, just for debug */
   if (agx_device(ctx->base.screen)->debug & AGX_DBG_FEEDBACK)
      agx_flush_writer(ctx, rsrc, "Feedback loop");
}

static void
agx_legalize_feedback_loops(struct agx_context *ctx)
{
   /* Trust that u_blitter knows what it's doing */
   if (ctx->blitter->running)
      return;

   for (unsigned stage = 0; stage < ARRAY_SIZE(ctx->stage); ++stage) {
      if (!(ctx->stage[stage].dirty & AGX_STAGE_DIRTY_IMAGE))
         continue;

      for (unsigned i = 0; i < ctx->stage[stage].texture_count; ++i) {
         if (!ctx->stage[stage].textures[i])
            continue;

         struct agx_resource *rsrc = ctx->stage[stage].textures[i]->rsrc;

         for (unsigned cb = 0; cb < ctx->framebuffer.nr_cbufs; ++cb) {
            agx_legalize_feedback_loop_surf(
               ctx, rsrc, &ctx->framebuffer.cbufs[cb], PIPE_CLEAR_COLOR0 << i);
         }

         /* TODO: Separate stencil? */
         agx_legalize_feedback_loop_surf(ctx, rsrc, &ctx->framebuffer.zsbuf,
                                         PIPE_CLEAR_DEPTH);
      }
   }
}

/*
 * Usually, non-attachment stores must be barriered explicitly by the app using
 * glMemoryBarrier. Transform feedback buffers are annoyingly excluded from this
 * requirement, so we need to handle those hazards ourselves. Transform feedback
 * will barrier with itself so we only need to consider write-after-read
 * hazards.
 *
 * The general case does require a flush. Consider binding a buffer as a UBO,
 * drawing with a fragment shader reading that UBO, then rebinding as XFB, and
 * drawing with XFB stores. We have to split the batch in the middle.
 * gles-3.0-transform-feedback-uniform-buffer-object does this.
 */
static void
agx_legalize_xfb(struct agx_context *ctx)
{
   /* If this draw isn't writing transform feedback, there's nothing to worry
    * about.
    */
   if (!ctx->streamout.num_targets ||
       !agx_last_uncompiled_vgt(ctx)->has_xfb_info)
      return;

   /* Otherwise, flush the readers of anything written by transform feedback. */
   for (unsigned i = 0; i < ctx->streamout.num_targets; ++i) {
      struct agx_streamout_target *tgt =
         agx_so_target(ctx->streamout.targets[i]);

      if (tgt != NULL) {
         agx_flush_readers(ctx, agx_resource(tgt->base.buffer),
                           "Transform feedback buffer");
      }
   }
}

static void
agx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
             unsigned drawid_offset,
             const struct pipe_draw_indirect_info *indirect,
             const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct agx_context *ctx = agx_context(pctx);

   if (unlikely(!agx_render_condition_check(ctx)))
      return;

   if (num_draws > 1) {
      util_draw_multi(pctx, info, drawid_offset, indirect, draws, num_draws);
      return;
   }

   if (indirect && indirect->draw_count > 1 && !indirect->indirect_draw_count) {
      assert(drawid_offset == 0);
      assert(num_draws == 1);

      util_draw_multi_unroll_indirect(pctx, info, indirect, draws);
      return;
   }

   if (indirect && indirect->count_from_stream_output) {
      agx_draw_vbo_from_xfb(pctx, info, drawid_offset, indirect);
      return;
   }

   /* DrawAuto can resolve to an empty direct draw after reading its stream
    * output count. Empty draws have no shader or query side effects, and the
    * G17P command encoder requires nonzero vertex and instance counts. Keep
    * indirect draws intact: their counts are supplied by the GPU buffer.
    */
   if (!indirect && (!draws->count || !info->instance_count))
      return;

   /* G17P's index fetch starts at a dword boundary even for U8/U16
    * indices. Qt's image strips commonly start at the second U16 in an
    * element buffer, which otherwise fetches the preceding restart index
    * and loses one triangle. Realign before acquiring a batch: mapping with
    * PIPE_MAP_READ must synchronize any GPU writer of the original indices.
    * Aligned draws, indirect draws, and uploaded user indices keep their
    * existing paths.
    */
   const uint64_t index_offset = indirect ? 0 :
      (uint64_t)draws->start * info->index_size;
   if (!indirect && info->index_size && !info->has_user_indices &&
       agx_native_graphics(agx_device(pctx->screen)) &&
       (index_offset & 3)) {
      const uint64_t bytes = (uint64_t)draws->count * info->index_size;
      struct pipe_resource *source = info->index.resource;
      if (index_offset >= source->width0 ||
          bytes > source->width0 - index_offset)
         return;

      struct pipe_transfer *transfer = NULL;
      const void *indices = pipe_buffer_map_range(
         pctx, source, index_offset, bytes, PIPE_MAP_READ, &transfer);
      if (!indices)
         return;

      struct pipe_draw_info aligned = *info;
      aligned.index.resource = NULL;
      unsigned offset = 0;
      u_upload_data_ref(pctx->stream_uploader, 0, bytes, 64, indices,
                        &offset, &aligned.index.resource);
      pipe_buffer_unmap(pctx, transfer);
      if (aligned.index.resource) {
         struct pipe_draw_start_count_bias draw = *draws;
         draw.start = offset / info->index_size;
         agx_draw_vbo(pctx, &aligned, drawid_offset, NULL, &draw, 1);
         pipe_resource_reference(&aligned.index.resource, NULL);
      }
      return;
   }

   /* TODO: stop cheating */
   if (indirect && indirect->indirect_draw_count) {
      perf_debug_ctx(ctx, "multi-draw indirect");
      util_draw_indirect(pctx, info, drawid_offset, indirect);
      return;
   }

   /* TODO: stop cheating.
    *
    * libagx supports this, just needs test coverage and gallium side wiring.
    */
   if (indirect && info->mode == MESA_PRIM_PATCHES && info->index_size) {
      perf_debug_ctx(ctx, "indexed indirect with tess");
      util_draw_indirect(pctx, info, drawid_offset, indirect);
      return;
   }

   bool xfb_passthrough = false;
   if (agx_needs_passthrough_gs(ctx, info, indirect, &xfb_passthrough)) {
      agx_apply_passthrough_gs(ctx, info, drawid_offset, indirect, draws,
                               num_draws, xfb_passthrough);
      return;
   }

   /* We must legalize feedback loops and transform feedback writes before
    * getting the batch, since once we have the batch we're not allowed to flush
    * the bound render targets.
    */
   agx_legalize_feedback_loops(ctx);
   agx_legalize_xfb(ctx);

   struct agx_batch *batch = agx_get_batch(ctx);
   uint64_t ib = 0;
   size_t ib_extent = 0;

   if (info->index_size) {
      ib =
         agx_index_buffer_ptr(batch, info, indirect ? NULL : draws, &ib_extent);
   }

   /* Increment IA statistics before lowering tessellation. This ensures we
    * count the patches instead of counting the tessellated outputs.
    */
   if (ctx->active_queries && !ctx->in_tess &&
       !ctx->active_draw_without_restart &&
       (ctx->pipeline_statistics[PIPE_STAT_QUERY_IA_VERTICES] ||
        ctx->pipeline_statistics[PIPE_STAT_QUERY_IA_PRIMITIVES] ||
        ctx->pipeline_statistics[PIPE_STAT_QUERY_VS_INVOCATIONS] ||
        ((ctx->pipeline_statistics[PIPE_STAT_QUERY_C_PRIMITIVES] ||
          ctx->pipeline_statistics[PIPE_STAT_QUERY_C_INVOCATIONS]) &&
         !ctx->stage[MESA_SHADER_TESS_EVAL].shader &&
         !ctx->stage[MESA_SHADER_GEOMETRY].shader))) {

      uint64_t ptr;
      if (indirect) {
         ptr = agx_indirect_buffer_ptr(batch, indirect);
      } else {
         uint32_t desc[] = {draws->count, info->instance_count, 0};
         ptr = agx_pool_upload(&batch->pool, &desc, sizeof(desc));
      }

      agx_ia_update(batch, info, ptr, ib,
                    info->index_size ? ib_extent / info->index_size : 1);
   }

   if (info->mode == MESA_PRIM_PATCHES) {
      agx_draw_patches(ctx, info, drawid_offset, indirect, draws, num_draws);
      return;
   }

   /* Only the rasterization stream counts */
   if (ctx->active_queries && ctx->prims_generated[0] &&
       !ctx->stage[MESA_SHADER_GEOMETRY].shader) {

      assert(!indirect && "we force a passthrough GS for this");
      agx_primitives_update_direct(ctx, info, draws);
   }

   if (ctx->stage[MESA_SHADER_GEOMETRY].shader && info->primitive_restart &&
       info->index_size) {

      agx_draw_without_restart(batch, info, drawid_offset, indirect, draws);
      return;
   }

   agx_batch_add_timestamp_query(batch, ctx->time_elapsed);

#ifndef NDEBUG
   if (unlikely(agx_device(pctx->screen)->debug & AGX_DBG_DIRTY))
      agx_dirty_all(ctx);
#endif

   agx_batch_init_state(batch);

   /* Dirty track the reduced prim: lines vs points vs triangles. Happens before
    * agx_update_vs/agx_update_fs, which specialize based on primitive.
    */
   enum mesa_prim reduced_prim = u_reduced_prim(info->mode);
   if (reduced_prim != batch->reduced_prim)
      ctx->dirty |= AGX_DIRTY_PRIM;
   batch->reduced_prim = reduced_prim;

   /* Update shaders first so we can use them after */
   if (agx_update_vs(batch, info->index_size)) {
      ctx->dirty |= AGX_DIRTY_VS | AGX_DIRTY_VS_PROG;
      ctx->stage[MESA_SHADER_VERTEX].dirty = ~0;
   } else if (ctx->stage[MESA_SHADER_VERTEX].dirty ||
              (ctx->dirty & AGX_DIRTY_VERTEX))
      ctx->dirty |= AGX_DIRTY_VS;

   if (agx_drop_for_unusable_program(ctx, ctx->vs, ctx->linked.vs != NULL,
                                     "vertex"))
      return;

   /* This is subtle. But agx_update_vs will be true at least once per batch. */
   assert(agx_batch_uses_bo(batch, ctx->vs->bo));
   assert(!ctx->linked.vs || agx_batch_uses_bo(batch, ctx->linked.vs->bo));

   agx_update_gs(ctx, info, indirect);

   if (ctx->gs &&
       agx_drop_for_unusable_program(ctx, ctx->gs, true, "geometry"))
      return;

   if (ctx->gs) {
      batch->uniforms.geometry_params =
         agx_batch_geometry_params(batch, ib, ib_extent, info, draws, indirect);

      agx_batch_add_bo(batch, ctx->gs->bo);
      agx_batch_add_bo(batch, ctx->gs->gs_copy->bo);
   }

   agx_update_gs_copy_link(ctx, batch);

   if (ctx->dirty & (AGX_DIRTY_VS_PROG | AGX_DIRTY_FS_PROG)) {
      struct agx_compiled_shader *vs = ctx->vs;
      if (ctx->gs)
         vs = ctx->gs->gs_copy;

      agx_assign_uvs(
         &batch->linked_varyings, &vs->uvs,
         ctx->stage[MESA_SHADER_FRAGMENT].shader->info.inputs_flat_shaded,
         ctx->stage[MESA_SHADER_FRAGMENT].shader->info.inputs_linear_shaded);

      for (unsigned i = 0; i < VARYING_SLOT_MAX; ++i) {
         batch->uniforms.uvs_index[i] = batch->linked_varyings.slots[i];
      }
   }

   /* Set draw ID */
   if (ctx->vs->b.info.uses_draw_id) {
      batch->uniforms.draw_id = drawid_offset;

      ctx->dirty |= AGX_DIRTY_VS;
   }

   if (agx_update_fs(batch)) {
      ctx->dirty |= AGX_DIRTY_FS | AGX_DIRTY_FS_PROG;
      ctx->stage[MESA_SHADER_FRAGMENT].dirty = ~0;
   } else if ((ctx->stage[MESA_SHADER_FRAGMENT].dirty) ||
              (ctx->dirty & (AGX_DIRTY_BLEND_COLOR | AGX_DIRTY_SAMPLE_MASK))) {
      ctx->dirty |= AGX_DIRTY_FS;
   }

   if (agx_drop_for_unusable_program(ctx, ctx->fs, ctx->linked.fs != NULL,
                                     "fragment"))
      return;

   /* This is subtle. But agx_update_fs will be true at least once per batch. */
   assert(!ctx->fs->bo || agx_batch_uses_bo(batch, ctx->fs->bo));
   assert(agx_batch_uses_bo(batch, ctx->linked.fs->bo));

   if (ctx->linked.vs->uses_base_param || ctx->gs) {
      agx_upload_draw_params(batch, indirect, draws, info);

      batch->uniforms.is_indexed_draw = (info->index_size > 0);
      ctx->dirty |= AGX_DIRTY_VS;
   }

   agx_update_descriptors(batch, ctx->vs);
   agx_update_descriptors(batch, ctx->gs);
   agx_update_descriptors(batch, ctx->fs);

   if (IS_DIRTY(VS) || IS_DIRTY(FS) || ctx->gs || IS_DIRTY(VERTEX) ||
       IS_DIRTY(BLEND_COLOR) || IS_DIRTY(QUERY) || IS_DIRTY(POLY_STIPPLE) ||
       IS_DIRTY(RS) || IS_DIRTY(PRIM) || ctx->in_tess) {

      if (IS_DIRTY(VERTEX)) {
         agx_upload_vbos(batch);
      }

      if (IS_DIRTY(BLEND_COLOR)) {
         memcpy(batch->uniforms.blend_constant, &ctx->blend_color,
                sizeof(ctx->blend_color));
      }

      if (IS_DIRTY(RS)) {
         struct pipe_rasterizer_state *rs = &ctx->rast->base;

         batch->uniforms.fixed_point_size =
            rs->point_size_per_vertex ? 0.0 : rs->point_size;

         /* TODO: tri fans */
         batch->uniforms.provoking_vertex = !rs->flatshade_first ? 2 : 0;
      }

      if (IS_DIRTY(QUERY)) {
         for (unsigned i = 0; i < ARRAY_SIZE(ctx->pipeline_statistics); ++i) {
            struct agx_query *query = ctx->pipeline_statistics[i];
            batch->uniforms.pipeline_statistics[i] =
               agx_get_query_address(batch, query);
         }
      }

      if (IS_DIRTY(POLY_STIPPLE)) {
         STATIC_ASSERT(sizeof(ctx->poly_stipple) == 32 * 4);

         batch->uniforms.polygon_stipple = agx_pool_upload_aligned(
            &batch->pool, ctx->poly_stipple, sizeof(ctx->poly_stipple), 4);
      }

      agx_upload_uniforms(batch, ctx->gs ? ctx->gs->gs_copy : ctx->vs);
   }

   struct pipe_draw_info info_gs;
   struct pipe_draw_indirect_info indirect_gs;
   struct pipe_draw_start_count_bias draw_gs;

   /* Wrap the pool allocation in a fake resource for meta-Gallium use */
   struct agx_resource indirect_rsrc = {.bo = batch->geom_params_bo};
   struct agx_resource index_rsrc = {};

   if (ctx->gs) {
      /* Launch the pre-rasterization parts of the geometry shader */
      agx_launch_gs_prerast(batch, info, draws, indirect);

      /* Setup to rasterize the GS results */
      struct poly_gs_info *gsi = &ctx->gs->gs;
      info_gs = (struct pipe_draw_info){
         .mode = gsi->mode,
         .index_size = poly_gs_index_size(gsi->shape),
         .primitive_restart = poly_gs_indexed(gsi->shape),
         .restart_index = poly_gs_index_size(gsi->shape) == 1 ? 0xFF : ~0,
         .index.resource = &index_rsrc.base,
         .instance_count = 1,
      };

      if (indirect) {
         indirect_gs = (struct pipe_draw_indirect_info){
            .draw_count = 1,
            .buffer = &indirect_rsrc.base,
            .offset = batch->uniforms.geometry_params -
                      indirect_rsrc.bo->va->addr +
                      offsetof(struct poly_geometry_params, draw),
         };

         indirect = &indirect_gs;

         batch->geom_index_bo = agx_resource(batch->ctx->heap)->bo;
         batch->geom_index = batch->geom_index_bo->va->addr;
      } else {
         unsigned prims =
            u_decomposed_prims_for_vertices(info->mode, draws->count);

         draw_gs = (struct pipe_draw_start_count_bias){
            .count = poly_gs_rast_vertices(gsi->shape, gsi->max_indices, prims,
                                           info->instance_count),
         };

         info_gs.instance_count = poly_gs_rast_instances(
            gsi->shape, gsi->max_indices, prims, info->instance_count);

         draws = &draw_gs;
      }

      info = &info_gs;
      index_rsrc.bo = batch->geom_index_bo;

      /* TODO: Deduplicate? */
      batch->reduced_prim = u_reduced_prim(info->mode);
      ctx->dirty |= AGX_DIRTY_PRIM;

      if (gsi->shape == POLY_GS_SHAPE_DYNAMIC_INDEXED) {
         ib = batch->geom_index;
         ib_extent = index_rsrc.bo->size - (batch->geom_index - ib);
      } else if (gsi->shape == POLY_GS_SHAPE_STATIC_INDEXED) {
         ib = agx_pool_upload(&batch->pool, gsi->topology, gsi->max_indices);
         ib_extent = gsi->max_indices;
      }

      /* We need to reemit geometry descriptors since the txf sampler may change
       * between the GS prepass and the GS rast program.
       */
      agx_update_descriptors(batch, ctx->gs->gs_copy);
   }

   /* Nothing left to rasterize -- a geometry or transform feedback pass with
    * an empty rasterized output, or too few vertices for one primitive. The
    * pre-rasterization work is already launched above; a native HAL200
    * direct draw cannot encode a zero count, and would draw nothing. */
   if (agx_native_graphics(agx_device(ctx->base.screen)) && !indirect &&
       (!draws->count || !info->instance_count))
      return;

   assert((!indirect || !indirect->indirect_draw_count) && "multidraw handled");

   /* Update batch masks based on current state */
   if (ctx->dirty & AGX_DIRTY_BLEND) {
      /* TODO: Any point to tracking load? */
      batch->draw |= ctx->blend->store;
      batch->resolve |= ctx->blend->store;
   }

   if (ctx->dirty & AGX_DIRTY_ZS) {
      batch->load |= ctx->zs->load;
      batch->draw |= ctx->zs->store;
      batch->resolve |= ctx->zs->store;
   }

   /* When we approach the end of a command buffer, cycle it out for a new one.
    * We only need to do this once per draw as long as we conservatively
    * estimate the maximum bytes of VDM commands that this draw will emit.
    */
   struct agx_device *dev = agx_device(ctx->base.screen);
   const size_t command_space =
      agx_native_graphics(dev)
         ? AGX_VDM_STATE_UPDATE_LENGTH + AGX_VDM_HAL200_BIND_TAIL_LENGTH +
              AGX_VDM_HAL200_DRAW_STATE_POINTER_LENGTH +
              AGX_VDM_HAL200_INDEXED_DRAW_BODY_LENGTH + AGX_VDM_BARRIER_LENGTH
         : (AGX_VDM_STATE_LENGTH * 2) +
              (AGX_PPP_STATE_LENGTH * MAX_PPP_UPDATES) +
              AGX_VDM_STATE_RESTART_INDEX_LENGTH +
              AGX_VDM_STATE_VERTEX_SHADER_WORD_0_LENGTH +
              AGX_VDM_STATE_VERTEX_SHADER_WORD_1_LENGTH +
              AGX_VDM_STATE_VERTEX_OUTPUTS_LENGTH +
              AGX_VDM_STATE_VERTEX_UNKNOWN_LENGTH + 4 /* padding */ +
              AGX_INDEX_LIST_LENGTH + AGX_INDEX_LIST_BUFFER_LO_LENGTH +
              AGX_INDEX_LIST_COUNT_LENGTH + AGX_INDEX_LIST_INSTANCES_LENGTH +
              AGX_INDEX_LIST_START_LENGTH + AGX_INDEX_LIST_BUFFER_SIZE_LENGTH;
   agx_ensure_cmdbuf_has_space(batch, &batch->vdm, command_space);

   uint32_t draw_state_offset = 0;
   /* Native HAL200 indirect records always carry a draw-state selector.
    * A state-clean indirect draw needs only its bind tail and selector;
    * dirtying PRIM would also relink varyings and rebuild the VS program. */
   uint8_t *out =
      agx_native_graphics(dev) && indirect && !ctx->dirty
         ? agx_g17p_emit_bind_state(batch, batch->vdm.current,
                                    ctx->gs ? ctx->gs->gs_copy : ctx->vs,
                                    &draw_state_offset)
         : agx_encode_state(batch, batch->vdm.current, &draw_state_offset);
   if (!out)
      return;

   if (!agx_native_graphics(dev) && info->index_size &&
       info->primitive_restart) {
      agx_push(out, VDM_STATE, cfg)
         cfg.restart_index_present = true;

      agx_push(out, VDM_STATE_RESTART_INDEX, cfg)
         cfg.value = info->restart_index;
   }

   struct agx_draw draw = {0};
   if (info->index_size) {
      draw.index_size = agx_translate_index_size(info->index_size);
      draw.index_buffer = ib;
      draw.index_buffer_range_B = ib_extent;
      draw.restart = info->primitive_restart;
      draw.indexed = true;
   } else {
      draw.start = indirect ? 0 : draws->start;
   }

   if (indirect) {
      draw.b = agx_grid_indirect(agx_indirect_buffer_ptr(batch, indirect));
   } else {
      draw.b = agx_3d(draws->count, info->instance_count, 1);
      if (info->index_size)
         draw.index_bias = draws->index_bias;
   }

   const enum agx_primitive primitive = agx_primitive_for_pipe(info->mode);
   if (agx_native_graphics(dev)) {
      out = indirect ? agx_g17p_emit_indirect_draw(
                          dev->chip, out, &draw, primitive,
                          info->restart_index, draw_state_offset)
                     : agx_g17p_emit_draw_body(
                          dev->chip, out, &draw, primitive,
                          info->restart_index, draw_state_offset);
   } else {
      out = (void *)agx_vdm_draw(
         (uint32_t *)out, dev->chip, draw, primitive,
         draw.restart ? info->restart_index
                      : agx_primitive_restart_index(draw.index_size));
   }

   /* Barrier transform feedback writes on themselves for consistency.
    * This is the other half of agx_legalize_xfb.
    */
   if (ctx->gs && ctx->streamout.num_targets > 0) {
      struct agx_device *dev = agx_device(ctx->base.screen);
      out = (void *)agx_vdm_barrier((uint32_t *)out, dev->chip);
   }

   batch->vdm.current = out;
   assert((batch->vdm.current + AGX_VDM_STREAM_LINK_LENGTH) <= batch->vdm.end &&
          "Failed to reserve sufficient space in encoder");
   agx_dirty_reset_graphics(ctx);

   assert(batch == agx_get_batch(ctx) && "batch should not change under us");

   batch->draws++;

   if (unlikely(dev->debug & AGX_DBG_PERF))
      ctx->perf.draws++;

   /* The scissor/zbias arrays are indexed with 16-bit integers, imposigin a
    * maximum of UINT16_MAX descriptors. Flush if the next draw would overflow
    */
   if (unlikely(
          (((batch->scissor.size / AGX_SCISSOR_LENGTH) + AGX_MAX_VIEWPORTS) >
           UINT16_MAX) ||
          (batch->depth_bias.size / AGX_DEPTH_BIAS_LENGTH) >= UINT16_MAX)) {
      agx_flush_batch_for_reason(ctx, batch, "Scissor/depth bias overflow");
   } else if (unlikely(batch->draws > 100000)) {
      /* Mostly so drawoverhead doesn't OOM */
      agx_flush_batch_for_reason(ctx, batch, "Absurd number of draws");
   } else if (unlikely(batch->sampler_heap.count >
                       (AGX_SAMPLER_HEAP_SIZE - (PIPE_MAX_SAMPLERS * 6)))) {
      agx_flush_batch_for_reason(ctx, batch, "Sampler heap overflow");
   }
}

static void
agx_texture_barrier(struct pipe_context *pipe, unsigned flags)
{
   struct agx_context *ctx = agx_context(pipe);

   /* Framebuffer fetch is coherent, so barriers are a no-op. */
   if (flags == PIPE_TEXTURE_BARRIER_FRAMEBUFFER)
      return;

   agx_flush_all(ctx, "Texture barrier");
}

void
agx_launch(struct agx_batch *batch, struct agx_grid grid,
           struct agx_workgroup wg, struct agx_compiled_shader *cs,
           struct agx_linked_shader *linked, mesa_shader_stage stage,
           unsigned variable_shared_mem, uint64_t indirect_num_workgroups)
{
   struct agx_context *ctx = batch->ctx;

   if (agx_drop_for_unusable_program(ctx, cs, true,
                                     _mesa_shader_stage_to_abbrev(stage)))
      return;

   if (!linked && agx_is_shader_empty(&cs->b))
      return;

   /* To implement load_num_workgroups, the number of workgroups needs to be
    * available in GPU memory. This is either the indirect buffer, or just a
    * buffer we upload ourselves if not indirect.
    */
   if (indirect_num_workgroups) {
      /* G17P expands indirect workgroup counts into a separate hardware
       * record of global thread counts. Keep the API counts for the shader.
       */
      batch->uniforms.tables[AGX_SYSVAL_TABLE_GRID] = indirect_num_workgroups;
   } else if (grid.mode == AGX_CDM_MODE_DIRECT) {
      uint32_t groups[3] = {
         grid.count[0] / wg.x,
         grid.count[1] / wg.y,
         grid.count[2] / wg.z,
      };

      batch->uniforms.tables[AGX_SYSVAL_TABLE_GRID] =
         agx_pool_upload_aligned(&batch->pool, groups, sizeof(groups), 4);
   } else {
      batch->uniforms.tables[AGX_SYSVAL_TABLE_GRID] = grid.ptr;
   }

   util_dynarray_foreach(&ctx->global_buffers, struct pipe_resource *, res) {
      if (!*res)
         continue;

      struct agx_resource *buffer = agx_resource(*res);
      agx_batch_writes(batch, buffer, 0);
      batch->incoherent_writes = true;
   }

   agx_update_descriptors(batch, cs);
   agx_upload_uniforms(batch, NULL);

   struct agx_device *dev = agx_device(ctx->base.screen);
   if (agx_native_graphics(dev)) {
      struct agx3_ioto_uniform_range ranges[AGX_MAX_PUSH_RANGES + 2];
      size_t range_count = 0;
      const bool resource_pair = batch->texture_count[stage] != 0;

      if (resource_pair) {
         if (!cs->g17p_texture_resources ||
             !batch->stage_uniforms[stage].texture_base ||
             !batch->samplers[stage] ||
             (batch->ctx->stage[stage].custom_borders &&
              !cs->g17p_custom_border_emulation))
            agx_native_descriptor_rejected(dev,
                                           "compute texture resources");

         const uint64_t pointers[2] = {
            batch->stage_uniforms[stage].texture_base,
            batch->samplers[stage],
         };
         const uint64_t pointer_va = agx_pool_upload_aligned(
            &batch->pool, pointers, sizeof(pointers), 8);
         if (!pointer_va)
            agx_native_descriptor_rejected(dev,
                                           "compute texture pointers");

         ranges[range_count++] = (struct agx3_ioto_uniform_range){
            .start_halfs = 0,
            .size_halfs = AGX_ABI_G17P_TEXTURE_POINTER_HALFS,
            .address = pointer_va,
         };
      } else if (cs->g17p_texture_resources) {
         agx_native_descriptor_rejected(dev, "compute texture table");
      }

      for (unsigned i = 0; i < cs->push_range_count; ++i) {
         const struct agx_push_range *push = &cs->push[i];
         const uint64_t table = batch->uniforms.tables[push->table];
         if (push->table == AGX_SYSVAL_TABLE_PARAMS && !table)
            continue;
         if (resource_pair && push->uniform <
                                AGX_ABI_G17P_TEXTURE_POINTER_HALFS)
            agx_native_descriptor_rejected(dev,
                                           "compute texture ABI overlap");
         if (!table || (push->uniform & 1) || (push->length & 1) ||
             !push->length || range_count == ARRAY_SIZE(ranges))
            agx_native_descriptor_rejected(dev, "compute uniform range");
         ranges[range_count++] = (struct agx3_ioto_uniform_range){
            .start_halfs = push->uniform,
            .size_halfs = push->length,
            .address = table + push->offset,
         };
      }

      if (cs->b.info.rodata.size_16) {
         const struct agx_rodata *rodata = &cs->b.info.rodata;
         const uint64_t size_B = (uint64_t)rodata->size_16 * 2;
         if (!cs->bo || !cs->bo->va || (rodata->base_uniform & 1) ||
             (rodata->size_16 & 1) || !rodata->size_16 ||
             range_count == ARRAY_SIZE(ranges) ||
             rodata->offset > cs->bo->size ||
             size_B > cs->bo->size - rodata->offset)
            agx_native_descriptor_rejected(dev, "compute constant range");
         ranges[range_count++] = (struct agx3_ioto_uniform_range){
            .start_halfs = rodata->base_uniform,
            .size_halfs = rodata->size_16,
            .address = cs->bo->va->addr + rodata->offset,
         };
      }

      if (!agx_g17p_append_compute(batch, &cs->b.info, cs->b.binary, cs->bo,
                                   linked, grid, wg, ranges, range_count,
                                   variable_shared_mem))
         agx_native_descriptor_rejected(dev, "compute Ioto dispatch");
      return;
   }

   // TODO: This is broken.
   size_t subgroups_per_core = 0;
#if 0
   if (!info->indirect) {
      size_t subgroups_per_workgroup =
         DIV_ROUND_UP(info->block[0] * info->block[1] * info->block[2], 32);
      subgroups_per_core =
         local_workgroups *
         DIV_ROUND_UP(info->grid[0] * info->grid[1] * info->grid[2],
                     ctx->scratch_cs.num_cores);
   }
#endif

   uint32_t usc = agx_build_pipeline(batch, cs, linked, MESA_SHADER_COMPUTE,
                                     variable_shared_mem, subgroups_per_core,
                                     NULL);

   if (cs)
      agx_batch_add_bo(batch, cs->bo);

   struct agx_cdm_launch_word_0_packed launch;
   agx_pack(&launch, CDM_LAUNCH_WORD_0, cfg) {
      cfg.uniform_register_count = cs->b.info.push_count;
      cfg.preshader_register_count = cs->b.info.nr_preamble_gprs;
      cfg.texture_state_register_count =
         cs ? agx_nr_tex_descriptors(batch, cs) : 0;
      cfg.sampler_state_register_count =
         translate_sampler_state_count(ctx, stage);
   }

   agx_launch_internal(batch, grid, wg, launch, stage, usc);
}

static void
agx_launch_grid(struct pipe_context *pipe, const struct pipe_grid_info *info)
{
   struct agx_context *ctx = agx_context(pipe);
   if (unlikely(!ctx->compute_blitter.active &&
                !agx_render_condition_check(ctx)))
      return;

   struct agx_batch *batch = agx_get_compute_batch(ctx);

   uint64_t indirect = 0;
   if (info->indirect) {
      struct agx_resource *rsrc = agx_resource(info->indirect);
      agx_batch_reads(batch, rsrc);
      indirect = agx_map_gpu(rsrc) + info->indirect_offset;
   }

   /* Increment the pipeline stats query.
    *
    * TODO: Can we use the hardware counter for this?
    */
   struct agx_query *statistic =
      ctx->pipeline_statistics[PIPE_STAT_QUERY_CS_INVOCATIONS];

   struct agx_workgroup wg =
      agx_workgroup(info->block[0], info->block[1], info->block[2]);

   if (statistic) {
      if (indirect) {
         uint64_t addr = agx_get_query_address(batch, statistic);

         libagx_increment_cs_invocations(batch, agx_1d(1), AGX_BARRIER_ALL,
                                         indirect, addr,
                                         agx_workgroup_threads(wg));
      } else {
         agx_query_increment_cpu(ctx, statistic,
                                 agx_workgroup_threads(wg) * info->grid[0] *
                                    info->grid[1] * info->grid[2]);
      }
   }

   agx_batch_add_timestamp_query(batch, ctx->time_elapsed);

   agx_batch_init_state(batch);

   struct agx_uncompiled_shader *uncompiled =
      ctx->stage[MESA_SHADER_COMPUTE].shader;

   union asahi_shader_key key = {
      .cs.custom_borders =
         agx_native_graphics(agx_device(pipe->screen)) &&
         ctx->stage[MESA_SHADER_COMPUTE].custom_borders,
   };
   /* The variant compiled at link time, or by an earlier dispatch, is in
    * the shader's table; agx_get_shader_variant() would compile (or fetch
    * from the disk cache) and insert it again on every dispatch. */
   struct hash_entry *variant =
      _mesa_hash_table_search(uncompiled->variants, &key);
   struct agx_compiled_shader *cs =
      variant ? variant->data
              : agx_get_shader_variant(agx_screen(pipe->screen), pipe,
                                       uncompiled, &key);

   struct agx_grid grid;
   if (indirect && agx_native_graphics(agx_device(pipe->screen))) {
      /* G17P launches an indirect dispatch from a six-word grid -- three
       * global thread counts then the three local sizes -- so the API's three
       * workgroup counts are not the record the hardware reads. Expand them
       * with the same one-invocation setup kernel the tessellator already
       * uses for its indirect launches, and dispatch through the grounded
       * indirect-local record rather than grounding a second launch form. */
      uint64_t expanded =
         agx_pool_alloc_aligned(&batch->pool, 6 * sizeof(uint32_t), 4).gpu;

      libagx_setup_indirect_dispatch(batch, agx_1d(1), AGX_BARRIER_ALL,
                                     indirect, expanded, wg.x, wg.y, wg.z);

      grid = agx_grid_indirect_local(expanded);
   } else if (indirect) {
      grid = agx_grid_indirect(indirect);
   } else {
      grid = agx_3d(0, 0, 0);

      for (unsigned d = 0; d < 3; ++d) {
         grid.count[d] = ((info->grid[d] - 1) * info->block[d]) +
                         (info->last_block[d] ?: info->block[d]);
      }
   }

   agx_launch(batch, grid, wg, cs, NULL, MESA_SHADER_COMPUTE,
              info->variable_shared_mem, indirect);

   /* TODO: Dirty tracking? */
   agx_dirty_all(ctx);

   batch->uniforms.tables[AGX_SYSVAL_TABLE_GRID] = 0;

   /* If the next dispatch might overflow, flush now. TODO: If this is ever hit
    * in practice, we can use CDM stream links.
    */
   size_t dispatch_upper_bound =
      AGX_CDM_LAUNCH_WORD_0_LENGTH + AGX_CDM_LAUNCH_WORD_1_LENGTH +
      AGX_CDM_UNK_G14X_LENGTH + AGX_CDM_INDIRECT_LENGTH +
      AGX_CDM_GLOBAL_SIZE_LENGTH + AGX_CDM_LOCAL_SIZE_LENGTH +
      AGX_CDM_BARRIER_LENGTH;

   if (batch->cdm.current + dispatch_upper_bound >= batch->cdm.end)
      agx_flush_batch_for_reason(ctx, batch, "CDM overfull");
}

static void
agx_set_global_binding(struct pipe_context *pipe, unsigned first,
                       unsigned count, struct pipe_resource **resources,
                       uint32_t **handles)
{
   struct agx_context *ctx = agx_context(pipe);
   unsigned old_size =
      util_dynarray_num_elements(&ctx->global_buffers, *resources);

   if (old_size < first + count) {
      /* we are screwed no matter what */
      if (!util_dynarray_grow(&ctx->global_buffers, *resources,
                              (first + count) - old_size))
         UNREACHABLE("out of memory");

      for (unsigned i = old_size; i < first + count; i++)
         *util_dynarray_element(&ctx->global_buffers, struct pipe_resource *,
                                i) = NULL;
   }

   for (unsigned i = 0; i < count; ++i) {
      struct pipe_resource **res = util_dynarray_element(
         &ctx->global_buffers, struct pipe_resource *, first + i);
      if (resources && resources[i]) {
         pipe_resource_reference(res, resources[i]);

         /* The handle points to uint32_t, but space is allocated for 64
          * bits. We need to respect the offset passed in. This interface
          * is so bad.
          */
         uint64_t addr = 0;
         struct agx_resource *rsrc = agx_resource(resources[i]);

         memcpy(&addr, handles[i], sizeof(addr));
         addr += agx_map_gpu(rsrc);
         memcpy(handles[i], &addr, sizeof(addr));
      } else {
         pipe_resource_reference(res, NULL);
      }
   }
}

void agx_init_state_functions(struct pipe_context *ctx);

void
agx_decompress_inplace(struct agx_batch *batch, struct pipe_surface *surf,
                       const char *reason)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);
   struct agx_resource *rsrc = agx_resource(surf->texture);
   struct ail_layout *layout = &rsrc->layout;
   unsigned level = surf->level;
   const enum agx_descriptor_generation descriptor_generation =
      agx_gallium_descriptor_generation(dev->chip);

   perf_debug(dev, "Decompressing in-place due to: %s", reason);

   /* libagx_decompress_images is a byte-exact AGX2 24+24 ABI.  A native
    * AGX3 version needs a 32+32 argument block and matching precompiled shader;
    * do not dispatch the legacy kernel against a wider descriptor generation.
    */
   if (!agx_gallium_descriptor_generation_is_agx2(descriptor_generation)) {
      agx_native_descriptor_rejected(dev, "decompression descriptor block");
   }

   if (!batch->cdm.bo)
      batch->cdm = agx_encoder_allocate(batch, dev);

   struct agx_ptr images = agx_pool_alloc_aligned(
      &batch->pool, sizeof(struct libagx_decompress_images), 64);
   struct libagx_decompress_images *img = images.cpu;

   struct pipe_sampler_view sampler_view = sampler_view_for_surface(surf);
   sampler_view.target = PIPE_TEXTURE_2D_ARRAY;
   struct pipe_image_view view = image_view_for_surface(surf);
   struct agx_gallium_texture_descriptor native_texture;
   struct agx_gallium_pbe_descriptor native_pbe;
   if (!agx_pack_texture(descriptor_generation, &native_texture, rsrc,
                         surf->format, &sampler_view))
      agx_native_descriptor_rejected(dev, "decompression texture");
   if (!agx_batch_upload_pbe(batch, &native_pbe, &view, false, true, true,
                             true))
      agx_native_descriptor_rejected(dev, "decompression PBE");
   memcpy(&img->compressed, &native_texture, AGX_TEXTURE_LENGTH);
   memcpy(&img->uncompressed, &native_pbe, AGX_PBE_LENGTH);

   struct agx_grid grid = agx_3d(ail_metadata_width_tl(layout, level) * 32,
                                 ail_metadata_height_tl(layout, level),
                                 surf->last_layer - surf->first_layer + 1);

   libagx_decompress(batch, grid, AGX_BARRIER_ALL, layout, surf->first_layer,
                     level, agx_map_gpu(rsrc), images.gpu);
}

void
agx_init_state_functions(struct pipe_context *ctx)
{
   ctx->create_blend_state = agx_create_blend_state;
   ctx->create_depth_stencil_alpha_state = agx_create_zsa_state;
   ctx->create_fs_state = agx_create_shader_state;
   ctx->create_rasterizer_state = agx_create_rs_state;
   ctx->create_sampler_state = agx_create_sampler_state;
   ctx->create_sampler_view = agx_create_sampler_view;
   ctx->create_vertex_elements_state = agx_create_vertex_elements;
   ctx->create_vs_state = agx_create_shader_state;
   ctx->create_gs_state = agx_create_shader_state;
   ctx->create_tcs_state = agx_create_shader_state;
   ctx->create_tes_state = agx_create_shader_state;
   ctx->create_compute_state = agx_create_compute_state;
   ctx->bind_blend_state = agx_bind_blend_state;
   ctx->bind_depth_stencil_alpha_state = agx_bind_zsa_state;
   ctx->bind_sampler_states = agx_bind_sampler_states;
   ctx->bind_fs_state = agx_bind_fs_state;
   ctx->bind_rasterizer_state = agx_bind_rasterizer_state;
   ctx->bind_vertex_elements_state = agx_bind_vertex_elements_state;
   ctx->bind_vs_state = agx_bind_vs_state;
   ctx->bind_gs_state = agx_bind_gs_state;
   ctx->bind_tcs_state = agx_bind_tcs_state;
   ctx->bind_tes_state = agx_bind_tes_state;
   ctx->bind_compute_state = agx_bind_cs_state;
   ctx->delete_blend_state = agx_delete_state;
   ctx->delete_depth_stencil_alpha_state = agx_delete_state;
   ctx->delete_fs_state = agx_delete_shader_state;
   ctx->delete_compute_state = agx_delete_shader_state;
   ctx->delete_rasterizer_state = agx_delete_state;
   ctx->delete_sampler_state = agx_delete_sampler_state;
   ctx->delete_vertex_elements_state = agx_delete_state;
   ctx->delete_vs_state = agx_delete_shader_state;
   ctx->delete_gs_state = agx_delete_shader_state;
   ctx->delete_tcs_state = agx_delete_shader_state;
   ctx->delete_tes_state = agx_delete_shader_state;
   ctx->set_blend_color = agx_set_blend_color;
   ctx->set_constant_buffer = agx_set_constant_buffer;
   ctx->set_shader_buffers = agx_set_shader_buffers;
   ctx->set_shader_images = agx_set_shader_images;
   ctx->set_sampler_views = agx_set_sampler_views;
   ctx->set_framebuffer_state = agx_set_framebuffer_state;
   ctx->set_polygon_stipple = agx_set_polygon_stipple;
   ctx->set_patch_vertices = agx_set_patch_vertices;
   ctx->set_sample_mask = agx_set_sample_mask;
   ctx->set_scissor_states = agx_set_scissor_states;
   ctx->set_stencil_ref = agx_set_stencil_ref;
   ctx->set_vertex_buffers = agx_set_vertex_buffers;
   ctx->set_viewport_states = agx_set_viewport_states;
   ctx->sampler_view_destroy = agx_sampler_view_destroy;
   ctx->sampler_view_release = u_default_sampler_view_release;
   ctx->resource_release = u_default_resource_release;
   ctx->draw_vbo = agx_draw_vbo;
   ctx->launch_grid = agx_launch_grid;
   ctx->set_global_binding = agx_set_global_binding;
   ctx->texture_barrier = agx_texture_barrier;
   ctx->get_compute_state_info = agx_get_compute_state_info;
   ctx->set_tess_state = agx_set_tess_state;
}
