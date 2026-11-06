/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded, target-aware AGX3 viewport-only PPP payload representation.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_PPP_VIEWPORT_PAYLOAD_MAX_VIEWPORTS 16u
#define AGX_PPP_VIEWPORT_PAYLOAD_MIN_LENGTH    40u
#define AGX_PPP_VIEWPORT_PAYLOAD_MAX_LENGTH    520u
#define AGX_PPP_VIEWPORT_PAYLOAD_GEN6_BASE_HEADER UINT32_C(0x00000c00)
#define AGX_PPP_VIEWPORT_PAYLOAD_GEN7_BASE_HEADER UINT32_C(0x00003000)

struct agx_ppp_viewport_transform_raw {
   uint32_t translate_x;
   uint32_t scale_x;
   uint32_t translate_y;
   uint32_t scale_y;
   uint32_t translate_z;
   uint32_t scale_z;
};

struct agx_ppp_viewport_payload_fields {
   uint8_t viewport_count;
   uint64_t region_clip_raw[AGX_PPP_VIEWPORT_PAYLOAD_MAX_VIEWPORTS];
   uint32_t viewport_control_raw;
   struct agx_ppp_viewport_transform_raw
      viewports[AGX_PPP_VIEWPORT_PAYLOAD_MAX_VIEWPORTS];
};

struct agx_ppp_viewport_payload_core {
   uint8_t bytes[AGX_PPP_VIEWPORT_PAYLOAD_MAX_LENGTH];
   uint16_t length;
};

/*
 * Pack/unpack only the viewport-only PPP payload emitted by the inspected
 * Apple fixed-layout render-state paths:
 *
 *   u32 PPP header (region-clip + viewport bits set)
 *   viewport_count raw 8-byte region-clip records
 *   u32 raw viewport-control record
 *   viewport_count raw 24-byte viewport transform records
 *
 * The six transform words are directly ordered as translate/scale X, Y, Z.
 * They remain integer bit patterns so NaNs and every other floating-point
 * payload round-trip losslessly.  Region-clip and viewport-control semantics
 * are deliberately not inferred here and remain raw.
 *
 * G15, G16, and G17P PPPEncoderGen6 place viewport_count - 1 in header bits
 * 12..15.  G17G/G17X PPPEncoderGen7 place it in bits 14..17.  All other
 * header bits are fixed to the exact viewport-only form, so mixed or extended
 * PPP updates fail closed.  The region-clip and viewport selectors themselves
 * move from bits 10/11 to bits 12/13 at Gen7, so even a one-viewport packet is
 * generation-specific.
 *
 * The target allowlist contains only identities directly served by the
 * inspected G15G/G15X, G16G/G16X, G17P, and G17G/G17X plugins, plus G18G,
 * whose J873 native renders carry the Gen7 one-viewport form.  This helper
 * does not allocate or emit a live PPP update, follow a VDM pointer, submit a
 * command, or admit a device.
 */
bool agx_ppp_viewport_payload_core_supported(enum agx_chip chip);

bool agx_ppp_viewport_payload_core_pack(
   enum agx_chip chip, const struct agx_ppp_viewport_payload_fields *fields,
   struct agx_ppp_viewport_payload_core *out);

bool agx_ppp_viewport_payload_core_unpack(
   enum agx_chip chip, const void *bytes, size_t size,
   struct agx_ppp_viewport_payload_fields *fields);

#ifdef __cplusplus
}
#endif
