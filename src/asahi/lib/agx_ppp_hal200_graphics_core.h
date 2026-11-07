/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded G17P/HAL200 fixed-layout PPP graphics-state records.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_PPP_HAL200_FS_LENGTH 28u
#define AGX_PPP_HAL200_OUTPUT_LENGTH 28u
#define AGX_PPP_HAL200_PRIMARY_FACE_LENGTH 28u
#define AGX_PPP_HAL200_SECONDARY_FACE_LENGTH 20u
#define AGX_PPP_HAL200_CULL_LENGTH 8u
#define AGX_PPP_HAL200_GRAPHICS_MAX_LENGTH 28u

#define AGX_PPP_HAL200_FS_HEADER UINT32_C(0x00800000)
#define AGX_PPP_HAL200_OUTPUT_HEADER UINT32_C(0x0c0a0000)
#define AGX_PPP_HAL200_PRIMARY_FACE_HEADER UINT32_C(0x010000b4)
#define AGX_PPP_HAL200_SECONDARY_FACE_HEADER UINT32_C(0x02000048)
#define AGX_PPP_HAL200_CULL_HEADER UINT32_C(0x00200000)

enum agx_ppp_hal200_graphics_bundle {
   AGX_PPP_HAL200_GRAPHICS_FS,
   AGX_PPP_HAL200_GRAPHICS_OUTPUT,
   AGX_PPP_HAL200_GRAPHICS_PRIMARY_FACE,
   AGX_PPP_HAL200_GRAPHICS_SECONDARY_FACE,
   AGX_PPP_HAL200_GRAPHICS_CULL,
};

struct agx_ppp_hal200_graphics_fields {
   uint32_t words[6];
};

/* Native setCullMode/setFrontFacingWinding merge only bits0..1/16. */
bool agx_ppp_hal200_cull_mode_pack(enum agx_chip chip, unsigned cull_mode,
                                  bool front_face_ccw, uint32_t original,
                                  uint32_t *out);

/* Native setProvokingVertexMode (458b4c..458b60): CULL bits7..8
 * encode raw vertex selector+1 independently of VDM and the VIT. */
bool agx_ppp_hal200_cull_provoking_pack(enum agx_chip chip, unsigned selector,
                                       uint32_t original, uint32_t *out);

/* Native G17P face-state setters preserve these legacy semantic fields inside
 * the fixed HAL200 PRIMARY/SECONDARY records. */
bool agx_ppp_hal200_fragment_face_raster_pack(
   enum agx_chip chip, unsigned line_width, unsigned polygon_mode,
   uint32_t original, uint32_t *out);

bool agx_ppp_hal200_fragment_face_object_pack(
   enum agx_chip chip, unsigned object_type, uint32_t original,
   uint32_t *out);

/* SECONDARY front/back face bits22:23 carry conservative depth. Native
 * late-memory shaders select ANY even with a conservative depth qualifier;
 * explicit early tests select UNCHANGED and ignore shader depth writes. */
static inline unsigned
agx_ppp_fragment_depth_mode(unsigned conservative_depth, bool early_tests,
                           bool writes_memory)
{
   return early_tests ? 3 : writes_memory ? 0 : conservative_depth;
}

bool agx_ppp_hal200_fragment_face_depth_pack(
   enum agx_chip chip, unsigned conservative_depth, bool early_tests,
   bool writes_memory, uint32_t original, uint32_t *out);

bool agx_ppp_hal200_primitive_msaa_pack(enum agx_chip chip, bool enable,
                                       uint32_t original, uint32_t *out);

/* Native setDepthClipMode: CULL bits11:10 are 01 for clip and 10 for clamp. */
bool agx_ppp_hal200_depth_clip_pack(enum agx_chip chip, bool depth_clip,
                                   uint32_t original, uint32_t *out);

/* Rasterizer discard is a dual-record contract on HAL200. */
bool agx_ppp_hal200_rasterizer_discard_pack(
   enum agx_chip chip, bool discard, uint32_t secondary_original,
   uint32_t cull_original, uint32_t *secondary_out, uint32_t *cull_out);

/* Translate the linker's generation-independent fragment control into the
 * two native HAL200 controls.  Triangle merging is not ABI-compatible: the
 * linker's bit 26 describes the legacy helper-invocation policy, while G17P
 * derives its bit from native shader metadata, coverage, and primitive state.
 */
struct agx_ppp_hal200_fragment_control_fields {
   uint32_t linked_control;
   bool shader_tri_merge_disable;
   bool coverage_tri_merge_disable;
   bool primitive_tri_merge_disable;
};

bool agx_ppp_hal200_fragment_controls_pack(
   enum agx_chip chip,
   const struct agx_ppp_hal200_fragment_control_fields *fields,
   uint32_t primary_original, uint32_t secondary_original,
   uint32_t *primary_out, uint32_t *secondary_out);

/* Native G17P visibility state: mode occupies fragment-control bits14..15;
 * the 64-bit counter byte offset is encoded as byteOffset<<14. */
bool agx_ppp_hal200_occlusion_pack(enum agx_chip chip, unsigned mode,
                                  unsigned index,
                                  uint32_t original_fragment_control,
                                  uint32_t *fragment_control,
                                  uint32_t *query_offset);

struct agx_ppp_hal200_graphics_core {
   uint8_t bytes[AGX_PPP_HAL200_GRAPHICS_MAX_LENGTH];
   uint8_t length;
};

struct agx_ppp_hal200_fragment_fields {
   uint32_t resource_counts;
   /* Absolute 48-bit VIT address; its upper 16 bits occupy counts[15:0].
    * Zero when the fragment has no varyings. The bounded uploader aligns to 64 bytes. */
   uint64_t varying_table;
   uint32_t unknown_0c;
   /* Absolute 48-bit, 64-byte-aligned Ioto/ESL stage-program address. */
   uint64_t stage_program;
   /* Caller-preserved low 22 bits of the stage-program pair's high word. */
   uint32_t stage_program_template;
   uint32_t flags;
};

/* G17P fragment thread-invariant resources, separate from main SPECTPR.
 * Spill bytes become quads in stage-program-template bits 3..20. The native
 * thread_invariant_largest_ipr_bytes field becomes an even quad count in
 * unknown_0c bits 0..11. This is not a temporary-GPR count. Other template
 * bits are preserved; failure leaves fields unchanged. */
bool agx_ppp_hal200_fragment_ti_resources(
   enum agx_chip chip, uint32_t spill_bytes, uint32_t largest_ipr_bytes,
   struct agx_ppp_hal200_fragment_fields *fields);

bool agx_ppp_hal200_graphics_core_supported(enum agx_chip chip);

bool agx_ppp_hal200_graphics_core_pack(
   enum agx_chip chip, enum agx_ppp_hal200_graphics_bundle bundle,
   const struct agx_ppp_hal200_graphics_fields *fields,
   struct agx_ppp_hal200_graphics_core *out);

bool agx_ppp_hal200_graphics_core_unpack(
   enum agx_chip chip, enum agx_ppp_hal200_graphics_bundle bundle,
   const void *bytes, size_t size,
   struct agx_ppp_hal200_graphics_fields *fields);

/* Pack the VDM-referenced G17P fragment record by semantic field. */
bool agx_ppp_hal200_fragment_core_pack(
   enum agx_chip chip, const struct agx_ppp_hal200_fragment_fields *fields,
   struct agx_ppp_hal200_graphics_core *out);

/* Update only native PRIMARY_FACE depth fields, preserving raster state.
 * Compare values use Metal/Vulkan ordering: NEVER=0 through ALWAYS=7. */
bool agx_ppp_hal200_depth_face_pack(enum agx_chip chip, bool test_enable,
                                    bool write_enable, uint32_t compare,
                                    uint32_t original, uint32_t *out);

/* Native stencil word: Vulkan/Metal operation and comparison ordering. */
uint32_t agx_ppp_hal200_stencil_pack(bool enabled, unsigned compare,
                                    unsigned fail, unsigned depth_fail,
                                    unsigned pass, unsigned compare_mask,
                                    unsigned write_mask);

/* Native VsStateConfig+0xc/+0x14 fields; the first includes all f32 UVS. */
bool agx_ppp_hal200_output_size_pack(uint32_t count_0,
                                      uint32_t count_2, uint32_t *out);

/* Merge the compiler's vertex OUTPUT_SELECT word with the fragment-owned
 * varying/depth inputs consumed by the HAL200 OUTPUT record. */
bool agx_ppp_hal200_output_select_pack(enum agx_chip chip,
                                      uint32_t vertex_output_select,
                                      bool fragment_varyings,
                                      bool fragment_reads_z,
                                      uint32_t *out);

/* Native vertex-state update writes viewport_count - 1 to context+0x1f94;
 * the HAL200 output bundle consumes that word unchanged. */
bool agx_ppp_hal200_viewport_count_pack(enum agx_chip chip,
                                       unsigned viewport_count,
                                       uint32_t *out);

#ifdef __cplusplus
}
#endif
