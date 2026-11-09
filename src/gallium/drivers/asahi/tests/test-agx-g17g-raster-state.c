/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

/* Exercise the real Gallium draw path under the compiler-only DRM shim.
 * Inspect encoded state changes; this test does not execute GPU work or
 * compare rendered pixels.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/driconf.h"
#include "util/u_inlines.h"
#include "util/u_simple_shaders.h"
#include "util/u_math.h"
#include "util/xmlconfig.h"
#include "asahi/lib/agx_m5_va.h"
#include "asahi/lib/agx_ppp_hal300_depth_bias_scissor_core.h"
#include "asahi/lib/agx_ppp_viewport_payload_core.h"
#include "asahi/lib/agx_vdm_ppp_state_core.h"
#include "asahi/lib/agx_vdm_state_update_core.h"
#include "agx_bo.h"
#include "agx_public.h"
#include "agx_state.h"

#define CHECK(expr) do { \
   if (!(expr)) { \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
      exit(1); \
   } \
} while (0)

struct expected_raster {
   unsigned minx, miny, maxx, maxy;
   float translate_x, translate_z, scale_z;
};

static const void *
batch_data(struct agx_batch *batch, uint64_t address, size_t size)
{
   util_dynarray_foreach(&batch->pool.bos, struct agx_bo *, bo) {
      if (address >= (*bo)->va->addr && address - (*bo)->va->addr <= (*bo)->size &&
          size <= (*bo)->size - (address - (*bo)->va->addr)) {
         const uint8_t *data = agx_bo_map(*bo);
         CHECK(data);
         return data + (address - (*bo)->va->addr);
      }
   }
   CHECK(!"raster state must remain owned by its batch");
   return NULL;
}

static void
draw_and_check(struct pipe_context *pipe, const char *label,
               const struct expected_raster *raster)
{
   struct agx_context *ctx = agx_context(pipe);
   struct agx_batch *batch = agx_get_batch(ctx);
   const unsigned draws_before = batch->draws;
   const unsigned refused_before = ctx->skipped_no_program;
   uint8_t *start = batch->vdm.current;
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_TRIANGLES, .instance_count = 1,
   };
   struct pipe_draw_start_count_bias draw = {.count = 3};
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   CHECK(ctx->batch == batch && batch->draws == draws_before + 1);
   CHECK(ctx->skipped_no_program == refused_before);

   /* A new batch has fixed initial records before the first vertex update.
    * Find that update and decode the following PPP pointers using the
    * native packet decoder, rather than assuming a total byte count.
    */
   uint8_t *end = batch->vdm.current;
   uint8_t *cursor = start;
   for (; cursor + 4 <= end; cursor += 4) {
      uint32_t word;
      memcpy(&word, cursor, sizeof(word));
      if (word == AGX_VDM_STATE_UPDATE_HEADER)
         break;
   }
   CHECK(cursor + AGX_VDM_STATE_UPDATE_LENGTH <= end);
   cursor += AGX_VDM_STATE_UPDATE_LENGTH;
   unsigned records = 0;
   for (; cursor + AGX_VDM_PPP_STATE_CORE_LENGTH <= end;
        cursor += AGX_VDM_PPP_STATE_CORE_LENGTH) {
      uint64_t address;
      uint32_t words;
      if (!agx_vdm_ppp_state_core_unpack(AGX_CHIP_G18G, cursor,
                                        AGX_VDM_PPP_STATE_CORE_LENGTH,
                                        &address, &words))
         break;
      CHECK(address && words);
      if (raster && records == 0) {
         struct agx_ppp_viewport_payload_fields viewport;
         CHECK(agx_ppp_viewport_payload_core_unpack(
            AGX_CHIP_G18G, batch_data(batch, AGX_M5_DRIVER_BASE + address, words * 4),
            words * 4, &viewport));
         CHECK(viewport.viewport_count == 1);
         CHECK(viewport.viewports[0].translate_x == fui(raster->translate_x));
         CHECK(viewport.viewports[0].translate_y == fui(32.0f));
         CHECK(viewport.viewports[0].scale_x == fui(32.0f));
         CHECK(viewport.viewports[0].scale_y == fui(32.0f));
         CHECK(viewport.viewports[0].translate_z == fui(raster->translate_z));
         CHECK(viewport.viewports[0].scale_z == fui(raster->scale_z));
      }
      if (raster && records == 1) {
         struct agx_ppp_hal300_depth_bias_scissor_bases_fields bases;
         CHECK(agx_ppp_hal300_depth_bias_scissor_bases_unpack(
            AGX_CHIP_G18G, batch_data(batch, AGX_M5_DRIVER_BASE + address, words * 4),
            words * 4, &bases));
         struct agx_ppp_hal300_scissor_record_fields scissor;
         CHECK(agx_ppp_hal300_scissor_record_unpack(
            AGX_CHIP_G18G, batch_data(batch, bases.scissor_base,
                                     AGX_PPP_HAL300_SCISSOR_RECORD_LENGTH),
            AGX_PPP_HAL300_SCISSOR_RECORD_LENGTH, &scissor));
         CHECK(scissor.min_x == raster->minx && scissor.min_y == raster->miny);
         CHECK(scissor.max_x == raster->maxx && scissor.max_y == raster->maxy);
         CHECK(scissor.min_z_raw == fui(raster->translate_z));
         CHECK(scissor.max_z_raw == fui(raster->translate_z + raster->scale_z));
         struct agx_ppp_hal300_depth_bias_record_fields bias;
         CHECK(agx_ppp_hal300_depth_bias_record_unpack(
            AGX_CHIP_G18G, batch_data(batch, bases.depth_bias_base,
                                     AGX_PPP_HAL300_DEPTH_BIAS_RECORD_LENGTH),
            AGX_PPP_HAL300_DEPTH_BIAS_RECORD_LENGTH, &bias));
         CHECK(!bias.depth_bias_raw && !bias.slope_scale_raw && !bias.clamp_raw);
      }
      records++;
   }
   printf("%s: %u PPP records, %zu stream bytes, %u draws in batch\n",
          label, records, (size_t)(end - start), batch->draws);
   CHECK(records == (raster ? 11 : 9));
}

int
main(void)
{
   const char *chip = getenv("ASAHI_SHIM_CHIP");
   CHECK(chip && !strcmp(chip, "g18g"));
   int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   CHECK(fd >= 0);
   static const driOptionDescription descriptions[] = {
#include "driinfo_asahi.h"
   };
   driOptionCache info, options;
   driParseOptionInfo(&info, descriptions, ARRAY_SIZE(descriptions));
   struct pipe_screen_config config = {.options = &options, .options_info = &info};
   struct pipe_screen *screen = agx_screen_create(fd, NULL, &config);
   CHECK(screen);
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   CHECK(pipe);

   struct pipe_resource texture_template = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_R8G8B8A8_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET,
   };
   struct pipe_resource *texture = screen->resource_create(screen, &texture_template);
   CHECK(texture);
   struct pipe_framebuffer_state framebuffer = {
      .width = 64, .height = 64, .layers = 1, .nr_cbufs = 1,
      .cbufs[0] = {.texture = texture, .format = texture_template.format},
   };
   pipe->set_framebuffer_state(pipe, &framebuffer);

   const float vertices[3][8] = {
      {-1, -1, 0, 1, 1, 0, 0, 1},
      { 1, -1, 0, 1, 0, 1, 0, 1},
      { 0,  1, 0, 1, 0, 0, 1, 1},
   };
   struct pipe_resource *buffer = pipe_buffer_create(
      screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_DEFAULT, sizeof(vertices));
   CHECK(buffer);
   pipe_buffer_write(pipe, buffer, 0, sizeof(vertices), vertices);
   struct pipe_vertex_buffer vertex_buffer = {
      .buffer.resource = buffer,
   };
   pipe->set_vertex_buffers(pipe, 1, &vertex_buffer);
   struct pipe_vertex_element elements[2] = {
      {.src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 32},
      {.src_offset = 16, .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 32},
   };
   void *vertex_elements = pipe->create_vertex_elements_state(pipe, 2, elements);
   CHECK(vertex_elements);
   pipe->bind_vertex_elements_state(pipe, vertex_elements);
   const enum tgsi_semantic semantics[] = {TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_GENERIC};
   const unsigned indices[] = {0, 0};
   void *vs = util_make_vertex_passthrough_shader(pipe, 2, semantics, indices, false);
   void *fs = util_make_fragment_passthrough_shader(
      pipe, TGSI_SEMANTIC_GENERIC, TGSI_INTERPOLATE_PERSPECTIVE, false);
   CHECK(vs && fs);
   pipe->bind_vs_state(pipe, vs);
   pipe->bind_fs_state(pipe, fs);

   struct pipe_blend_state blend = {
      .rt[0] = {
         .blend_enable = true, .rgb_func = PIPE_BLEND_ADD, .alpha_func = PIPE_BLEND_ADD,
         .rgb_src_factor = PIPE_BLENDFACTOR_CONST_COLOR, .rgb_dst_factor = PIPE_BLENDFACTOR_ZERO,
         .alpha_src_factor = PIPE_BLENDFACTOR_ONE, .alpha_dst_factor = PIPE_BLENDFACTOR_ZERO,
         .colormask = PIPE_MASK_RGBA,
      },
   };
   void *blend_state = pipe->create_blend_state(pipe, &blend);
   struct pipe_depth_stencil_alpha_state zs = {0};
   void *zs_state = pipe->create_depth_stencil_alpha_state(pipe, &zs);
   struct pipe_rasterizer_state raster = {
      .front_ccw = true, .depth_clip_near = true, .depth_clip_far = true,
      .clip_halfz = true,
   };
   void *raster_state = pipe->create_rasterizer_state(pipe, &raster);
   CHECK(blend_state && zs_state && raster_state);
   pipe->bind_blend_state(pipe, blend_state);
   pipe->bind_depth_stencil_alpha_state(pipe, zs_state);
   pipe->bind_rasterizer_state(pipe, raster_state);
   struct pipe_viewport_state viewport = {
      .scale = {32, 32, 1}, .translate = {32, 32, 0},
   };
   pipe->set_viewport_states(pipe, 0, 1, &viewport);
   struct pipe_blend_color colour = {.color = {1, 1, 1, 1}};
   pipe->set_blend_color(pipe, &colour);
   const struct expected_raster initial = {0, 0, 64, 64, 32, 0, 1};
   const struct expected_raster moved = {0, 0, 56, 64, 24, 0, 1};
   const struct expected_raster clipped = {8, 12, 40, 48, 24, 0, 1};
   const struct expected_raster full_z = {8, 12, 40, 48, 24, -1, 2};
   const struct expected_raster small = {8, 12, 32, 32, 24, -1, 2};
   draw_and_check(pipe, "initial", &initial);

   colour.color[0] = 0.5f;
   pipe->set_blend_color(pipe, &colour);
   draw_and_check(pipe, "fragment uniform", NULL);
   viewport.translate[0] = 24;
   pipe->set_viewport_states(pipe, 0, 1, &viewport);
   draw_and_check(pipe, "viewport", &moved);
   struct pipe_scissor_state scissor = {.minx = 8, .miny = 12, .maxx = 40, .maxy = 48};
   pipe->set_scissor_states(pipe, 0, 1, &scissor);
   draw_and_check(pipe, "scissor", &moved);
   raster.scissor = true;
   void *scissored = pipe->create_rasterizer_state(pipe, &raster);
   CHECK(scissored);
   pipe->bind_rasterizer_state(pipe, scissored);
   draw_and_check(pipe, "enable scissor", &clipped);
   colour.color[1] = 0.25f;
   pipe->set_blend_color(pipe, &colour);
   draw_and_check(pipe, "fragment uniform with scissor", NULL);

   raster.clip_halfz = false;
   void *full_depth = pipe->create_rasterizer_state(pipe, &raster);
   CHECK(full_depth);
   pipe->bind_rasterizer_state(pipe, full_depth);
   draw_and_check(pipe, "depth clip convention", &full_z);

   /* A different framebuffer creates a new batch, then returning to the old
    * framebuffer must republish state even though that batch already existed.
    */
   texture_template.width0 = texture_template.height0 = 32;
   struct pipe_resource *other = screen->resource_create(screen, &texture_template);
   CHECK(other);
   struct pipe_framebuffer_state other_framebuffer = framebuffer;
   other_framebuffer.width = other_framebuffer.height = 32;
   other_framebuffer.cbufs[0].texture = other;
   pipe->set_framebuffer_state(pipe, &other_framebuffer);
   draw_and_check(pipe, "new batch", &small);
   pipe->set_framebuffer_state(pipe, &framebuffer);
   draw_and_check(pipe, "return to previous batch", &full_z);

   /* Destination-only blending may become a no-op, but any remaining tile
    * read must use the translucent pass. In particular, sRGB transfer
    * lowering can retain a load/decode/encode/store even for ZERO,ONE.
    */
   blend.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_ZERO;
   blend.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ZERO;
   blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
   void *preserve_blend = pipe->create_blend_state(pipe, &blend);
   CHECK(preserve_blend);
   pipe->bind_blend_state(pipe, preserve_blend);
   draw_and_check(pipe, "destination-only UNORM", NULL);
   struct agx_linked_shader *linked = agx_context(pipe)->linked.fs;
   unsigned pass = linked->fragment_control.opaque[0] >> 29;
   printf("destination-only UNORM: pass=%u no_op=%u\n", pass, linked->no_op);
   CHECK(linked->no_op || pass == AGX_PASS_TYPE_TRANSLUCENT);

   texture_template.width0 = texture_template.height0 = 64;
   texture_template.format = PIPE_FORMAT_R8G8B8A8_SRGB;
   struct pipe_resource *srgb = screen->resource_create(screen, &texture_template);
   CHECK(srgb);
   struct pipe_framebuffer_state srgb_framebuffer = framebuffer;
   srgb_framebuffer.cbufs[0].texture = srgb;
   srgb_framebuffer.cbufs[0].format = texture_template.format;
   pipe->set_framebuffer_state(pipe, &srgb_framebuffer);
   draw_and_check(pipe, "destination-only sRGB", &full_z);
   linked = agx_context(pipe)->linked.fs;
   pass = linked->fragment_control.opaque[0] >> 29;
   printf("destination-only sRGB: pass=%u no_op=%u\n", pass, linked->no_op);
   CHECK(linked->no_op || pass == AGX_PASS_TYPE_TRANSLUCENT);

   /* The shim rejects any cleanup submission. No GPU result is claimed. */
   pipe->bind_vs_state(pipe, NULL);
   pipe->bind_fs_state(pipe, NULL);
   pipe->delete_vs_state(pipe, vs);
   pipe->delete_fs_state(pipe, fs);
   pipe->bind_vertex_elements_state(pipe, NULL);
   pipe->delete_vertex_elements_state(pipe, vertex_elements);
   pipe->bind_blend_state(pipe, NULL);
   pipe->delete_blend_state(pipe, blend_state);
   pipe->delete_blend_state(pipe, preserve_blend);
   pipe->bind_depth_stencil_alpha_state(pipe, NULL);
   pipe->delete_depth_stencil_alpha_state(pipe, zs_state);
   pipe->bind_rasterizer_state(pipe, NULL);
   pipe->delete_rasterizer_state(pipe, raster_state);
   pipe->delete_rasterizer_state(pipe, scissored);
   pipe->delete_rasterizer_state(pipe, full_depth);
   pipe_resource_reference(&other, NULL);
   pipe_resource_reference(&srgb, NULL);
   pipe_resource_reference(&texture, NULL);
   pipe_resource_reference(&buffer, NULL);
   pipe->destroy(pipe);
   screen->destroy(screen);
   driDestroyOptionInfo(&info);
   close(fd);
   puts("G18G raster state emission checks passed (no GPU execution)");
   return 0;
}
