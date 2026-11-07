/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Bounded, target-aware AGX3 VDM Stream Link representation.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agx_chip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGX_VDM_STREAM_LINK_CORE_LENGTH      8u
#define AGX_VDM_STREAM_LINK_CORE_HEADER      UINT32_C(0x80000000)
#define AGX_VDM_STREAM_LINK_CORE_HEADER_MASK UINT32_C(0xffffff00)

struct agx_vdm_stream_link_core {
   uint8_t bytes[AGX_VDM_STREAM_LINK_CORE_LENGTH];
   uint8_t length;
};

bool agx_vdm_stream_link_core_supported(enum agx_chip chip);

bool agx_vdm_stream_link_core_pack(enum agx_chip chip, uint64_t target,
                                   struct agx_vdm_stream_link_core *out);

bool agx_vdm_stream_link_core_unpack(enum agx_chip chip, const void *bytes,
                                     size_t size, uint64_t *target);

#ifdef __cplusplus
}
#endif
