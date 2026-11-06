/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded HAL300 PPP depth/stencil state-block representation.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_PPP_HAL300_DEPTH_STENCIL_CORE_LENGTH 24u
#define AGX_PPP_HAL300_DEPTH_STENCIL_HEADER UINT32_C(0x00000001)

struct agx_ppp_hal300_depth_stencil_fields {
   uint32_t fragment_control_raw;
   uint32_t fragment_front_face_raw;
   uint32_t fragment_back_face_raw;
   uint32_t fragment_front_stencil_raw;
   uint32_t fragment_back_stencil_raw;
};

struct agx_ppp_hal300_depth_stencil_core {
   uint8_t bytes[AGX_PPP_HAL300_DEPTH_STENCIL_CORE_LENGTH];
   uint8_t length;
};

/*
 * Pack/unpack only the exact six-word HAL300 PPP block recovered from the
 * inspected G17G fixed-layout render-state path:
 *
 *   u32 header = bit 0
 *   u32 fragment control
 *   u32 fragment front face
 *   u32 fragment back face
 *   u32 fragment front stencil
 *   u32 fragment back stencil
 *
 * The G17G path copies the 24-byte constant block at 0x4e8180..0x4e81c0.
 * Its local rodata witness at file offset 0x952a40 begins with header bit 0
 * and five recognizable depth/stencil payload words.  Dynamic lowering at
 * 0x4ec5a4..0x4ec628 stages the same five slots in the order above.  The
 * homologous G16 path at 0x213e49834..0x213e498b8 instead stages front
 * stencil before back face, proving the HAL300-only adjacent-word swap.
 *
 * The admitted chip identities are the three HAL300 G17 GPU classes covered
 * by the G17G/G17X plugin witnesses, and G18G, whose J873 native renders
 * carry this block (00000001 000007aa e2e00000 f0c00000 00023020 0001ffff)
 * in every mode.  This codec neither emits the surrounding VDM PPP pointer
 * nor admits a device or advertises a feature.
 */
bool agx_ppp_hal300_depth_stencil_core_supported(enum agx_chip chip);

bool agx_ppp_hal300_depth_stencil_core_pack(
   enum agx_chip chip,
   const struct agx_ppp_hal300_depth_stencil_fields *fields,
   struct agx_ppp_hal300_depth_stencil_core *out);

bool agx_ppp_hal300_depth_stencil_core_unpack(
   enum agx_chip chip, const void *bytes, size_t size,
   struct agx_ppp_hal300_depth_stencil_fields *fields);

#ifdef __cplusplus
}
#endif
