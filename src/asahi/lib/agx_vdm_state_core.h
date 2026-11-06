/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded, target-aware AGX3 VDM-state representations.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_VDM_STATE_VERTEX_PIPELINE_PAIR_LENGTH 8u
#define AGX_VDM_STATE_VERTEX_PIPELINE_TEMPLATE_MASK UINT32_C(0x003fffff)

struct agx_vdm_state_vertex_pipeline_pair {
   uint8_t bytes[AGX_VDM_STATE_VERTEX_PIPELINE_PAIR_LENGTH];
   uint8_t length;
};

/*
 * Pack/unpack only the two-word AGX3 VDM State vertex-pipeline field proven
 * in Apple's VDMEncoderGen5/Gen6 command encoders:
 *
 *   word 0 bits  0..21: caller-preserved raw state-template payload
 *   word 0 bits 22..31: pipeline address bits 38..47
 *   word 1 bits  0..31: pipeline address bits  6..37
 *
 * The pipeline address is therefore a 64-byte-aligned, 48-bit GPU virtual
 * address.  The raw template bits remain unnamed because their individual
 * semantics are not completely grounded.
 *
 * The target allowlist contains only identities directly served by the
 * inspected Apple Metal plugins, plus G18G, whose J873 native VDM state and
 * fragment PPP records carry this exact pair.  G16P and G17A share
 * neighboring command-stream generations but are not claimed by those
 * plugins and fail closed.
 * This helper does not represent the surrounding VDM State record, emit a
 * live command, or enable scheduling, submission, or device admission.
 */
bool agx_vdm_state_vertex_pipeline_pair_supported(enum agx_chip chip);

bool agx_vdm_state_vertex_pipeline_pair_pack(
   enum agx_chip chip, uint64_t pipeline, uint32_t raw_template,
   struct agx_vdm_state_vertex_pipeline_pair *out);

bool agx_vdm_state_vertex_pipeline_pair_unpack(
   enum agx_chip chip, const void *bytes, size_t size, uint64_t *pipeline,
   uint32_t *raw_template);

#ifdef __cplusplus
}
#endif
