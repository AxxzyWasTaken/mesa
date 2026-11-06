/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx_ppp_hal200_graphics_core.h"
#include "agx_bytes.h"
#include "agx_vdm_state_core.h"

#include <string.h>

bool
agx_ppp_hal200_cull_mode_pack(enum agx_chip chip, unsigned cull_mode,
                              bool front_face_ccw, uint32_t original,
                              uint32_t *out)
{
   if (!agx_chip_native_agx3(chip) || cull_mode > 3 || !out)
      return false;

   *out = (original & ~UINT32_C(0x10003)) | cull_mode |
          ((uint32_t)front_face_ccw << 16);
   return true;
}

bool
agx_ppp_hal200_cull_provoking_pack(enum agx_chip chip, unsigned selector,
                                   uint32_t original, uint32_t *out)
{
   if (!agx_chip_native_agx3(chip) || selector > 2 || !out)
      return false;

   *out = (original & ~UINT32_C(0x180)) | ((selector + 1) << 7);
   return true;
}

bool
agx_ppp_hal200_fragment_face_raster_pack(
   enum agx_chip chip, unsigned line_width, unsigned polygon_mode,
   uint32_t original, uint32_t *out)
{
   if (!agx_chip_native_agx3(chip) || line_width > 0xff || polygon_mode > 2 || !out)
      return false;

   /* setLineWidth is consumed by the native finalize path as unsigned 4:4
    * fixed point minus one. setTriangleFillMode at 0x45a1a4 writes the same
    * two-bit field to both face words with BFI #18,#2. */
   const uint32_t mask = UINT32_C(0x000cff00);
   *out = (original & ~mask) | (line_width << 8) | (polygon_mode << 18);
   return true;
}

bool
agx_ppp_hal200_fragment_face_object_pack(
   enum agx_chip chip, unsigned object_type, uint32_t original,
   uint32_t *out)
{
   /* These are the four native Object Type encodings: triangle, line, and
    * the two point-sprite UV conventions. Reject every ungrounded nibble. */
   if (!agx_chip_native_agx3(chip) ||
       (object_type != 0 && object_type != 1 && object_type != 2 &&
        object_type != 4) ||
       !out)
      return false;

   *out = (original & ~UINT32_C(0xf0000000)) | (object_type << 28);
   return true;
}

bool
agx_ppp_hal200_primitive_msaa_pack(enum agx_chip chip, bool enable,
                                   uint32_t original, uint32_t *out)
{
   if (!agx_chip_native_agx3(chip) || !out)
      return false;

   *out = (original & ~UINT32_C(0x00008000)) |
          (enable ? UINT32_C(0x00008000) : 0);
   return true;
}

bool
agx_ppp_hal200_depth_clip_pack(enum agx_chip chip, bool depth_clip,
                               uint32_t original, uint32_t *out)
{
   if (!agx_chip_native_agx3(chip) || !out)
      return false;

   *out = (original & ~UINT32_C(0xc00)) |
          (depth_clip ? UINT32_C(0x400) : UINT32_C(0x800));
   return true;
}

bool
agx_ppp_hal200_rasterizer_discard_pack(
   enum agx_chip chip, bool discard, uint32_t secondary_original,
   uint32_t cull_original, uint32_t *secondary_out, uint32_t *cull_out)
{
   if (!agx_chip_native_agx3(chip) || !secondary_out || !cull_out)
      return false;

   const uint32_t secondary_mask = UINT32_C(1) << 21;
   const uint32_t cull_mask = UINT32_C(1) << 17;
   *secondary_out = (secondary_original & ~secondary_mask) |
                    (discard ? secondary_mask : 0);
   *cull_out = (cull_original & ~cull_mask) |
               (discard ? cull_mask : 0);
   return true;
}

bool
agx_ppp_hal200_fragment_controls_pack(
   enum agx_chip chip,
   const struct agx_ppp_hal200_fragment_control_fields *fields,
   uint32_t primary_original, uint32_t secondary_original,
   uint32_t *primary_out, uint32_t *secondary_out)
{
   if (!agx_chip_native_agx3(chip) || !fields || !primary_out || !secondary_out)
      return false;

   const uint32_t pass_type = fields->linked_control >> 29;
   if (pass_type > 3)
      return false;

   const uint32_t tri_merge_disable = UINT32_C(1) << 26;

   /* updateFragmentShaderISPState masks these semantic fields into the
    * HAL200 SECONDARY word.  Bit 26 is deliberately absent: Apple's G17P
    * metadata leaves it clear for implicit texture gradients, explicit
    * gradients, storage writes, and shadow sampling.  The generation-neutral
    * linker sets its legacy bit for all of those cases, so it cannot be copied
    * as native shader metadata. */
   const uint32_t linked_native_mask = UINT32_C(0xe2600000);
   uint32_t secondary = (secondary_original & ~tri_merge_disable) |
                        (fields->linked_control & linked_native_mask);

   if (fields->shader_tri_merge_disable ||
       fields->coverage_tri_merge_disable ||
       fields->primitive_tri_merge_disable)
      secondary |= tri_merge_disable;

   /* encodeAndEmitRenderState applies only the primitive-derived override to
    * both controls.  Shader and coverage policy belong to SECONDARY. */
   uint32_t primary = primary_original & ~tri_merge_disable;
   if (fields->primitive_tri_merge_disable)
      primary |= tri_merge_disable;

   *primary_out = primary;
   *secondary_out = secondary;
   return true;
}

bool
agx_ppp_hal200_occlusion_pack(enum agx_chip chip, unsigned mode,
                              unsigned index,
                              uint32_t original_fragment_control,
                              uint32_t *fragment_control,
                              uint32_t *query_offset)
{
   if (!agx_chip_native_agx3(chip) || (mode != 0 && mode != 2 && mode != 3) ||
       index >= 32768 || !fragment_control || !query_offset)
      return false;

   *fragment_control =
      (original_fragment_control & ~UINT32_C(0xc000)) | (mode << 14);
   *query_offset = index << 17;
   return true;
}

static bool
bundle_layout(enum agx_ppp_hal200_graphics_bundle bundle, uint32_t *header,
              unsigned *payload_words)
{
   switch (bundle) {
   case AGX_PPP_HAL200_GRAPHICS_FS:
      *header = AGX_PPP_HAL200_FS_HEADER;
      *payload_words = 6;
      return true;
   case AGX_PPP_HAL200_GRAPHICS_OUTPUT:
      *header = AGX_PPP_HAL200_OUTPUT_HEADER;
      *payload_words = 6;
      return true;
   case AGX_PPP_HAL200_GRAPHICS_PRIMARY_FACE:
      *header = AGX_PPP_HAL200_PRIMARY_FACE_HEADER;
      *payload_words = 6;
      return true;
   case AGX_PPP_HAL200_GRAPHICS_SECONDARY_FACE:
      *header = AGX_PPP_HAL200_SECONDARY_FACE_HEADER;
      *payload_words = 4;
      return true;
   case AGX_PPP_HAL200_GRAPHICS_CULL:
      *header = AGX_PPP_HAL200_CULL_HEADER;
      *payload_words = 1;
      return true;
   default:
      *header = 0;
      *payload_words = 0;
      return false;
   }
}

/*
 * Pack one complete fixed-layout G17P record.  Each record has its own header
 * and outer VDM PPP_STATE pointer; records must never be concatenated behind a
 * single legacy selector header.
 *
 * Payload order and word count by bundle:
 *
 *   FS:             counts, VIT, unknown-0c, stage-program-high,
 *                   stage-program-low, flags
 *   OUTPUT:         output-select, varying-counts-16, output-unknown,
 *                   output-size, primary-face header, fragment-control
 *                   (the traced seven-word window deliberately overlaps the
 *                   first two words of the following primary-face record)
 *   PRIMARY_FACE:   fragment-control, front-face, front-stencil, back-face,
 *                   back-stencil, occlusion-query
 *   SECONDARY_FACE: fragment-control-2, front-face-2, back-face-2,
 *                   occlusion-query-2
 *   CULL:           cull
 */
bool
agx_ppp_hal200_graphics_core_supported(enum agx_chip chip)
{
   return agx_chip_native_agx3(chip);
}

bool
agx_ppp_hal200_output_select_pack(enum agx_chip chip,
                                  uint32_t vertex_output_select,
                                  bool fragment_varyings,
                                  bool fragment_reads_z, uint32_t *out)
{
   const uint32_t clip_distances = UINT32_C(0xffff);
   const uint32_t point_size = UINT32_C(1) << 18;
   const uint32_t viewport_target = UINT32_C(1) << 19;
   const uint32_t render_target = UINT32_C(1) << 20;
   const uint32_t vertex_mask = clip_distances | point_size |
                                viewport_target | render_target;

   if (!agx_chip_native_agx3(chip) || !out ||
       (vertex_output_select & ~vertex_mask) ||
       !!(vertex_output_select & viewport_target) !=
          !!(vertex_output_select & render_target))
      return false;

   *out = vertex_output_select |
          (fragment_varyings ? UINT32_C(1) << 16 : 0) |
          (fragment_reads_z ? UINT32_C(1) << 21 : 0);
   return true;
}

bool
agx_ppp_hal200_graphics_core_pack(
   enum agx_chip chip, enum agx_ppp_hal200_graphics_bundle bundle,
   const struct agx_ppp_hal200_graphics_fields *fields,
   struct agx_ppp_hal200_graphics_core *out)
{
   uint32_t header = 0;
   unsigned payload_words = 0;

   if (out)
      memset(out, 0, sizeof(*out));

   if (!out || !fields || !agx_ppp_hal200_graphics_core_supported(chip) ||
       !bundle_layout(bundle, &header, &payload_words))
      return false;

   agx_store_le32(out->bytes, header);
   for (unsigned i = 0; i < payload_words; ++i)
      agx_store_le32(&out->bytes[4 + (4 * i)], fields->words[i]);

   out->length = (uint8_t)(4 + (4 * payload_words));
   return true;
}

bool
agx_ppp_hal200_graphics_core_unpack(
   enum agx_chip chip, enum agx_ppp_hal200_graphics_bundle bundle,
   const void *bytes, size_t size,
   struct agx_ppp_hal200_graphics_fields *fields)
{
   uint32_t header = 0;
   unsigned payload_words = 0;

   if (fields)
      memset(fields, 0, sizeof(*fields));

   if (!bytes || !fields ||
       !agx_ppp_hal200_graphics_core_supported(chip) ||
       !bundle_layout(bundle, &header, &payload_words) ||
       size != 4 + (4 * payload_words))
      return false;

   const uint8_t *raw = bytes;
   if (agx_load_le32(raw) != header)
      return false;

   for (unsigned i = 0; i < payload_words; ++i)
      fields->words[i] = agx_load_le32(&raw[4 + (4 * i)]);

   return true;
}

bool
agx_ppp_hal200_fragment_ti_resources(
   enum agx_chip chip, uint32_t spill_bytes, uint32_t largest_ipr_bytes,
   struct agx_ppp_hal200_fragment_fields *fields)
{
   const uint64_t spill_quads = ((uint64_t)spill_bytes + 3) >> 2;
   const uint64_t ipr_quads = ((((uint64_t)largest_ipr_bytes + 3) >> 2) + 1) &
                             ~UINT64_C(1);
   if (!agx_chip_native_agx3(chip) || !fields || spill_quads > 0x3ffff ||
       ipr_quads > 0xffe)
      return false;

   /* Pinned G17P FragmentProgramVariantESLState::finalize:
    * 0x218841194..0x2188411c4 consumes the builder's TI spill quads at
    * +0x128 and rounded TI largest-IPR bytes at +0x12c. It stores the
    * seven-word fragment record at +0x544: IPR at +0x550, spill at +0x554. */
   fields->unknown_0c = (fields->unknown_0c & ~UINT32_C(0xfff)) | ipr_quads;
   fields->stage_program_template =
      (fields->stage_program_template & ~UINT32_C(0x1ffff8)) | (spill_quads << 3);
   return true;
}

bool
agx_ppp_hal200_fragment_core_pack(
   enum agx_chip chip, const struct agx_ppp_hal200_fragment_fields *fields,
   struct agx_ppp_hal200_graphics_core *out)
{
   if (out)
      memset(out, 0, sizeof(*out));

   if (!fields || !out || (fields->varying_table & UINT64_C(0x3f)) != 0 ||
       fields->varying_table >= (UINT64_C(1) << 48) ||
       (fields->resource_counts & UINT32_C(0xffff)) != 0)
      return false;

   struct agx_vdm_state_vertex_pipeline_pair stage_program;
   if (!agx_vdm_state_vertex_pipeline_pair_pack(
          chip, fields->stage_program, fields->stage_program_template,
          &stage_program))
      return false;

   const struct agx_ppp_hal200_graphics_fields raw = {
      .words = {
         /* HAL200 emitFragmentProgramVariant at 0x4c5e38..0x4c5ec4
          * merges the absolute VIT pair, using masks at 0x812f70. */
         fields->resource_counts | (uint32_t)(fields->varying_table >> 32),
         (uint32_t)fields->varying_table,
         fields->unknown_0c,
         agx_load_le32(&stage_program.bytes[0]),
         agx_load_le32(&stage_program.bytes[4]),
         fields->flags,
      },
   };
   return agx_ppp_hal200_graphics_core_pack(
      chip, AGX_PPP_HAL200_GRAPHICS_FS, &raw, out);
}

bool
agx_ppp_hal200_output_size_pack(uint32_t count_0, uint32_t count_2,
                                 uint32_t *out)
{
   if (!out)
      return false;
   *out = 0;
   if (count_0 > 0xff || count_2 > 0xff)
      return false;

   /* VertexProgramVariant 0x35b314..0x35b328 packs VsStateConfig+0xc/+0x14.
    * The first field includes all position and user f32 words, as proven by
    * Apple vs0/vs2/vs4 metadata giving (4,0)/(6,0)/(8,0). */
   *out = count_0 | (count_2 << 16);
   return true;
}

bool
agx_ppp_hal200_viewport_count_pack(enum agx_chip chip,
                                   unsigned viewport_count, uint32_t *out)
{
   if (!agx_chip_native_agx3(chip) || viewport_count == 0 ||
       viewport_count > 16 || !out)
      return false;

   *out = viewport_count - 1;
   return true;
}

/* Native HAL200 setDepthStencilState, 0x458878..0x458894, derives inverted
 * write-enable at bit21 and comparison at bits24..26. 0x458970..0x4589ac
 * merges those fields into both front and back face state. The depth descriptor
 * constructor reads Metal comparison directly at 0x26ab04..0x26ab10; its values
 * are the same eight comparison operations as Vulkan. No stencil bits move.
 */
bool
agx_ppp_hal200_depth_face_pack(enum agx_chip chip, bool test_enable,
                               bool write_enable, uint32_t compare,
                               uint32_t original, uint32_t *out)
{
   if (!out || !agx_chip_native_agx3(chip) || compare > 7)
      return false;

   const uint32_t mask = UINT32_C(0x07200000);
   const uint32_t function = test_enable ? compare : 7;
   const uint32_t disable_write = !(test_enable && write_enable);
   *out = (original & ~mask) | (function << 24) | (disable_write << 21);
   return true;
}

bool
agx_ppp_hal200_fragment_face_depth_pack(
   enum agx_chip chip, unsigned conservative_depth, bool early_tests,
   bool writes_memory, uint32_t original, uint32_t *out)
{
   if (!out || !agx_chip_native_agx3(chip) || conservative_depth > 3)
      return false;

   /* Native __GPU_LD_MD root2.field4 -> FragmentProgramVariant+0xfcd
    * (216ba0bac..bd8), then updateFragmentShaderISPState (216c71904..950)
    * inserts this mode into both SECONDARY face words. Isolated native
    * witnesses give ANY=0, GREATER=1, LESS=2, UNCHANGED=3; adding a late
    * atomic to either conservative depth shader changes its mode to ANY. */
   unsigned mode = agx_ppp_fragment_depth_mode(conservative_depth, early_tests,
                                              writes_memory);
   *out = (original & ~UINT32_C(0x00c00000)) | (mode << 22);
   return true;
}

/* AGXMetalG17P 26aa64..26ab64 constructs the descriptor; 458898..45891c
 * lowers both faces to this layout. The vector shifts/masks at811700/811710
 * route pass/depth-fail/fail/compare to16/19/22/25, respectively. Disabled
 * stencil is ALWAYS+KEEP with zero masks, exactly the native0x0e000000.
 */
uint32_t
agx_ppp_hal200_stencil_pack(bool enabled, unsigned compare, unsigned fail,
                           unsigned depth_fail, unsigned pass,
                           unsigned compare_mask, unsigned write_mask)
{
   if (!enabled)
      return UINT32_C(0x0e000000);

   return (write_mask & 0xff) | ((compare_mask & 0xff) << 8) |
          ((pass & 7) << 16) | ((depth_fail & 7) << 19) |
          ((fail & 7) << 22) | ((compare & 7) << 25);
}
