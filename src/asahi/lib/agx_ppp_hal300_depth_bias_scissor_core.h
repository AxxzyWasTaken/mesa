/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded HAL300 PPP depth-bias/scissor records and base block.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_PPP_HAL300_DEPTH_BIAS_RECORD_LENGTH        12u
#define AGX_PPP_HAL300_SCISSOR_RECORD_LENGTH           16u
#define AGX_PPP_HAL300_DEPTH_BIAS_SCISSOR_BASES_LENGTH 20u

#define AGX_PPP_HAL300_DEPTH_BIAS_BASE_SELECTOR (UINT32_C(1) << 8)
#define AGX_PPP_HAL300_SCISSOR_BASE_SELECTOR    (UINT32_C(1) << 9)
#define AGX_PPP_HAL300_DEPTH_BIAS_SCISSOR_BASES_HEADER                         \
   (AGX_PPP_HAL300_DEPTH_BIAS_BASE_SELECTOR |                                  \
    AGX_PPP_HAL300_SCISSOR_BASE_SELECTOR)

struct agx_ppp_hal300_depth_bias_record_fields {
   uint32_t depth_bias_raw;
   uint32_t slope_scale_raw;
   uint32_t clamp_raw;
};

struct agx_ppp_hal300_scissor_record_fields {
   uint16_t max_x;
   uint16_t min_x;
   uint16_t max_y;
   uint16_t min_y;
   uint32_t min_z_raw;
   uint32_t max_z_raw;
};

struct agx_ppp_hal300_depth_bias_scissor_bases_fields {
   uint64_t depth_bias_base;
   uint64_t scissor_base;
};

struct agx_ppp_hal300_depth_bias_record {
   uint8_t bytes[AGX_PPP_HAL300_DEPTH_BIAS_RECORD_LENGTH];
   uint8_t length;
};

struct agx_ppp_hal300_scissor_record {
   uint8_t bytes[AGX_PPP_HAL300_SCISSOR_RECORD_LENGTH];
   uint8_t length;
};

struct agx_ppp_hal300_depth_bias_scissor_bases {
   uint8_t bytes[AGX_PPP_HAL300_DEPTH_BIAS_SCISSOR_BASES_LENGTH];
   uint8_t length;
};

/*
 * The G17G/G17X render-state handlers preserve the G15/G16 record geometry:
 *
 *   depth bias: f32 depth bias, f32 slope scale, f32 clamp
 *   scissor:    u16 max X, u16 min X, u16 max Y, u16 min Y,
 *               f32 min Z, f32 max Z
 *
 * The PPP reference block is new.  Both initial render-state emission and a
 * FlexiblePSOUpdateToken emit these five words in exactly this order:
 *
 *   u32 header = bits 8 | 9
 *   u32 depth bias base bits 32..47 in bits 16..31
 *   u32 depth bias base bits 0..31
 *   u32 scissor base bits 32..47 in bits 16..31
 *   u32 scissor base bits 0..31
 *
 * G17X RenderContext initialization at 0x21783a534 stores 0x300 at
 * impl+0x79ec; its direct emitter at 0x217815f50 copies all 20 bytes.  The
 * G17G counterparts at 0x2160a0e48 and 0x21607c894 have the same geometry.
 * G17X FlexiblePSOUpdateToken::emit at 0x2177245a4 consumes selector bit 8
 * before bit 9 and copies the two base pairs in that order.  In contrast,
 * G15G_B0 stores the scissor and depth-bias indices at impl+0x7a00 and
 * impl+0x7a02 (0x21115e8d8 and 0x21119bd10), and G16G does likewise in
 * handleScissorUpdates/handleDepthBiasUpdates (0x214968560/0x214968284).
 *
 * G18G is admitted too: every J873 native render points its VDM at this
 * five-word block, e.g. 00000300 01000000 002d0000 0 0 in the colour mode.
 *
 * Bases are 48-bit and four-byte aligned.  G15/G16 use header bit 8 followed
 * by one packed index word (depth bias in high 16, scissor in low 16); that
 * legacy form is intentionally not accepted here.  These codecs do not emit
 * a VDM pointer, alter a command buffer, submit work, admit a device, or
 * advertise a feature.
 */
bool agx_ppp_hal300_depth_bias_scissor_core_supported(enum agx_chip chip);

bool agx_ppp_hal300_depth_bias_record_pack(
   enum agx_chip chip,
   const struct agx_ppp_hal300_depth_bias_record_fields *fields,
   struct agx_ppp_hal300_depth_bias_record *out);

bool agx_ppp_hal300_depth_bias_record_unpack(
   enum agx_chip chip, const void *bytes, size_t size,
   struct agx_ppp_hal300_depth_bias_record_fields *fields);

bool agx_ppp_hal300_scissor_record_pack(
   enum agx_chip chip,
   const struct agx_ppp_hal300_scissor_record_fields *fields,
   struct agx_ppp_hal300_scissor_record *out);

bool agx_ppp_hal300_scissor_record_unpack(
   enum agx_chip chip, const void *bytes, size_t size,
   struct agx_ppp_hal300_scissor_record_fields *fields);

bool agx_ppp_hal300_depth_bias_scissor_bases_pack(
   enum agx_chip chip,
   const struct agx_ppp_hal300_depth_bias_scissor_bases_fields *fields,
   struct agx_ppp_hal300_depth_bias_scissor_bases *out);

bool agx_ppp_hal300_depth_bias_scissor_bases_unpack(
   enum agx_chip chip, const void *bytes, size_t size,
   struct agx_ppp_hal300_depth_bias_scissor_bases_fields *fields);

#ifdef __cplusplus
}
#endif
