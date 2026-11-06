/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "hk_drm_render_core.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void
test_uapi_extension_layout(void)
{
   static_assert(DRM_ASAHI_RENDER_RSRC_SPEC_HI == (1U << 4),
                 "resource-specifier high-tail flag ABI");
   static_assert(offsetof(struct drm_asahi_cmd_render,
                          bg_eot_rsrc_spec_hi) == 240,
                 "append-only render command ABI");
   static_assert(offsetof(struct drm_asahi_cmd_render,
                          bg_eot_partial_rsrc_spec_hi) == 244,
                 "append-only render command ABI");
   static_assert(offsetof(struct drm_asahi_cmd_render, bg_rsrc_spec_hi) == 248,
                 "append-only render command ABI");
   static_assert(offsetof(struct drm_asahi_cmd_render,
                          bg_partial_rsrc_spec_hi) == 252,
                 "append-only render command ABI");
   static_assert(offsetof(struct drm_asahi_cmd_render, ts_frag_end_copies) ==
                    256,
                 "append-only render command ABI");
   static_assert(sizeof(struct drm_asahi_cmd_render) == 320,
                 "extended render command ABI");
   static_assert(sizeof(struct drm_asahi_cmd_render_with_prior) == 336,
                 "exact-prior render command ABI");
   static_assert(sizeof(struct drm_asahi_cmd_compute) == 128,
                 "compute command ABI");

   /* The G18 commands embed the complete render/compute command. */
   static_assert(sizeof(struct drm_asahi_cmd_render_with_ray_query) == 448,
                 "ray-query render command ABI");
   static_assert(offsetof(struct drm_asahi_cmd_render_g18, resources) == 448,
                 "G18 render resources ABI");
   static_assert(sizeof(struct drm_asahi_cmd_render_g18) == 528,
                 "G18 render command ABI");
   static_assert(sizeof(struct drm_asahi_cmd_compute_with_prior) == 200,
                 "exact-prior compute command ABI");
   static_assert(offsetof(struct drm_asahi_cmd_compute_g18, resources) == 200,
                 "G18 compute resources ABI");
   static_assert(sizeof(struct drm_asahi_cmd_compute_g18) == 264,
                 "G18 compute command ABI");
   static_assert(DRM_ASAHI_RENDER_G18_RESOURCES == (1U << 25),
                 "G18 render flag ABI");
   static_assert(DRM_ASAHI_COMPUTE_USES_TEXTURE_STATE == (1U << 18),
                 "compute texture-state flag ABI");
   static_assert(DRM_ASAHI_COMPUTE_G18_RESOURCES == (1U << 19),
                 "G18 compute flag ABI");
   static_assert(DRM_ASAHI_FEATURE_G18_GRAPHICS_SCRATCH == (1ULL << 31),
                 "G18 graphics scratch feature ABI");
   static_assert(DRM_ASAHI_FEATURE_G18_NATIVE_TILE_SAMPLES == (1ULL << 32),
                 "G18 native tile samples feature ABI");
}

static void
test_exact_g17p_specs(void)
{
   struct drm_asahi_cmd_render cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.flags = DRM_ASAHI_RENDER_VERTEX_SCRATCH;
   cmd.bg.usc = UINT32_C(0x12345644);
   cmd.eot.usc = UINT32_C(0x23456784);

   const struct hk_drm_render_resource_specs specs = {
      .bg = UINT64_C(0x0007800000000040),
      .eot = UINT64_C(0x8),
      .partial_bg = UINT64_C(0x0007800000000040),
      .partial_eot = UINT64_C(0x8),
   };
   hk_drm_render_resource_specs_pack(&specs, &cmd);

   assert(cmd.bg.rsrc_spec == UINT32_C(0x40));
   assert(cmd.eot.rsrc_spec == UINT32_C(0x8));
   assert(cmd.partial_bg.rsrc_spec == UINT32_C(0x40));
   assert(cmd.partial_eot.rsrc_spec == UINT32_C(0x8));
   assert(cmd.bg_eot_rsrc_spec_hi == 0);
   assert(cmd.bg_eot_partial_rsrc_spec_hi == 0);
   assert(cmd.bg_rsrc_spec_hi == UINT32_C(0x00078000));
   assert(cmd.bg_partial_rsrc_spec_hi == UINT32_C(0x00078000));
   assert(cmd.flags == (DRM_ASAHI_RENDER_VERTEX_SCRATCH |
                        DRM_ASAHI_RENDER_RSRC_SPEC_HI));
   assert(cmd.bg.usc == UINT32_C(0x12345644));
   assert(cmd.eot.usc == UINT32_C(0x23456784));
}

static void
test_field_order_and_flag_clear(void)
{
   struct drm_asahi_cmd_render cmd;
   memset(&cmd, 0, sizeof(cmd));
   const struct hk_drm_render_resource_specs distinct = {
      .bg = UINT64_C(0x1111111122222222),
      .eot = UINT64_C(0x3333333344444444),
      .partial_bg = UINT64_C(0x5555555566666666),
      .partial_eot = UINT64_C(0x7777777788888888),
   };
   hk_drm_render_resource_specs_pack(&distinct, &cmd);
   assert(cmd.bg_eot_rsrc_spec_hi == UINT32_C(0x33333333));
   assert(cmd.bg_eot_partial_rsrc_spec_hi == UINT32_C(0x77777777));
   assert(cmd.bg_rsrc_spec_hi == UINT32_C(0x11111111));
   assert(cmd.bg_partial_rsrc_spec_hi == UINT32_C(0x55555555));
   assert(cmd.eot.rsrc_spec == UINT32_C(0x44444444));
   assert(cmd.partial_eot.rsrc_spec == UINT32_C(0x88888888));
   assert(cmd.bg.rsrc_spec == UINT32_C(0x22222222));
   assert(cmd.partial_bg.rsrc_spec == UINT32_C(0x66666666));
   assert(cmd.flags & DRM_ASAHI_RENDER_RSRC_SPEC_HI);

   const struct hk_drm_render_resource_specs low_only = {
      .bg = 1,
      .eot = 2,
      .partial_bg = 3,
      .partial_eot = 4,
   };
   hk_drm_render_resource_specs_pack(&low_only, &cmd);
   assert((cmd.flags & DRM_ASAHI_RENDER_RSRC_SPEC_HI) == 0);
   assert(cmd.bg_eot_rsrc_spec_hi == 0);
   assert(cmd.bg_eot_partial_rsrc_spec_hi == 0);
   assert(cmd.bg_rsrc_spec_hi == 0);
   assert(cmd.bg_partial_rsrc_spec_hi == 0);
}

static void
test_submit_command_limit(void)
{
   assert(hk_drm_submit_command_limit(false, 64) == 1);
   assert(hk_drm_submit_command_limit(true, 64) == 64);
   assert(hk_drm_submit_command_limit(true, 32) == 32);
   assert(hk_drm_submit_command_limit(true, 1) == 1);
   assert(hk_drm_submit_command_limit(false, 0) == 1);
}

static void
test_g17p_zls_pixels_follow_backing_storage(void)
{
   /* Native M5 color has no ZLS dimensions; D32 clear/store is ff00ff.
    * Keep the G17P representation unchanged below. */
   assert(hk_drm_m5_zls_pixels(256, 256, 0, 0, 0, 0) == 0);
   assert(hk_drm_m5_zls_pixels(256, 256, 0x100000c8000,
                              0x10000108000, 0, 0) == 0x00ff00ff);
   assert(hk_drm_m5_zls_pixels(256, 256, 0x100000c8000, 0, 0, 0) ==
          0x00ff00ff);
   /* All 303 colour-only records in the valid G17P corpus have a zero
    * dimension register beside four null depth/stencil addresses.  This is
    * true for both the main and partial render phases. */
   for (unsigned phase = 0; phase < 2; ++phase) {
      (void)phase;
      assert(hk_drm_g17p_zls_pixels(0, 0, 0, 0, 0, 0) == 0);
      assert(hk_drm_g17p_zls_pixels(32, 32, 0, 0, 0, 0) == 0);
      assert(hk_drm_g17p_zls_pixels(2408, 1506, 0, 0, 0, 0) == 0);
   }

   /* Every valid native record with non-zero dimensions also publishes at
    * least one Z/S mapping.  Either a depth or stencil plane is sufficient. */
   const uint32_t pixels_32 = UINT32_C(0x000f801f);
   assert(hk_drm_g17p_zls_pixels(32, 32, 0x1000, 0, 0, 0) ==
          pixels_32);
   assert(hk_drm_g17p_zls_pixels(32, 32, 0, 0, 0x2000, 0) ==
          pixels_32);

   /* Count a metadata-only witness as active rather than silently emitting
    * the inactive tuple.  Such a tuple would be malformed, but fail closed. */
   assert(hk_drm_g17p_zls_pixels(32, 32, 0, 0x3000, 0, 0) ==
          pixels_32);
   assert(hk_drm_g17p_zls_pixels(32, 32, 0, 0, 0, 0x4000) ==
          pixels_32);
}

static void
test_g17p_inactive_zls_uses_neutral_depth(void)
{
   /* The valid corpus has a perfect split: every item with all Z/S mappings
    * absent uses 1.0, while mapped Z/S preserves the application clear. */
   assert(hk_drm_g17p_depth_clear(0, 0, 0, 0, 0) == UINT32_C(0x3f800000));
   assert(hk_drm_g17p_depth_clear(UINT32_C(0x3f000000), 0, 0, 0, 0) ==
          UINT32_C(0x3f800000));
   assert(hk_drm_g17p_depth_clear(0, 0x1000, 0, 0, 0) == 0);
   assert(hk_drm_g17p_depth_clear(UINT32_C(0x3f000000), 0, 0, 0x2000, 0) ==
          UINT32_C(0x3f000000));
}

static void
test_fragment_attachment_ranges(void)
{
   struct hk_drm_attachment_set set;
   hk_drm_attachment_set_init(&set);
   assert(set.complete && set.count == 0);

   assert(hk_drm_attachment_set_add(&set, 0x1000, 0x1000));
   assert(hk_drm_attachment_set_add(&set, 0x4000, 0x1000));
   assert(set.count == 2);

   /* Bridge both ranges, proving adjacency and overlap are coalesced even if
    * the bridge is appended after both original ranges. */
   assert(hk_drm_attachment_set_add(&set, 0x2000, 0x2000));
   assert(set.count == 1);
   assert(set.list[0].pointer == 0x1000);
   assert(set.list[0].size == 0x4000);

   /* Duplicate depth/stencil or resolve views do not consume another slot. */
   assert(hk_drm_attachment_set_add(&set, 0x1800, 0x100));
   assert(set.count == 1 && set.complete);

   assert(!hk_drm_attachment_set_add(&set, 0, 0x1000));
   assert(!set.complete);
}

static void
append_command(uint8_t *buffer, size_t *size, uint16_t type,
               uint16_t payload_size)
{
   const struct drm_asahi_cmd_header header = {
      .cmd_type = type,
      .size = payload_size,
      .vdm_barrier = DRM_ASAHI_BARRIER_NONE,
      .cdm_barrier = DRM_ASAHI_BARRIER_NONE,
   };
   memcpy(buffer + *size, &header, sizeof(header));
   *size += sizeof(header);
   memset(buffer + *size, 0, payload_size);
   *size += payload_size;
}

static void
test_hardware_command_spans_skip_attachment_commands(void)
{
   uint8_t buffer[1024] = {0};
   size_t size = 0;
   append_command(buffer, &size, DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS,
                  2 * sizeof(struct drm_asahi_attachment));
   const size_t first_render = size;
   append_command(buffer, &size, DRM_ASAHI_CMD_RENDER,
                  sizeof(struct drm_asahi_cmd_render));
   append_command(buffer, &size, DRM_ASAHI_SET_COMPUTE_ATTACHMENTS,
                  sizeof(struct drm_asahi_attachment));
   append_command(buffer, &size, DRM_ASAHI_CMD_COMPUTE,
                  sizeof(struct drm_asahi_cmd_compute));
   const size_t second_span = size;
   append_command(buffer, &size, DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS, 0);
   const size_t second_render = size;
   append_command(buffer, &size, DRM_ASAHI_CMD_RENDER,
                  sizeof(struct drm_asahi_cmd_render));

   struct hk_drm_command_span span;
   assert(hk_drm_next_command_span(buffer, size, 0, 2, &span));
   assert(span.first_hardware_offset == first_render);
   assert(span.end_offset == second_span);
   assert(span.hardware_count == 2);
   assert(span.render_count == 1);
   assert(span.compute_count == 1);

   assert(hk_drm_next_command_span(buffer, size, span.end_offset, 1, &span));
   assert(span.first_hardware_offset == second_render);
   assert(span.end_offset == size);
   assert(span.hardware_count == 1 && span.render_count == 1);

   /* A synthetic command must keep both barriers at NONE. */
   ((struct drm_asahi_cmd_header *)buffer)->vdm_barrier = 0;
   assert(!hk_drm_next_command_span(buffer, size, 0, 1, &span));
}

static void
test_legacy_kernel_command_boundaries(void)
{
   uint8_t buffer[1024] = {0};
   size_t size = 0;
   append_command(buffer, &size, DRM_ASAHI_CMD_RENDER,
                  hk_drm_render_command_size(false, false));
   const size_t compute_offset = size;
   append_command(buffer, &size, DRM_ASAHI_CMD_COMPUTE,
                  sizeof(struct drm_asahi_cmd_compute));

   /* The legacy kernel's Reader::read_up_to consumes min(sizeof(T), size).
    * Its next header must be the compute command, not a resource-spec tail.
    * Pin the old kernel's size independently of the extended C structure. */
   struct drm_asahi_cmd_header header;
   memcpy(&header, buffer, sizeof(header));
   const size_t consumed = header.size < 240 ? header.size : 240;
   const size_t next = sizeof(header) + consumed;
   assert(next == compute_offset);
   memcpy(&header, buffer + next, sizeof(header));
   assert(header.cmd_type == DRM_ASAHI_CMD_COMPUTE);
   assert(next + sizeof(header) + header.size == size);

   struct hk_drm_command_span span;
   assert(hk_drm_next_command_span(buffer, size, 0, 2, &span));
   assert(span.end_offset == size);
   assert(span.render_count == 1 && span.compute_count == 1);

   assert(hk_drm_render_command_size(true, false) == 320);
   assert(hk_drm_render_command_size(false, true) == 336);
   assert(hk_drm_render_command_size(true, true) == 336);
}

int
main(void)
{
   test_uapi_extension_layout();
   test_exact_g17p_specs();
   test_field_order_and_flag_clear();
   test_submit_command_limit();
   test_g17p_zls_pixels_follow_backing_storage();
   test_g17p_inactive_zls_uses_neutral_depth();
   test_fragment_attachment_ranges();
   test_hardware_command_spans_skip_attachment_commands();
   test_legacy_kernel_command_boundaries();
   puts("Honeykrisp DRM render resource specs: all checks passed");
   return 0;
}
