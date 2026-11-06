/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded, target-aware AGX3 VDM PPP State Update representation.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_VDM_PPP_STATE_CORE_LENGTH         8u
#define AGX_VDM_PPP_STATE_CORE_RESERVED_MASK  UINT32_C(0xffff0000)
#define AGX_VDM_PPP_STATE_CORE_MAX_SIZE_WORDS UINT32_C(0xff)

struct agx_vdm_ppp_state_core {
   uint8_t bytes[AGX_VDM_PPP_STATE_CORE_LENGTH];
   uint8_t length;
};

/*
 * Pack/unpack the exact eight-byte VDM PPP State Update emitted by the
 * inspected Apple G15, HAL200, and HAL300 fixed-layout render-state paths:
 *
 *   word 0 bits  0..7: PPP-state pointer bits 32..39
 *   word 0 bits  8..15: PPP-state size in 32-bit words
 *   word 0 bits 16..28: zero
 *   word 0 bits 29..31: VDM block type 0 (PPP State Update)
 *   word 1 bits  0..31: PPP-state pointer bits 0..31
 *
 * The direct emitters insert pointer bits 2..31 into serialized bits 34..63,
 * establishing the four-byte alignment requirement.  The existing exact PPP
 * packet grammar independently establishes the complete eight-bit size field.
 * This helper deliberately validates only representation: it does not inspect
 * the pointed-to PPP payload or prove that every representable size describes
 * a semantically complete payload.
 *
 * The target allowlist contains only identities directly served by the
 * inspected G15G/G15X, G16G/G16X, G17P, and G17G/G17X Apple Metal plugins,
 * plus G18G, whose J873 native VDM streams use this exact record for every
 * PPP update.  G16P and G17A remain closed despite sharing neighboring command-stream
 * generations.  This helper does not emit a live command or enable command
 * allocation, submission, or device admission.
 */
bool agx_vdm_ppp_state_core_supported(enum agx_chip chip);

bool agx_vdm_ppp_state_core_pack(enum agx_chip chip, uint64_t pointer,
                                 uint32_t size_words,
                                 struct agx_vdm_ppp_state_core *out);

bool agx_vdm_ppp_state_core_unpack(enum agx_chip chip, const void *bytes,
                                   size_t size, uint64_t *pointer,
                                   uint32_t *size_words);

#ifdef __cplusplus
}
#endif
